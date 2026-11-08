// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony Salina Gigabit Ethernet (PCI 104d:9104) for macOS.
 * Register layout, descriptor format and PHY bring-up follow the Linux "mts" driver for the PS5 (ps5-linux-patches), which in turn
 * is based on the MediaTek Star Ethernet MAC. Receive and transmit are polled: the stage that boots macOS leaves MSI off.
 */

// Port of the "mts" Ethernet driver from ps5-linux-patches
// (MODULE_AUTHOR: Armandas Kvietkus), itself based on mtk_star_emac.


#ifndef PS5_SALINA_GBE_HPP
#define PS5_SALINA_GBE_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOBasicOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>

#define GBE_REG_SMI          0x00
#define GBE_REG_LINK         0x04
#define GBE_REG_CLK_CTRL     0x08
#define GBE_REG_CTRL         0x0c
#define GBE_REG_MODE         0x10
#define GBE_REG_MAC_HI       0x14
#define GBE_REG_MAC_LO       0x18
#define GBE_REG_RXBUF        0x30
#define GBE_REG_TX_DMA_CTRL  0x34
#define GBE_REG_RX_DMA_CTRL  0x38
#define GBE_REG_TX_RING_CUR  0x3c
#define GBE_REG_RX_RING_CUR  0x40
#define GBE_REG_TX_RING_BASE 0x44
#define GBE_REG_RX_RING_BASE 0x48
#define GBE_REG_ISR          0x50
#define GBE_REG_IMR          0x54            // interrupt MASK: a set bit masks that source
#define GBE_REG_SERDES       0x74
#define GBE_REG_RX_PCODE     0x78
#define GBE_REG_TXKICK       0x9c
#define GBE_REG_RESET        0xac
#define GBE_REG_COAL         0x204

#define GBE_PCS_BASE         0x4000          // in the glue function's BAR4; offset 0 of this block is the chip revision
#define GBE_GLUE_MAC         0x3000
#define GBE_CLK_ENABLE       0x7597c00
#define GBE_SERDES_1G        0x303277
#define GBE_COAL_VAL         0x10001388
#define GBE_IMR_ALL          0x7fffff        // every source masked: the datapath is polled
#define GBE_ISR_ERRORS       0x7be600
#define GBE_ISR_LSO_RECOVER  0x500000

#define GBE_DMA_STOP         (1u << 1)
#define GBE_DMA_KICK         (1u << 2)

#define GBE_DESC_DONE        (1u << 31)
#define GBE_DESC_WRAP        (1u << 30)
#define GBE_DESC_SOP         (1u << 29)
#define GBE_DESC_EOP         (1u << 28)
#define GBE_DESC_LEN_MASK    0xffffu
#define GBE_DESC_RX_LEN      0x600u
#define GBE_DESC_RX_LEN_MASK 0x7ffu
#define GBE_DESC_W2_ARM      0xffff0000u
#define GBE_DESC_W2_TX       0x4u

#define GBE_RING_SIZE        256u
#define GBE_DESC_RING_BYTES  (GBE_RING_SIZE * 16u)
#define GBE_TX_SCRATCH_SIZE  0xa0000u
#define GBE_RX_RING_SIZE     1024u           // as in the newer Linux driver (the ring end is only marked by GBE_DESC_WRAP)
#define GBE_RX_RING_BYTES    (GBE_RX_RING_SIZE * 16u)
#define GBE_RX_POOL_SIZE     (GBE_RX_RING_SIZE * GBE_DESC_RX_LEN)
#define GBE_MAX_FRAME        1536u
#define GBE_DMA_POOL_PA      0x70000000ULL   // 2 MiB of Linux's RAM reserved in uefi/mkbundle.py (DMA_POOL): the southbridge DMA reaches it
#define GBE_DMA_POOL_SIZE    0x200000ULL

struct GbeDesc {
    uint32_t flags, buf_addr, word2, word3;
};

class PS5SalinaGbE : public IOEthernetController {
    OSDeclareDefaultStructors(PS5SalinaGbE)

public:
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;
    virtual IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;

    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *addr) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setHardwareAddress(const IOEthernetAddress *addr) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) APPLE_KEXT_OVERRIDE;
    virtual UInt32 outputPacket(mbuf_t m, void *param) APPLE_KEXT_OVERRIDE;
    virtual IOOutputQueue *createOutputQueue() APPLE_KEXT_OVERRIDE;
    virtual const OSString *newVendorString() const APPLE_KEXT_OVERRIDE;
    virtual const OSString *newModelString() const APPLE_KEXT_OVERRIDE;
    virtual IOReturn selectMedium(const IONetworkMedium *medium) APPLE_KEXT_OVERRIDE;
    virtual UInt32 getFeatures() const APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

private:
    // register access
    uint32_t rd(uint32_t reg) { return OSReadLittleInt32((volatile void *)regs, reg); }
    void wr(uint32_t reg, uint32_t v) { OSWriteLittleInt32((volatile void *)regs, reg, v); }
    void setBits(uint32_t reg, uint32_t b) { wr(reg, rd(reg) | b); }
    void clrBits(uint32_t reg, uint32_t b) { wr(reg, rd(reg) & ~b); }
    uint32_t pcsRd(uint32_t off) { return OSReadLittleInt32((volatile void *)glueRegs, GBE_PCS_BASE + off); }

    // setup
    uint32_t sfcRead(uint16_t rid, uint32_t reg);
    void sfcWrite(uint16_t rid, uint32_t reg, uint32_t val);
    void setRealCommand(uint16_t bits, bool on);
    void recap();
    void publishStats();
    void probeXhci();
    void cacheBench();
    void allocProbe(uint32_t n);
    static inline void copyFromPool(void *dst, const volatile void *src, size_t len);
    static void probeTimerFired(OSObject *owner, IOTimerEventSource *t);
    void startPoller();
    static void pollThread(void *arg);
    IOMemoryMap *mapPhys(uint64_t phys, uint64_t len, IOMemoryDescriptor **mdOut);
    IOPCIDevice *findGlue();
    bool allocDma(IOBufferMemoryDescriptor **md, size_t size, void **va, uint32_t *pa);
    void deferredInit();
    static void initTimerFired(OSObject *owner, IOTimerEventSource *t);
    bool allocRings();
    void freeRings();
    void dumpRegs();

    // hardware
    void hwReset();
    void hwInit();
    void initTxRing();
    void rxFill();
    void ringsHw();
    void stopDma();
    void writeMac();
    void lsoRecover();
    void linkCheck(bool force);
    void txTimeout();
    void status(bool force);

    // phy (mts_phy.c)
    int smiWait();
    uint16_t cl22Read(uint8_t reg);
    void smiWrite(uint8_t reg, uint16_t val);
    int cl45Addr(uint32_t sel);
    void cl45Write(uint32_t sel, uint16_t val);
    uint16_t cl45Read(uint32_t sel);
    uint8_t calLutIdx(uint32_t field);
    void calLane(uint32_t sel, uint8_t cal, bool high);
    uint16_t eqPreset(uint32_t field);
    void phyCalibrate();
    void smiPaged(uint16_t a, uint16_t b, uint16_t c);
    uint16_t pagedBegin(uint16_t r11, uint16_t r12);
    void pagedFinish(uint16_t r10, uint16_t page);
    void phyStaticInit();
    void phyInit();

    // datapath
    void poll();
    void txComplete();
    int rxPoll(int budget);
    static void pollTimer(OSObject *owner, IOTimerEventSource *t);

    IOPCIDevice *pci = nullptr, *glue = nullptr;
    IOMemoryMap *regMap = nullptr, *glueMap = nullptr, *ecamMap = nullptr;
    IOMemoryDescriptor *regMd = nullptr, *glueMd = nullptr, *ecamMd = nullptr;
    volatile uint8_t *regs = nullptr, *glueRegs = nullptr;
    IOWorkLoop *ownLoop = nullptr;
    IOTimerEventSource *timer = nullptr, *initTimer = nullptr, *probeTimer = nullptr;
    IOPCIDevice *xhci = nullptr;
    IOMemoryDescriptor *xhciMd = nullptr;
    IOMemoryMap *xhciMap = nullptr;
    uint32_t probeRound = 0, probeLast[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint16_t xhciRid = 0;
    IOEthernetInterface *netif = nullptr;
    IOBasicOutputQueue *outQueue = nullptr;
    IOSimpleLock *lock = nullptr;
    OSDictionary *mediumDict = nullptr;
    const IONetworkMedium *autoMedium = nullptr;     // the 1000BaseT-FD entry of mediumDict, reported as the active medium

    IOBufferMemoryDescriptor *highMd = nullptr;     // cacheable pool in Linux's RAM (0x40000000-0x5fffffff), found by allocating past the low range
    bool allocHighPool();
    bool dmaSelfTest();
    uint64_t poolHighPa = 0;
    bool poolPreferHigh = true;
    IOMemoryDescriptor *poolMd = nullptr;
    IOMemoryMap *poolMap = nullptr;
    IOBufferMemoryDescriptor *txRingMd = nullptr, *rxRingMd = nullptr, *txScratchMd = nullptr, *rxPoolMd = nullptr;
    GbeDesc *txRing = nullptr, *rxRing = nullptr;
    uint8_t *txScratch = nullptr, *rxPool = nullptr;
    uint32_t txRingPa = 0, rxRingPa = 0, txScratchPa = 0, rxPoolPa = 0;
    uint32_t txHead = 0, txTail = 0, txScratchOff = 0, rxHead = 0;

    IOEthernetAddress mac;
    uint32_t chipId = 0, chipRev = 0;
    uint16_t nicRid = 0, glueRid = 0;
    uint64_t nicBar0Real = 0, glueBar4Real = 0;
    bool recapDone = false;
    uint32_t recapSecs = 0;
    uint32_t txTimeoutLogs = 0;
    volatile bool running = false;
    bool linkUp = false;
    uint32_t pollTick = 0;
    uint32_t dbgTx = 0, dbgRx = 0, dbgTxDone = 0, errLogs = 0, lsoLogs = 0, smiTimeouts = 0;
    uint32_t isrSeen = 0, txTailSeen = 0, txStuck = 0, txOk = 0, txDrop = 0, txStall = 0, rxOk = 0, rxBad = 0, rxNoMem = 0;
    uint32_t polls = 0, rxBurstMax = 0, rxEmptyKicks = 0, pollUs = 250, initDelayMs = 45000, rxBudget = 256, pollModeUnused = 0, poolCopyMBps = 0, ramCopyMBps = 0;     // pollMode 1: a kernel thread spins (IODelay), 0: timer
    volatile bool threadRunning = false;
    volatile uint32_t pollMode = 0;
    uint64_t txStuckTsc = 0, cAlloc = 0, cCopy = 0, cInput = 0, cFlush = 0, cPoll = 0, cTx = 0, cStatus = 0;     // cycles spent per section, published in Stats (us)
    uint64_t lastPollTsc = 0, nextSecTsc = 0, gapSumTsc = 0, gapMaxTsc = 0, gapCount = 0;
    uint32_t lastTxHead = 0, lastTxTail = 0, lastRxHead = 0, lastLink = 0, lastIsrSeen = 0;
};

#endif
