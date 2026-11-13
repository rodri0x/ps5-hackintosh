#!/bin/bash
# Build kexts/PS5SalinaGbE (or kexts/$KEXT) into build/kexts/<name>.kext (needs Xcode with the macOS SDK; the kext is loaded by OpenCore).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${KEXT:-PS5SalinaGbE}"        # KEXT=PS5GpuProbe builds the GPU register probe instead, KEXT=PS5Power the Shut Down / Restart kext
SRC="$ROOT/kexts/$NAME"
# KEXT_SUFFIX=2 builds a copy under another class and bundle id (PS5SalinaGbE2) whose personality starts the hardware after 500 ms: load it at run time with
# `kmutil load -p` after unloading the boot-time one, so a change does not cost a console re-jailbreak.
if [ -n "${KEXT_SUFFIX:-}" ]; then
    NAME="PS5SalinaGbE$KEXT_SUFFIX"
    TMPSRC="$ROOT/build/kext-src-$KEXT_SUFFIX"
    rm -rf "$TMPSRC" && mkdir -p "$TMPSRC"
    for f in PS5SalinaGbE.cpp PS5SalinaGbE.hpp kmod.cpp Info.plist; do sed "s/PS5SalinaGbE/$NAME/g" "$SRC/$f" > "$TMPSRC/${f/PS5SalinaGbE/$NAME}"; done
    python3 - "$TMPSRC/Info.plist" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], "rb"))
for pers in p["IOKitPersonalities"].values():
    pers["InitDelayMs"] = 500
plistlib.dump(p, open(sys.argv[1], "wb"))
PY
    SRC="$TMPSRC"
fi
OUT="$ROOT/build/kexts/$NAME.kext"
SDK="$(xcrun --show-sdk-path)"
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
# The Xcode 26 SDK declares IONetworkController slots 6 and 7 (allocatePacketNoWait, setHardwareAssists) as used; on Monterey they are still padding.
# OpenCore's vtable patcher then refuses every IOEthernetController subclass ("Failed to patch symbol ...allocatePacketNoWait", the kext is silently
# left out). Build against a copy of the header that keeps them as padding.
OVL="$ROOT/build/kext-overlay"
rm -rf "$OVL" && mkdir -p "$OVL/IOKit/network"
python3 - "$KH/IOKit/network/IONetworkController.h" "$OVL/IOKit/network/IONetworkController.h" <<'PY'
import sys
s = open(sys.argv[1]).read()
for old, new in (("    virtual mbuf_t allocatePacketNoWait(UInt32 size);\n", ""),
                 ("OSMetaClassDeclareReservedUsed( IONetworkController,  6);", "OSMetaClassDeclareReservedUnused( IONetworkController,  6);"),
                 ("    virtual IOReturn setHardwareAssists( UInt32 hardwareAssists, UInt32 hardwareAssistsMask )\n", ""),
                 ("OSMetaClassDeclareReservedUsed( IONetworkController,  7);", "OSMetaClassDeclareReservedUnused( IONetworkController,  7);")):
    assert s.count(old) == 1, old
    s = s.replace(old, new)
open(sys.argv[2], "w").write(s)
PY
rm -rf "$OUT" && mkdir -p "$OUT/Contents/MacOS"
cp "$SRC/Info.plist" "$OUT/Contents/Info.plist"
xcrun clang++ -arch x86_64 -mmacosx-version-min=12.0 -isysroot "$SDK" -std=c++17 -O2 -Wall -Wno-unused-private-field -Wno-inconsistent-missing-override -Wno-deprecated-declarations -Wno-sign-conversion -Wno-implicit-int-conversion \
    -fno-builtin -fno-rtti -fno-exceptions -fno-common -fapple-kext -mkernel -nostdlibinc \
    -I"$OVL" -I"$KH" -I"$KH/libkern/c++" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
    -c "$SRC/$NAME.cpp" -o "$OUT/../$NAME.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=12.0 -isysroot "$SDK" -std=c++17 -O2 -fno-builtin -fno-rtti -fno-exceptions -fno-common -fapple-kext -mkernel \
    -nostdlibinc -I"$KH" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -c "$SRC/kmod.cpp" -o "$OUT/../kmod-$NAME.o"
xcrun ld -arch x86_64 -kext -static -no_data_const -macos_version_min 12.0 -syslibroot "$SDK" \
    -o "$OUT/Contents/MacOS/$NAME" "$OUT/../$NAME.o" "$OUT/../kmod-$NAME.o" -lkmod -lkmodc++
# a stack frame over 4 KiB makes clang call ___chkstk_darwin, which jumps through a pointer slot that OpenCore's linker fills with garbage: keep frames small
if nm "$OUT/Contents/MacOS/$NAME" | grep -q "chkstk"; then echo "the kext calls ___chkstk_darwin (a big stack frame): move the array to the heap" >&2; exit 1; fi
# a kext without kmod_info is silently skipped by the prelinker
nm "$OUT/Contents/MacOS/$NAME" | grep -q " _kmod_info" || { echo "no kmod_info in the kext" >&2; exit 1; }
echo "built $OUT"
