# shellcheck shell=bash
# Shared by clean_run.sh, ktty_run.sh and tripwire_run.sh (sourced after tools/lib.sh, with CARVE_BASE exported): rebuild the two files
# that run_stage.sh boots on the console, so an old one can never be sent:
#   build/bringup/chainV5.bzImage  stage 1 wrapped around the kernel the console runs now (bringup/build.sh, a few seconds)
#   build/ps5-upl.bundle           the UEFI payload with this boot's memory map and carve-out base (uefi/mkbundle.py)
# Both are deleted before they are rebuilt; any missing input or failed step stops the caller (set -e) before anything is sent.
build_stage1() {
    : "${CARVE_BASE:?build_stage1 needs CARVE_BASE (read it from the console first)}"
    local krel vmlinuz elf=build/edk2/Build/UefiPayloadPkgX64/UniversalPayload.elf
    krel=$(ssh "$PS5" uname -r)
    [ -n "$krel" ] || { echo "could not read the console's kernel version (ssh $PS5 uname -r)" >&2; return 1; }
    vmlinuz="build/ext/vmlinuz-$krel"
    [ -f "$vmlinuz" ] || { echo "missing $vmlinuz, the kernel the console runs now: scp root@<console>:/boot/vmlinuz-$krel build/ext/ (INSTALL.md)" >&2; return 1; }
    [ -f "$elf" ] || { echo "missing $elf (uefi/build_upl.sh): cannot rebuild build/ps5-upl.bundle for carve-out base $CARVE_BASE" >&2; return 1; }
    rm -f build/bringup/chainV5.bzImage build/ps5-upl.bundle
    bringup/build.sh "$vmlinuz" >/dev/null || { echo "bringup/build.sh failed for $vmlinuz" >&2; return 1; }
    [ -s build/bringup/chainV5.bzImage ] || { echo "bringup/build.sh did not produce build/bringup/chainV5.bzImage" >&2; return 1; }
    python3 uefi/mkbundle.py "$elf" build/ps5-upl.bundle >/dev/null || { echo "uefi/mkbundle.py failed" >&2; return 1; }
    [ -s build/ps5-upl.bundle ] || { echo "uefi/mkbundle.py did not produce build/ps5-upl.bundle" >&2; return 1; }
    echo "stage 1 rebuilt for kernel $krel, bundle for carve-out base $CARVE_BASE"
}

# The files every deploy sends to /root on the console, besides the EFI tar (run_stage.sh needs all of them)
STAGE1_FILES=(tools/install_efi.sh build/ps5-upl.bundle build/bringup/chainV5.bzImage bringup/tools/patch_tripwire.py bringup/tools/run_stage.sh
              bringup/tools/kms_linear.py build/welcome.raw)
