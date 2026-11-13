#!/usr/bin/env python3
"""XNU's clocks on this AMD: OpenCore hands boot.efi a 1 GHz TSC (the real one runs at 1596.3 MHz, Linux: 'tsc: Detected 1596.300 MHz') and
a 100 MHz bus while the LAPIC timer really counts at ~25 MHz. Every XNU nanosecond was then 1.6 real ones and every one-shot timer fired
about 6x late; the USB 3 controller's power-up timeouts (IOSleep/absolute time) expired before it was ready.

Patch for the Monterey KC (21.6.0), as OpenCore Kernel>Patch entries (prints nothing; imported by make_baremetal_efi.py):
  _tsc_init (its epilogue, pops + ret)  -> jmp stub
  _nfs4_open_rpc_internal (dead NFSv4 code) -> stub: store the globals tsc_init would have computed for bus = TSC/64, ratio 64, then the
                                              displaced epilogue.
Globals (xnu osfmk/i386/tsc.c): busFreq, tscGranularity, busFCvtt2n/n2t, tscFCvtt2n/n2t, tscFreq, bus2tsc."""
import re, struct, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
from toolpaths import llvm_bin, nm_bin, require_kc     # LLVM_BIN / NM_BIN override the Homebrew and Xcode defaults
KC = ROOT / "build/ext/kernel/BootKernelExtensions-monterey.kc"
STUB_SYM = "_nfs4_open_rpc_internal"
EPILOGUE = bytes.fromhex("5b415c415d415e415f5dc3")        # pop rbx, r12-r15, rbp; ret (the end of tsc_init)
TSC_HZ = 1596300000
RATIO = 64
NS = 10**9
M64 = (1 << 64) - 1


def _symbols():
    require_kc(KC)
    out = subprocess.run([nm_bin(), str(KC)], capture_output=True, text=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3 and l.split()[1] in ("T", "t", "S", "D", "s", "d")}


def _kernel_bytes(va, n):
    d = KC.read_bytes()
    text_va, text_off = 0xffffff8000210000, 0x10000            # __TEXT_EXEC of the Monterey KC
    return d[text_off + (va - text_va):text_off + (va - text_va) + n]


def values():
    bus = TSC_HZ // RATIO
    bus_t2n = (NS << 32) // bus
    bus_n2t = M64 // bus_t2n
    tsc_t2n = bus_t2n // RATIO
    tsc_hz = (NS << 32) // tsc_t2n
    tsc_n2t = M64 // tsc_t2n
    bus2tsc = ((bus_t2n * tsc_n2t) >> 32) & M64           # tmrCvt(busFCvtt2n, tscFCvtn2t)
    return {"busFreq": bus, "tscGranularity": RATIO, "busFCvtt2n": bus_t2n, "busFCvtn2t": bus_n2t,
            "tscFCvtt2n": tsc_t2n, "tscFCvtn2t": tsc_n2t, "tscFreq": tsc_hz, "bus2tsc": bus2tsc}


def build_stub(stub_va):
    vals = values()
    syms = _symbols()
    lines = [".code64", ".globl stub", "stub:"]
    for name, v in vals.items():
        lines += [f"    movabsq ${v}, %rax", f"    movq %rax, {name}(%rip)"]
    lines += ["    popq %rbx", "    popq %r12", "    popq %r13", "    popq %r14", "    popq %r15", "    popq %rbp", "    retq"]
    with tempfile.TemporaryDirectory() as td:
        Path(f"{td}/s.S").write_text("\n".join(lines) + "\n")
        subprocess.check_call([f"{llvm_bin()}/clang", "-target", "x86_64-unknown-none-elf", "-c", f"{td}/s.S", "-o", f"{td}/s.o"])
        defs = [f"--defsym={n}={syms['_' + n]:#x}" for n in vals]
        subprocess.check_call([f"{llvm_bin()}/ld.lld", "-nostdlib", "-static", f"-Ttext={stub_va:#x}", "-e", "stub", *defs, f"{td}/s.o", "-o", f"{td}/s.elf"])
        subprocess.check_call([f"{llvm_bin()}/llvm-objcopy", "-O", "binary", f"{td}/s.elf", f"{td}/s.bin"])
        return Path(f"{td}/s.bin").read_bytes()


def patches():
    def entry(sym, find, code, comment, limit=0):
        return {"Arch": "x86_64", "Base": sym, "Comment": comment, "Count": 1, "Enabled": True, "Find": find,
                "Identifier": "kernel", "Limit": limit, "Mask": b"", "MaxKernel": "", "MinKernel": "",
                "Replace": code, "ReplaceMask": b"", "Skip": 0}
    sym = _symbols()
    tsc_init, stub_va = sym["_tsc_init"], sym[STUB_SYM]
    body = _kernel_bytes(tsc_init, 0x600)
    hits = [m.start() for m in re.finditer(re.escape(EPILOGUE), body)]
    assert len(hits) == 1, hits
    site = tsc_init + hits[0]
    jmp = b"\xE9" + struct.pack("<i", stub_va - (site + 5)) + b"\x90" * (len(EPILOGUE) - 5)
    stub = build_stub(stub_va)
    assert len(stub) < 0x400
    return [entry("_tsc_init", EPILOGUE, jmp, "tsc: tsc_init epilogue jumps to the clock fix", limit=0x600),
            entry(STUB_SYM, b"", stub, "tsc: clock fix (TSC 1596.3 MHz, bus = TSC/64) in unused NFSv4 code")]


if __name__ == "__main__":
    for k, v in values().items():
        print(k, v, hex(v))
    for p in patches():
        print(p["Comment"], len(p["Replace"]), p["Replace"][:12].hex())
