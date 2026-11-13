#!/bin/bash
# Runs on the console (clean_run.sh, ktty_run.sh and tripwire_run.sh copy it to /root): replaces ESP/EFI with the EFI folder in TAR.
# Extract into ESP/EFI.new and check it, move the folder in place aside, move the new one in; if that last move fails, put the folder
# that was in place back.
#   ESP/EFI.bak     the last folder that was in place AND passed the check below (BOOTx64.efi, OpenCore.efi, a readable config.plist).
#                   It is only ever replaced by another folder that passes it, so a good copy is never deleted (way back: mv EFI.bak EFI).
#   ESP/EFI.broken  a folder found in place that failed the check (an earlier install that went wrong); kept for a look, never booted.
# The one moment without an ESP/EFI is between the two renames (a power cut there): the next run finds EFI.bak without EFI and puts it
# back before doing anything else. Likewise, a broken EFI with a good EFI.bak next to it is swapped for the backup first.
#   Usage: install_efi.sh TAR [ESP]      (ESP defaults to /boot/efi)
set -euo pipefail
TAR="${1:?usage: install_efi.sh TAR [ESP]}"
ESP="${2:-/boot/efi}"
NEW="$ESP/EFI.new"
BAK="$ESP/EFI.bak"
BROKEN="$ESP/EFI.broken"
fail() { echo "install_efi: $*" >&2; exit 1; }
note() { echo "install_efi: $*" >&2; }
plist_ok() {       # without python3 on the console, only the file checks below apply
    command -v python3 >/dev/null 2>&1 || return 0
    python3 -I -c 'import plistlib, sys; plistlib.load(open(sys.argv[1], "rb"))' "$1" 2>/dev/null
}
efi_ok() {         # efi_ok DIR: DIR looks like a bootable OpenCore EFI folder
    [ -s "$1/BOOT/BOOTx64.efi" ] && [ -s "$1/OC/OpenCore.efi" ] && [ -s "$1/OC/config.plist" ] && plist_ok "$1/OC/config.plist"
}

[ -s "$TAR" ] || fail "no EFI archive at $TAR"
[ -d "$ESP" ] || fail "no ESP directory $ESP"

# an earlier run stopped between the two renames: EFI.bak is the last good EFI that was in place, put it back first
if [ ! -e "$ESP/EFI" ] && [ -d "$BAK" ]; then
    mv "$BAK" "$ESP/EFI" || fail "$ESP/EFI is missing and $BAK could not be moved back: restore it by hand (mv $BAK $ESP/EFI)"
    note "$ESP/EFI was missing (an earlier install was interrupted): restored it from $BAK"
fi
# the folder in place is incomplete but the backup is good: put the backup back first (the broken one is kept as EFI.broken)
if [ -e "$ESP/EFI" ] && ! efi_ok "$ESP/EFI" && [ -d "$BAK" ] && efi_ok "$BAK"; then
    rm -rf "$BROKEN"
    mv "$ESP/EFI" "$BROKEN" || fail "$ESP/EFI is incomplete and could not be moved aside: nothing changed ($BAK is the good copy)"
    if ! mv "$BAK" "$ESP/EFI"; then
        mv "$BROKEN" "$ESP/EFI" || true
        fail "$ESP/EFI is incomplete and $BAK could not be moved back: nothing deleted, restore it by hand (mv $BAK $ESP/EFI)"
    fi
    note "$ESP/EFI was incomplete: restored $BAK in its place (the incomplete one is $BROKEN)"
fi

# the folder in place and the new one are both on the ESP for a moment: make sure it has room for the new one
need_kb=$(( $(du -k "$TAR" | cut -f1) + 1024 ))
free_kb=$(df -Pk "$ESP" | awk 'NR == 2 {print $4}')
[ "$free_kb" -ge "$need_kb" ] || fail "only ${free_kb} KiB free on $ESP, the new EFI needs about ${need_kb} KiB: nothing changed"

rm -rf "$NEW"
mkdir "$NEW"
if ! tar -C "$NEW" -xf "$TAR"; then
    rm -rf "$NEW"
    fail "could not extract $TAR: the current EFI folder is untouched"
fi
for f in EFI/BOOT/BOOTx64.efi EFI/OC/OpenCore.efi EFI/OC/config.plist; do
    [ -s "$NEW/$f" ] || { rm -rf "$NEW"; fail "the archive has no $f: the current EFI folder is untouched"; }
done
plist_ok "$NEW/EFI/OC/config.plist" || { rm -rf "$NEW"; fail "EFI/OC/config.plist in the archive is not a valid plist: the current EFI folder is untouched"; }

# move the folder in place aside: a good one becomes the new EFI.bak; a bad one goes to EFI.broken and EFI.bak stays as it is
PREV=""
if [ -e "$ESP/EFI" ]; then
    if efi_ok "$ESP/EFI"; then
        rm -rf "$BAK"
        mv "$ESP/EFI" "$BAK" || { rm -rf "$NEW"; fail "could not move the current EFI folder aside: it is untouched"; }
        PREV="$BAK"
    else
        rm -rf "$BROKEN"
        mv "$ESP/EFI" "$BROKEN" || { rm -rf "$NEW"; fail "could not move the current (incomplete) EFI folder aside: it is untouched"; }
        PREV="$BROKEN"
        note "the EFI folder in place was incomplete: kept as $BROKEN, $BAK left as it was"
    fi
fi
if ! mv "$NEW/EFI" "$ESP/EFI"; then
    rm -rf "$ESP/EFI"
    if [ -n "$PREV" ]; then
        mv "$PREV" "$ESP/EFI" || fail "the new EFI could not be put in place AND the old one could not be restored: it is at $PREV, move it back by hand"
        fail "the new EFI could not be put in place: the folder that was there is restored"
    fi
    fail "the new EFI could not be put in place (there was no old one)"
fi
rmdir "$NEW" 2>/dev/null || rm -rf "$NEW"
sync
echo "install_efi: new EFI in place at $ESP/EFI${PREV:+ (the one before is at $PREV)}"
