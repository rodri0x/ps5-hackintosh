# macOS on PS5 — bare metal, no VM

> I put **macOS Monterey** on a **PlayStation 5**. Not in a VM. Not streamed. Not "technically." The Apple kernel is running on the console's Zen 2 cores and drawing the desktop on the TV.
> Sony was not consulted.

![macOS Monterey 12.7.6 on the PS5: a Terminal running fastfetch next to About This Mac](docs/desktop.png)

*The desktop on the console, photographed off the TV. The text over it is my own debug console, drawn on the screen by the kernel; the user name, local address and serial number are blurred. 14 GB is the memory size I declare to macOS (see* Known issues*), and the CPU name and 1 GHz are what OpenCore and AMD's firmware tell it, not what runs.*

```
                ┌──────────────────────────────────────────────┐
 PS5 (jailbroken)│  Linux  →  crash-kexec  →  my own stage 1    │
                └───────────────┬──────────────────────────────┘
                                ▼
              EDK2 UEFI payload  →  OpenCore  →  boot.efi  →  XNU 21.6.0
                                ▼
                  macOS 12.7.6 on an AMD "Oberon" SoC.
```

**Status: it boots to a stable desktop on all 16 threads, with keyboard, mouse and trackpad.** Supported hardware, for now: a launch-model ("fat") PS5 on firmware 13.42, the only one I have run it on. It's slow because there's no GPU acceleration yet (more on that below), and it only knows how to be a computer after a very specific ritual. Details, receipts and a lot of bit-twiddling below.

---

## What works (and what doesn't)

| | Status | Notes |
|---|---|---|
| 🧠 CPU | ✅ 16 threads | 8 Zen 2 cores / 16 threads. Two bugs stood in the way: a cache-topology deadlock and a performance counter Linux leaves running, see below |
| 🖥️ Display | ✅ | 2560×1440, WindowServer on the firmware framebuffer. **No hardware acceleration** (yet) |
| ⌨️🖱️ USB input | ✅ | Keyboard, mouse, trackpad (even the scroll) |
| 💾 USB storage | ✅ | Boot disk is a USB SSD (APFS). The AMD xHCI behaves once the clocks are right |
| 🌐 Ethernet | ✅ | Sony's proprietary "Salina" gigabit NIC, driven by my own kext (`kexts/PS5SalinaGbE`, a port of the Linux `mts` driver). DHCP, ping, ssh into the console. Polled (no interrupts yet): about 25 MB/s today, from 3.5 MB/s when it first came up |
| 📶 Wi-Fi / 🔵 BT | ❌ | NXP IW620 combo chip. No driver on this planet for macOS. USB dongles are your friends |
| 🔊 Audio | ❌ | HDMI audio needs a real AMD framebuffer driver. USB DAC / headset works in principle |
| 🎮 GPU acceleration | ❌ | RDNA 1 "Cyan Skillfish". There's a Metal project for the sibling BC-250 board; see [Roadmap](#-roadmap) |
| 🔌 Shutdown / restart | ⚠️ | You get to power-cycle it yourself, like it's 1998 |
| 💿 Internal SSD | 🚫 | **Blocked on purpose.** macOS never touches it. See [Safety](#-safety-please-read) |

---

## The bugs that cost me the most time

This console has **no UART**. No serial port, no debug header, nothing. When it breaks, you get a black screen and nothing else. So the very first problem was: *how do you debug a computer that refuses to talk?* Answer: with the TV.

1. **Printf, but with colored squares.** First debugging tool: tiny code patches that paint a colored square on screen when a kernel function runs. Great for *"did I reach `i386_init`?"*, terrible for everything else. So I wrote **ktty**: a text console injected into the kernel itself that draws every `kprintf` character through the GPU's I/O-port window (no page tables needed). Now the macOS verbose boot shows up on the PS5 like it was always meant to. (Sony, presumably: *"why does that work?"*)
2. **The CPU that couldn't be interrupted.** The LAPIC timer expired, the interrupt was *pending*, the CPU had interrupts enabled… and **nothing was ever delivered**. Turns out AMD's *extended APIC* has per-vector interrupt-enable registers (IER) that come up with almost every vector masked. XNU has never heard of them. Linux sees them in a sane state; something between Linux and macOS leaves them closed. One tiny kernel hook later: interrupts.
3. **The 1 GHz lie.** OpenCore told XNU the TSC runs at 1 GHz. It runs at 1596.3 MHz. Every timeout in the kernel was ~37 % too short, so the USB 3 controller's power-up timed out *before it was ready*. The boot only worked at all when interrupts were *broken* (funny how that goes). I now overwrite the clock globals at the end of `tsc_init`.
4. **The framebuffer wants to live in a PCI BAR.** macOS' stock display driver (`IONDRVFramebuffer`) only accepts a boot framebuffer that sits inside one of the GPU's BARs. Mine sat in the middle of Sony's carve-out. So Linux (before I jump to UEFI) fills VRAM top-down until the picture lands in the first 256 MB, and my stage tells UEFI "the screen is at BAR0 + offset". WindowServer, finally: *oh, that's what you meant.*
5. **cpus=2 deadlocked in the scheduler.** A thread was waiting for a lock it already held. Root cause: on AMD there's no Intel CPUID leaf 4, so XNU decided every logical CPU has **its own private cache**, so every logical CPU got **its own processor set**, so the scheduler ended up in a state it can't recover from. I now tell XNU what a Zen 2 cache hierarchy looks like (L1/L2 shared by 2 threads, L3 per CCX) and the problem goes away.
6. **The interrupt that never ended.** With more than two threads the kernel died after 10–30 s, always on a different core: IPIs and TLB flushes just stopped being delivered. The stuck core had a processor priority of `0xD0` with nothing in service and the timer and IPI vectors waiting; one EOI write cleared it. Linux's hard-lockup detector keeps a hardware performance counter armed, the crash-kexec doesn't stop it, and XNU doesn't expect that interrupt. My best explanation is that its overflow vector was left "in service" and blocked everything below it. Zeroing the PMC control registers on every core before XNU takes interrupts made 16 threads stable (9 minutes under load, no panic).
7. **The NIC that answered nobody.** The Salina functions hang off an ARI bus whose PCI config space is a *proxy*: macOS and the UEFI happily "assign" BARs there (at `0x066xxxxx`, inside RAM) and the chip never hears about it. Linux knows: it reads and writes the real config through a window in the root complex. The kext does the same, finds the real BARs, and maps them by physical address. Reads stopped coming back `0xFFFFFFFF`.
8. **DMA that only works in the right RAM.** Next, both DMA engines failed with bus errors on the first descriptor. Linux never touches the 0x01000000-0x3fffffff range (e820 "Reserved") and neither can the southbridge, but macOS allocates its buffers exactly there. The fix is a 2 MiB pool inside RAM that Linux used, reserved in the UEFI memory map and mapped by the kext.
9. **A kext that was silently dropped.** For a while the Ethernet kext simply did not exist as far as the kernel was concerned: OpenCore's vtable patcher refused it, because a newer SDK declares two `IONetworkController` slots as used that are still padding on Monterey. I found out by building OpenCore's own injection test tool and running it on the host. The kext is now built against a header copy that fixes the two slots.
10. **Own goal of the year.** My debug console lived in kernel code that looked unused. `launchd` called `auditon`, jumped into my *font data*, and… yeah. It now lives in dead NFSv4 code, which this boot never runs. (Hey, a console with no network. Fits.)

> **Method:** built with AI pair-programming. Every fix above came from *measure → hypothesis → one boot → photo of the TV → repeat*. About 120 commits, zero serial cables.

---

## How it boots

1. Jailbreak the console and load **Linux** with `ps5-linux-loader` (not included, and I won't be explaining the exploit part). Needs `crashkernel=400M` on the kernel command line.
2. Linux puts a linear 2560×1440 framebuffer on screen — a welcome screen, because I have *standards* — and holds it.
3. `kexec_file_load` stages **my own stage 1** as the *crash kernel*, then `sysrq-c` runs it. A crash doesn't shut devices down, so the GPU just keeps scanning out. (Yes, I crash the kernel on purpose to boot an OS.)
4. Stage 1 builds an identity map, turns the AMD IOMMU off, builds the UEFI hand-off (HOBs, memory map, the real ACPI tables Sony shipped), and jumps into an **EDK2 UefiPayload**.
5. **OpenCore** takes over. A pile of kernel patches (see below) make XNU believe it's on hardware it can live with. `boot.efi` → **XNU 21.6.0** → desktop.

One crash-kexec per jailbreak boot. Chain two and the console resets. So: one attempt, one photo, think, repeat.

Starting from a blank console and a blank USB drive: [INSTALL.md](INSTALL.md). The short version:

```bash
cp .env.example .env        # then put the console's Linux IP in PS5_LINUX_HOST (Mac identity is generated on first build)

# on the Mac you build on, with the console up in Linux:
tools/clean_run.sh                       # quiet build, 2 CPUs, internal SSD blocked: the one to try first
CPUS=16 tools/clean_run.sh               # all 16 threads, once the first one gave you a desktop (power-cycle and jailbreak again before it)
tools/ktty_run.sh                        # debug build: kernel log drawn on the TV (CPUS=16 works here too)
```

Then pick **Macintosh HD** in the OpenCore menu.

---

## A note on QEMU

Early on I ran macOS as a **QEMU/KVM guest** on the console's own Linux (with the GPU passed through). That was never the goal, but it was a very good lab: with a working macOS next to me I could pull the kernel collection apart, compare IOKit registry trees, check what a healthy boot looks like and test ideas before touching the real hardware.

**It is not part of the boot you see here.** Nothing in the current chain starts QEMU, and macOS is not running inside any VM: the Apple kernel runs directly on the CPU, started by my own stage 1 and OpenCore (see *How it boots*). The QEMU scripts are still in the repo as research tooling, not as a way to run it.

---

## The kernel patches

All of these are OpenCore `Kernel > Patch` entries, built by `tools/make_baremetal_efi.py`. Most of the code lives in **stubs placed in unused NFSv4 functions** of the kernel image.

| Patch | Why |
|---|---|
| `tools/make_ierfix.py` | On every CPU: open AMD's APIC IER vectors and stop the performance counters Linux left running |
| `tools/make_tscfix.py` | TSC 1596.3 MHz, bus = TSC/64 (the real LAPIC timer is ~25 MHz) |
| `kexts/PS5SalinaGbE` | Ethernet: PCI 104d:9104, real BARs through the ARI proxy window, DMA pool in Linux's RAM, polled datapath. GPL-2.0 (derived from the Linux driver) |
| `tools/make_cachefix.py` | Zen 2 cores/caches for XNU, one pset per CCX instead of per thread |
| topology divisions | `_cpu_thread_alloc` otherwise divides by zero on AMD |
| XCPM off, AppleRTC, MCE reporter | Intel-only power management & friends |
| `IONVMeFamily` → *Disable* | The internal SSD controller is never to be touched |

Plus a fake IOAPIC, a 16-LAPIC MADT, SMBIOS `iMac20,2`, and Sony's own DSDT/FADT.

---

## Things worth knowing

- **The internal SSD stays untouched.** macOS has an NVMe driver that would try to attach to Sony's SSD controller. I disable it (`IONVMeFamily`, *Disable*). Boot from USB.
- Don't accept a "disk not readable, initialize?" prompt on anything you didn't create.
- Turn **automatic updates off.** 12.7.6 is the last Monterey, and an update replaces the kernel my patches are matched to.
- **The display is hard-wired to 2560×1440@60**, because that is the mode my screen offers. Yours may not offer it (`kms_linear.py` stops with "no connected connector with a 2560x1440 mode"), so change it before your first boot, in three places that have to agree: `W, H` in `bringup/tools/kms_linear.py` and `tools/make_welcome.py` (the framebuffer macOS inherits), and `Resolution` in `tools/make_baremetal_efi.py` (OpenCore). The `video=` on the console's Linux command line does not matter for this: `kms_linear.py` picks the mode itself.
- The IOMMU is off, so a misbehaving driver can DMA anywhere. Be careful with kexts for PCI devices you don't recognize.

The repo ships no Apple software, no Sony firmware images (binary dumps are git-ignored). The ACPI sources in `opencore/ACPI` are the exception worth knowing about: `DSDT-PS5-v2.dsl` is a disassembly of the 500-byte DSDT in the console's own firmware, patched by me, so it is Sony's table description rather than something I wrote from scratch and no exploit code. Bring your own console, macOS install and jailbreak.


---

## Known issues

**The boot disk sometimes vanishes.** The symptom, in the order you meet it: the verbose log shows `AppleUSBXHCIPCI: setPowerStateGated: going to state 2 failed with 0xe00002bc`, `interrupt while controller is unavailable` and `hardwareExceptionThreadCallGated ... attempting recovery` about 3.5 seconds into the kernel, and then the "prohibited" sign with a `support.apple.com/mac/startup` address; or you reach the desktop and a minute or two later it freezes while the screen fills with `disk1: media is not present` and `tx_enter() failed: 6`. The kernel and the network stay up, only the USB disk is gone (so `sshd` cannot start either). The disk sits on one of the console's AMD xHCI controllers (`pci1022,13ed`), not on Sony's.

What I know, and how much of it I trust:

- **How often.** Roughly half of the boots at a 4 GiB carve-out (`vram.txt`) lose the disk, and about 3 in 10 at the default 512 MB. Over eight boots at 4 GiB with my Ethernet kext's xHCI watch switched off, four reached the desktop and held and four lost the disk, two of them at the quiet build and two at the debug build (which draws the kernel log on screen).
- **What it is not.** It is not that diagnostic watch (the failures went on with it off; it is now **off by default**, `XhciWatch=1` in the kext personality turns it on), not the debug console versus the quiet build, and not the GPU register probe.
- **Leading suspect.** macOS hands out contiguous DMA buffers from the lowest free pages first, and the lowest free pages are the range `0x01000000-0x3fffffff`, which Linux marks reserved and which my Ethernet could not DMA into (it got bus errors there). I have no proof the AMD xHCI is affected: in a boot that held, its rings sat high, at `0x1f41b9000`.
- **The experiment under way.** `kexts/PS5Bus20Watch` logs the PCI and xHCI state of the bus the disk is on (so a failure leaves evidence) and, by default, **allocates and holds every free page below 1 GiB** so nothing can be placed in that range. The two boots I have run with it, one with the debug build and one with the quiet build, held 27 and 15+ minutes (the first under a read/write loop; I then switched the console off myself) without a disk error, where the four boots before it, without the pinning, had lost the disk within two or three minutes. Two clean boots against four bad ones is a lead, not proof; to try it: `KEXT=PS5Bus20Watch tools/build_kext.sh` and then `BUS20WATCH=1 tools/clean_run.sh`.
- **Other leads, untested.** Linux leaves devices running when it crashes into stage 1 (the HDMI audio controller, the GPU's interrupt ring, its own xHCI driver on both USB controllers), and its interrupt remapping stays configured. Quiescing them in Linux right before the crash is the obvious next step.
- **Root cause: not known.** The `0x4` in the "interrupt while controller is unavailable" lines looks like the xHCI's *Host System Error* bit (a failed DMA of its own), but I have not checked that against Apple's driver.

If it happens to you: power-cycle, jailbreak again and retry. I have not seen damage from it, but a disk that disappears in the middle of writes is how APFS earns its fsck, so keep a copy of the macOS partition. Keep `vram.txt` at the default 512 MB until you have a boot you trust. If you find the cause, I would very much like to hear it.


---

## Where I left it

I am moving on to other things, so here is what is open, in the order I would pick it up. All of it is yours: fork it, break it, send it back or don't.

1. **The disk that vanishes** (*Known issues*). Needs five or more clean boots in a row with `PS5Bus20Watch` before anyone trusts it; after that the pinning could move into stage 1 (leave that RAM out of the memory map) instead of a kext.
2. **GPU acceleration.** The console's GPU arrives alive: Linux leaves it initialised, with the 4 GiB carve-out as VRAM and its microcode loaded (`kexts/PS5GpuProbe` reads a few dozen registers to show it). `kexts/PS5Cyan` is step one of a plan, a Lilu plugin that stops Apple's AMD driver from ever talking to the power controller, which belongs to Sony's firmware. Next would be a personality for PCI `1002:13fc` pointing at the Navi 10/14 accelerator classes, a way to make `HWLibs` (which only knows GC 10.1.0 to 10.1.2; this one is 10.1.3) accept it, the memory and page-table functions, and polling instead of interrupts for SDMA. Forty-three of the forty-five functions that MetalCyan hooks on the BC-250 exist by name in Monterey's `AMDRadeonX6000`. Never let anything send a message to the SMU/MP1.
3. **Ethernet speed.** About 25 MB/s polled; per-packet copy cost dominates. Interrupts instead of polling are untried.
4. **Install without QEMU.** Step 2 of `INSTALL.md` is the part that is not scripted end to end.

---

## Repo map

| Path | What's in it |
|---|---|
| `bringup/` | The crash-kexec **stage 1** (`stub.c`), the **ktty** kernel console, the return-to-Linux path, debug tools. Has its own README |
| `kexts/` | My own macOS kexts (Ethernet). Built with `tools/build_kext.sh`, injected by OpenCore |
| `uefi/` | EDK2 UefiPayloadPkg build + the HOB/bundle builder. Has its own README |
| `opencore/` | OpenCore config and the ACPI tables (Sony's, dumped and patched) |
| `tools/` | Build/deploy scripts: `clean_run.sh`, `ktty_run.sh`, the patch generators, the welcome screen |
| `third_party/ps5-linux-loader` | The upstream Linux loader: fetched, not vendored (`tools/fetch_binaries.sh`). Not mine; go star it |
| `kernel/`, `initramfs/`, `tools/*qemu*`, `tools/create_macos_img.sh`, `tools/deploy.sh`, `tools/launch_hackintosh.sh`, `make deploy` | Research tooling from the QEMU phase (see below). **Not used to boot macOS anymore** |

---

## Roadmap

- [x] Boot XNU on the PS5 with a visible log
- [x] Interrupts, clocks, USB boot disk
- [x] Stable desktop with input
- [x] More than one CPU thread
- [x] 8 cores / 16 threads
- [ ] Clean shutdown / restart
- [x] Internal Ethernet (own kext)
- [ ] Ethernet at gigabit speed. The DMA pool is now ordinary cacheable RAM and the CPU copy is no longer the limit; the next suspect is the single thread that runs the whole receive path. The kext publishes per-stage timings in the IORegistry (`ioreg -c PS5SalinaGbE -l`) to find out
- [ ] Audio / Bluetooth dongles
- [ ] Cosmetics: *About This Mac* says 1 GHz and "DDR4" (the PS5 has 16 GB of **GDDR6**, I know)
- [ ] **GPU acceleration.** The PS5's GPU is a Cyan Skillfish with no PSP/SMU in its path, and Linux already accelerates it. The closest prior art ([MetalCyan](https://github.com/amethyst8118/MetalCyan), for the ASRock BC-250, macOS Tahoe) needs a 4 GB carve-out; the PS5 loader's `vram.txt` sets the carve-out size (512 MB by default, 1 GiB tested and working). I'm mapping what Monterey's AMD drivers would need.
- [ ] Boot without the jailbreak/Linux dance — *(I'm not holding my breath)*

---

## Credits

- **`ps5-linux-loader`** and the people behind the PS5 Linux work: the door I walked through.
- **Acidanthera** (OpenCore, Lilu, VirtualSMC, RestrictEvents) and **AMD-OSX / AMD_Vanilla** (the base AMD kernel patches).
- **EDK2** / TianoCore.
- **The Linux `amdgpu` driver**: my reference for how this GPU actually behaves.
- **Armandas Kvietkus** and the [ps5-linux-patches](https://github.com/ps5-linux/ps5-linux-patches) contributors: the Linux `mts` Ethernet driver that `kexts/PS5SalinaGbE` is ported from.
- Everyone who ever posted a register dump on a forum.

## License

MIT, see [LICENSE](LICENSE): use it, change it, ship it, I don't mind what you do with it. The one exception is `kexts/PS5SalinaGbE`, which is GPL-2.0-only because it is a port of the Linux Ethernet driver for the PS5 (license text next to it). Third-party software (OpenCore, EDK2, Lilu, VirtualSMC, ps5-linux-loader...) is downloaded by the scripts, not included, and keeps its own license.

---

<sub>Built with a TV used as a logic analyzer and a lot of patience.</sub>
