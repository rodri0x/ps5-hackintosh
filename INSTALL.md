# Installing

From a jailbroken console and a blank USB SSD to the macOS desktop. Read it once before you start: some steps take hours (the macOS
install) and the last one has a budget of one attempt per jailbreak boot.

I have only ever run this on my own console: a launch-model ("fat") PS5 on firmware 13.42. Some constants in the repo were measured on it (Sony's ACPI tables in `opencore/ACPI/`, the
RSDP address in `uefi/README.md`, the carve-out layout), so a different model or firmware is unsupported for now and would need work in stage 1. The README tells you what
each piece does; this file tells you the order.

## How long it takes

Roughly, from my own runs (your Mac, network and USB disk will move these around):

| Step | Time |
|---|---|
| Installing Monterey in QEMU (step 2) | hours on Apple Silicon (emulated), less on an Intel Mac |
| Writing the APFS container to the USB (step 2) | about 10 minutes over gigabit |
| UEFI payload build (step 3, once) | Docker and a clone of edk2: plan for a coffee, I did not time it |
| One boot attempt, from Linux up to the desktop (step 4) | a few minutes: `clean_run.sh` takes a couple, then the Apple boot another minute or two |
| The jailbreak itself | whatever your method takes, once per attempt |

Plan a free afternoon for the first time (the QEMU install is most of it), and a few minutes per attempt afterwards.

## What you need

- A launch-model ("fat") PS5 on **firmware 13.42**, and a way to jailbreak it every time you boot. That is the only combination this has been run on, and the one the [ps5-linux loader](https://github.com/ps5-linux/ps5-linux-loader) is known to work on here. Other models and firmwares are not supported for now.
- A USB SSD for the Linux image (64 GB minimum) plus a ~70 GB macOS partition, so 256 GB in practice. Two drives work as well.
- A USB keyboard and mouse, an HDMI screen and an Ethernet cable (the scripts reach the console over ssh).
- A Mac to build on, with Xcode (only the kext needs it) and Homebrew:

```bash
brew install llvm@18 docker qemu acpica
pip3 install pillow
```

`qemu` is only for step 2; `acpica` (`iasl`) compiles the ACPI sources. The scripts look for LLVM in `/opt/homebrew/opt/llvm@18/bin` (or `/usr/local/opt/llvm@18/bin` on an Intel Mac); set `LLVM_BIN` if yours lives elsewhere, and `NM_BIN` for an nm other than `/usr/bin/nm`.

- A copy of macOS Monterey. The patches are matched to 12.7.6 (XNU 21.6.0), the last Monterey. Other builds are untested.

Pick your display mode before building anything: the three places that have to agree are listed under *Things worth knowing* in the README.

## 1. Linux on the console

Follow the [ps5-linux README](https://github.com/ps5-linux/ps5-linux-loader#installation) from "Get a Linux image" to "Send the payload"
(image, flash, USB port, jailbreak, loader). It is a separate project and it explains its steps better than I would.
Any way of sending the loader ELF to the console's payload port (9021) does the job, for example `nc <console-ip> 9021 < ps5-linux-loader.elf`.
`tools/send_loader.sh` does it for you: leave it running in a terminal while you power-cycle and jailbreak, and it sends the loader as soon as the port opens and retries until Linux answers on ssh.
`tools/deploy.sh` and `make deploy` do the same for the QEMU phase (they also need `socat`) and are not part of this flow.

When Linux is up on the console:

1. Make `root` reachable over ssh with your key. The scripts run `ssh` and `scp` as `PS5_LINUX_USER` (root by default), write to `/root`
   and to `/boot/efi`, and prompt for a password at every call if you do not use a key.
2. Give it a stable address (a DHCP reservation is enough) and put it in `.env` later.
3. Edit `/boot/efi/cmdline.txt` (the FAT partition of the USB drive) and add `crashkernel=400M`. This reserves the memory the crash
   kernel, which is my stage 1, runs from. Keep the rest of the line as the image ships it. Do not copy `usb/cmdline.txt` from this repo:
   it belongs to the QEMU phase.
4. `/boot/efi/vram.txt` sets the size of the VRAM carve-out: `0x20000000` (512 MB) is the default and the one to start with; 1 GiB and 4 GiB (`0x100000000`) boot to the desktop too, but see *Known issues* in the README.
   `tools/clean_run.sh` reads the resulting base address from `/proc/iomem` by itself.
5. `python3` has to be on the console. `bringup/tools/run_stage.sh` stops `sway` of the user `ps5` so that `kms_linear.py` can take the
   display; if your image runs another compositor, stop that one instead (edit the `pkill` line).
6. Reboot into Linux once and check that `crashkernel=400M` shows up in `cat /proc/cmdline` and `dmesg | grep -i crashkernel`.

## 2. macOS on the USB SSD

macOS has to end up as a GPT partition of type *Apple APFS* on a USB disk. OpenCore finds it there and boots it.
The way I did it, which is not scripted end to end:

**Install Monterey in QEMU on the Mac.**

```bash
tools/fetch_binaries.sh                 # busybox and OVMF_CODE.fd, into build/bin
tools/create_macos_img.sh monterey      # recovery image from Apple + a 64 GB qcow2 (build/macOS.img)
tools/install_macos_qemu.sh
```

`install_macos_qemu.sh` expects `build/OVMF_CODE.fd` and `build/OVMF_VARS.fd`: an OVMF build for macOS guests, for example the files in
[kholia/OSX-KVM](https://github.com/kholia/OSX-KVM). `fetch_binaries.sh` only downloads the code half, into `build/bin`.
On an Apple Silicon Mac the guest is emulated, so the install takes hours, not minutes.

In the guest: format the 64 GB disk as APFS in Disk Utility, install, create a user, turn on auto-login and, if you want to ssh in later,
Remote Login. Turn **automatic updates off**. Shut the guest down when it is done.

**Extract the APFS container and write it to the USB.**

```bash
qemu-img convert -O raw build/macOS.img build/macOS.raw
hdiutil attach -nomount build/macOS.raw     # prints /dev/diskN; the Apple_APFS slice is diskNs2
sudo dd if=/dev/rdiskNs2 of=build/macos-apfs.raw bs=4m
hdiutil detach /dev/diskN
```

On the console (Linux), make room and a partition for it. If the Linux image grew its root to fill the drive, shrink that first.

```bash
lsblk                                       # find the USB SSD; do not guess
sgdisk -n 0:0:+70G -t 0:AF0A /dev/sdX       # sgdisk is in the gdisk package; AF0A = Apple APFS
```

The partition must be at least as big as `build/macos-apfs.raw` (`ls -l` it; a 64 GB disk gives a container just under 69 GB).

Then stream the container into the new partition from the Mac (about ten minutes on gigabit):

```bash
dd if=build/macos-apfs.raw bs=4m | ssh root@<console-ip> 'dd of=/dev/sdXn bs=4M conv=fsync'
```

Check twice that `sdXn` is the new partition. This is the one command in the whole process that can eat a disk.

The internal SSD of the console is never touched by macOS: the NVMe driver is disabled in the EFI (`NVMEBLOCK`, on by default).

## 3. Build

```bash
git clone https://github.com/rodri0x/ps5-hackintosh && cd ps5-hackintosh
cp .env.example .env     # set PS5_LINUX_HOST (and PS5_LINUX_USER if it is not root); the Mac identity is generated on first build
tools/fetch_opencore.sh
```

**UEFI payload** (once; needs Docker running):

```bash
docker build -t ps5-edk2 uefi
git clone --depth 1 --branch edk2-stable202505 --recurse-submodules --shallow-submodules https://github.com/tianocore/edk2.git build/edk2
uefi/build_upl.sh -b RELEASE -l 1744830464
```

**The macOS kernel collection**, which the patch generators (`tools/make_ierfix.py`, `make_tscfix.py`, `make_cachefix.py`, `make_ktty.py`) read to find the
functions they patch. It is Apple's file, so it is not in the repo: take it from the macOS you installed in step 2 (the raw image is still there):

```bash
hdiutil attach build/macOS.raw                                   # mounts the volumes; note the "Macintosh HD" one
mkdir -p build/ext/kernel
cp "/Volumes/Macintosh HD/System/Library/KernelCollections/BootKernelExtensions.kc" build/ext/kernel/BootKernelExtensions-monterey.kc
hdiutil detach "/Volumes/Macintosh HD"
```

If the path is not there, `find "/Volumes/Macintosh HD" -name BootKernelExtensions.kc` will say where it is. It should be about 68 MB and `file` should call it
`Mach-O 64-bit x86_64`. The copy I develop with, from 12.7.6 (build 21H1320), has SHA-256 `df05bb028db35ea9ba87d3276c03501796a7e311e85c4d2587a575de665fe8ba`; a different hash means a different macOS build,
and the patches are matched to that one.

**Your console's MADT** (Sony's interrupt table, which I extend with a fake I/O APIC) is not in the repo either. `tools/clean_run.sh` and `tools/ktty_run.sh` dump it from the
running Linux the first time (`tools/dump_sony_acpi.sh`, into `opencore/ACPI/sony/APIC.bin`); you can also run that script yourself.

**Kernel of the console**, for stage 1. Copy the one Linux is running, so the machine comes back to the same kernel it left:

```bash
mkdir -p build/ext
scp root@<console-ip>:/boot/vmlinuz-"$(ssh root@<console-ip> uname -r)" build/ext/
```

**Ethernet kext** (optional; without it the EFI is built without the NIC and macOS has no network):

```bash
tools/build_kext.sh
```

Stage 1 itself (`bringup/build.sh`) is built by `tools/clean_run.sh` on demand.

## 4. Boot

One attempt per jailbreak boot: chain two crash-kexecs and the console resets.

1. Boot the console, jailbreak it, send the loader, wait for Linux and for `ssh root@<console-ip>` to answer.
2. On the Mac:

```bash
tools/clean_run.sh
```

It builds the EFI, uploads it to `/boot/efi`, loads stage 1 as the crash kernel and crashes into it. The script prints
`console is in UEFI now` when the ssh session dies, and the TV shows the welcome screen, then the OpenCore picker.

3. With the USB keyboard, pick **Macintosh HD** in the picker. The Apple verbose text scrolls for a while, then WindowServer starts and
   the desktop appears. Nothing answers over the network while macOS is booting.

What you should see, in order, once `clean_run.sh` prints `console is in UEFI now`:

1. A welcome screen (the picture Linux left on the display). It stays for a moment.
2. The OpenCore picker, with **Macintosh HD** in it. Pick it with the USB keyboard.
3. White text scrolling on a black background: the Apple kernel's verbose boot. It looks like a crash and is not, give it a minute.
4. WindowServer starts and the screen switches to the login window or, with auto-login on, straight to the desktop.
5. About a minute after the kernel starts, the Ethernet kext comes up (it waits 45 s on purpose) and the console answers ping and ssh.

The kernel log stopping at `kdp_core zlib memory` is not necessarily a hang: ping the console before you reboot. If the "prohibited" sign appears after errors from `AppleUSBXHCIPCI`, see *Known issues* in the README.

The default is `CPUS=2` with the internal SSD blocked. Once you have a desktop, power-cycle, jailbreak again and try more cores:

```bash
CPUS=16 tools/clean_run.sh
```

If you want to watch the kernel log instead of the quiet boot, `tools/ktty_run.sh` draws it on the screen.

## When it does not boot

You have no serial port, so the screen and the files are the instrument.

- Black screen right after the jailbreak/Linux boot: the display settings, not macOS. Check the three resolution places in the README.
- No welcome screen after `clean_run.sh`: `ssh root@<console-ip> cat /root/run_stage.log /root/kms.log` before you reboot.
- It stops somewhere in UEFI or OpenCore: re-jailbreak, boot Linux, and read `/boot/efi/EFI/OC/opencore-*.txt` (OpenCore's debug log) **before**
  running `clean_run.sh` again, because the script replaces `/boot/efi/EFI`.
- A panic or a hang in the Apple kernel: `tools/ktty_run.sh` shows the kernel log on the TV; photograph it. `bringup/README.md` explains how
  the stage 1 marks and logs are read back from Linux.
- Not sure it hung? Ping the console first: with the Ethernet kext, a frozen-looking log at `kdp_core` is often just a slow boot.

## Afterwards

- With the Ethernet kext, the console takes a DHCP lease and answers ping and ssh (if you enabled Remote Login in step 2).
- There is no shutdown or restart: power-cycle the console, and the next boot is a new jailbreak.
- Do not update macOS. An update replaces the kernel the patches in `tools/make_*.py` are matched to.
