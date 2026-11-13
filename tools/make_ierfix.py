#!/usr/bin/env python3
"""Two fixes that both have to run on every CPU, before it takes interrupts (hooks below).

(1) AMD's extended APIC interrupt-enable registers (IERE, 0x410 bit 0, and IER0..7 at 0x480+16*n) come up set with almost every vector
masked (IER6 = 0x1f): XNU's timer vector 0xDD and every IPI sit in IRR and are never delivered. Linux finds IERE off and all of IER on.
XNU does not know these registers, so open them at the start of `_lapic_configure` (BSP only: i386_init, i386_smp_init, wake from sleep) and at the
start of `_lapic_init_slave` (every AP, called from _vstart); an AP that keeps the closed IER ignores IPIs (TLB shootdown timeouts at cpus>1).

Patch for the Monterey KC (21.6.0), as OpenCore Kernel>Patch entries (prints nothing; imported by make_baremetal_efi.py):
  _lapic_configure (push rbp; mov rbp,rsp; push r14 = 6 bytes)          -> jmp stub 1
  _lapic_init_slave (push rbp; mov rbp,rsp; mov ecx,0x1b = 9 bytes)       -> jmp stub 2
  _nfs4_parsefattr (dead NFSv4 code)                                      -> stub 1 at +0, stub 2 at +0x80: IER fix, the displaced instructions, jmp back
IER registers only accept writes while IERE is set, so the fix is: if IERE then IER0..7 = 0xFFFFFFFF.

(2) Performance counters left running by Linux. Linux's hard-lockup detector keeps a core PMC counting with an overflow
interrupt; the crash kexec does not stop it. On a multi-CPU boot an AP ends up with PPR 0xD0 while ISR reads empty and IRR holds 0xDD (timer) and
0xDE (IPI); an EOI clears the PPR. A perf-counter overflow (LVT vector in XNU's 0xD0 class) that never gets its EOI would look exactly like that: zeroing
the counters made cpus=16 stable (9 minutes under load). The fix zeroes the six Zen 2 core PerfCtl MSRs (0xC0010200, +2 ... 0xC001020A) at the start of both hooks; KTTY builds also record what PerfCtl0
held on each CPU before the fix."""
import os, struct, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
from toolpaths import llvm_bin, nm_bin, require_kc     # LLVM_BIN / NM_BIN override the Homebrew and Xcode defaults
KC = ROOT / "build/ext/kernel/BootKernelExtensions-monterey.kc"
STUB_SYM = "_nfs4_parsefattr"
LAPIC_VBASE = 0xffffff80012fc8a8           # the kernel variable holding the LAPIC's virtual base (Monterey 21.6.0)
PROLOGUE = bytes.fromhex("554889e54156")    # push rbp; mov rbp, rsp; push r14
SLAVE_PROLOGUE = bytes.fromhex("554889e5b91b000000")    # push rbp; mov rbp, rsp; mov ecx, 0x1b (the MSR number _lapic_init_slave reads next)
STUB2_OFFSET = 0x200


def _symbols():
    require_kc(KC)
    out = subprocess.run([nm_bin(), str(KC)], capture_output=True, text=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3 and l.split()[1] in ("T", "t", "S", "D", "s", "d")}


def _kernel_bytes(va, n):
    d = KC.read_bytes()
    text_va, text_off = 0xffffff8000210000, 0x10000            # __TEXT_EXEC of the Monterey KC
    return d[text_off + (va - text_va):text_off + (va - text_va) + n]


PITCH = 10240
# KTTY=1 builds record, per LAPIC id, what the IER registers looked like before and after the fix, as two VRAM pixels per CPU in a spot the
# console never draws on (bottom right: x=2000.., y=1430 before the fix, y=1431 after: row 89, below the 89 text rows). bringup/ktty prints them when the panic text shows up.
PROBE = os.environ.get("KTTY") == "1"
ROW_BEFORE, ROW_AFTER, PROBE_X = 1430, 1431, 2000
ROW_PMC = 1432                                   # PerfCtl0 (low dword) as this CPU found it, before the fix zeroes it


def probe_asm(row):
    """Needs rax = LAPIC base; keeps rax, clobbers r8-r10, edx (scratch at both hook sites). Pixel 0 = 0xA5000000 | id << 8 | IERE, pixel 1 = IER6."""
    off = row * PITCH + PROBE_X * 4
    return ["    pushq %rax",
            "    movl 0x20(%rax), %r10d", "    shrl $24, %r10d", "    andl $15, %r10d",           # r10d = LAPIC id
            "    movl %r10d, %r8d", "    shll $3, %r8d", "    addl $0xDEADF00D, %r8d", f"    addl ${off}, %r8d", "    orl $0x80000000, %r8d",
            "    movl 0x410(%rax), %r9d", "    andl $1, %r9d", "    shll $8, %r10d", "    orl %r10d, %r9d", "    orl $0xA5000000, %r9d",
            "    movw $0x2000, %dx", "    movl %r8d, %eax", "    outl %eax, %dx",
            "    movw $0x2004, %dx", "    movl %r9d, %eax", "    outl %eax, %dx",
            "    addl $4, %r8d", "    movw $0x2000, %dx", "    movl %r8d, %eax", "    outl %eax, %dx",
            "    popq %rax", "    pushq %rax", "    movl 0x4E0(%rax), %eax",
            "    movw $0x2004, %dx", "    outl %eax, %dx", "    popq %rax"]


def pmc_asm():
    lines = []
    if PROBE:
        off = ROW_PMC * PITCH + PROBE_X * 4
        lines += ["    movl $0xC0010200, %ecx", "    rdmsr", "    movl %eax, %r9d",
                  "    movq lapic_vbase(%rip), %r8", "    testq %r8, %r8", "    jz 3f",
                  "    movl 0x20(%r8), %r10d", "    shrl $24, %r10d", "    andl $15, %r10d", "    shll $3, %r10d",
                  "    addl $0xDEADF00D, %r10d", f"    addl ${off}, %r10d", "    orl $0x80000000, %r10d",
                  "    movw $0x2000, %dx", "    movl %r10d, %eax", "    outl %eax, %dx",
                  "    movw $0x2004, %dx", "    movl %r9d, %eax", "    outl %eax, %dx", "3:"]
    lines += ["    xorl %eax, %eax", "    xorl %edx, %edx"]
    for i in range(6):
        lines += [f"    movl ${0xC0010200 + 2 * i:#x}, %ecx", "    wrmsr"]
    return lines


def build_stub(stub_va, back_va, displaced):
    lines = [".code64", ".globl stub", "stub:", *pmc_asm(),
             "    movq lapic_vbase(%rip), %rax", "    testq %rax, %rax", "    jz 1f"]
    if PROBE:
        lines += probe_asm(ROW_BEFORE)
    lines += ["    movl 0x410(%rax), %ecx", "    testl $1, %ecx", "    jz 2f",
              "    movl $0xFFFFFFFF, %ecx"]
    lines += [f"    movl %ecx, {0x480 + 16 * i:#x}(%rax)" for i in range(8)]
    lines += ["2:"]
    if PROBE:
        lines += probe_asm(ROW_AFTER)
    lines += ["1:", *[f"    {i}" for i in displaced], "    jmp back"]
    with tempfile.TemporaryDirectory() as td:
        Path(f"{td}/s.S").write_text("\n".join(lines) + "\n")
        subprocess.check_call([f"{llvm_bin()}/clang", "-target", "x86_64-unknown-none-elf", "-c", f"{td}/s.S", "-o", f"{td}/s.o"])
        subprocess.check_call([f"{llvm_bin()}/ld.lld", "-nostdlib", "-static", f"-Ttext={stub_va:#x}", "-e", "stub",
                               f"--defsym=lapic_vbase={LAPIC_VBASE:#x}", f"--defsym=back={back_va:#x}", f"{td}/s.o", "-o", f"{td}/s.elf"])
        subprocess.check_call([f"{llvm_bin()}/llvm-objcopy", "-O", "binary", f"{td}/s.elf", f"{td}/s.bin"])
        return Path(f"{td}/s.bin").read_bytes()


def patches():
    def entry(sym, find, code, comment, limit=0):
        return {"Arch": "x86_64", "Base": sym, "Comment": comment, "Count": 1, "Enabled": True, "Find": find,
                "Identifier": "kernel", "Limit": limit, "Mask": b"", "MaxKernel": "", "MinKernel": "",
                "Replace": code, "ReplaceMask": b"", "Skip": 0}
    sym = _symbols()
    cfg, slave, stub_va = sym["_lapic_configure"], sym["_lapic_init_slave"], sym[STUB_SYM]
    assert _kernel_bytes(cfg, len(PROLOGUE)) == PROLOGUE, "unexpected _lapic_configure prologue"
    assert _kernel_bytes(slave, len(SLAVE_PROLOGUE)) == SLAVE_PROLOGUE, "unexpected _lapic_init_slave prologue"
    stub1 = build_stub(stub_va, cfg + len(PROLOGUE), ["pushq %rbp", "movq %rsp, %rbp", "pushq %r14"])
    stub2 = build_stub(stub_va + STUB2_OFFSET, slave + len(SLAVE_PROLOGUE), ["pushq %rbp", "movq %rsp, %rbp", "movl $0x1b, %ecx"])
    assert len(stub1) <= STUB2_OFFSET and len(stub2) < 0x300
    blob = stub1 + b"\x90" * (STUB2_OFFSET - len(stub1)) + stub2
    jmp1 = b"\xE9" + struct.pack("<i", stub_va - (cfg + 5)) + b"\x90"
    jmp2 = b"\xE9" + struct.pack("<i", stub_va + STUB2_OFFSET - (slave + 5)) + b"\x90" * (len(SLAVE_PROLOGUE) - 5)
    return [entry("_lapic_configure", PROLOGUE, jmp1, "ier: _lapic_configure jumps to the IER fix", limit=0x10),
            entry("_lapic_init_slave", SLAVE_PROLOGUE, jmp2, "ier: _lapic_init_slave (every AP) jumps to the IER fix", limit=0x10),
            entry(STUB_SYM, b"", blob, ("ktty: ier probe + " if PROBE else "ier: ") + "open AMD's APIC IER vectors (unused NFSv4 code)")]


if __name__ == "__main__":
    for p in patches():
        print(p["Comment"], len(p["Replace"]), p["Replace"][:16].hex())
