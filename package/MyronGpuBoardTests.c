// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of MyronGpuBoard.h: cc -std=c11 -Wall -Wextra -Werror MyronGpuBoardTests.c -o t && ./t
#include <stdio.h>
#include <string.h>
#include "MyronGpuBoard.h"

static int failures;
#define CHECK(c) do {if (!(c)) {printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures;}} while (0)

// A fake of the blocks: words per window, a write log, and the hardware reactions the sequence relies on (CX powers up when
// SW_COLLAPSE is cleared, a branch runs once enabled), each of which a test can switch off.
struct fake {
    uint32_t w[MG_WINDOWS][0x9800 / 4];
    struct {unsigned window; uint32_t offset, value;} log[64];
    unsigned writes, reads[MG_WINDOWS];
    int cx_dead, clocks_dead, pll_dead, fail_window;
};
static int is_branch(uint32_t offset) {
    static const uint32_t branches[] = {MG_CC_AHB_CLK, MG_CC_CX_GMU_CLK, MG_CC_CXO_CLK, MG_CC_HUB_CX_INT_CLK, MG_CC_MEMNOC_GFX_CLK,
                                        MG_CC_GX_GMU_CLK, MG_CC_SMMU_VOTE_CLK, MG_CC_RSCC_XO_AON_CLK, MG_CC_CXO_AON_CLK,
                                        MG_CC_RSCC_HUB_AON_CLK, MG_CC_HUB_AON_CLK};
    for (unsigned i = 0; i < sizeof branches / sizeof branches[0]; ++i) if (branches[i] == offset) return 1;
    return 0;
}
static int fake_rd(void *cookie, unsigned window, uint32_t offset, uint32_t *out) {
    struct fake *f = cookie;
    if ((int)window == f->fail_window) return -1;
    ++f->reads[window];
    *out = f->w[window][offset / 4];
    return 0;
}
static int fake_wr(void *cookie, unsigned window, uint32_t offset, uint32_t value) {
    struct fake *f = cookie;
    if (f->writes < 64) {f->log[f->writes].window = window; f->log[f->writes].offset = offset; f->log[f->writes].value = value;}
    ++f->writes;
    f->w[window][offset / 4] = value;
    if (window == MG_WIN_GPUCC && offset == MG_CC_CX_GDSCR && !(value & MG_GDSC_SW_COLLAPSE) && !f->cx_dead)
        f->w[MG_WIN_GPUCC][MG_CC_CX_GDS_HW_CTRL / 4] |= MG_GDSC_PWR_ON;
    if (window == MG_WIN_GPUCC && (value & MG_CBCR_ENABLE) && is_branch(offset) && !f->clocks_dead)
        f->w[window][offset / 4] &= ~MG_CBCR_CLK_OFF;
    if (window == MG_WIN_GPUCC && offset == MG_CC_PLL0 + MG_PLL_OPMODE && value == MG_PLL_RUN && !f->pll_dead)
        f->w[window][(MG_CC_PLL0 + MG_PLL_MODE) / 4] |= MG_PLL_LOCK_DET;
    if (window == MG_WIN_GPUCC && (offset == MG_CC_HUB_RCGR || offset == MG_CC_GMU_RCGR)) {
        uint32_t *cmd = &f->w[window][offset / 4];
        if (value & MG_RCG_ROOT_EN) *cmd &= ~MG_RCG_ROOT_OFF;      // the root runs once forced on
        *cmd &= ~MG_RCG_UPDATE;                                     // the update completes at once
    }
    return 0;
}
static const unsigned all = (1u << MG_WINDOWS) - 1;

// As a cold SoC is expected to read: CX collapsed (SW_COLLAPSE set), every GPUCC branch disabled and off.
static void cold(struct fake *f) {
    memset(f, 0, sizeof *f);
    f->fail_window = -1;
    f->w[MG_WIN_GCC_GPU][MG_GCC_GPU_CFG_AHB / 4] = 0x80000003;   // enabled, idle under hardware clock gating
    f->w[MG_WIN_GPUCC][MG_CC_CX_GDSCR / 4] = 0x0022f001;          // collapsed, with some other wait values
    static const uint32_t off[] = {MG_CC_AHB_CLK, MG_CC_CX_GMU_CLK, MG_CC_SMMU_VOTE_CLK, MG_CC_RSCC_XO_AON_CLK, MG_CC_CXO_AON_CLK,
                                   MG_CC_RSCC_HUB_AON_CLK, MG_CC_CXO_CLK};
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; ++i) f->w[MG_WIN_GPUCC][off[i] / 4] = MG_CBCR_CLK_OFF;
    f->w[MG_WIN_SMMU][MG_SMMU_ID0 / 4] = 0x00000018;               // 24 stream-match groups
    f->w[MG_WIN_SMMU][MG_SMMU_SMR(3) / 4] = 0x80000005;
    f->w[MG_WIN_SMMU][MG_SMMU_S2CR(3) / 4] = 0x00000001;
}
static struct mg_io io_of(struct fake *f) {return (struct mg_io){f, fake_rd, fake_wr, NULL};}

static void test_layout(void) {
    CHECK(MG_GMU_REG(0x1f7e8) == 0xfa0 && MG_GMU_REG(0x1f7e9) == 0xfa4 && MG_GMU_REG(0x1f800) == 0x1000);
    CHECK(MG_GMU_CM3_FW_INIT_RESULT == 0x1070 && MG_GMU_AO_COUNTER_LO == 0x1100 && MG_GMU_AO_COUNTER_LO + 4 <= MG_GMU_WINDOW);
    CHECK(MG_GMU_PAGE >= 0x3d37000 && MG_GMU_PAGE + MG_GMU_WINDOW <= 0x3d37000 + 0x68000);   // qcom,gmu@3d37000 reg
    CHECK(MG_GXCC_PAGE + MG_GX_GDSCR == 0x3d68024 && MG_GX_CFG_GDSCR - MG_GX_GDSCR == 4);     // clock-controller@3d68024, size 8
    CHECK(MG_GPUCC_BASE + MG_CC_CX_GDS_HW_CTRL == 0x3d99094);                                  // the DT's gpu_cc_cx_gdsc_hw_ctrl
    CHECK(MG_CC_HUB_RCGR + 8 <= MG_GPUCC_SIZE && MG_RBBM_GFX_STATUS + 4 <= MG_GPU_WINDOW);
    CHECK(MG_CC_CX_GDSC_WAITS == 0x228000);
}

static void test_read_only_cold(void) {
    struct fake f; cold(&f);
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 1, all, &r);
    CHECK(f.writes == 0 && !r.io_error);
    CHECK(r.gcc == MG_DONE && r.cc == MG_DONE && r.cx_before == 0x0022f001);
    CHECK(r.gx == MG_SKIPPED_PREREQ && r.smmu == MG_SKIPPED_PREREQ && r.gmu == MG_SKIPPED_PREREQ && r.gpu == MG_SKIPPED_PREREQ);
    CHECK(f.reads[MG_WIN_GXCC] == 0 && f.reads[MG_WIN_SMMU] == 0 && f.reads[MG_WIN_GMU] == 0 && f.reads[MG_WIN_GPU] == 0);
}

static void test_ahb_gate(void) {
    struct fake f; cold(&f);
    f.w[MG_WIN_GCC_GPU][MG_GCC_GPU_CFG_AHB / 4] = 0x80000002;   // disabled
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 1, all, &r);
    CHECK(f.writes == 0 && r.cc == MG_SKIPPED_PREREQ && f.reads[MG_WIN_GPUCC] == 0);
    cold(&f);
    f.w[MG_WIN_GCC_GPU][MG_GCC_GPU_CFG_AHB / 4] = 0x80000002;
    mg_run(&io, 2, all, &r);
    CHECK(f.writes == 1 && f.log[0].window == MG_WIN_GCC_GPU && f.log[0].offset == MG_GCC_GPU_CFG_AHB && f.log[0].value == 0x80000003);
    CHECK(r.enabled_ahb && r.cc == MG_DONE && r.smmu == MG_SKIPPED_PREREQ);
}

static void test_power_cx(void) {
    struct fake f; cold(&f);
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 3, all, &r);
    CHECK(r.powered_cx && r.clocks_on && !r.io_error && r.cx_polls >= 1);
    // gdsc_init, collapse cleared, retain_ff: in that order, all on the GDSCR
    CHECK(f.log[0].offset == MG_CC_CX_GDSCR && (f.log[0].value & MG_GDSC_WAIT_MASK) == MG_CC_CX_GDSC_WAITS);
    CHECK((f.log[0].value & (MG_GDSC_HW_CONTROL | MG_GDSC_SW_OVERRIDE)) == 0 && (f.log[0].value & MG_GDSC_SW_COLLAPSE));
    CHECK(f.log[1].offset == MG_CC_CX_GDSCR && !(f.log[1].value & MG_GDSC_SW_COLLAPSE));
    CHECK(f.log[2].offset == MG_CC_CX_GDSCR && (f.log[2].value & MG_GDSC_RETAIN_FF));
    CHECK(f.writes == 3 + 5);
    for (unsigned i = 0; i < f.writes; ++i) CHECK(f.log[i].window == MG_WIN_GPUCC);   // GX, the SMMU and the GPU stay untouched
    CHECK(r.smmu == MG_DONE && r.smr_valid == 1 && r.smr_first == 3 && r.s2cr_first_val == 1);
    CHECK(r.gx == MG_DONE && r.gpu == MG_SKIPPED_PREREQ);   // GX reads off: no RBBM read
    CHECK(r.gmu == MG_SKIPPED_PREREQ);                      // the GMU clock is not turned on
}

static void test_cx_stuck(void) {
    struct fake f; cold(&f);
    f.cx_dead = 1;
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 3, all, &r);
    CHECK(!r.powered_cx && r.cx_polls > 100 && f.writes == 2);   // gdsc_init and the collapse write, then nothing
    CHECK(r.smmu == MG_SKIPPED_PREREQ && r.gx == MG_SKIPPED_PREREQ);
}

static void test_cx_already_on(void) {
    struct fake f; cold(&f);
    f.w[MG_WIN_GPUCC][MG_CC_CX_GDS_HW_CTRL / 4] = MG_GDSC_PWR_ON;
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 3, all, &r);
    CHECK(!r.powered_cx && r.clocks_on && f.writes == 5 && r.smmu == MG_DONE);   // only the clock enables
}

static void test_all_on(void) {
    struct fake f; cold(&f);
    f.w[MG_WIN_GPUCC][MG_CC_CX_GDS_HW_CTRL / 4] = MG_GDSC_PWR_ON;
    f.w[MG_WIN_GPUCC][MG_CC_AHB_CLK / 4] = 1;
    f.w[MG_WIN_GPUCC][MG_CC_CX_GMU_CLK / 4] = 1;
    f.w[MG_WIN_GPUCC][MG_CC_SMMU_VOTE_CLK / 4] = 1;
    f.w[MG_WIN_GXCC][MG_GX_GDSCR / 4] = MG_GDSC_PWR_ON;
    f.w[MG_WIN_GXCC][MG_GX_CFG_GDSCR / 4] = MG_GX_POWER_UP_COMPLETE;
    f.w[MG_WIN_GMU][MG_GMU_GFX_PWR_CLK_STATUS / 4] = 0x00c00000;
    f.w[MG_WIN_GMU][MG_GMU_CM3_FW_INIT_RESULT / 4] = 0xbabeface;
    f.w[MG_WIN_GPU][MG_RBBM_STATUS / 4] = 0x00800000;
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 1, all, &r);
    CHECK(f.writes == 0 && r.gx == MG_DONE && r.smmu == MG_DONE && r.gmu == MG_DONE && r.gpu == MG_DONE);
    CHECK(r.gmu_pwr_clk == 0x00c00000 && r.gmu_fw_init == 0xbabeface && r.rbbm_status == 0x00800000);
    // GX half on (status not complete): no RBBM read
    cold(&f);
    f.w[MG_WIN_GPUCC][MG_CC_CX_GDS_HW_CTRL / 4] = MG_GDSC_PWR_ON;
    f.w[MG_WIN_GPUCC][MG_CC_AHB_CLK / 4] = 1;
    f.w[MG_WIN_GXCC][MG_GX_GDSCR / 4] = MG_GDSC_PWR_ON;
    mg_run(&io, 1, all, &r);
    CHECK(r.gx == MG_DONE && r.gpu == MG_SKIPPED_PREREQ && f.reads[MG_WIN_GPU] == 0);
}

static void test_missing_windows(void) {
    struct fake f; cold(&f);
    struct mg_io io = io_of(&f);
    struct mg_result r;
    mg_run(&io, 3, all & ~(1u << MG_WIN_SMMU), &r);
    CHECK(r.smmu == MG_NOMAP && r.powered_cx);
    mg_run(&io, 1, 0, &r);
    CHECK(r.gcc == MG_NOMAP);
    mg_run(&io, 0, all, &r);
    CHECK(r.gcc == MG_SKIPPED_MODE);
    cold(&f);
    f.fail_window = MG_WIN_GCC_GPU;
    mg_run(&io, 3, all, &r);
    CHECK(r.io_error && r.gcc == MG_NOT_RUN && f.writes == 0);
}

// As myron-gpu=3 read it on 2026-10-22, after CX came up: PLL0 off and unconfigured, the GMU root off on XO, the hub root on XO.
static void after_mode3(struct fake *f, struct mg_result *r) {
    cold(f);
    f->w[MG_WIN_GPUCC][MG_CC_CX_GDS_HW_CTRL / 4] = 0x82000202;
    f->w[MG_WIN_GPUCC][MG_CC_PLL0 / 4] = 0x00000100;
    f->w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_L_VAL) / 4] = 0x00480000;
    f->w[MG_WIN_GPUCC][MG_CC_GMU_RCGR / 4] = 0x80000000;
    f->w[MG_WIN_GPUCC][(MG_CC_GMU_RCGR + 4) / 4] = 0x00100000;
    f->w[MG_WIN_GPUCC][(MG_CC_HUB_RCGR + 4) / 4] = 0x00100000;
    f->w[MG_WIN_GCC_VOTE][MG_GCC_GPLL0_VOTE / 4] = 0x00e0217f;
    f->w[MG_WIN_GPUCC][MG_CC_CX_GMU_CLK / 4] = 0x08002001;   // running once CX was on (mode 3 read)
    struct mg_io io = io_of(f);
    mg_run(&io, 3, all, r);
    f->writes = 0;
}

static void test_gmu_clocks(void) {
    struct fake f;
    struct mg_result r;
    struct mg_gmu_clocks o;
    after_mode3(&f, &r);
    struct mg_io io = io_of(&f);
    CHECK(r.powered_cx == 0 && r.clocks_on);   // CX already read on in this fixture, so mode 3 only enabled the clocks
    mg_gmu_clocks_on(&io, &r, &o);
    CHECK(o.step == 7 && !o.failed && !r.io_error);
    CHECK(o.gpll0 == (0x00e0217fu | MG_GPLL0_VOTES));
    CHECK(f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_L_VAL) / 4] == 0x00480031 && f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_ALPHA) / 4] == 0x7aaa);
    CHECK(f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_CONFIG_CTL) / 4] == 0x25c400e7 && f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_CONFIG_CTL_U1) / 4] == 0xf51dea20);
    CHECK(o.pll0_opmode == MG_PLL_RUN && (o.pll0_mode & (MG_PLL_OUTCTRL | MG_PLL_RESET_N | MG_PLL_LOCK_DET)) == (MG_PLL_OUTCTRL | MG_PLL_RESET_N | MG_PLL_LOCK_DET));
    CHECK((o.pll0_user & MG_PLL_OUT_MASK) == MG_PLL_OUT_MASK && (o.pll0_user & 0x408) == 0x408);
    CHECK(o.hub_cfg == 0x00100603 && o.gmu_cfg == 0x00100200);
    CHECK(!(o.hub_cmd & MG_RCG_ROOT_EN) && !(o.gmu_cmd & (MG_RCG_ROOT_EN | MG_RCG_ROOT_OFF)));
    CHECK(mg_clock_on(o.cx_gmu) && mg_enabled(o.cxo) && mg_enabled(o.hub_cx_int) && mg_enabled(o.hub_aon));
    for (unsigned i = 0; i < f.writes && i < 64; ++i)
        CHECK(f.log[i].window == MG_WIN_GPUCC || (f.log[i].window == MG_WIN_GCC_VOTE && f.log[i].offset == MG_GCC_GPLL0_VOTE));
    // A PLL that already runs is not reconfigured.
    after_mode3(&f, &r);
    f.w[MG_WIN_GPUCC][MG_CC_PLL0 / 4] = MG_PLL_OUTCTRL | MG_PLL_RESET_N | MG_PLL_LOCK_DET;
    f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_OPMODE) / 4] = MG_PLL_RUN;
    mg_gmu_clocks_on(&io, &r, &o);
    CHECK(o.step == 7 && f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_ALPHA) / 4] == 0 && o.lock_polls == 0);
    // An FSM-voted PLL is refused before any PLL write; a PLL that never locks stops at step 3.
    after_mode3(&f, &r);
    f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_USER_CTL) / 4] = MG_PLL_VOTE_RUN;
    mg_gmu_clocks_on(&io, &r, &o);
    CHECK(o.step == 2 && o.failed == 1 && f.w[MG_WIN_GPUCC][(MG_CC_PLL0 + MG_PLL_ALPHA) / 4] == 0);
    after_mode3(&f, &r);
    f.pll_dead = 1;
    mg_gmu_clocks_on(&io, &r, &o);
    CHECK(o.step == 3 && o.failed == 1 && o.lock_polls == 1500 && o.hub_cfg == 0x00100000);
    // Without CX the sequence does not start.
    cold(&f);
    mg_run(&io, 1, all, &r);
    f.writes = 0;
    mg_gmu_clocks_on(&io, &r, &o);
    CHECK(o.step == 1 && o.failed == 1 && f.writes == 0);
}

int main(void) {
    test_layout();
    test_read_only_cold();
    test_ahb_gate();
    test_power_cx();
    test_cx_stuck();
    test_cx_already_on();
    test_all_on();
    test_missing_windows();
    test_gmu_clocks();
    if (failures) {printf("%d failures\n", failures); return 1;}
    printf("MyronGpuBoard: all tests passed\n");
    return 0;
}
