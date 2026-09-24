#!/usr/bin/env python3
"""Compile-time hardening of every ELF file in a root filesystem.

    binary-hardening-report.py <rootfs dir> [--list]

What checksec reports, read directly from the ELF headers (pyelftools), so
it works on a cross-built aarch64 rootfs without running anything:

  PIE        executable is ET_DYN with an interpreter (ASLR for the main binary)
  RELRO      PT_GNU_RELRO present; "full" if also BIND_NOW (GOT read-only)
  NX         PT_GNU_STACK without the execute bit
  canary     imports __stack_chk_fail (-fstack-protector*)
  FORTIFY    imports at least one __*_chk function (-D_FORTIFY_SOURCE)
  BTI/PAC    GNU property note AARCH64_FEATURE_1_AND has BTI and PAC set
             (-mbranch-protection=standard). Encoded in hint space: on a CPU
             without FEAT_BTI / FEAT_PAuth (the Pi 5's Cortex-A76 is Armv8.2
             and has neither) the instructions execute as NOPs.

Caveats, stated in the output too: "canary" and "FORTIFY" can only be seen
where the compiler actually inserted a check - a program with no arrays on
the stack and no fortifiable calls shows "no" although it was built with the
flags. The numbers are a lower bound for those two columns.
"""
import os
import sys
from collections import Counter

from elftools.elf.dynamic import DynamicSection
from elftools.elf.elffile import ELFFile

FORTIFY_EXCEPTIONS = {"__stack_chk_fail"}


def inspect(path):
    with open(path, "rb") as f:
        try:
            elf = ELFFile(f)
        except Exception:
            return None
        if elf["e_machine"] not in ("EM_AARCH64",):
            return None
        etype = elf["e_type"]
        interp = any(seg["p_type"] == "PT_INTERP" for seg in elf.iter_segments())
        # glibc's libc.so.6 has an interpreter too (it runs and prints its
        # version) - a ".so" name decides.
        is_so = ".so" in os.path.basename(path)
        if etype == "ET_EXEC" or (etype == "ET_DYN" and interp and not is_so):
            kind = "exe"
        elif etype == "ET_DYN":
            kind = "lib"
        else:
            return None  # relocatable objects, kernel modules
        r = {"kind": kind, "pie": etype == "ET_DYN"}
        segs = {seg["p_type"]: seg for seg in elf.iter_segments()}
        r["relro"] = "PT_GNU_RELRO" in segs
        stack = segs.get("PT_GNU_STACK")
        r["nx"] = stack is not None and not (stack["p_flags"] & 1)
        bind_now = False
        imports = set()
        for sec in elf.iter_sections():
            if isinstance(sec, DynamicSection):
                for tag in sec.iter_tags():
                    if tag.entry.d_tag == "DT_BIND_NOW":
                        bind_now = True
                    elif tag.entry.d_tag == "DT_FLAGS" and tag.entry.d_val & 0x8:
                        bind_now = True
                    elif tag.entry.d_tag == "DT_FLAGS_1" and tag.entry.d_val & 0x1:
                        bind_now = True
            if sec.name == ".dynsym":
                for sym in sec.iter_symbols():
                    if sym["st_shndx"] == "SHN_UNDEF" and sym.name:
                        imports.add(sym.name)
        r["full_relro"] = r["relro"] and bind_now
        r["bti_pac"] = False
        for sec in elf.iter_sections():
            if sec.name == ".note.gnu.property":
                for note in sec.iter_notes():
                    for prop in note["n_desc"] or []:
                        # GNU_PROPERTY_AARCH64_FEATURE_1_AND, bit 0 BTI, bit 1 PAC
                        if prop["pr_type"] == 0xC0000000 and len(prop["pr_data"]) >= 4:
                            bits = int.from_bytes(prop["pr_data"][:4], "little")
                            r["bti_pac"] = (bits & 3) == 3
        r["canary"] = "__stack_chk_fail" in imports
        r["fortify"] = any(n.startswith("__") and n.endswith("_chk")
                           and n not in FORTIFY_EXCEPTIONS for n in imports)
        return r


def main():
    root = sys.argv[1]
    listing = "--list" in sys.argv
    rows = []
    seen = set()
    for dirpath, _, files in os.walk(root):
        for name in files:
            p = os.path.join(dirpath, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            st = os.stat(p)
            if (st.st_dev, st.st_ino) in seen:  # hard links (busybox etc.)
                continue
            seen.add((st.st_dev, st.st_ino))
            if "/lib/modules/" in p or p.endswith(".ko"):
                continue
            r = inspect(p)
            if r:
                r["path"] = os.path.relpath(p, root)
                rows.append(r)

    for kind, title in (("exe", "executables"), ("lib", "shared libraries")):
        sel = [r for r in rows if r["kind"] == kind]
        n = len(sel)
        c = Counter()
        for r in sel:
            for k in ("pie", "relro", "full_relro", "nx", "canary", "fortify", "bti_pac"):
                c[k] += r[k]
        print(f"{title}: {n}")
        cols = ["pie", "relro", "full_relro", "nx", "canary", "fortify", "bti_pac"] if kind == "exe" \
            else ["relro", "full_relro", "nx", "canary", "fortify", "bti_pac"]
        for k in cols:
            print(f"  {k:<11} {c[k]:>4}/{n}  ({100 * c[k] / n:5.1f} %)" if n else f"  {k}: -")
    if listing:
        print()
        print("kind pie relro full nx  canary fortify bti/pac  path")
        for r in sorted(rows, key=lambda r: r["path"]):
            yn = lambda b: "y" if b else "-"
            print(f"{r['kind']:<4} {yn(r['pie']):<3} {yn(r['relro']):<5} {yn(r['full_relro']):<4} "
                  f"{yn(r['nx']):<3} {yn(r['canary']):<6} {yn(r['fortify']):<7} {yn(r['bti_pac']):<7}  {r['path']}")
    print("\ncanary/FORTIFY are a lower bound: only visible where the compiler inserted a check.")


if __name__ == "__main__":
    main()
