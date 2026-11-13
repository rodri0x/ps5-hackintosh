#!/bin/bash
# Usage: tools/tripwire_run.sh <kernel symbol> [colour AARRGGBB]
# Builds the OpenCore EFI with a tripwire on that XNU symbol and deploys + launches it on the console (Linux up, crash kexec
# available). A red square (bottom right, right of the firmware milestones) then proves the kernel executed that function.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/lib.sh
# the memory description in the EFI depends on the carve-out size, as in clean_run.sh (without this the 512 MB default was assumed)
CARVE_BASE=$(ssh "$PS5" cat /proc/iomem | python3 tools/carveout.py); export CARVE_BASE
[ -f opencore/ACPI/sony/APIC.bin ] || tools/dump_sony_acpi.sh      # the console's own MADT, dumped once from the running Linux
OC_DEBUG=1 TRIPWIRE="${1:?usage: tools/tripwire_run.sh <kernel symbol> [colour]}=${2:-FFFF0000}" python3 tools/make_baremetal_efi.py | head -1
# the same fresh stage 1 and bundle as clean_run.sh (it used to boot whatever an earlier deploy had left in /root)
source tools/stage1.sh
build_stage1
python3 tools/make_welcome.py build/welcome.raw >/dev/null
(cd build/baremetal-efi && tar -cf ../baremetal-efi.tar EFI)
scp -q build/baremetal-efi.tar "${STAGE1_FILES[@]}" "$PS5:/root/"
ssh $PS5 'bash /root/install_efi.sh /root/baremetal-efi.tar /boot/efi || exit 1
for p in $(pgrep -x qemu-system-x86); do kill -9 $p; done; systemctl stop hackintosh --no-block; sleep 3
for p in $(pgrep -x stop_qemu.sh); do kill -9 $p; done
nohup bash /root/run_stage.sh /root/chainV5.bzImage /root/ps5-upl.bundle > /root/run_stage.log 2>&1 &'
for i in $(seq 1 20); do sleep 5; ssh -o ConnectTimeout=3 $PS5 true 2>/dev/null || { echo "console is in UEFI now (ssh down after ~$((i*5))s)"; break; }; done
