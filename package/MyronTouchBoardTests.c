// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of MyronTouchBoard.h: cc -std=c11 -Wall -Wextra -Werror MyronTouchBoardTests.c -o t && ./t
#include <stdio.h>
#include <string.h>
#include "MyronTouchBoard.h"

static int failures;
#define CHECK(c) do {if (!(c)) {printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures;}} while (0)

struct fake_smmu {uint32_t regs[0x1000 / 4]; unsigned writes; int stuck;};
static uint32_t fake_rd(void *ctx, unsigned off) {return ((struct fake_smmu *)ctx)->regs[off / 4];}
static void fake_wr(void *ctx, unsigned off, uint32_t value) {
    struct fake_smmu *f = ctx;
    ++f->writes;
    if (!f->stuck) f->regs[off / 4] = value;
}
static struct fake_smmu myron_smmu(void) {   // as read on myron: 116 groups, UFS in group 0, the display in another
    struct fake_smmu f;
    memset(&f, 0, sizeof f);
    f.regs[0x20 / 4] = 0x4c017e74;
    f.regs[0x800 / 4] = 0x80000060; f.regs[0xc00 / 4] = 0x00000000;
    f.regs[0x804 / 4] = 0x84000800; f.regs[0xc04 / 4] = 0x00000001;
    return f;
}

// The driver maps one GCC page per register group; every register of a group must sit in that page.
static void test_gcc_pages(void) {
    CHECK(mt_gcc_page(MT_CBCR_S_AHB) == mt_gcc_page(MT_CBCR_M_AHB) && mt_gcc_page(MT_CBCR_CORE) == mt_gcc_page(MT_CBCR_M_AHB));
    CHECK(mt_gcc_page(MT_CBCR_CORE_2X) == mt_gcc_page(MT_CBCR_M_AHB));
    CHECK(mt_gcc_page(MT_GCC_GPLL0_ENABLE) == mt_gcc_page(MT_GCC_VOTE_ENABLE));
    CHECK(mt_gcc_page(MT_S0_RCG) == mt_gcc_page(MT_CBCR_S0) && mt_gcc_page(MT_S0_RCG + MT_RCG_PERF_N(MT_DFS_LEVELS - 1)) == mt_gcc_page(MT_CBCR_S0));
    CHECK(mt_gcc_page(MT_S0_RCG + 0xc) == mt_gcc_page(MT_CBCR_S0) && mt_gcc_page(MT_S0_RCG + MT_RCG_DFSR) == mt_gcc_page(MT_CBCR_S0));
    CHECK(MT_GCC_BASE + MT_GCC_VOTE_ENABLE == 0x152018 && MT_GCC_BASE + MT_CBCR_S0 == 0x1a9004);   // gcc-kaanapali.c
    CHECK(mt_gcc_page(MT_CBCR_M_AHB) + mt_gcc_off(MT_CBCR_M_AHB) == MT_GCC_BASE + MT_CBCR_M_AHB);
}

static void test_pins(void) {
    CHECK(mt_pin_cfg(0x801, MT_CFG_SPI_DATA) == 0x1884);   // stock SPI pin: claimed, function 1, 6 mA
    CHECK(mt_pin_cfg(0x801, MT_CFG_SPI_CS) == 0x1887);
    CHECK(mt_pin_cfg(0x1884, MT_CFG_SPI_CLK) == 0x1844);   // already claimed: stays claimed
    CHECK(mt_pin_cfg(0x001, MT_CFG_INPUT_PULLUP) == 0x003); // plain pin (IRQ 158): no egpio bits appear
    CHECK(mt_pin_cfg(0x200, MT_CFG_OUTPUT) == 0x240);       // reset/avdd as TouchPowerDxe left them
    CHECK(mt_pin_cfg(0xffffe000u | 0x1801, 0) == (0xffffe000u | 0x1800));   // unknown upper bits kept
    CHECK(mt_pin_cfg(0x884 | MT_CFG_EGPIO_PRESENT, MT_CFG_SPI_DATA) == 0x1884);   // after stage 3 cleared bit 11: forced back
}

static void test_crc(void) {
    CHECK(mt_fts_crc((const uint8_t *)"123456789", 9) == 0x6f91);   // CRC-16/MCRF4XX check value
    CHECK(mt_fts_crc(NULL, 0) == 0xffff);
}

static void test_clock(void) {
    CHECK(mt_dfs_level_hz(0x010b, 0, 0) == 100000000);    // GPLL0 main / 6
    CHECK(mt_dfs_level_hz(0x0000, 0, 0) == 19200000);     // TCXO, no divider
    CHECK(mt_dfs_level_hz(0x0600, 0, 0) == 300000000);    // GPLL0 even
    CHECK(mt_dfs_level_hz(0x2101, 1, (uint32_t)~(4u - 1u)) == 150000000);   // 600 MHz * 1/4 (N stored as ~(n - m))
    uint64_t levels[MT_DFS_LEVELS] = {19200000, 100000000, 0};
    struct mt_clock_choice c = {0, 0, 0, 0};
    CHECK(mt_choose_clock(levels, MT_DFS_LEVELS, 1, MT_SPI_HZ, &c) == 0);
    CHECK(c.level == 0 && c.div == 2 && c.actual_hz == 9600000);
    CHECK(mt_choose_clock(levels, MT_DFS_LEVELS, 2, MT_SPI_HZ, &c) == 0);
    CHECK(c.actual_hz <= MT_SPI_HZ && c.actual_hz == 9600000 && c.level == 0 && c.div == 1);
    uint64_t none[MT_DFS_LEVELS] = {0};
    CHECK(mt_choose_clock(none, MT_DFS_LEVELS, 1, MT_SPI_HZ, &c) == -1);
}

static void put_v2(uint8_t *buf, unsigned i, unsigned event, unsigned id, unsigned x16, unsigned y16) {
    uint8_t *p = buf + 4 + 8 * i;
    p[0] = (uint8_t)(event << 6 | (x16 >> 12 & 0xf));
    p[1] = (uint8_t)(x16 >> 4);
    p[2] = (uint8_t)(id << 4 | (y16 >> 12 & 0xf));
    p[3] = (uint8_t)(y16 >> 4);
    p[4] = (uint8_t)((x16 & 0xf) << 4 | (y16 & 0xf));
}

static void test_decode(void) {
    uint8_t buf[MT_FRAME_LEN];
    int ids[10], xs[10], ys[10];
    memset(buf, 0, sizeof buf);
    CHECK(mt_decode(buf, ids, xs, ys) == 0);             // all zero: v1, count 0, slots "down": abnormal, dropped
    memset(buf, 0xff, sizeof buf);                       // the firmware leaves unused slots at 0xff
    buf[0] = 0x01;
    buf[1] = 0x23;                                       // v2, three points
    put_v2(buf, 0, 0, 3, 600 * 16, 1304 * 16);          // down at the panel centre
    put_v2(buf, 1, 1, 4, 100 * 16, 100 * 16);           // lifted: skipped
    put_v2(buf, 2, 2, 9, 1199 * 16 + 15, 2607 * 16 + 15);   // contact at the far corner
    CHECK(mt_decode(buf, ids, xs, ys) == 2);
    CHECK(ids[0] == 3 && xs[0] == 15240 && ys[0] == 10160);
    CHECK(ids[1] == 9 && xs[1] == 30478 && ys[1] == MT_HID_Y - 1);     // inside the axes, never at or past the maximum
    put_v2(buf, 0, 0, 1, 0x1234, 0xabcd);               // all 16 bits of both axes
    CHECK(mt_decode(buf, ids, xs, ys) == 2 && ids[0] == 1);
    CHECK(xs[0] == (int)(0x1234ull * MT_HID_X / (MT_PANEL_W * 16)) && ys[0] == (int)(0xabcdull * MT_HID_Y / (MT_PANEL_H * 16)));
    buf[1] = 0x20;                                       // v2 with count 0: nothing
    CHECK(mt_decode(buf, ids, xs, ys) == 0);
    buf[1] = 0x24;                                       // v2 count 4 but slot 3 is unused (id 15): dropped whole
    CHECK(mt_decode(buf, ids, xs, ys) == 0);
    memset(buf, 0, sizeof buf);
    buf[1] = 0x01;                                       // v1: 6-byte points, whole pixels, id 15 ends the list
    uint8_t *p = buf + 2;
    p[0] = 0x80 | (600 >> 8); p[1] = 600 & 0xff; p[2] = 0x20 | (1304 >> 8); p[3] = 1304 & 0xff;
    p += 6;
    p[2] = 0xf0;
    CHECK(mt_decode(buf, ids, xs, ys) == 1 && ids[0] == 2 && xs[0] == 15240 && ys[0] == 10160);
    buf[1] = 0x00;                                       // the same point with count 0: abnormal, dropped
    CHECK(mt_decode(buf, ids, xs, ys) == 0);
    buf[1] = 0x0b;                                       // count above 10: dropped
    CHECK(mt_decode(buf, ids, xs, ys) == 0);
    memset(buf, 0, sizeof buf);
    buf[1] = 0x10;                                       // unknown point format: nothing
    CHECK(mt_decode(buf, ids, xs, ys) == 0);
    memset(buf, 0xff, sizeof buf);
    CHECK(mt_frame_restarted(buf));
    buf[1] = 0x21;                                       // a v2 frame whose gesture byte happens to be 0xff
    CHECK(!mt_frame_restarted(buf));
}

static void test_smmu(void) {
    struct fake_smmu f = myron_smmu();
    struct mt_smmu_io io = {&f, fake_rd, fake_wr};
    struct mt_smmu_plan p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(!p.error && p.groups == 116 && p.like_slot == 0 && p.slot == 2 && !p.reuse && p.s2cr == 0);
    CHECK(mt_smmu_route_stream(&io, &p, MT_QUP4_SMMU_SID) == 0);
    CHECK(f.regs[0x808 / 4] == (0x80000000u | MT_QUP4_SMMU_SID) && f.regs[0xc08 / 4] == 0 && f.writes == 2);
    // A second run finds its own group and writes nothing.
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(!p.error && p.reuse && p.slot == 2);
    CHECK(mt_smmu_route_stream(&io, &p, MT_QUP4_SMMU_SID) == 0 && f.writes == 2);
    // The S2CR template is copied whatever it holds.
    f = myron_smmu();
    f.regs[0xc00 / 4] = 0x00030005;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(!p.error && p.s2cr == 0x00030005);
    CHECK(mt_smmu_route_stream(&io, &p, MT_QUP4_SMMU_SID) == 0 && f.regs[0xc08 / 4] == 0x00030005);
    // A masked group of someone else already covers the stream: refused, nothing written.
    f = myron_smmu();
    f.regs[0x808 / 4] = 0x80ff0500;
    f.regs[0xc08 / 4] = 7;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(p.error == MT_SMMU_CONFLICT);
    CHECK(mt_smmu_route_stream(&io, &p, MT_QUP4_SMMU_SID) == MT_SMMU_CONFLICT && f.writes == 0);
    // An exact group for the stream that routes elsewhere: refused.
    f = myron_smmu();
    f.regs[0x808 / 4] = 0x80000000u | MT_QUP4_SMMU_SID;
    f.regs[0xc08 / 4] = 1;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(p.error == MT_SMMU_CONFLICT);
    // No UFS group to copy.
    f = myron_smmu();
    f.regs[0x800 / 4] = 0;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(p.error == MT_SMMU_NO_TEMPLATE);
    // Every group in use.
    f = myron_smmu();
    for (unsigned i = 2; i < 116; ++i) f.regs[0x800 / 4 + i] = 0x80000100u + i;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(p.error == MT_SMMU_FULL);
    // Writes that do not stick (a protected GR0): reported at the first readback, SMR never written.
    f = myron_smmu();
    f.regs[0xc00 / 4] = 0x00000003;
    f.stuck = 1;
    p = mt_smmu_plan_stream(&io, MT_QUP4_SMMU_SID, MT_UFS_SMMU_SID);
    CHECK(mt_smmu_route_stream(&io, &p, MT_QUP4_SMMU_SID) == MT_SMMU_S2CR_READBACK && f.writes == 1);
}

int main(void) {
    test_gcc_pages();
    test_pins();
    test_crc();
    test_clock();
    test_decode();
    test_smmu();
    if (failures) {printf("%d failures\n", failures); return 1;}
    printf("MyronTouchBoard: all tests passed\n");
    return 0;
}
