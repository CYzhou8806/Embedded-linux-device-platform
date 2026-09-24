#!/usr/bin/env python3
"""Check a kernel .config against a short list of hardening options.

    kconfig-hardening-check.py <.config> [<.config> ...]

Not a replacement for kernel-hardening-checker
(https://github.com/a13xp0p0v/kernel-hardening-checker, several hundred
checks from the KSPP recommendations and others); a hand-picked subset for
arm64 whose every line can be explained, and whose result for this device
is discussed in docs/security/hardening.md §7. With two configs it prints
them side by side (before/after a kernel upgrade).

Each check: option, wanted value ("y", "n" = not set, or "y|n" when only one
of several is needed), group, and why.
"""
import sys

CHECKS = [
    # --- kernel self-protection: make a kernel bug harder to exploit ---
    ("STRICT_KERNEL_RWX", "y", "self", "kernel text read-only, data non-executable"),
    ("STRICT_MODULE_RWX", "y", "self", "same for modules"),
    ("RANDOMIZE_BASE", "y", "self", "KASLR"),
    ("VMAP_STACK", "y", "self", "stack overflow hits a guard page instead of memory next to it"),
    ("STACKPROTECTOR_STRONG", "y", "self", "stack canaries in the kernel"),
    ("HARDENED_USERCOPY", "y", "self", "bounds-check copy_to/from_user against the slab object"),
    ("FORTIFY_SOURCE", "y", "self", "compile-time/run-time checks of memcpy & co."),
    ("INIT_STACK_ALL_ZERO", "y", "self", "no uninitialised stack variables leak or get used"),
    ("INIT_ON_ALLOC_DEFAULT_ON", "y", "self", "heap memory zeroed on allocation"),
    ("INIT_ON_FREE_DEFAULT_ON", "y", "self", "heap memory zeroed on free (costs a few %)"),
    ("SLAB_FREELIST_HARDENED", "y", "self", "slab freelist pointers obfuscated/checked"),
    ("SLAB_FREELIST_RANDOM", "y", "self", "slab allocation order randomised"),
    ("SHUFFLE_PAGE_ALLOCATOR", "y", "self", "page allocator randomised"),
    ("LIST_HARDENED", "y", "self", "linked-list corruption checks (6.6+)"),
    ("BUG_ON_DATA_CORRUPTION", "y", "self", "stop on detected corruption instead of continuing"),
    ("SCHED_STACK_END_CHECK", "y", "self", "detect task stack overrun at schedule()"),
    ("DEBUG_WX", "y", "self", "warn at boot about writable+executable mappings"),
    ("UNMAP_KERNEL_AT_EL0", "y", "self", "KPTI (Meltdown class)"),
    ("MITIGATE_SPECTRE_BRANCH_HISTORY", "y", "self", "Spectre-BHB"),
    ("ARM64_PAN", "y", "self", "kernel can't touch user memory by accident (Armv8.1 PAN)"),
    ("ARM64_PTR_AUTH_KERNEL", "y", "self", "return-address signing (needs Armv8.3; A76 hasn't it)"),
    ("ARM64_BTI_KERNEL", "y", "self", "branch target identification (needs Armv8.5; A76 hasn't it)"),
    ("SECCOMP_FILTER", "y", "self", "seccomp (used by the device-service sandbox)"),
    ("SECURITY_YAMA", "y", "self", "ptrace scope restriction"),
    ("SECURITY_LOCKDOWN_LSM", "y", "self", "lockdown: root can't modify the running kernel"),
    ("SECURITY_DMESG_RESTRICT", "y", "self", "dmesg_restrict=1 by default"),
    ("MODULE_SIG", "y", "self", "kernel module signatures"),
    ("MODULE_SIG_FORCE", "y", "self", "refuse unsigned modules"),
    ("BPF_UNPRIV_DEFAULT_OFF", "y", "self", "unprivileged eBPF off by default"),
    ("STRICT_DEVMEM", "y", "self", "/dev/mem limited to I/O regions"),
    ("IO_STRICT_DEVMEM", "y", "self", "... and not to regions a driver claimed"),
    # --- attack surface: code that shouldn't be reachable on this device ---
    ("DEVMEM", "n", "surface", "/dev/mem at all"),
    ("PROC_KCORE", "n", "surface", "/proc/kcore: kernel memory as a file"),
    ("KEXEC", "n", "surface", "replace the running kernel"),
    ("HIBERNATION", "n", "surface", "memory image on disk"),
    ("LEGACY_TIOCSTI", "n", "surface", "inject input into another terminal"),
    ("LDISC_AUTOLOAD", "n", "surface", "load line disciplines on request"),
    ("COMPAT", "n", "surface", "32-bit syscall ABI - a second syscall surface"),
    ("IO_URING", "n", "surface", "io_uring - frequent source of kernel CVEs"),
    ("USERFAULTFD", "n", "surface", "used to win kernel race conditions"),
    ("BINFMT_MISC", "n", "surface", "arbitrary interpreters for binaries"),
    ("MAGIC_SYSRQ", "n", "surface", "SysRq"),
    ("KPROBES", "n", "surface", "debugging: arbitrary kernel instrumentation"),
    ("FTRACE", "n", "surface", "debugging: function tracer (this project uses it - case 04/09)"),
]


def load(path):
    cfg = {}
    for line in open(path):
        line = line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            k, v = line.split("=", 1)
            cfg[k[7:]] = v
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            cfg[line[9:-11]] = "n"
    return cfg


def state(cfg, opt, want):
    v = cfg.get(opt)
    if v is None:
        v = "-"   # option doesn't exist in this kernel version, or not set
    have = "y" if v in ("y", "m") else "n"
    if want == "n" and v == "m":
        have = "m"
    ok = (have == want) if want != "n" else (have == "n")
    return v, ok


def main():
    paths = sys.argv[1:]
    cfgs = [load(p) for p in paths]
    names = [p.split("/")[-1] for p in paths]
    print(f"{'option':<34}{'want':<6}" + "".join(f"{n[:18]:<20}" for n in names) + "why")
    totals = [[0, 0] for _ in cfgs]
    group = None
    for opt, want, grp, why in CHECKS:
        if grp != group:
            group = grp
            print(f"-- {'self-protection' if grp == 'self' else 'attack surface'}")
        cells = ""
        for i, cfg in enumerate(cfgs):
            v, ok = state(cfg, opt, want)
            totals[i][0] += ok
            totals[i][1] += 1
            cells += f"{('OK  ' if ok else 'FAIL') + ' ' + v:<20}"
        print(f"{opt:<34}{want:<6}{cells}{why}")
    print()
    for n, (ok, tot) in zip(names, totals):
        print(f"{n}: {ok}/{tot} as recommended")


if __name__ == "__main__":
    main()
