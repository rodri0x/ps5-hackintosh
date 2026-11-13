#!/bin/bash
# Deploys the EFI built with the in-kernel text console (KTTY=1) and launches UEFI. Pick Macintosh HD in the picker: the whole
# kernel log (printf/kprintf) is drawn on the screen.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/lib.sh
# The VRAM carve-out size depends on /boot/efi/vram.txt of the console: read its base from the running Linux, rebuild stage 1 if its source changed,
# and the bundle (it carries the memory map, the reserved DMA pool and the carve-out base) from the payload ELF.
CARVE_BASE=$(ssh "$PS5" cat /proc/iomem | python3 tools/carveout.py); export CARVE_BASE
echo "VRAM carve-out base $CARVE_BASE"
[ -f opencore/ACPI/sony/APIC.bin ] || tools/dump_sony_acpi.sh      # the console's own MADT, dumped once from the running Linux
OC_DEBUG=1 KTTY=1 python3 tools/make_baremetal_efi.py | head -1
# stage 1 (for the kernel the console runs now) and the bundle (for the carve-out base read above), rebuilt on every deploy: tools/stage1.sh
source tools/stage1.sh
build_stage1
python3 tools/make_welcome.py build/welcome.raw >/dev/null
(cd build/baremetal-efi && tar -cf ../baremetal-efi.tar EFI)
scp -q build/baremetal-efi.tar "${STAGE1_FILES[@]}" "$PS5:/root/"
# install_efi.sh swaps the EFI folder only after the new one is extracted and checked, keeps the old one as /boot/efi/EFI.bak and puts it back
# if the swap fails; on any failure nothing below runs
ssh $PS5 'bash /root/install_efi.sh /root/baremetal-efi.tar /boot/efi || exit 1
for p in $(pgrep -x qemu-system-x86); do kill -9 $p; done; systemctl stop hackintosh --no-block; sleep 3
for p in $(pgrep -x stop_qemu.sh); do kill -9 $p; done
nohup bash /root/run_stage.sh /root/chainV5.bzImage /root/ps5-upl.bundle > /root/run_stage.log 2>&1 &'
for i in $(seq 1 30); do sleep 5; ssh -o ConnectTimeout=3 $PS5 true 2>/dev/null || { echo "console is in UEFI now"; break; }; done
