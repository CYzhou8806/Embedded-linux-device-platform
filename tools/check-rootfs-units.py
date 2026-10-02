#!/usr/bin/env python3
"""Release gate: catch the systemd failures that only show up on the board.

Twice now an image built cleanly and then failed on the Pi for reasons the
build never looked at (docs/debugging/case-11, and update 1.3.0 on
2026-10-02):

  1. An ordering cycle. systemd breaks it by *deleting a start job* and
     logs one line; the unit is simply never started. (case 11: the
     health check; 1.3.0: device-service itself.)
  2. A command a unit runs that the image doesn't contain - a BusyBox
     applet cut with the package diet (case 11: SSH; 1.3.0: `install`).

Both are visible offline in the rootfs bitbake produced:

  1. `systemd-analyze verify --root=ROOTFS` builds the boot transaction for
     multi-user.target and every enabled unit and reports cycles;
  2. every Exec*= line of every unit and drop-in is resolved against the
     rootfs - the binary, and for `sh -c '...'` the first word of every
     command in the script.

    check-rootfs-units.py ROOTFS        exit 0 = clean, 1 = findings

make-signed-ab-release.sh runs it before signing anything.
"""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

PATH_DIRS = ["usr/sbin", "usr/bin", "sbin", "bin"]
SH_BUILTINS = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in",
    "!", "[", "[[", "]]", "test", "echo", "printf", "exit", "return", "set", "unset", "export", "local",
    "read", "cd", "true", "false", ":", ".", "source", "exec", "eval", "shift", "trap", "wait", "command",
    "type", "umask", "break", "continue", "{", "}", "(", ")",
}
UNIT_DIRS = ["usr/lib/systemd/system", "lib/systemd/system", "etc/systemd/system"]


def exists_in_root(root: Path, path: str) -> bool:
    """Does an absolute path exist inside root, following symlinks within root?"""
    p = root / path.lstrip("/")
    for _ in range(16):
        if p.is_symlink():
            target = os.readlink(p)
            p = (root / target.lstrip("/")) if target.startswith("/") else (p.parent / target)
            continue
        return p.exists()
    return False


def find_command(root: Path, name: str) -> bool:
    if name.startswith("/"):
        return exists_in_root(root, name)
    return any(exists_in_root(root, f"/{d}/{name}") for d in PATH_DIRS)


def script_commands(script: str):
    """First word of each simple command in a small sh -c script."""
    for part in re.split(r"\|\||&&|[;|\n]|\$\(|`", script):
        words = part.strip().split()
        while words and (words[0] in SH_BUILTINS or "=" in words[0].split("/")[0]):
            if words[0] in ("echo", "printf", "exit", "return", "test", "[", "read", "cd", "set", "export",
                            "trap", "wait", "type", "umask", ".", "source", "eval"):
                words = []  # the rest are arguments
                break
            words = words[1:]
        if words:
            w = words[0].strip("'\"()")
            if w and not w.startswith("$") and not w.startswith("-") and w not in SH_BUILTINS:
                yield w


def check_exec(root: Path) -> list[str]:
    findings = []
    seen = set()
    visited = set()
    for d in UNIT_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*")):
            if not f.is_file() or f.suffix not in (".service", ".conf", ".socket", ".timer", ".mount"):
                continue
            real = f.resolve()
            if real in visited:  # /lib is /usr/lib with usrmerge
                continue
            visited.add(real)
            rel = f.relative_to(root)
            text = f.read_text(errors="replace")
            # A unit that is skipped when the path is absent can name it freely.
            guarded = set(re.findall(r"^\s*Condition(?:PathExists|FileIsExecutable)=\|?(/\S+)", text, re.M))
            for n, line in enumerate(text.splitlines(), 1):
                m = re.match(r"\s*(Exec(?:Start|StartPre|StartPost|Stop|StopPost|Reload|Condition))=(.*)", line)
                if not m or not m.group(2).strip():
                    continue
                raw = m.group(2).strip()
                prefixes = raw[: len(raw) - len(raw.lstrip("-+@:!|"))]
                if "-" in prefixes:
                    continue  # "-": failure (including a missing binary) is ignored by design
                cmd = raw.lstrip("-+@:!|")
                try:
                    argv = shlex.split(cmd)
                except ValueError:
                    continue
                if not argv:
                    continue
                if not find_command(root, argv[0]) and argv[0] not in guarded:
                    findings.append(f"{rel}:{n}: {argv[0]}: not in the image")
                    continue
                if os.path.basename(argv[0]) in ("sh", "bash") and len(argv) >= 3 and argv[1] == "-c":
                    for w in script_commands(argv[2]):
                        key = (str(rel), w)
                        if key in seen:
                            continue
                        seen.add(key)
                        if not find_command(root, w):
                            findings.append(f"{rel}:{n}: sh -c uses '{w}', which is not in the image")
    return findings


def check_cycles(root: Path) -> list[str]:
    targets = ["multi-user.target"]
    wants = root / "etc/systemd/system/multi-user.target.wants"
    if wants.is_dir():
        targets += sorted(p.name for p in wants.iterdir())
    out = subprocess.run(["systemd-analyze", "verify", f"--root={root}", "--man=no", *targets],
                         capture_output=True, text=True)
    return sorted({l.strip() for l in (out.stdout + out.stderr).splitlines()
                   if "ordering cycle" in l or "deleted to break" in l})


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1]).resolve()
    cycles = check_cycles(root)
    missing = check_exec(root)
    for l in cycles:
        print(f"CYCLE    {l}")
    for l in missing:
        print(f"MISSING  {l}")
    print(f"{len(cycles)} ordering-cycle lines, {len(missing)} missing commands")
    return 1 if cycles or missing else 0


if __name__ == "__main__":
    sys.exit(main())
