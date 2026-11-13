#!/usr/bin/env python3
"""XNU learns which logical CPUs share a cache from Intel's CPUID leaf 4, which AMD does not implement. cpuid_set_cache_info() (a static function that
sits right after _cpuid_wa_required in the Monterey KC) then takes its legacy AMD fallback: only the L2 is described, shared by ONE thread, and
cpuid_cores_per_package = 1. cpu_topology_sort() makes one affinity set and one processor set per distinct last-level cache, so every logical
CPU got its own pset: the second pset of a cpus=2 boot, whose lock this very thread leaked (self deadlock in thread_setrun).

Patch for the Monterey KC (21.6.0), as OpenCore Kernel>Patch entries (prints nothing; imported by make_baremetal_efi.py):
  the fallback block (cores_per_package = 1, L2 size/sharing/partitions) -> jmp stub
  _nfs4_getattr_rpc-sized dead NFSv4 code (STUB_SYM)                      -> stub: the numbers of a Zen 2 package, then the displaced
                                                                              instructions and a jump back
Zen 2 as XNU wants it: 8 cores per package; L1I/L1D 32 KB and L2 512 KB shared by 2 threads (SMT), L3 4 MB per CCX shared by 8 threads."""
import re, struct, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
from toolpaths import llvm_bin, nm_bin, require_kc     # LLVM_BIN / NM_BIN override the Homebrew and Xcode defaults
KC = ROOT / "build/ext/kernel/BootKernelExtensions-monterey.kc"
STUB_SYM = "_nfs4_getattr_rpc"
BLOCK, BLOCK_LEN = 0x60F, 0x38           # offsets from _cpuid_wa_required: the fallback block, up to (not including) 'cmpl $0, debug flag'
# offsets (from _cpuid_wa_required) of the cpuid_info fields, taken from the disassembly
CORES_PER_PACKAGE, CACHE_SIZE, CACHE_SHARING, CACHE_PARTS, LINESIZE_SRC = 0xF49B38, 0xF49AE0, 0xF49B40, 0xF49B54, 0xF49BC4
# index: 1 = L1I, 2 = L1D, 3 = L2, 4 = L3
ZEN2 = {1: (32768, 2), 2: (32768, 2), 3: (524288, 2), 4: (4194304, 8)}
CORES = 8


def _symbols():
    require_kc(KC)
    out = subprocess.run([nm_bin(), str(KC)], capture_output=True, text=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3 and re.fullmatch(r"[0-9a-f]{16}", l.split()[0])}


def _kernel_bytes(va, n):
    d = KC.read_bytes()
    text_va, text_off = 0xffffff8000210000, 0x10000            # __TEXT_EXEC of the Monterey KC
    return d[text_off + (va - text_va):text_off + (va - text_va) + n]


def build_stub(base, stub_va):
    lines = [".code64", ".globl stub", "stub:", f"    movl ${CORES}, cores_per_package(%rip)"]
    for i, (size, share) in ZEN2.items():
        lines += [f"    movl ${size}, size{i}(%rip)", f"    movl ${share}, share{i}(%rip)", f"    movl $1, parts{i}(%rip)"]
    # the instructions that follow the replaced block in the original: r14d = line size (kept), then back to the compare
    lines += ["    movl linesize_src(%rip), %r14d", "    movl %r14d, -0x44(%rbp)", "    jmp back"]
    defs = {"cores_per_package": base + CORES_PER_PACKAGE, "linesize_src": base + LINESIZE_SRC, "back": base + BLOCK + BLOCK_LEN}
    for i in ZEN2:
        defs[f"size{i}"] = base + CACHE_SIZE + 4 * i
        defs[f"share{i}"] = base + CACHE_SHARING + 4 * i
        defs[f"parts{i}"] = base + CACHE_PARTS + 4 * i
    with tempfile.TemporaryDirectory() as td:
        Path(f"{td}/s.S").write_text("\n".join(lines) + "\n")
        subprocess.check_call([f"{llvm_bin()}/clang", "-target", "x86_64-unknown-none-elf", "-c", f"{td}/s.S", "-o", f"{td}/s.o"])
        subprocess.check_call([f"{llvm_bin()}/ld.lld", "-nostdlib", "-static", f"-Ttext={stub_va:#x}", "-e", "stub",
                               *[f"--defsym={k}={v:#x}" for k, v in defs.items()], f"{td}/s.o", "-o", f"{td}/s.elf"])
        subprocess.check_call([f"{llvm_bin()}/llvm-objcopy", "-O", "binary", f"{td}/s.elf", f"{td}/s.bin"])
        return Path(f"{td}/s.bin").read_bytes()


def patches():
    def entry(sym, find, code, comment, limit=0):
        return {"Arch": "x86_64", "Base": sym, "Comment": comment, "Count": 1, "Enabled": True, "Find": find,
                "Identifier": "kernel", "Limit": limit, "Mask": b"", "MaxKernel": "", "MinKernel": "",
                "Replace": code, "ReplaceMask": b"", "Skip": 0}
    sym = _symbols()
    base, stub_va = sym["_cpuid_wa_required"], sym[STUB_SYM]
    block = _kernel_bytes(base + BLOCK, BLOCK_LEN)
    # sanity: the block starts with 'movl $1, cores_per_package' and ends right before 'cmpl $0, ...' (83 3d)
    assert block[:2] == b"\xc7\x05" and _kernel_bytes(base + BLOCK + BLOCK_LEN, 2) == b"\x83\x3d", "unexpected cpuid_set_cache_info fallback block"
    jmp = b"\xE9" + struct.pack("<i", stub_va - (base + BLOCK + 5)) + b"\x90" * (BLOCK_LEN - 5)
    stub = build_stub(base, stub_va)
    assert len(stub) < 0x200
    return [entry("_cpuid_wa_required", block, jmp, "cache: AMD legacy cache fallback jumps to the Zen 2 cache description", limit=0x800),
            entry(STUB_SYM, b"", stub, "cache: Zen 2 cores/caches for XNU (unused NFSv4 code)")]


if __name__ == "__main__":
    for p in patches():
        print(p["Comment"], len(p["Replace"]), p["Replace"][:16].hex())
