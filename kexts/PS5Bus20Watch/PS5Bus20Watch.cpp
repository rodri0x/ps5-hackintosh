// SPDX-License-Identifier: MIT
// Read-only watcher of the PCIe bus that holds the console's GPU and the AMD xHCI controllers (bridge 00:08.1, bus 0x20 functions .0 .4 .5).
// Some boots lose the USB root disk: both xHCI controllers log a Host System Error. This kext polls the registers that tell link, power and DMA
// problems apart and logs one line each time one of them changes, so the evidence is on the screen (and in `log stream` if an ssh session
// is already open) at the moment of the failure. It writes nothing to any device.
// PinLow (personality property, on by default): at start it also allocates and keeps every free page below 1 GiB, so nothing else can be placed in
// the range 0x01000000-0x3fffffff that Linux marks reserved and that the console's own devices may not be able to DMA into (the DMA buffers of
// macOS are handed out from the lowest free pages first). The kext never unloads, so the pages stay pinned.
// DmaTest (personality number, seconds after start, 0 = off): the experiment that settles whether that range is the problem. It reads the first
// 4 KiB of the boot disk into an ordinary buffer (the control) and then into pinned chunks spread over the low range, comparing the data. If the
// disk's controller cannot DMA there, the read into a low chunk ends in the Host System Error on purpose: THE BOOT IS SACRIFICED.
//   per device: PCI command/status, bridge secondary status and control, PCIe link/device control and status, PM control/status, AER status;
//   xHCI: USBCMD, USBSTS (the event-interrupt bit masked), HCCPARAMS1, DCBAAP and the interrupter 0 ERST base.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <kern/thread.h>

#define LOG(fmt, ...) IOLog("Bus20Watch: " fmt "\n", ##__VA_ARGS__)
#define NDEV 4
#define NVAL 16

struct WatchSpec { uint8_t bus, dev, fn; bool xhci; };
static const WatchSpec specs[NDEV] = { {0, 8, 1, false}, {0x20, 0, 0, false}, {0x20, 0, 4, true}, {0x20, 0, 5, true} };

class PS5Bus20Watch : public IOService {
    OSDeclareDefaultStructors(PS5Bus20Watch)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOPCIDevice *dev[NDEV] = {};
    IOMemoryMap *map[NDEV] = {};
    uint8_t pcieOff[NDEV] = {}, pmOff[NDEV] = {};
    uint16_t aerOff[NDEV] = {};
    uint32_t last[NDEV][NVAL] = {};
    bool seen[NDEV] = {};
    uint32_t round = 0, lines = 0, found = 0, errLines = 0, seq = 0;
    OSArray *pinned = nullptr;
    uint32_t dmaTestDelay = 0, dmaScanBelow = 0;
    void pinLow();
    void dmaTest();
    static void dmaTestThread(void *arg, wait_result_t);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    static void pollFired(OSObject *owner, IOTimerEventSource *t);
    void poll(IOTimerEventSource *t);
    void find();
    void sample(int i, uint32_t *v);
};

#define super IOService
OSDefineMetaClassAndStructors(PS5Bus20Watch, IOService)

bool PS5Bus20Watch::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    LOG("started");
    OSBoolean *pin = OSDynamicCast(OSBoolean, getProperty("PinLow"));
    if (!pin || pin->isTrue()) pinLow();
    OSNumber *dt = OSDynamicCast(OSNumber, getProperty("DmaTest"));
    if (dt) dmaTestDelay = dt->unsigned32BitValue();
    dt = OSDynamicCast(OSNumber, getProperty("DmaScanBelow"));       // fine scan: every pinned chunk below this address, highest first
    if (dt) dmaScanBelow = dt->unsigned32BitValue();
    loop = IOWorkLoop::workLoop();
    if (!loop) return false;
    timer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action)&PS5Bus20Watch::pollFired);
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) return false;
    timer->setTimeoutMS(500);
    registerService();
    if (dmaTestDelay && pinned && pinned->getCount()) {
        thread_t th;
        if (kernel_thread_start(&PS5Bus20Watch::dmaTestThread, this, &th) == KERN_SUCCESS) thread_deallocate(th);
    }
    return true;
}


void PS5Bus20Watch::pinLow()
{
    pinned = OSArray::withCapacity(520);
    if (!pinned) return;
    uint64_t lo = ~0ULL, hi = 0, total = 0;
    for (int n = 0; n < 520; n++) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 0x200000, 0x000000003FFFF000ULL);
        if (!b) break;
        IOByteCount seg = 0;
        uint64_t pa = b->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!pa || pa >= 0x40000000ULL) { b->release(); break; }
        pinned->setObject(b);
        b->release();
        total += 0x200000;
        if (pa < lo) lo = pa;
        if (pa + 0x200000 > hi) hi = pa + 0x200000;
    }
    LOG("pinned %llu MiB below 1 GiB in %u chunks, lowest 0x%llx, highest end 0x%llx", (unsigned long long)(total >> 20), pinned->getCount(), (unsigned long long)(lo == ~0ULL ? 0 : lo), (unsigned long long)hi);
}

void PS5Bus20Watch::dmaTestThread(void *arg, wait_result_t)
{
    PS5Bus20Watch *self = (PS5Bus20Watch *)arg;
    IOSleep(self->dmaTestDelay * 1000);
    self->dmaTest();
    thread_terminate(current_thread());
}

static uint64_t chunkPhys(IOBufferMemoryDescriptor *b)
{
    IOByteCount seg = 0;
    return b->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
}

void PS5Bus20Watch::dmaTest()
{
    IOMedia *media = nullptr;
    OSDictionary *m = IOService::serviceMatching("IOMedia");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    if (m) m->release();
    if (it) {
        OSObject *o;
        while ((o = it->getNextObject())) {
            IOMedia *c = OSDynamicCast(IOMedia, o);
            if (c && c->isWhole() && c->getSize() > (1ULL << 30)) { media = c; media->retain(); break; }
        }
        it->release();
    }
    if (!media) { LOG("DMATEST: no whole-disk IOMedia found"); return; }
    LOG("DMATEST: disk '%s', %llu MiB, %u pinned chunks", media->getName(), (unsigned long long)(media->getSize() >> 20), pinned->getCount());
    if (!media->open(this, 0, kIOStorageAccessReader)) { LOG("DMATEST: cannot open the disk for reading"); media->release(); return; }
    const uint32_t len = 4096;
    // control: an ordinary buffer, wherever macOS puts it
    IOBufferMemoryDescriptor *ctl = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, len, 4096);
    UInt64 actual = 0;
    IOReturn r = ctl ? media->read(this, 0, ctl, nullptr, &actual) : kIOReturnNoMemory;
    LOG("DMATEST: control read into phys 0x%llx: result 0x%x, %llu bytes", ctl ? (unsigned long long)chunkPhys(ctl) : 0ULL, r, (unsigned long long)actual);
    if (r != kIOReturnSuccess || actual != len) { media->close(this); media->release(); if (ctl) ctl->release(); return; }
    const uint8_t *want = (const uint8_t *)ctl->getBytesNoCopy();
    // chunks spread over what was pinned, highest address first (nearest to the RAM Linux uses), 8 of them at most
    uint32_t n = pinned->getCount(), step = dmaScanBelow ? 1 : (n > 8 ? n / 8 : 1);
    for (int32_t i = (int32_t)n - 1; i >= 0; i -= (int32_t)step) {
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, pinned->getObject((uint32_t)i));
        if (!b) continue;
        if (dmaScanBelow && chunkPhys(b) >= dmaScanBelow) continue;
        uint8_t *va = (uint8_t *)b->getBytesNoCopy();
        memset(va, 0xa5, len);
        IOMemoryDescriptor *d = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)va, len, kIODirectionIn, kernel_task);
        if (!d) continue;
        if (d->prepare() != kIOReturnSuccess) { LOG("DMATEST: cannot wire the low buffer"); d->release(); continue; }      // the storage stack panics on a descriptor that is not wired
        LOG("DMATEST: reading the same block into LOW phys 0x%llx (chunk %d) ...", (unsigned long long)chunkPhys(b), i);
        IOSleep(dmaScanBelow ? 300 : 2000);
        actual = 0;
        r = media->read(this, 0, d, nullptr, &actual);
        bool same = memcmp(va, want, len) == 0, untouched = true;
        for (uint32_t k = 0; k < len; k++) if (va[k] != 0xa5) { untouched = false; break; }
        LOG("DMATEST: LOW phys 0x%llx: result 0x%x, %llu bytes, data %s", (unsigned long long)chunkPhys(b), r, (unsigned long long)actual,
            same ? "MATCHES the control (DMA works here)" : untouched ? "NOT WRITTEN (buffer still holds the fill pattern)" : "DIFFERENT from the control");
        d->complete();
        d->release();
        if (r != kIOReturnSuccess) break;
    }
    LOG("DMATEST: done");
    media->close(this);
    media->release();
    ctl->release();
}

void PS5Bus20Watch::stop(IOService *provider)
{
    if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); timer->release(); timer = nullptr; }
    for (int i = 0; i < NDEV; i++) { if (map[i]) map[i]->release(); if (dev[i]) dev[i]->release(); map[i] = nullptr; dev[i] = nullptr; }
    if (loop) { loop->release(); loop = nullptr; }
    super::stop(provider);
}

void PS5Bus20Watch::find()
{
    OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    if (m) m->release();
    if (!it) return;
    OSObject *o;
    while ((o = it->getNextObject())) {
        IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
        if (!d) continue;
        for (int i = 0; i < NDEV; i++) {
            if (dev[i] || d->getBusNumber() != specs[i].bus || d->getDeviceNumber() != specs[i].dev || d->getFunctionNumber() != specs[i].fn) continue;
            d->retain();
            dev[i] = d;
            UInt8 off = 0;
            if (d->findPCICapability(kIOPCIPCIExpressCapability, &off)) pcieOff[i] = off;
            off = 0;
            if (d->findPCICapability(kIOPCIPowerManagementCapability, &off)) pmOff[i] = off;
            for (uint32_t p = 0x100, n = 0; p && p < 0x1000 && n < 48; n++) {          // extended capability list, AER is ID 1
                uint32_t h = d->extendedConfigRead32(p);
                if (h == 0 || h == 0xffffffff) break;
                if ((h & 0xffff) == 1) { aerOff[i] = (uint16_t)p; break; }
                p = (h >> 20) & 0xffc;
            }
            if (specs[i].xhci) map[i] = d->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
            found++;
            LOG("%02x:%02x.%x found: pcie cap %02x, pm cap %02x, aer %03x, mmio %s", specs[i].bus, specs[i].dev, specs[i].fn, pcieOff[i], pmOff[i], aerOff[i], specs[i].xhci ? (map[i] ? "mapped" : "NOT mapped") : "-");
        }
    }
    it->release();
}

void PS5Bus20Watch::sample(int i, uint32_t *v)
{
    IOPCIDevice *d = dev[i];
    v[0] = d->extendedConfigRead32(0x04) & ~0x00080000u;                    // command | status << 16 (the INTx status bit toggles on its own)
    v[1] = specs[i].bus == 0 ? d->extendedConfigRead32(0x1c) : 0;           // bridge: secondary status | IO base/limit
    v[2] = specs[i].bus == 0 ? d->extendedConfigRead32(0x3c) : 0;           // bridge: control << 16 | interrupt line/pin
    v[3] = pcieOff[i] ? d->extendedConfigRead32(pcieOff[i] + 0x10) : 0;     // link control | link status << 16
    v[4] = pcieOff[i] ? d->extendedConfigRead32(pcieOff[i] + 0x08) : 0;     // device control | device status << 16
    v[5] = pmOff[i] ? d->extendedConfigRead32(pmOff[i] + 4) : 0;            // PM control/status
    v[6] = aerOff[i] ? d->extendedConfigRead32(aerOff[i] + 4) : 0;          // AER uncorrectable status
    v[7] = aerOff[i] ? d->extendedConfigRead32(aerOff[i] + 0x10) : 0;       // AER correctable status
    if (map[i]) {
        volatile uint8_t *cap = (volatile uint8_t *)map[i]->getVirtualAddress();
        uint32_t caplen = OSReadLittleInt32((volatile void *)cap, 0) & 0xff;
        if (caplen < 0x10 || caplen > 0x100) caplen = 0x20;
        volatile uint8_t *op = cap + caplen;
        uint32_t rtsoff = OSReadLittleInt32((volatile void *)cap, 0x18) & ~0x1fu;
        v[8] = OSReadLittleInt32((volatile void *)op, 0);                   // USBCMD
        v[9] = OSReadLittleInt32((volatile void *)op, 4) & ~0x18u;          // USBSTS without the event-interrupt and port-change bits, which toggle all the time
        v[10] = OSReadLittleInt32((volatile void *)cap, 0x10);              // HCCPARAMS1 (AC64 is bit 0)
        v[11] = OSReadLittleInt32((volatile void *)op, 0x30);               // DCBAAP low
        v[12] = OSReadLittleInt32((volatile void *)op, 0x34);               // DCBAAP high
        v[13] = rtsoff < 0x1800 ? OSReadLittleInt32((volatile void *)(cap + rtsoff), 0x30) : 0;   // interrupter 0 ERST base low
        v[14] = rtsoff < 0x1800 ? OSReadLittleInt32((volatile void *)(cap + rtsoff), 0x34) : 0;   // ... high
    }
}

void PS5Bus20Watch::pollFired(OSObject *owner, IOTimerEventSource *t)
{
    PS5Bus20Watch *self = OSDynamicCast(PS5Bus20Watch, owner);
    if (self) self->poll(t);
}

void PS5Bus20Watch::poll(IOTimerEventSource *t)
{
    round++;
    if (found < NDEV && (round % 20) == 1) find();
    for (int i = 0; i < NDEV; i++) {
        if (!dev[i]) continue;
        uint32_t v[NVAL] = {};
        sample(i, v);
        bool changed = !seen[i];
        for (int k = 0; k < NVAL; k++) if (v[k] != last[i][k]) changed = true;
        // error evidence is logged whatever the line budget says: PCI status error bits (15:11, 8), AER status, USBSTS HSE/HCE, a controller reading all ones
        bool err = (v[0] & 0xf9000000u) != 0 || v[6] != 0 || (specs[i].xhci && ((v[9] & 0x1004u) != 0 || v[9] == 0xffffffe7u));
        if ((changed && lines < 400) || (changed && err && errLines < 200)) {
            if (err) errLines++; else lines++;
            LOG("#%u t=%u %02x:%02x.%x%s cmd %08x br %08x/%08x lnk %08x dev %08x pm %08x aer %08x/%08x | xhci cmd %08x sts %08x hcc %08x dcbaap %08x%08x%s erst %08x%08x",
                ++seq, round, specs[i].bus, specs[i].dev, specs[i].fn, err ? " ERROR" : "", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[12], v[11],
                (specs[i].xhci && v[11] && v[11] < 0x40000000u && v[12] == 0) ? " (LOW)" : "", v[14], v[13]);
            for (int k = 0; k < NVAL; k++) last[i][k] = v[k];
            seen[i] = true;
        }
    }
    if ((round % 500) == 0) LOG("#%u hb t=%u lines %u err %u", ++seq, round, lines, errLines);       // about every 5 s early on
    t->setTimeoutMS(round < 3000 ? 10 : 100);        // every 10 ms for the first ~30 s of watching, then every 100 ms
}
