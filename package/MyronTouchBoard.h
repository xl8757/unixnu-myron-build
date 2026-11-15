// SPDX-License-Identifier: BSD-2-Clause-Patent
// Touch of myron (Redmi K90 Pro Max, SM8850): FocalTech FT3683 on QUP4 SE0. Sources: the stock Android DT
// (myron-work/dtbo0.dts focaltech_ts@0 and touch_avdd_vreg, base2.dts spi@1a80000 / qupv3_4), the stock UEFI
// configuration DTB in xbl_config.elf (TOP_QUP_4_SE_0, pinctrl@f100000) and the GCC offsets of Linux
// drivers/clk/qcom/gcc-kaanapali.c. The stock ClockDxe tables hold the same branches 0x10000 lower: they are relative to
// its own base (GCC + 0x10000), and reading GCC + 0x42018 (their vote register taken as a GCC offset) external-aborts.
#ifndef MYRON_TOUCH_BOARD_H
#define MYRON_TOUCH_BOARD_H
#include <stdint.h>

#define MT_QUP_WRAPPER_BASE 0x01ac0000ULL     // qupv3_4
#define MT_SE_BASE 0x01a80000ULL              // spi@1a80000 = qupv3_se19_spi = QUP4 SE0
#define MT_SE_SIZE 0x4000ULL

#define MT_GCC_BASE 0x00100000ULL
#define MT_GCC_GPLL0_MODE 0x0                 // bit 31 lock
#define MT_GCC_VOTE_ENABLE 0x52018            // APCS vote register of the QUP4 branches below
#define MT_GCC_GPLL0_ENABLE 0x52020           // gcc_gpll0 vote, bit 0
#define MT_VOTE_WRAP4_M_AHB (1u<<22)          // gcc_qupv3_wrap_4_m_ahb_clk, CBCR 0x23548
#define MT_VOTE_WRAP4_S_AHB (1u<<23)          // gcc_qupv3_wrap_4_s_ahb_clk, CBCR 0x2354c
#define MT_VOTE_WRAP4_CORE (1u<<24)           // gcc_qupv3_wrap4_core_clk, CBCR 0x23550
#define MT_VOTE_WRAP4_CORE_2X (1u<<25)        // gcc_qupv3_wrap4_core_2x_clk, CBCR 0x23564
#define MT_VOTE_WRAP4_S0 (1u<<26)             // gcc_qupv3_wrap4_s0_clk, CBCR 0xa9004
#define MT_VOTE_ALL (MT_VOTE_WRAP4_M_AHB|MT_VOTE_WRAP4_S_AHB|MT_VOTE_WRAP4_CORE|MT_VOTE_WRAP4_CORE_2X|MT_VOTE_WRAP4_S0)
#define MT_CBCR_M_AHB 0x23548
#define MT_CBCR_S_AHB 0x2354c
#define MT_CBCR_CORE 0x23550
#define MT_CBCR_CORE_2X 0x23564
#define MT_CBCR_S0 0xa9004
#define MT_CBCR_CLK_OFF (1u<<31)
// The S0 clock root (gcc_qupv3_wrap4_s0_clk_src, RCG with DFS), same layout as sheng's S4 root: CFG +0x4, DFSR +0x14
// (bit 0 DFS on, 4:1 level), per level CFG +0x1c, M +0x5c, N +0x9c.
#define MT_S0_RCG 0xa9014
#define MT_RCG_CFG 0x4
#define MT_RCG_DFSR 0x14
#define MT_RCG_PERF_CFG(l) (0x1c+4*(l))
#define MT_RCG_PERF_M(l) (0x5c+4*(l))
#define MT_RCG_PERF_N(l) (0x9c+4*(l))
#define MT_DFS_LEVELS 8

#define MT_TLMM_BASE 0x0f100000ULL            // pin registers: 4 KiB per pin, CFG +0, IN_OUT +4
#define MT_GPIO_STRIDE 0x1000
#define MT_GPIO_SPI_MISO 48                   // qup4_se0_l0..l3 = GPIO 48..51, function 1
#define MT_GPIO_RESET 66                      // focaltech,reset-gpio
#define MT_GPIO_AVDD 117                      // touch_avdd_vreg: fixed regulator, enable active high
#define MT_GPIO_IRQ 158                       // focaltech,irq-gpio, active low (stays low until the frame is read)
#define MT_FUNC_QUP4_SE0 1
#define MT_CFG_SPI_DATA 0x84u                 // function 1, 6 mA, no pull (MISO, MOSI)
#define MT_CFG_SPI_CLK 0x44u                  // function 1, 4 mA
#define MT_CFG_SPI_CS 0x87u                   // function 1, 6 mA, pull-up
#define MT_CFG_INPUT_PULLUP 0x3u              // GPIO input, pull-up
#define MT_CFG_OUTPUT 0x240u                  // GPIO output (OE), 4 mA, no pull
#define MT_IO_OUT (1u<<1)
#define MT_IO_IN (1u<<0)
// Pins that another subsystem can take over (GPIO 48-51: function 11 "egpio" in pinctrl-kaanapali.c) report
// egpio_present; the TLMM function only reaches the pin with egpio_enable set, as Linux sets it when it selects any other
// function. Stock state of the SPI pins: 0x801 (present, not enabled).
#define MT_CFG_EGPIO_PRESENT (1u<<11)
#define MT_CFG_EGPIO_ENABLE (1u<<12)
#define MT_CFG_LOW_MASK 0x3ffu                // pull, function, drive, output enable
// A new configuration for a pin whose current register reads `old`: the low bits replaced, the bits above kept, the pin
// claimed when it is egpio capable.
static inline uint32_t mt_pin_cfg(uint32_t old, uint32_t value) {
    uint32_t cfg=(old&~(MT_CFG_LOW_MASK|MT_CFG_EGPIO_ENABLE))|(value&MT_CFG_LOW_MASK);
    return (old&MT_CFG_EGPIO_PRESENT)?cfg|MT_CFG_EGPIO_ENABLE:cfg;
}

#define MT_SPI_MODE 0                         // focaltech_ts@0: no spi-cpha/spi-cpol
#define MT_SPI_HZ 9600000u                    // below the DT's 15 MHz for the first runs
#define MT_PANEL_W 1200                       // focaltech,max-x / max-y + 1; points come in 1/16 pixel
#define MT_PANEL_H 2608

static inline uint32_t mt_tlmm_cfg(unsigned gpio) {return (uint32_t)gpio*MT_GPIO_STRIDE;}
static inline uint32_t mt_tlmm_io(unsigned gpio) {return (uint32_t)gpio*MT_GPIO_STRIDE+4;}
// The 4 KiB GCC page holding a register, and the register's offset in it.
static inline uint64_t mt_gcc_page(uint32_t reg) {return MT_GCC_BASE+(reg&~0xfffu);}
static inline uint32_t mt_gcc_off(uint32_t reg) {return reg&0xfffu;}

// DFS level rates, as sheng's TouchBoard.h: source by select value (0 TCXO 19.2 MHz, 1 GPLL0 main 600 MHz, 6 GPLL0
// even 300 MHz), half-integer divider, then M/N (N stored inverted, mnd width 16).
static inline uint64_t mt_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}
static inline uint64_t mt_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint64_t rate=mt_src_hz(src);
    if (hid) {rate*=2;rate/=hid+1;}
    if (mode) {uint32_t m=mreg&0xffff,n=((~nreg)&0xffff)+m;if (n) {rate*=m;rate/=n;}}
    return rate;
}
struct mt_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
// The level giving the highest SPI rate not above target, with div = ceil(sclk / (os * target)). 0 on success.
static inline int mt_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct mt_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=(f+(uint64_t)os*target-1)/((uint64_t)os*target);if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}

// FocalTech SPI frame CRC: CRC-16/MCRF4XX (reflected 0x8408, init 0xFFFF), stored little endian after the data.
static inline uint16_t mt_fts_crc(const uint8_t *p, unsigned n) {
    uint16_t c=0xffff;
    while (n--) {c^=*p++;for (unsigned b=0;b<8;++b) c=(c&1)?(uint16_t)((c>>1)^0x8408):(uint16_t)(c>>1);}
    return c;
}

// Touch frame at 0x01 (84 bytes): buf[1] low nibble the contact count, high nibble the layout: 2 = v2 points (8 bytes each
// from buf+4, X/Y in 1/16 pixel), 0 = v1 points (6 bytes each from buf+2, whole pixels). In both, event in p0 bits 7:6
// (0 down, 1 up, 2 contact) and id in p2 bits 7:4. Validation as Xiaomi's fts_read_parse_touchdata: v1 walks the slots
// until an id of 10 or more and drops the whole frame when a slot is down while the count is 0 ("abnormal touch data from
// fw"; an all-zero frame reads exactly like that); v2 takes count slots, 1..10, each with an id below 10. Down and contact
// slots are returned in NativeHidInput's axes (0..30479 across the panel, 0..20319 down it); 0 for no or dropped contacts.
#define MT_FRAME_LEN 84
#define MT_HID_X 30480
#define MT_HID_Y 20320
static inline unsigned mt_decode(const uint8_t *buf, int *ids, int *xs, int *ys) {
    unsigned type=buf[1]>>4,reported=buf[1]&0xf,count=0;
    if (reported>10) return 0;
    if (type==2) {
        if (!reported) return 0;
        for (unsigned i=0;i<reported;++i) {
            const uint8_t *p=buf+4+8*i;
            if ((p[2]>>4)>=10) return 0;
        }
        for (unsigned i=0;i<reported;++i) {
            const uint8_t *p=buf+4+8*i;
            unsigned event=p[0]>>6;
            if (event!=0 && event!=2) continue;
            unsigned x=(p[0]&0xfu)<<12|(unsigned)p[1]<<4|p[4]>>4,y=(p[2]&0xfu)<<12|(unsigned)p[3]<<4|(p[4]&0xfu);
            ids[count]=p[2]>>4;
            xs[count]=(int)((uint64_t)x*MT_HID_X/(MT_PANEL_W*16));
            ys[count]=(int)((uint64_t)y*MT_HID_Y/(MT_PANEL_H*16));
            ++count;
        }
    } else if (type==0) {
        for (unsigned i=0;i<10;++i) {
            const uint8_t *p=buf+2+6*i;
            unsigned id=p[2]>>4,event=p[0]>>6;
            if (id>=10) break;
            if (event!=0 && event!=2) continue;
            if (!reported) return 0;
            unsigned x=(p[0]&0xfu)<<8|p[1],y=(p[2]&0xfu)<<8|p[3];
            ids[count]=(int)id;
            xs[count]=(int)((uint64_t)x*MT_HID_X/MT_PANEL_W);
            ys[count]=(int)((uint64_t)y*MT_HID_Y/MT_PANEL_H);
            ++count;
        }
    }
    return count;
}
// An all-ones header: the firmware restarted (and is back in its default THP mode), as MeowArch's zorn driver checks it.
static inline int mt_frame_restarted(const uint8_t *buf) {return buf[0]==0xff && buf[1]==0xff;}

// SE DMA (TZ sets QUP4 SE0 up for GSI with the FIFO interface disabled) reaches memory through the apps SMMU, where an
// unmatched stream faults (sCR0 0x00250406: USFCFG). The firmware owns the context banks (GR1 aborts from EL1), so the QUP
// stream gets a stream-match group of its own in GR0 that routes it exactly as the UFS stream is routed (its S2CR copied),
// whose context the UFS driver already uses for DMA to physical memory. Only GR0 is read or written.
#define MT_APPS_SMMU_BASE 0x15000000ULL
#define MT_QUP4_SMMU_SID 0x523                // qupv3_4 iommus (SE DMA; GPI is 0x536)
#define MT_UFS_SMMU_SID 0x60
struct mt_smmu_io {void *ctx;uint32_t (*rd)(void *ctx,unsigned off);void (*wr)(void *ctx,unsigned off,uint32_t value);};
struct mt_smmu_plan {unsigned slot,like_slot,groups;uint32_t s2cr;int reuse,error;};
enum {MT_SMMU_NO_TEMPLATE=1,MT_SMMU_CONFLICT,MT_SMMU_FULL,MT_SMMU_S2CR_READBACK,MT_SMMU_SMR_READBACK};
static inline int mt_smr_matches(uint32_t smr,uint16_t stream) {return (smr>>31) && (((smr^stream)&0xffffu&~((smr>>16)&0x7fffu))==0);}
static inline struct mt_smmu_plan mt_smmu_plan_stream(const struct mt_smmu_io *io,uint16_t stream,uint16_t like) {
    struct mt_smmu_plan p={0xffffffffu,0xffffffffu,0,0,0,0};
    unsigned count=io->rd(io->ctx,0x20)&255,free_slot=0xffffffffu,matches=0;
    if (count>128) count=128;
    p.groups=count;
    for (unsigned i=0;i<count;++i) {
        uint32_t smr=io->rd(io->ctx,0x800+4*i);
        if (!(smr>>31)) {if (free_slot==0xffffffffu) free_slot=i;continue;}
        if (p.like_slot==0xffffffffu && mt_smr_matches(smr,like)) {p.like_slot=i;p.s2cr=io->rd(io->ctx,0xc00+4*i);}
        if (mt_smr_matches(smr,stream)) {
            ++matches;
            p.slot=i;
            p.reuse=(smr==(0x80000000u|stream));
        }
    }
    if (p.like_slot==0xffffffffu) {p.error=MT_SMMU_NO_TEMPLATE;return p;}
    if (matches>1 || (matches && (!p.reuse || io->rd(io->ctx,0xc00+4*p.slot)!=p.s2cr))) {p.error=MT_SMMU_CONFLICT;p.reuse=0;return p;}
    if (!matches) {p.slot=free_slot;if (p.slot==0xffffffffu) p.error=MT_SMMU_FULL;}
    return p;
}
// S2CR before SMR: the group never matches with a stale routing.
static inline int mt_smmu_route_stream(const struct mt_smmu_io *io,const struct mt_smmu_plan *p,uint16_t stream) {
    if (p->error) return p->error;
    if (p->reuse) return 0;
    io->wr(io->ctx,0xc00+4*p->slot,p->s2cr);
    if (io->rd(io->ctx,0xc00+4*p->slot)!=p->s2cr) return MT_SMMU_S2CR_READBACK;
    io->wr(io->ctx,0x800+4*p->slot,0x80000000u|stream);
    if (io->rd(io->ctx,0x800+4*p->slot)!=(0x80000000u|stream)) return MT_SMMU_SMR_READBACK;
    return 0;
}
#endif
