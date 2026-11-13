#!/usr/bin/env python3
"""Build the kernel text console patch (bringup/ktty/ktty.c) as OpenCore Kernel>Patch entries for Monterey (21.6.0):
  _serial_init          -> mov eax,1; ret          (the kernel believes a UART is present, so it routes console output to serial_putc)
  _serial_putc_options  -> jmp main           (rel32; the kernel's own serial output routines)
  _serial_putc          -> jmp main
  _nfs4_named_attr_get  -> main + font        (33 KB of NFSv4 code a local APFS boot never runs; the audit MAC checks that this blob
                           used to overwrite are needed as soon as launchd runs auditon/setaudit_addr)
Prints nothing; imported by make_baremetal_efi.py (KTTY=1)."""
import os, struct, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
from toolpaths import llvm_bin, nm_bin, require_kc     # LLVM_BIN / NM_BIN override the Homebrew and Xcode defaults
PLACEHOLDER = bytes.fromhex("0df0adde")          # 0xDEADF00D little endian


def _symbols():
    # nm prints the kernel's symbols and then aborts on the first kext it cannot parse: keep what it printed
    kc = ROOT / "build/ext/kernel/BootKernelExtensions-monterey.kc"
    require_kc(kc)
    out = subprocess.run([nm_bin(), str(kc)], capture_output=True, text=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3 and l.split()[1] in ("T", "t", "S", "D", "s", "d")}


MAIN_SYM, BLOB_MAX = "_nfs4_named_attr_get", 0x83f0


def _main_addr():
    return _symbols()[MAIN_SYM]


def halt_delta():
    """Distance from ktty_main (patched at MAIN_SYM) to the patched _halt_all_cpus, from the Monterey KC."""
    return _symbols()["_halt_all_cpus"] - _main_addr()


LAPIC_VBASE = 0xffffff80012fc8a8        # the kernel variable holding the LAPIC's virtual base (legacy_read/legacy_write, Monterey 21.6.0)


def build_blob():
    with tempfile.TemporaryDirectory() as td:
        obj, elf, binf = f"{td}/k.o", f"{td}/k.elf", f"{td}/k.bin"
        subprocess.check_call([f"{llvm_bin()}/clang", "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-fno-stack-protector",
                               "-mno-red-zone", "-mno-sse", "-mno-mmx", "-mno-avx", "-fPIE", "-fno-builtin", "-Os",
                               "-fvisibility=hidden", f"-DKTTY_HALT_DELTA={halt_delta()}", f"-DKTTY_QUIET_UNITS={int(float(os.environ.get('KTTY_QUIET_AFTER', '60')) * 1596.3e6 / (1 << 20))}u", f"-DKTTY_LAPIC_DELTA={LAPIC_VBASE - _main_addr()}", f"-DKTTY_PESTATE_DELTA={_symbols()['_PE_state'] - _main_addr()}", f"-DKTTY_VINFO_DELTA={_symbols()['_vinfo'] - _main_addr()}"] + [f"-DKTTY_{n}_DELTA={_symbols()['_' + sym] - _main_addr()}" for n, sym in (("CPUPTR", "cpu_data_ptr"), ("PSETNODE", "pset_node0"), ("PSET0", "pset0"), ("PKGS", "x86_pkgs"))] + ([f'-DKTTY_RETURN_PAT="{os.environ["KTTY_RETURN"]}"'] if os.environ.get("KTTY_RETURN") else []) +
                              ([f'-DKTTY_DUMP_PAT="{os.environ["KTTY_DUMP"]}"'] if os.environ.get("KTTY_DUMP") else []) +
                              ([f'-DKTTY_DUMP2_PAT="{os.environ["KTTY_DUMP2"]}"'] if os.environ.get("KTTY_DUMP2") else []) +
                              ["-c", str(ROOT / "bringup/ktty/ktty.c"), "-o", obj])
        Path(f"{td}/l.ld").write_text("ENTRY(ktty_main)\nSECTIONS { . = 0; .text : { *(.text.entry) *(.text .text.*) } "
                                      ".rodata : { *(.rodata .rodata.*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n")
        subprocess.check_call([f"{llvm_bin()}/ld.lld", "-nostdlib", "-static", "-T", f"{td}/l.ld", obj, "-o", elf])
        subprocess.check_call([f"{llvm_bin()}/llvm-objcopy", "-O", "binary", elf, binf])
        main = Path(binf).read_bytes()
    assert len(main) <= BLOB_MAX, f"blob too big: {len(main)}"
    assert PLACEHOLDER in main
    return bytes(main)


def header_blob():
    """The layout bringup/ebstest expects: 0x80 bytes of padding, then main."""
    return b"\xCC" * 0x80 + build_blob()


def _jmp(site, target):
    return b"\xE9" + struct.pack("<i", target - (site + 5))


def patches():
    def entry(sym, code, comment):
        return {"Arch": "x86_64", "Base": sym, "Comment": comment, "Count": 1, "Enabled": True, "Find": b"",
                "Identifier": "kernel", "Limit": 0, "Mask": b"", "MaxKernel": "", "MinKernel": "",
                "Replace": code, "ReplaceMask": b"", "Skip": 0}
    sym = _symbols()
    main = sym[MAIN_SYM]
    return [entry("_serial_init", bytes.fromhex("b801000000c3"), "ktty: serial_init always succeeds"),
            entry("_serial_putc_options", _jmp(sym["_serial_putc_options"], main), "ktty: serial_putc_options jumps to the console"),
            entry("_serial_putc", _jmp(sym["_serial_putc"], main), "ktty: serial_putc jumps to the console"),
            entry(MAIN_SYM, build_blob(), "ktty: text console (replaces unused NFSv4 code)")]


if __name__ == "__main__":
    import sys
    b = header_blob()
    print(len(b), "bytes; placeholder count", b.count(PLACEHOLDER))
    if len(sys.argv) > 1:                          # C header for bringup/ebstest
        with open(sys.argv[1], "w") as f:
            f.write("static const unsigned char ktty_blob[%d] = {\n" % len(b))
            for i in range(0, len(b), 16):
                f.write("  " + ",".join("0x%02x" % x for x in b[i:i + 16]) + ",\n")
            f.write("};\n")
