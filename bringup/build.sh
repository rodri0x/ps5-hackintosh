#!/bin/bash
#
# Build the stage-1 stub variants and splice each one in front of the real kernel (bzImage) so that
# kexec_file_load treats it as a Linux kernel whose 64-bit entry (+0x200) is our stub.
#
# Usage: bringup/build.sh /path/to/vmlinuz          # builds build/bringup/chainV{0,1,2e,3,4}.bzImage
#
# Variants (see stub.c):  0 = jump only, 1 = + own page tables and CPU marks,
#   2 = paint the (tiled) sway scanout BOs through MM_INDEX, 3 = display endurance ticks, 4 = text console.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../build/bringup"
VMLINUZ="${1:?usage: build.sh /path/to/vmlinuz}"
source "$HERE/../tools/llvm.sh"; L="$(llvm_bin)"
mkdir -p "$OUT"

CFLAGS="-target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -mno-avx \
        -fPIE -fvisibility=hidden -fno-builtin -O2"
"$L/clang" -target x86_64-unknown-none-elf -I"$HERE" -c "$HERE/entry.S" -o "$OUT/entry.o"
# the low-memory "return to Linux" stub (linked at its fixed physical address) as a C array for stub.c
"$L/clang" -target x86_64-unknown-none-elf -I"$HERE/retlinux" -c "$HERE/retlinux/retlow.S" -o "$OUT/retlow.o"
"$L/ld.lld" -nostdlib -static -e rl_start -Ttext=0x67200 "$OUT/retlow.o" -o "$OUT/retlow.elf"
"$L/llvm-objcopy" -O binary --only-section=.text "$OUT/retlow.elf" "$OUT/retlow.bin"
RL_NEXT=$("$L/llvm-nm" "$OUT/retlow.elf" | awk '/ rl_next$/ {print $1}')
python3 - "$OUT/retlow.bin" "$OUT/retlow.h" "$RL_NEXT" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
open(sys.argv[2], "w").write("#define RL_NEXT 0x%s\nstatic const unsigned char retlow_code[%d] = {%s};\n" % (sys.argv[3], len(d), ",".join("0x%02x" % x for x in d)))
PY

build() {  # name defines...
    local name="$1"; shift
    "$L/clang" $CFLAGS -I"$HERE" -I"$OUT" "$@" -c "$HERE/stub.c" -o "$OUT/stub$name.o"
    "$L/ld.lld" -nostdlib -static -T "$HERE/link.ld" "$OUT/entry.o" "$OUT/stub$name.o" -o "$OUT/stub$name.elf"
    "$L/llvm-objcopy" -O binary --only-section=.pad --only-section=.text --only-section=.rodata --only-section=.data \
        "$OUT/stub$name.elf" "$OUT/stub$name.bin"
    python3 "$HERE/mkchain.py" "$VMLINUZ" "$OUT/stub$name.bin" "$OUT/chainV$name.bzImage"
}

build 0 -DVARIANT=0
build 1 -DVARIANT=1
build 2e -DVARIANT=2 -DPAINT_BYTES=0xF00000ULL -DTHROTTLE=1024
build 3 -DVARIANT=3 -DHOLD_SECONDS=90
build 4 -DVARIANT=4 -DHOLD_SECONDS=40
build 5 -DVARIANT=5
build 6 -DVARIANT=6
