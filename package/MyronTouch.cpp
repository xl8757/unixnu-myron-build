// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of myron's FocalTech FT3683 touch controller (constants and sources: MyronTouchBoard.h).
// `myron-touch=<n>`:
//   1  read only: GCC votes and branches, the S0 clock levels and the touch pins; panics with them
//   2  as 1, then (only if the QUP4 clocks already run) the GENI engine identity, read only; panics
//   3  votes the QUP4 clocks, muxes the SPI pins, reads the engine identity; panics
//   4  as 3, then avdd on, reset, the SPI bus (SE DMA through a GR0 stream mapping when the FIFO interface is disabled),
//      reads the chip id (0xA3, 0x9F) and firmware (0xA6); panics
//   5  as 4 without the panic: TIC mode (0x9E = 0), then reads a frame whenever the IRQ line is low and publishes the
//      contacts for NativeHidInput (sheng-touch-hid=1), scaled into its 30480 x 20320 axes
//   6  as 5, but panics with the decoded frame of the first touch (orientation check)
//   7  as 5 without publishing; 3 s after the first frame the IRQ line announced (touch and drag meanwhile) panics with
//      the counters, the last idle frame and the last three distinct touch frames, in hex; runs on even when the TIC
//      switch fails, to show what the firmware streams instead
// `myron-touch-hz=<hz>` overrides the SPI rate (100 kHz to 15 MHz, default MT_SPI_HZ). `myron-touch-dma=<m>` picks the
// transfer path: 0 FIFO even when the FIFO interface reads as disabled, 1 (default) SE DMA only then, 2 SE DMA always.
// `myron-touch-sid=<n>` overrides the SMMU stream of the SE DMA (default MT_QUP4_SMMU_SID). `myron-touch-bind=0` leaves the
// driver thread to the scheduler; by default it runs on the boot core only (2026-10-22, eight cores: the poll loop stalled,
// suspected late or lost timer wakeups on the secondary cores).
// iovdd (L4I, 1.8 V) is an RPMh rail XNU cannot switch; stage 4 shows whether it is on (the chip answers) or not.
// Protocol (FocalTech GPL driver, myron's stock focaltech_touch_3683.ko): one full-duplex frame per access, {addr, 0xA0,
// len_hi, len_lo, 3 dummy} for reads with the data from byte 7 and a CRC-16 after it, {addr, 0x00, len_hi, len_lo, 3 dummy,
// data} for writes; status byte 3 must have 0xA0 clear; 150 us chip-select high between frames. After a reset the firmware
// streams raw frames for Xiaomi's host algorithm (THP); 0x9E = 0x00 selects TIC, where the chip reports contacts at 0x01.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "MyronTouchBoard.h"
extern char native_touch_rows[4][120];
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile unsigned native_touch_stage_req;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile uint32_t myron_touch_step, myron_touch_frames, myron_touch_bad, myron_touch_contacts;
volatile int myron_touch_error;
void myron_touch_bind_boot_cpu(void);   // MyronTouchSched.c, built with the kernel's C flags
int myron_touch_cpu(void);
// Liveness of the poll loop, read by the diagnostic page itself (the summary row below stops when the loop does): passes
// and the time of the last one, where the loop is (1 sleeping, 2 reading a frame, 3 switching to TIC), the core it last
// ran on, whether it is bound to the boot core, and its 2 ms sleeps: the last and the longest (us), those over 50 ms.
volatile uint32_t myron_touch_beat, myron_touch_where, myron_touch_on_cpu, myron_touch_bound;
volatile uint32_t myron_touch_nap_last, myron_touch_nap_max, myron_touch_nap_late;
volatile uint64_t myron_touch_beat_at;
}

namespace {
// Plain pointers, so the session stays a trivial type that can be cleared with bzero; the mappings live as long as the driver.
struct Region {IOMemoryMap *map; uintptr_t va;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    region.va = region.map ? static_cast<uintptr_t>(region.map->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void ioDelayUs(void *, unsigned us) {IODelay(us);}
// A pin's CFG register (at the start of its 4 KiB page): read, then the new low bits with the pin claimed (mt_pin_cfg).
// egpio_present is writable (stage 3 of 2026-10-22 cleared it on the SPI pins by writing 0x884), so the egpio-capable
// SPI pins get it forced: present and enable, the state Linux leaves them in.
void setPin(uintptr_t pin, uint32_t value, bool egpio = false) {
    wr32(pin, 0, mt_pin_cfg(rd32(pin, 0) | (egpio ? MT_CFG_EGPIO_PRESENT : 0u), value));
}
uint32_t smmuRd(void *ctx, unsigned off) {return rd32(reinterpret_cast<uintptr_t>(ctx), off);}
void smmuWr(void *ctx, unsigned off, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), off, value);}

// The engine's DMA master moves one frame each way through a cached, physically contiguous buffer (as on sheng: an
// uncached mapping of RAM above 4 GiB panics in phystokv_range), kept coherent by explicit maintenance.
constexpr size_t kDmaBytes = 4096;
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}

struct Session {
    unsigned stage, hz, dmaMode, sid;
    Region gccLow, gccHalt, gccVote, gccS0, tlmmSpi, tlmmReset, tlmmAvdd, tlmmIrq, wrapper, se, smmu;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    struct mt_smmu_plan smmuPlan;
    int smmuResult;
    // 0 vote, 1 gpll0 vote, 2 gpll0 mode, 3-7 CBCR m_ahb s_ahb core core2x s0, 8 RCG cmd, 9 cfg, 10 DFSR,
    // 11-14 SPI pin CFG, 15 reset CFG, 16 reset IO, 17 avdd CFG, 18 avdd IO, 19 irq CFG, 20 irq IO
    uint32_t v[24];
    uint64_t levelHz[MT_DFS_LEVELS];
    struct mt_clock_choice clock;
    struct geni_spi spi;
    int rcA3, rc9F, rcA6, rc9E, chipTries;
    uint8_t idA3, id9F, fwA6, mode9E;
    uint8_t firstRx[12];
    unsigned firstLen, xfers;
    uint8_t tx[128], rx[128], frame[96];
    bool ticOk;
    // Loop counters: frames read because the IRQ line was low, frames read on the ~100 ms timer, firmware restarts,
    // frames by event type (buf[1] >> 4: 0 v1 points, 2 v2 points, anything else), frames with contacts.
    uint32_t irqFrames, timedFrames, restarts, typeV1, typeV2, typeOther, touchFrames;
    uint8_t lastType, idle[MT_FRAME_LEN], touch[3][MT_FRAME_LEN];
    unsigned touchNext, touchKept;
};

void mark(unsigned step) {myron_touch_step = step;}
void fail(int error) {myron_touch_error = error;}

// The poll loop's sleep, measured: a 2 ms sleep that returns much later means the wakeup reached the core late.
void nap(unsigned ms) {
    myron_touch_where = 1;
    uint64_t start = mach_absolute_time(), ns = 0;
    IOSleep(ms);
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    uint32_t us = ns / 1000 > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(ns / 1000);
    myron_touch_nap_last = us;
    if (us > myron_touch_nap_max) myron_touch_nap_max = us;
    if (us > 50000) myron_touch_nap_late = myron_touch_nap_late + 1;
    myron_touch_on_cpu = static_cast<uint32_t>(myron_touch_cpu());
}

bool snapshot(Session &s) {
    mark(1);
    // Each GCC page from the registers it holds: GPLL0 mode; the four QUP4 CBCRs; the vote registers; the S0 CBCR and root.
    if (!mapRegion(mt_gcc_page(MT_GCC_GPLL0_MODE), 0x1000, s.gccLow) || !mapRegion(mt_gcc_page(MT_CBCR_M_AHB), 0x1000, s.gccHalt) ||
        !mapRegion(mt_gcc_page(MT_GCC_VOTE_ENABLE), 0x1000, s.gccVote) || !mapRegion(mt_gcc_page(MT_CBCR_S0), 0x1000, s.gccS0) ||
        !mapRegion(MT_TLMM_BASE + mt_tlmm_cfg(MT_GPIO_SPI_MISO), 4 * MT_GPIO_STRIDE, s.tlmmSpi) ||
        !mapRegion(MT_TLMM_BASE + mt_tlmm_cfg(MT_GPIO_RESET), MT_GPIO_STRIDE, s.tlmmReset) ||
        !mapRegion(MT_TLMM_BASE + mt_tlmm_cfg(MT_GPIO_AVDD), MT_GPIO_STRIDE, s.tlmmAvdd) ||
        !mapRegion(MT_TLMM_BASE + mt_tlmm_cfg(MT_GPIO_IRQ), MT_GPIO_STRIDE, s.tlmmIrq)) {
        fail(-12);
        return false;
    }
    uint32_t *v = s.v;
    v[0] = rd32(s.gccVote.va, MT_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccVote.va, MT_GCC_GPLL0_ENABLE & 0xfff);
    v[2] = rd32(s.gccLow.va, MT_GCC_GPLL0_MODE);
    v[3] = rd32(s.gccHalt.va, MT_CBCR_M_AHB & 0xfff);
    v[4] = rd32(s.gccHalt.va, MT_CBCR_S_AHB & 0xfff);
    v[5] = rd32(s.gccHalt.va, MT_CBCR_CORE & 0xfff);
    v[6] = rd32(s.gccHalt.va, MT_CBCR_CORE_2X & 0xfff);
    v[7] = rd32(s.gccS0.va, MT_CBCR_S0 & 0xfff);
    v[8] = rd32(s.gccS0.va, MT_S0_RCG & 0xfff);
    v[9] = rd32(s.gccS0.va, (MT_S0_RCG + MT_RCG_CFG) & 0xfff);
    v[10] = rd32(s.gccS0.va, (MT_S0_RCG + MT_RCG_DFSR) & 0xfff);
    if (v[10] & 1) {
        for (unsigned l = 0; l < MT_DFS_LEVELS; ++l)
            s.levelHz[l] = mt_dfs_level_hz(rd32(s.gccS0.va, (MT_S0_RCG + MT_RCG_PERF_CFG(l)) & 0xfff),
                                           rd32(s.gccS0.va, (MT_S0_RCG + MT_RCG_PERF_M(l)) & 0xfff),
                                           rd32(s.gccS0.va, (MT_S0_RCG + MT_RCG_PERF_N(l)) & 0xfff));
    } else {   // DFS off: the root's current configuration (M +0x8, N +0xc), selected as level 0
        s.levelHz[0] = mt_dfs_level_hz(v[9], rd32(s.gccS0.va, (MT_S0_RCG + 0x8) & 0xfff), rd32(s.gccS0.va, (MT_S0_RCG + 0xc) & 0xfff));
    }
    for (unsigned i = 0; i < 4; ++i) v[11 + i] = rd32(s.tlmmSpi.va, i * MT_GPIO_STRIDE);
    v[15] = rd32(s.tlmmReset.va, 0); v[16] = rd32(s.tlmmReset.va, 4);
    v[17] = rd32(s.tlmmAvdd.va, 0); v[18] = rd32(s.tlmmAvdd.va, 4);
    v[19] = rd32(s.tlmmIrq.va, 0); v[20] = rd32(s.tlmmIrq.va, 4);
    return true;
}

// Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access.
bool running(uint32_t cbcr) {return !(cbcr & MT_CBCR_CLK_OFF) || (cbcr & 2u);}
bool clocksRunning(Session &s) {
    return running(rd32(s.gccHalt.va, MT_CBCR_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, MT_CBCR_S_AHB & 0xfff)) &&
           !(rd32(s.gccHalt.va, MT_CBCR_CORE & 0xfff) & MT_CBCR_CLK_OFF) &&
           !(rd32(s.gccHalt.va, MT_CBCR_CORE_2X & 0xfff) & MT_CBCR_CLK_OFF) &&
           !(rd32(s.gccS0.va, MT_CBCR_S0 & 0xfff) & MT_CBCR_CLK_OFF);
}

bool readEngine(Session &s) {
    mark(3);   // the next accesses touch the engine: a hang or abort here means the engine is not reachable
    if (!mapRegion(MT_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(MT_SE_BASE, MT_SE_SIZE, s.se)) {fail(-12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    int r = geni_spi_read_info(&s.spi);
    if (r) {fail(r); return false;}
    return true;
}

bool enable(Session &s) {
    mark(2);
    // GPLL0 feeds the S0 levels: vote for it (a shared vote) only if it is not locked, and wait for the lock.
    if (!(rd32(s.gccLow.va, MT_GCC_GPLL0_MODE) & (1u << 31))) {
        wr32(s.gccVote.va, MT_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, MT_GCC_GPLL0_ENABLE & 0xfff) | 1u);
        for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, MT_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    }
    wr32(s.gccVote.va, MT_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, MT_GCC_VOTE_ENABLE & 0xfff) | MT_VOTE_ALL);
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !(ok = clocksRunning(s)); ++i) IODelay(10);
    s.v[0] = rd32(s.gccVote.va, MT_GCC_VOTE_ENABLE & 0xfff);
    s.v[2] = rd32(s.gccLow.va, MT_GCC_GPLL0_MODE);
    s.v[3] = rd32(s.gccHalt.va, MT_CBCR_M_AHB & 0xfff);
    s.v[4] = rd32(s.gccHalt.va, MT_CBCR_S_AHB & 0xfff);
    s.v[5] = rd32(s.gccHalt.va, MT_CBCR_CORE & 0xfff);
    s.v[6] = rd32(s.gccHalt.va, MT_CBCR_CORE_2X & 0xfff);
    s.v[7] = rd32(s.gccS0.va, MT_CBCR_S0 & 0xfff);
    if (!ok) {fail(-110); return false;}   // never touch the engine without running clocks
    setPin(s.tlmmSpi.va + 0 * MT_GPIO_STRIDE, MT_CFG_SPI_DATA, true);
    setPin(s.tlmmSpi.va + 1 * MT_GPIO_STRIDE, MT_CFG_SPI_DATA, true);
    setPin(s.tlmmSpi.va + 2 * MT_GPIO_STRIDE, MT_CFG_SPI_CLK, true);
    setPin(s.tlmmSpi.va + 3 * MT_GPIO_STRIDE, MT_CFG_SPI_CS, true);
    setPin(s.tlmmIrq.va, MT_CFG_INPUT_PULLUP);
    for (unsigned i = 0; i < 4; ++i) s.v[11 + i] = rd32(s.tlmmSpi.va, i * MT_GPIO_STRIDE);
    s.v[19] = rd32(s.tlmmIrq.va, 0);
    return true;
}

// Stock order: reset low, avdd on (iovdd follows ~2.5 ms later on the stock kernel; here it is whatever the boot chain
// left), reset high, 100 ms before the first frame.
void powerOn(Session &s) {
    mark(4);
    wr32(s.tlmmReset.va, 4, 0);                        // level first, then output enable: no glitch high
    setPin(s.tlmmReset.va, MT_CFG_OUTPUT);
    IODelay(2000);
    wr32(s.tlmmAvdd.va, 4, MT_IO_OUT);
    setPin(s.tlmmAvdd.va, MT_CFG_OUTPUT);
    IODelay(3000);
    wr32(s.tlmmReset.va, 4, MT_IO_OUT);
    IOSleep(200);                                      // firmware boot (MeowArch waits 200 ms; nothing answers before)
    s.v[15] = rd32(s.tlmmReset.va, 0); s.v[16] = rd32(s.tlmmReset.va, 4);
    s.v[17] = rd32(s.tlmmAvdd.va, 0); s.v[18] = rd32(s.tlmmAvdd.va, 4);
    s.v[20] = rd32(s.tlmmIrq.va, 4);
}

bool prepareDma(Session &s) {
    mark(41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 2 * kDmaBytes,
        0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(-12); return false;}
    addr64_t segment = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &segment);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || segment < 2 * kDmaBytes) {fail(-14); return false;}
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    mark(42);
    if (!mapRegion(MT_APPS_SMMU_BASE, 0x1000, s.smmu)) {fail(-12); return false;}   // GR0 only
    struct mt_smmu_io io = {reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr};
    s.smmuPlan = mt_smmu_plan_stream(&io, static_cast<uint16_t>(s.sid), MT_UFS_SMMU_SID);
    mark(43);
    s.smmuResult = mt_smmu_route_stream(&io, &s.smmuPlan, static_cast<uint16_t>(s.sid));
    if (s.smmuResult) {fail(-100 - s.smmuResult); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s) {
    mark(5);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;                               // keep the top-level IRQ enable: status stays visible to the poll
    s.spi.allow_fifo_disabled = s.dmaMode == 0;
    if ((s.dmaMode == 2 || (s.dmaMode == 1 && s.spi.info.fifo_disabled)) && !prepareDma(s)) return false;
    mark(5);
    int r = geni_spi_init(&s.spi);
    if (r) {fail(r); return false;}
    if (mt_choose_clock(s.levelHz, MT_DFS_LEVELS, s.spi.oversampling, s.hz, &s.clock)) {fail(-22); return false;}
    r = geni_spi_set_clock(&s.spi, s.hz, static_cast<unsigned>(s.clock.sclk_hz), (s.v[10] & 1) ? s.clock.level : 0);
    if (!r) r = geni_spi_setup(&s.spi, MT_SPI_MODE, 0);
    if (r) {fail(r); return false;}
    return true;
}

int xfer(Session &s, unsigned len) {
    int rc = geni_spi_transfer(&s.spi, s.tx, s.rx, len, 0);
    IODelay(150);                                      // chip select high between frames
    if (!s.xfers++) {
        s.firstLen = len;
        for (unsigned i = 0; i < 12 && i < len; ++i) s.firstRx[i] = rc ? 0 : s.rx[i];
    }
    return rc;
}

int ftsRead(Session &s, uint8_t addr, uint8_t *out, unsigned n) {
    if (n + 9 > sizeof s.tx) return GENI_ERR_PARAM;
    int rc = GENI_ERR_IO;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        memset(s.tx, 0, n + 9);
        s.tx[0] = addr; s.tx[1] = 0xa0; s.tx[2] = static_cast<uint8_t>(n >> 8); s.tx[3] = static_cast<uint8_t>(n);
        rc = xfer(s, n + 9);
        if (rc) continue;
        if (s.rx[3] & 0xa0) {rc = -200 - s.rx[3]; continue;}
        if (mt_fts_crc(s.rx + 7, n) != (s.rx[7 + n] | (s.rx[8 + n] << 8))) {rc = -2; continue;}
        memcpy(out, s.rx + 7, n);
        return 0;
    }
    return rc;
}

int ftsWrite(Session &s, uint8_t addr, const uint8_t *data, unsigned n) {
    unsigned len = n ? 7 + n : 4;
    if (len > sizeof s.tx) return GENI_ERR_PARAM;
    int rc = GENI_ERR_IO;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        memset(s.tx, 0, len);
        s.tx[0] = addr; s.tx[1] = 0x00; s.tx[2] = static_cast<uint8_t>(n >> 8); s.tx[3] = static_cast<uint8_t>(n);
        if (n) memcpy(s.tx + 7, data, n);
        rc = xfer(s, len);
        if (rc) continue;
        if (s.rx[3] & 0xa0) {rc = -200 - s.rx[3]; continue;}
        return 0;
    }
    return rc;
}

bool identify(Session &s) {
    mark(6);
    for (s.chipTries = 1; s.chipTries <= 5; ++s.chipTries) {
        s.rcA3 = ftsRead(s, 0xa3, &s.idA3, 1);
        s.rc9F = ftsRead(s, 0x9f, &s.id9F, 1);
        if (!s.rcA3 && !s.rc9F && s.idA3 == 0x36 && s.id9F == 0x83) break;
        IOSleep(200);
    }
    s.rcA6 = ftsRead(s, 0xa6, &s.fwA6, 1);
    return !s.rcA3 && !s.rc9F && s.idA3 == 0x36 && s.id9F == 0x83;
}

bool ticMode(Session &s) {
    mark(7);
    const uint8_t tic = 0x00;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        s.rc9E = ftsWrite(s, 0x9e, &tic, 1);
        IODelay(1000);
        if (!s.rc9E) s.rc9E = ftsRead(s, 0x9e, &s.mode9E, 1);
        if (!s.rc9E && s.mode9E == tic) return true;
    }
    return false;
}

unsigned kHz(const Session &s, unsigned level) {return static_cast<unsigned>(s.levelHz[level] / 1000);}

[[noreturn]] void report(Session &s, const char *what) {
    static char text[1600];
    const uint32_t *v = s.v;
    int n = snprintf(text, sizeof text,
        // The diagnostic page paints its magenta band over the first lines of the panic text: start below it.
        "\n\n\n\n\n\nMYRON touch stage %u: %s (step %u error %d)\n"
        "GCC vote %08x gpll0 vote %08x mode %08x\n"
        "CBCR m_ahb %08x s_ahb %08x core %08x 2x %08x s0 %08x\n"
        "S0 rcg %08x cfg %08x dfsr %08x levels kHz %u %u %u %u %u %u %u %u\n"
        "pins spi %08x %08x %08x %08x reset %08x/%08x avdd %08x/%08x irq %08x/%08x\n",
        s.stage, what, myron_touch_step, myron_touch_error, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10],
        kHz(s, 0), kHz(s, 1), kHz(s, 2), kHz(s, 3), kHz(s, 4), kHz(s, 5), kHz(s, 6), kHz(s, 7),
        v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20]);
    if (n > 0 && s.spi.se.rd && static_cast<unsigned>(n) < sizeof text)
        n += snprintf(text + n, sizeof text - n,
            "GENI fw %08x proto %u ifdis %08x hwp0 %08x status %08x qup %08x depth %u width %u os %u\n"
            "clock level %u div %u sclk %llu spi %llu (asked %u)\n",
            s.spi.info.fw_revision, s.spi.info.protocol, s.spi.info.if_disable, s.spi.info.hw_param0, s.spi.info.geni_status,
            s.spi.info.qup_hw_version, s.spi.info.tx_fifo_depth, s.spi.info.fifo_width_bits, s.spi.oversampling,
            s.clock.level, s.clock.div, s.clock.sclk_hz, s.clock.actual_hz, s.hz);
    if (n > 0 && s.dmaMem && static_cast<unsigned>(n) < sizeof text)
        n += snprintf(text + n, sizeof text - n,
            "DMA phys %llx sid %x smmu groups %u like %u s2cr %08x slot %u reuse %d plan %d route %d\n",
            s.dmaPhys, s.sid, s.smmuPlan.groups, s.smmuPlan.like_slot, s.smmuPlan.s2cr, s.smmuPlan.slot, s.smmuPlan.reuse,
            s.smmuPlan.error, s.smmuResult);
    if (n > 0 && s.xfers && static_cast<unsigned>(n) < sizeof text)
        n += snprintf(text + n, sizeof text - n,
            "chip tries %d A3 %02x rc %d 9F %02x rc %d fw A6 %02x rc %d 9E %02x rc %d xfers %u\n"
            "first rx (%u): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "snap valid %d irq %08x st %08x txfs %08x rxfs %08x dma tx %08x rx %08x in %08x/%08x\n",
            s.chipTries, s.idA3, s.rcA3, s.id9F, s.rc9F, s.fwA6, s.rcA6, s.mode9E, s.rc9E, s.xfers, s.firstLen,
            s.firstRx[0], s.firstRx[1], s.firstRx[2], s.firstRx[3], s.firstRx[4], s.firstRx[5], s.firstRx[6], s.firstRx[7],
            s.firstRx[8], s.firstRx[9], s.firstRx[10], s.firstRx[11], s.spi.snap_valid, s.spi.snap_irq, s.spi.snap_status,
            s.spi.snap_txfs, s.spi.snap_rxfs, s.spi.snap_dma_tx, s.spi.snap_dma_rx, s.spi.snap_dma_txin, s.spi.snap_dma_rxin);
    panic("%s", text);
    __builtin_unreachable();
}

// Whoever consumes the finger set registers a function here (NativeHidInput since upstream 588c96f, which dropped its 8 ms
// seqlock poll); it is called from the touch thread after the seqlock closes, as sheng's TouchProbe.cpp does.
void (*gTouchSink)(void *);
void *gTouchSinkContext;
void publish(unsigned count, const int *ids, const int *xs, const int *ys) {
    native_touch_seq = native_touch_seq + 1;           // odd: being written
    __asm__ volatile("dmb ish" ::: "memory");
    for (unsigned i = 0; i < count; ++i) {native_touch_ids[i] = ids[i]; native_touch_xs[i] = xs[i]; native_touch_ys[i] = ys[i];}
    native_touch_count = count;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    myron_touch_contacts = count;
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}

// Stage 7: the counters, the last frame read on the timer (idle) and the last three distinct frames read because the IRQ
// line was low (a touch), 84 bytes each in four rows of 21. Printed by itself: the register lines of report() would push
// the frames off the panel.
void hexFrame(char *text, size_t size, int &n, const char *title, const uint8_t *frame) {
    if (n > 0 && static_cast<size_t>(n) < size) n += snprintf(text + n, size - n, "%s\n", title);
    for (unsigned i = 0; i < MT_FRAME_LEN && n > 0 && static_cast<size_t>(n) < size; ++i)
        n += snprintf(text + n, size - n, "%02x%s", frame[i], i % 21 == 20 ? "\n" : " ");
}

[[noreturn]] void dump(Session &s) {
    static char text[1800];
    int n = snprintf(text, sizeof text,
        "\n\n\n\n\n\nMYRON touch stage 7: 3 s after the first touch frame\n"
        "tic %d 9E %02x rc %d frames %u bad %u irq %u timed %u restarts %u types v1 %u v2 %u other %u touch %u\n",
        s.ticOk, s.mode9E, s.rc9E, myron_touch_frames, myron_touch_bad, s.irqFrames, s.timedFrames, s.restarts, s.typeV1,
        s.typeV2, s.typeOther, s.touchFrames);
    hexFrame(text, sizeof text, n, "idle (timer) frame:", s.idle);
    for (unsigned k = 0; k < 3; ++k) {
        unsigned slot = (s.touchNext + k) % 3;   // oldest first
        if (k < 3 - s.touchKept) continue;
        hexFrame(text, sizeof text, n, k == 2 ? "touch frame (newest):" : "touch frame:", s.touch[slot]);
    }
    panic("%s", text);
    __builtin_unreachable();
}

// Row 0 is the one line of the diagnostic page given to touch, so it carries what tells the failures apart.
void rows(Session &s) {
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
             "TOUCH s%u st%u e%d 9E=%02x/%d tic%d fr%u bad%u irq%u tim%u rst%u t%x v2:%u ct%u hdr %02x %02x %02x %02x",
             s.stage, myron_touch_step, myron_touch_error, s.mode9E, s.rc9E, s.ticOk, myron_touch_frames, myron_touch_bad,
             s.irqFrames, s.timedFrames, s.restarts, s.lastType, s.typeV2, s.touchFrames, s.frame[0], s.frame[1], s.frame[2],
             s.frame[3]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1], "id %02x %02x fw %02x mode %02x dma %d head %02x %02x %02x %02x",
             s.idA3, s.id9F, s.fwA6, s.mode9E, s.spi.use_dma, s.frame[0], s.frame[1], s.frame[2], s.frame[3]);
}

void run(Session &s) {
    if (!snapshot(s)) report(s, "map failed");
    if (s.stage == 1) report(s, "snapshot");
    if (s.stage == 2) {
        if (clocksRunning(s) && readEngine(s)) report(s, "engine identity (clocks were running)");
        report(s, "clocks not running: engine not read");
    }
    if (!enable(s)) report(s, "clocks did not start");
    if (!readEngine(s)) report(s, "engine not readable");
    if (s.stage == 3) report(s, "clocks voted, pins muxed");
    powerOn(s);
    if (!openBus(s)) report(s, "SPI bus not opened");
    bool found = identify(s);
    if (s.stage == 4) report(s, found ? "chip found" : "chip not found");
    if (!found) {rows(s); return;}
    s.ticOk = ticMode(s);
    rows(s);
    if (!s.ticOk && s.stage != 7) return;
    int ids[10], xs[10], ys[10];
    unsigned idle = 0, lastCount = 0;
    uint64_t deadline = 0, window = 0, second = 0, rowsAt = 0;
    nanoseconds_to_absolutetime(3000000000ULL, &window);
    nanoseconds_to_absolutetime(1000000000ULL, &second);
    for (;;) {
        uint64_t now = mach_absolute_time();
        myron_touch_beat = myron_touch_beat + 1;
        myron_touch_beat_at = now;
        if (s.stage == 7 && deadline && now > deadline) dump(s);
        if (now - rowsAt > second) {rows(s); rowsAt = now;}
        bool ready = !(rd32(s.tlmmIrq.va, 4) & MT_IO_IN);
        if (!ready && ++idle < 50) {nap(2); continue;}   // the line stays low until the frame is read; else ~100 ms
        idle = 0;
        myron_touch_where = 2;
        if (ftsRead(s, 0x01, s.frame, MT_FRAME_LEN)) {myron_touch_bad = myron_touch_bad + 1; nap(2); continue;}
        myron_touch_frames = myron_touch_frames + 1;
        if (ready) s.irqFrames = s.irqFrames + 1; else s.timedFrames = s.timedFrames + 1;
        s.lastType = s.frame[1] >> 4;
        if (mt_frame_restarted(s.frame)) {             // firmware restarted, back in THP mode
            s.restarts = s.restarts + 1;
            if (lastCount) publish(0, ids, xs, ys);
            lastCount = 0;
            myron_touch_where = 3;
            s.ticOk = ticMode(s);
            rows(s);
            continue;
        }
        if (s.lastType == 0) s.typeV1 = s.typeV1 + 1;
        else if (s.lastType == 2) s.typeV2 = s.typeV2 + 1;
        else s.typeOther = s.typeOther + 1;
        if (!ready) memcpy(s.idle, s.frame, MT_FRAME_LEN);
        else if (!s.touchKept || memcmp(s.touch[(s.touchNext + 2) % 3], s.frame, MT_FRAME_LEN)) {
            memcpy(s.touch[s.touchNext], s.frame, MT_FRAME_LEN);
            s.touchNext = (s.touchNext + 1) % 3;
            if (s.touchKept < 3) ++s.touchKept;
            if (!deadline) deadline = mach_absolute_time() + window;
        }
        unsigned count = mt_decode(s.frame, ids, xs, ys);
        if (count) s.touchFrames = s.touchFrames + 1;
        if (s.stage == 7) continue;
        if (s.stage == 6 && count) {
            s.firstLen = MT_FRAME_LEN;
            for (unsigned i = 0; i < 12; ++i) s.firstRx[i] = s.frame[i];
            static char text[160];
            snprintf(text, sizeof text, "first touch: %u contacts, id %d at %d,%d (of 30480 x 20320)", count, ids[0], xs[0], ys[0]);
            report(s, text);
        }
        if (count || lastCount) publish(count, ids, xs, ys);
        lastCount = count;
    }
}

void touchThread(void *argument, wait_result_t) {
    unsigned bind = 1;
    PE_parse_boot_argn("myron-touch-bind", &bind, sizeof bind);
    if (bind) {myron_touch_bind_boot_cpu(); myron_touch_bound = 1;}
    myron_touch_on_cpu = static_cast<uint32_t>(myron_touch_cpu());
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(-12); return;}
    bzero(s, sizeof *s);
    s->stage = static_cast<unsigned>(reinterpret_cast<uintptr_t>(argument));
    s->hz = MT_SPI_HZ;
    PE_parse_boot_argn("myron-touch-hz", &s->hz, sizeof s->hz);
    if (s->hz < 100000 || s->hz > 15000000) s->hz = MT_SPI_HZ;
    s->dmaMode = 1;
    PE_parse_boot_argn("myron-touch-dma", &s->dmaMode, sizeof s->dmaMode);
    if (s->dmaMode > 2) s->dmaMode = 1;
    s->sid = MT_QUP4_SMMU_SID;
    PE_parse_boot_argn("myron-touch-sid", &s->sid, sizeof s->sid);
    if (s->sid > 0xffff) s->sid = MT_QUP4_SMMU_SID;
    run(*s);
}
}  // namespace

extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    gTouchSinkContext = context;
    __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

extern "C" void myron_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("myron-touch", &stage, sizeof stage) || stage < 1 || stage > 7 || native_touch_stage_req) return;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, reinterpret_cast<void *>(static_cast<uintptr_t>(stage)), &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else fail(-12);
}
