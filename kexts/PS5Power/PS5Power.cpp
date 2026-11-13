// SPDX-License-Identifier: GPL-2.0-only
// Shut Down and Restart for macOS on the PS5. The console has no ACPI way to power off or reset: Linux asks Sony's southbridge
// controller (the "EMC", reached through the ICC mailbox of the Salina glue function, PCI 104d:9107) to do it. This kext sends the same
// two messages, at the very end of a macOS shutdown or restart, after the file systems are unmounted.
//
// Ported from the ps5-linux kernel patches (drivers/ps5/spcie.c: icc_send, icc_query, icc_power_shutdown, icc_power_reboot,
// icc_configuration_clear_cpu_info_bit; include/linux/ps5.h: struct icc_msg), GPL-2.0-only, hence this file's license.
//   * Polled, no interrupt: the ICC interrupt mask is cleared first (macOS has no handler for that vector).
//   * Only the ICC power service and the "cpu info bit" of the general service are used. Nothing goes to the SMU/MP1.
//   * A restart brings the console back to the PS5 system software, like a reboot from Linux: macOS needs a new jailbreak after it.
//
// The glue function's config space as macOS sees it is a proxy (see kexts/PS5SalinaGbE): the real BAR2 and BAR4 are read through the
// root complex's window, as PS5SalinaGbE does for the NIC.
//
// Personality properties: DryRun (bool) logs everything and sends nothing; Enabled (bool, default true).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>

#define LOG(fmt, ...) IOLog("PS5Power: " fmt "\n", ##__VA_ARGS__)

// ---- ICC mailbox (drivers/ps5/spcie.c) ------------------------------------------------------------------------------------------------
#define ICC_DOORBELL_OFFSET   0x108000u      // in BAR2
#define ICC_REG_DOORBELL      0x04u          // in the doorbell block
#define ICC_REG_INTR_STATUS   0x14u
#define ICC_REG_INTR_MASK     0x24u
#define ICC_QUERY_OFFSET      0x000u         // in BAR4 ("pervasive0")
#define ICC_REPLY_OFFSET      0x800u
#define ICC_REG_SOW           0x7f0u
#define ICC_REG_SOR           0x7f4u
#define ICC_REG_EMW           0xff0u
#define ICC_REG_EMR           0xff4u
#define ICC_SEND              0x01u
#define ICC_ACK               0x02u
#define ICC_MSG_TYPE_REPLY    0x4000u
#define ICC_MSG_TYPE_NOTIF    0x8000u
#define ICC_MSG_MIN_SIZE      0x20u
#define ICC_MSG_MAX_SIZE      0x7f0u
#define ICC_SERVICE_GENERAL   0x02u
#define ICC_SERVICE_POWER     0x04u

struct __attribute__((packed)) IccHeader {   // struct icc_msg
    uint8_t  magic;
    uint8_t  serviceId;
    uint16_t msgType;
    uint16_t unk04;
    uint16_t id;
    uint16_t length;
    uint16_t checksum;
};
static_assert(sizeof(IccHeader) == 12, "icc_msg header");

#define GLUE_ECAM_BASE 0xF0000000ULL          // bus 0 dev 0 extended config space, holds the real-config window (as in PS5SalinaGbE)

class PS5Power : public IOService {
    OSDeclareDefaultStructors(PS5Power)
    IOPCIDevice *pci = nullptr;
    IOMemoryDescriptor *ecamMd = nullptr, *bellMd = nullptr, *iccMd = nullptr;
    IOMemoryMap *ecamMap = nullptr, *bellMap = nullptr, *iccMap = nullptr;
    volatile uint8_t *bell = nullptr, *icc = nullptr;
    uint16_t rid = 0, nextId = 0x5000;
    bool dryRun = false, pmJoined = false;

    IOMemoryMap *mapPhys(uint64_t phys, uint64_t len, IOMemoryDescriptor **mdOut);
    uint32_t sfcRead(uint16_t rid, uint32_t reg);
    uint64_t realBar(int reg);
    void freeMaps();
    bool iccQuery(uint8_t *msg, uint32_t timeoutMs);
    bool clearCpuInfoBits();
    bool power(bool reboot);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void systemWillShutdown(IOOptionBits specifier) override;
};

#define super IOService
OSDefineMetaClassAndStructors(PS5Power, IOService)

static IOPMPowerState powerStates[2] = {
    {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

IOMemoryMap *PS5Power::mapPhys(uint64_t phys, uint64_t len, IOMemoryDescriptor **mdOut)
{
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys, len, kIODirectionNone);
    if (!md) return nullptr;
    IOMemoryMap *m = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!m) { md->release(); return nullptr; }
    *mdOut = md;
    return m;
}

// Read a register of the REAL function behind the ARI proxy (arch/x86/pci/common.c sfc_read; same code as PS5SalinaGbE::sfcRead)
uint32_t PS5Power::sfcRead(uint16_t r, uint32_t reg)
{
    if (!ecamMap) return 0xFFFFFFFF;
    volatile uint8_t *e = (volatile uint8_t *)ecamMap->getVirtualAddress();
    uint32_t busno = (r >> 8) & 0xff, slot = (r & 0xff) >> 3, func = r & 7;
    if (busno == 0 || busno == 32) {
        OSWriteLittleInt32((volatile void *)e, 0xd90, 4);
        OSWriteLittleInt32((volatile void *)e, 0xd80, (32u << 24) | (slot << 19) | (func << 16) | (reg & 0xf80));
        return OSReadLittleInt32((volatile void *)e, (reg & 0x7f) | 0xf80);
    }
    if (busno < 32) {
        OSWriteLittleInt32((volatile void *)e, 0xd10, busno == 1 ? 4 : 5);
        OSWriteLittleInt32((volatile void *)e, 0xd00, (busno << 24) | (slot << 19) | (func << 16) | (reg & 0xf80));
        return OSReadLittleInt32((volatile void *)e, (reg & 0x7f) | 0xf00);
    }
    return 0xFFFFFFFF;
}

// A 64-bit memory BAR of the real glue function (config offset reg and reg + 4), or 0 when it does not look like one
// The window is shared with PS5SalinaGbE (select register, then read): if both kexts used it at the same moment, one read could come from
// the other's selection. So each BAR is read three times, 1 ms apart, and only taken when all three agree.
uint64_t PS5Power::realBar(int reg)
{
    uint64_t first = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t lo = sfcRead(rid, reg), hi = sfcRead(rid, reg + 4);
        if (i == 0) LOG("real config %02x/%02x: %08x %08x", reg, reg + 4, lo, hi);
        if (lo == 0xffffffff || (lo & 1) || ((lo >> 1) & 3) != 2) return 0;     // a 64-bit memory BAR, or nothing
        uint64_t a = ((uint64_t)hi << 32) | (lo & ~0xfu);
        if ((a & 0xfff) || a == 0) return 0;
        if (i == 0) first = a;
        else if (a != first) { LOG("real config %02x read %llx then %llx: not trusted", reg, (unsigned long long)first, (unsigned long long)a); return 0; }
        IODelay(1000);
    }
    return first;
}

// The mailbox flags are 0 or 1 and the interrupt registers use bits 0-1 only: anything else means the mapping is not the ICC mailbox
static bool flag01(uint16_t v) { return v == 0 || v == 1; }

void PS5Power::freeMaps()
{
    if (iccMap) { iccMap->release(); iccMap = nullptr; }
    if (iccMd) { iccMd->release(); iccMd = nullptr; }
    if (bellMap) { bellMap->release(); bellMap = nullptr; }
    if (bellMd) { bellMd->release(); bellMd = nullptr; }
    if (ecamMap) { ecamMap->release(); ecamMap = nullptr; }
    if (ecamMd) { ecamMd->release(); ecamMd = nullptr; }
    bell = icc = nullptr;
}

bool PS5Power::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    OSBoolean *en = OSDynamicCast(OSBoolean, getProperty("Enabled"));
    if (en && !en->getValue()) { LOG("Enabled = false in the personality: not attaching"); return false; }
    OSBoolean *dry = OSDynamicCast(OSBoolean, getProperty("DryRun"));
    dryRun = dry && dry->getValue();

    ecamMap = mapPhys(GLUE_ECAM_BASE, 0x1000, &ecamMd);
    rid = pci->extendedConfigRead16(0x150);
    uint64_t bar2 = 0, bar4 = 0;
    if (ecamMap && rid && rid != 0xffff) { bar2 = realBar(0x18); bar4 = realBar(0x20); }
    LOG("glue requester id %04x, real BAR2 %llx, BAR4 %llx%s", rid, (unsigned long long)bar2, (unsigned long long)bar4, dryRun ? " (DryRun: nothing will be sent)" : "");
    // no fallback address on purpose: writing a doorbell at a guessed address is not something to do at shutdown
    if (!bar2 || !bar4) { LOG("could not read the glue function's real BARs: Shut Down and Restart stay as they were"); freeMaps(); return false; }
    bellMap = mapPhys(bar2 + ICC_DOORBELL_OFFSET, 0x1000, &bellMd);
    iccMap = mapPhys(bar4, 0x1000, &iccMd);
    if (!bellMap || !iccMap) { LOG("mapping the ICC mailbox failed"); freeMaps(); return false; }
    bell = (volatile uint8_t *)bellMap->getVirtualAddress();
    icc = (volatile uint8_t *)iccMap->getVirtualAddress();
    uint32_t st = OSReadLittleInt32((volatile void *)bell, ICC_REG_INTR_STATUS), mask = OSReadLittleInt32((volatile void *)bell, ICC_REG_INTR_MASK);
    uint16_t sow = OSReadLittleInt16((volatile void *)icc, ICC_REG_SOW), sor = OSReadLittleInt16((volatile void *)icc, ICC_REG_SOR);
    uint16_t emw = OSReadLittleInt16((volatile void *)icc, ICC_REG_EMW), emr = OSReadLittleInt16((volatile void *)icc, ICC_REG_EMR);
    LOG("ICC as found: intr status %08x mask %08x, SOW %04x SOR %04x EMW %04x EMR %04x", st, mask, sow, sor, emw, emr);
    if ((st & ~3u) || (mask & ~3u) || !flag01(sow) || !flag01(sor) || !flag01(emw) || !flag01(emr)) {
        LOG("these do not look like the ICC mailbox: not attaching, Shut Down and Restart stay as they were");
        freeMaps();
        return false;
    }

    // a member of the power plane gets systemWillShutdown() when the system shuts down or restarts
    PMinit();
    pci->joinPMtree(this);
    registerPowerDriver(this, powerStates, 2);
    pmJoined = true;
    setProperty("DryRun", dryRun);
    registerService();
    LOG("ready: Shut Down powers the console off, Restart reboots it%s", dryRun ? " (DryRun)" : "");
    return true;
}

void PS5Power::stop(IOService *provider)
{
    if (pmJoined) { PMstop(); pmJoined = false; }
    freeMaps();
    super::stop(provider);
}

// icc_send + icc_query, polled: write the message, ring the doorbell, wait for the reply (acknowledging notifications on the way)
bool PS5Power::iccQuery(uint8_t *msg, uint32_t timeoutMs)
{
    IccHeader *h = (IccHeader *)msg;
    if (h->length < ICC_MSG_MIN_SIZE) h->length = ICC_MSG_MIN_SIZE;
    h->magic = 0x42;
    h->unk04 = 3;
    h->id = nextId++;
    h->checksum = 0;
    uint16_t sum = 0;
    for (uint16_t i = 0; i < h->length; i++) sum += msg[i];
    h->checksum = sum;
    LOG("ICC query service %u type %04x id %04x length %u%s", h->serviceId, h->msgType, h->id, h->length, dryRun ? ": DryRun, not sent" : "");
    if (dryRun) return true;

    // A message from the controller that nobody acknowledged (EMW still 1) would hold the mailbox: acknowledge it first, the way
    // Linux's probe does (spcie.c), then clear the stale interrupt status
    if (OSReadLittleInt16((volatile void *)icc, ICC_REG_EMW)) {
        LOG("ICC: an unacknowledged message from the controller (type %04x): acknowledging it", OSReadLittleInt16((volatile void *)icc, ICC_REPLY_OFFSET + 2));
        OSWriteLittleInt16((volatile void *)icc, ICC_REG_EMW, 0);
        OSWriteLittleInt16((volatile void *)icc, ICC_REG_EMR, 1);
        OSWriteLittleInt32((volatile void *)bell, ICC_REG_DOORBELL, ICC_ACK);
    }
    OSWriteLittleInt32((volatile void *)bell, ICC_REG_INTR_STATUS, ICC_SEND | ICC_ACK);
    OSWriteLittleInt16((volatile void *)icc, ICC_REG_SOR, 0);
    for (uint16_t i = 0; i < h->length; i++) icc[ICC_QUERY_OFFSET + i] = msg[i];
    OSWriteLittleInt16((volatile void *)icc, ICC_REG_SOW, 1);
    __asm__ volatile("mfence" ::: "memory");
    OSWriteLittleInt32((volatile void *)bell, ICC_REG_DOORBELL, ICC_SEND);

    for (uint32_t waited = 0; waited < timeoutMs; waited++) {
        uint32_t st = OSReadLittleInt32((volatile void *)bell, ICC_REG_INTR_STATUS);
        if (st) OSWriteLittleInt32((volatile void *)bell, ICC_REG_INTR_STATUS, st);
        // a message is waiting when the controller raised SEND, or (polled, status already cleared) when EMW is set
        if ((st & ICC_SEND) || OSReadLittleInt16((volatile void *)icc, ICC_REG_EMW)) {
            uint16_t type = OSReadLittleInt16((volatile void *)icc, ICC_REPLY_OFFSET + 2);
            uint16_t rlen = OSReadLittleInt16((volatile void *)icc, ICC_REPLY_OFFSET + 8);
            uint32_t d0 = OSReadLittleInt32((volatile void *)icc, ICC_REPLY_OFFSET + sizeof(IccHeader));
            OSWriteLittleInt16((volatile void *)icc, ICC_REG_EMW, 0);
            OSWriteLittleInt16((volatile void *)icc, ICC_REG_EMR, 1);
            OSWriteLittleInt32((volatile void *)bell, ICC_REG_DOORBELL, ICC_ACK);
            if (type & ICC_MSG_TYPE_REPLY) { LOG("ICC reply after %u ms: type %04x length %u data %08x", waited, type, rlen, d0); return true; }
            // a notification (button, thermal...): acknowledged above, keep waiting for our reply
            LOG("ICC notification type %04x while waiting: acknowledged", type);
        }
        IODelay(1000);
    }
    LOG("ICC: no reply within %u ms", timeoutMs);
    return false;
}

// icc_configuration_clear_cpu_info_bit: two "set cpu info bit" messages of the general service, bit (0,0) then (0,1)
bool PS5Power::clearCpuInfoBits()
{
    static const uint8_t bits[2][2] = {{0, 0}, {0, 1}};
    for (auto &b : bits) {
        uint8_t m[ICC_MSG_MIN_SIZE] = {};
        IccHeader *h = (IccHeader *)m;
        h->serviceId = ICC_SERVICE_GENERAL;
        h->msgType = 0x10;
        h->length = ICC_MSG_MIN_SIZE;
        m[sizeof(IccHeader) + 0] = b[1];
        m[sizeof(IccHeader) + 1] = b[0];
        if (!iccQuery(m, 2000)) return false;
    }
    return true;
}

// icc_power_shutdown / icc_power_reboot
bool PS5Power::power(bool reboot)
{
    if (!clearCpuInfoBits()) LOG("clearing the cpu info bits got no reply: sending the power message anyway");
    uint8_t m[ICC_MSG_MIN_SIZE] = {};
    IccHeader *h = (IccHeader *)m;
    h->serviceId = ICC_SERVICE_POWER;
    h->msgType = 1;
    h->length = ICC_MSG_MIN_SIZE;
    uint8_t *d = m + sizeof(IccHeader);
    d[0] = 0;
    d[1] = reboot ? 1 : 0;   // 0 shutdown, 1 reboot
    d[2] = 2;                // depth
    d[3] = 0;                // cause
    d[4] = 1;                // hand
    // d[5..8] = 0
    return iccQuery(m, 5000);
}

void PS5Power::systemWillShutdown(IOOptionBits specifier)
{
    bool reboot = specifier == kIOMessageSystemWillRestart;
    if (bell && icc && (reboot || specifier == kIOMessageSystemWillPowerOff)) {
        LOG("%s: asking the console to %s", reboot ? "restart" : "shut down", reboot ? "reboot" : "power off");
        if (!dryRun) OSWriteLittleInt32((volatile void *)bell, ICC_REG_INTR_MASK, 0);   // polled from here on, as Linux does when it unbinds
        bool sent = power(reboot);
        if (sent && !dryRun) {
            LOG("the console accepted it: waiting for the power to go");
            IOSleep(10000);   // Linux spins forever here; give the controller ten seconds, then let macOS go on as before
            LOG("still running after ten seconds");
        }
    }
    super::systemWillShutdown(specifier);
}
