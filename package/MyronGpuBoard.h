// SPDX-License-Identifier: BSD-2-Clause-Patent
// GPU power and clocks of myron (Redmi K90 Pro Max, SM8850): Adreno 840, KGSL gen8_2_1, chip id 0x44050a01 (Linux
// a6xx_catalog.c lists the same id with gen80200 firmware). Board facts and the staged, gated read sequence of
// MyronGpuPower.cpp, after sheng's GpuPowerBoard.h. Sources: the stock Android DT (myron-work/base0.dts: qcom,kgsl-3d0@3d00000,
// qcom,gmu@3d37000, kgsl-smmu@3da0000, clock-controller@3d90000 canoe-gpucc, clock-controller@3d68024 canoe-gx_clkctl),
// Linux drivers/clk/qcom/{gcc,gpucc,gxclkctl}-kaanapali.c and gdsc.c, and Qualcomm's KGSL gen8_reg.h (register offsets in
// dwords from the GPU base, as Linux a6xx_gmu.c uses them for A8XX). Nothing here touches hardware; MyronGpuPower.cpp owns
// the MMIO. Host-tested by MyronGpuBoardTests.c.
//
// Staged because the GPU blocks sit behind clocks and power domains no Darwin driver manages: reading a block whose clock or
// rail is off can hang the bus instead of faulting, so each block is read only when an earlier read showed its prerequisite on.
#ifndef MYRON_GPU_BOARD_H
#define MYRON_GPU_BOARD_H
#include <stdint.h>

// Windows, each mapped on its own; register offsets below are relative to their window.
#define MG_GCC_GPU_PAGE 0x00171000ULL     // GCC 0x100000 + 0x71000
#define MG_GCC_VOTE_PAGE 0x00152000ULL    // GCC 0x100000 + 0x52000
#define MG_GPUCC_BASE 0x03d90000ULL       // canoe-gpucc reg size 0x9800 (Linux regmap max_register 0x95e8)
#define MG_GPUCC_SIZE 0x9800ULL
#define MG_GXCC_PAGE 0x03d68000ULL        // gx_clkctl 0x3d64000 + gdscr 0x4024 = the DT's clock-controller@3d68024
#define MG_SMMU_BASE 0x03da0000ULL        // kgsl-smmu@3da0000 (qsmmu-v500); global register space 0 only
#define MG_SMMU_GR0_SIZE 0x1000ULL
#define MG_GPU_BASE 0x03d00000ULL         // kgsl_3d0_reg_memory; the first page holds the RBBM status words
#define MG_GPU_WINDOW 0x1000ULL
#define MG_GMU_PAGE 0x03d7d000ULL         // inside qcom,gmu@3d37000 (0x68000): the GMU CX status words below
#define MG_GMU_WINDOW 0x2000ULL
#define MG_GMU_REG(dword) ((uint32_t)((dword) * 4u - (uint32_t)(MG_GMU_PAGE - MG_GPU_BASE)))

// gcc-kaanapali.c: gcc_gpu_cfg_ahb_clk 0x71004 is a critical branch the GCC probe enables (gcc_kaanapali_critical_cbcrs);
// gcc_qmip_gpu_ahb_clk 0x71008 and gcc_gpu_gemnoc_gfx_clk 0x71010 are voted with hardware clock gating (bit 1);
// gcc_gpu_gpll0_clk_src / gcc_gpu_gpll0_div_clk_src are bits 15 / 16 of 0x52000.
#define MG_GCC_GPU_CFG_AHB 0x004
#define MG_GCC_QMIP_GPU_AHB 0x008
#define MG_GCC_GPU_GEMNOC_GFX 0x010
#define MG_GCC_GPLL0_VOTE 0x000
#define MG_GPLL0_VOTES ((1u << 15) | (1u << 16))
#define MG_CBCR_CLK_OFF (1u << 31)        // clk-branch.c BRANCH_CLK_OFF
#define MG_CBCR_ENABLE (1u << 0)

// gpucc-kaanapali.c. The one GPUCC power domain is gpu_cc_cx_gdsc (gdscr 0x9080, gds_hw_ctrl 0x9094, VOTABLE,
// RETAIN_FF_ENABLE, waits en_rest 2 / en_few 2 / clk_dis 8). GX moved out of GPUCC into gx_clkctl (below).
#define MG_CC_CX_GDSCR 0x9080
#define MG_CC_CX_GDS_HW_CTRL 0x9094
#define MG_CC_AHB_CLK 0x90bc
#define MG_CC_CX_GMU_CLK 0x90d4
#define MG_CC_CXO_CLK 0x90e4
#define MG_CC_HUB_CX_INT_CLK 0x90e8
#define MG_CC_MEMNOC_GFX_CLK 0x90ec
#define MG_CC_GX_GMU_CLK 0x9060
#define MG_CC_SMMU_VOTE_CLK 0x7000        // gpu_cc_gpu_smmu_vote_clk, the clock of the kgsl-smmu node
#define MG_CC_RSCC_XO_AON_CLK 0x9004      // gpu_cc_kaanapali_critical_cbcrs, enabled by the GPUCC probe
#define MG_CC_CXO_AON_CLK 0x9008
#define MG_CC_RSCC_HUB_AON_CLK 0x93e8
#define MG_CC_HUB_AON_CLK 0x93ec
#define MG_CC_GMU_RCGR 0x9318             // gpu_cc_gmu_clk_src cmd_rcgr; CFG at +4
#define MG_CC_HUB_RCGR 0x93f0             // gpu_cc_hub_clk_src
#define MG_CC_PLL0 0x0000                 // gpu_cc_pll0 (taycan_eko_t): MODE +0, OPMODE +4, L_VAL +0x10
// gdsc.c
#define MG_GDSC_PWR_ON (1u << 31)
#define MG_GDSC_SW_COLLAPSE (1u << 0)
#define MG_GDSC_HW_CONTROL (1u << 1)
#define MG_GDSC_SW_OVERRIDE (1u << 2)
#define MG_GDSC_RETAIN_FF (1u << 11)
#define MG_GDSC_WAIT_MASK (0xfffu << 12)  // EN_REST_WAIT 23:20, EN_FEW_WAIT 19:16, CLK_DIS_WAIT 15:12
#define MG_CC_CX_GDSC_WAITS ((2u << 20) | (2u << 16) | (8u << 12))
#define MG_GDSC_POLL_US 2000              // gdsc.c waits 500 us; the probe allows more and records the polls

// gxclkctl-kaanapali.c: gx_clkctl_gx_gdsc gdscr 0x4024 with POLL_CFG_GDSCR, so its status is CFG_GDSCR (gdscr + 4)
// bit 16 POWER_UP_COMPLETE. Linux never enables it (gdsc_gx_do_nothing_enable): the GMU powers GX.
#define MG_GX_GDSCR 0x024
#define MG_GX_CFG_GDSCR 0x028
#define MG_GX_POWER_UP_COMPLETE (1u << 16)

// ARM SMMUv2 global space 0: sCR0 0x0, IDR0 0x20, IDR1 0x24, IDR2 0x28, sGFSR 0x48, SMR 0x800, S2CR 0xc00.
#define MG_SMMU_SCR0 0x0
#define MG_SMMU_ID0 0x20
#define MG_SMMU_ID1 0x24
#define MG_SMMU_ID2 0x28
#define MG_SMMU_GFSR 0x48
#define MG_SMMU_SMR(n) (0x800 + 4 * (n))
#define MG_SMMU_S2CR(n) (0xc00 + 4 * (n))
#define MG_SMMU_MAX_SMR 128

// gen8_reg.h, in dwords from the GPU base.
#define MG_RBBM_GBIF_HALT (0x00a * 4)
#define MG_RBBM_STATUS (0x012 * 4)
#define MG_RBBM_GFX_STATUS (0x015 * 4)
#define MG_GMU_GFX_PWR_CLK_STATUS MG_GMU_REG(0x1f7e8)
#define MG_GMU_RPMH_POWER_STATE MG_GMU_REG(0x1f7e9)
#define MG_GMU_CM3_SYSRESET MG_GMU_REG(0x1f800)
#define MG_GMU_CM3_FW_INIT_RESULT MG_GMU_REG(0x1f81c)
#define MG_GMU_AO_COUNTER_LO MG_GMU_REG(0x1f840)

enum mg_state {MG_NOT_RUN = 0, MG_DONE = 1, MG_SKIPPED_PREREQ = 2, MG_SKIPPED_MODE = 3, MG_NOMAP = 4};
enum {MG_WIN_GCC_GPU = 0, MG_WIN_GCC_VOTE = 1, MG_WIN_GPUCC = 2, MG_WIN_GXCC = 3, MG_WIN_SMMU = 4, MG_WIN_GMU = 5, MG_WIN_GPU = 6,
      MG_WINDOWS = 7};

struct mg_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                                     // may be NULL
};

struct mg_result {
    unsigned mode;                       // 1 read only; 2 + gcc_gpu_cfg_ahb_clk on if off; 3 + CX power domain and its clocks
    enum mg_state gcc, cc, gx, smmu, gmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, qmip_ahb, gemnoc, gpll0;
    uint32_t cx_gdscr, cx_hwctrl, cc_ahb, cc_cx_gmu, cc_cxo, cc_hub_cx_int, cc_memnoc, cc_gx_gmu, cc_smmu;
    uint32_t aon_rscc_xo, aon_cxo, aon_rscc_hub, aon_hub, gmu_cmd, gmu_cfg, hub_cmd, hub_cfg, pll0_mode, pll0_opmode, pll0_l;
    uint32_t gx_gdscr, gx_cfg;
    uint32_t scr0, id0, id1, id2, gfsr, smr_valid, smr_first, smr_first_val, s2cr_first_val;
    uint32_t gmu_pwr_clk, gmu_rpmh, gmu_sysreset, gmu_fw_init, gmu_ao;
    uint32_t rbbm_status, rbbm_gfx_status, gbif_halt;
    uint32_t cx_before, smmu_before;     // mode 3: the GDSCR and the SMMU vote clock before the sequence
    unsigned cx_polls, clock_polls;
    int enabled_ahb, powered_cx, clocks_on, io_error;
};

static inline int mg_clock_on(uint32_t cbcr) {return (cbcr & MG_CBCR_CLK_OFF) == 0;}
// gcc_gpu_cfg_ahb_clk: an idle branch with hardware clock gating reads CLK_OFF though enabled (sheng read 0x80000003 for
// its twin), so its enable bit is the test.
static inline int mg_enabled(uint32_t cbcr) {return (cbcr & MG_CBCR_ENABLE) != 0;}
static inline int mg_gdsc_on(uint32_t word) {return (word & MG_GDSC_PWR_ON) != 0;}
static inline int mg_gx_on(uint32_t gdscr, uint32_t cfg) {return mg_gdsc_on(gdscr) && (cfg & MG_GX_POWER_UP_COMPLETE);}
static inline unsigned mg_smmu_num_smr(uint32_t id0) {return id0 & 0xff;}

static inline int mg_rd(const struct mg_io *io, struct mg_result *r, unsigned window, uint32_t offset, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, window, offset, &v)) {r->io_error = 1; *out = 0; return -1;}
    *out = v;
    return 0;
}
static inline int mg_wr(const struct mg_io *io, struct mg_result *r, unsigned window, uint32_t offset, uint32_t value) {
    if (io->write32(io->cookie, window, offset, value)) {r->io_error = 1; return -1;}
    return 0;
}
static inline void mg_delay(const struct mg_io *io, unsigned us) {if (io->delay_us) io->delay_us(io->cookie, us);}

// The GPUCC words: power domain, the branches the GMU and the SMMU need, the critical always-on branches, the two RCGs, PLL0.
static inline void mg_read_gpucc(const struct mg_io *io, struct mg_result *r) {
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, &r->cx_gdscr);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDS_HW_CTRL, &r->cx_hwctrl);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_AHB_CLK, &r->cc_ahb);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GMU_CLK, &r->cc_cx_gmu);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CXO_CLK, &r->cc_cxo);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_CX_INT_CLK, &r->cc_hub_cx_int);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_MEMNOC_GFX_CLK, &r->cc_memnoc);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_GX_GMU_CLK, &r->cc_gx_gmu);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_SMMU_VOTE_CLK, &r->cc_smmu);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_RSCC_XO_AON_CLK, &r->aon_rscc_xo);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CXO_AON_CLK, &r->aon_cxo);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_RSCC_HUB_AON_CLK, &r->aon_rscc_hub);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_AON_CLK, &r->aon_hub);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_GMU_RCGR, &r->gmu_cmd);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_GMU_RCGR + 4, &r->gmu_cfg);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_RCGR, &r->hub_cmd);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_RCGR + 4, &r->hub_cfg);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + 0x0, &r->pll0_mode);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + 0x4, &r->pll0_opmode);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + 0x10, &r->pll0_l);
}

// Mode 3, when CX reads off: gdsc.c for gpu_cc_cx_gdsc, as the GPUCC probe and the first power-on run it: gdsc_init (hardware
// trigger off, the wait values), gdsc_toggle_logic(ON) (SW_COLLAPSE cleared, 1 us for the hw controller, gds_hw_ctrl PWR_ON
// polled), 1 us, gdsc_retain_ff_on. GX is never written.
static inline void mg_power_cx(const struct mg_io *io, struct mg_result *r) {
    uint32_t v = r->cx_gdscr, st = 0;
    if (mg_wr(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, (v & ~(MG_GDSC_HW_CONTROL | MG_GDSC_SW_OVERRIDE | MG_GDSC_WAIT_MASK)) | MG_CC_CX_GDSC_WAITS))
        return;
    if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, &v) || mg_wr(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, v & ~MG_GDSC_SW_COLLAPSE)) return;
    mg_delay(io, 1);
    for (unsigned waited = 0;; waited += 10) {
        ++r->cx_polls;
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDS_HW_CTRL, &st)) return;
        if (mg_gdsc_on(st) || waited >= MG_GDSC_POLL_US) break;
        mg_delay(io, 10);
    }
    r->cx_hwctrl = st;
    if (!mg_gdsc_on(st)) return;
    mg_delay(io, 1);
    if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, &v) || mg_wr(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, v | MG_GDSC_RETAIN_FF)) return;
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GDSCR, &r->cx_gdscr);
    r->powered_cx = 1;
}

// Mode 3, with CX on: the three critical branches of the GPUCC probe, gpu_cc_ahb_clk and the SMMU vote clock (the kgsl-smmu
// node's clock), each only if its enable bit is clear; then wait for the vote clock to run.
static inline void mg_cx_clocks(const struct mg_io *io, struct mg_result *r) {
    uint32_t v = 0;
    static const uint32_t branches[] = {MG_CC_RSCC_XO_AON_CLK, MG_CC_CXO_AON_CLK, MG_CC_RSCC_HUB_AON_CLK, MG_CC_AHB_CLK, MG_CC_SMMU_VOTE_CLK};
    for (unsigned i = 0; i < sizeof branches / sizeof branches[0]; ++i)
        if (mg_rd(io, r, MG_WIN_GPUCC, branches[i], &v) || (!mg_enabled(v) && mg_wr(io, r, MG_WIN_GPUCC, branches[i], v | MG_CBCR_ENABLE))) return;
    for (unsigned waited = 0;; waited += 10) {
        ++r->clock_polls;
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_SMMU_VOTE_CLK, &r->cc_smmu)) return;
        if (mg_clock_on(r->cc_smmu) || waited >= 500) break;
        mg_delay(io, 10);
    }
    r->clocks_on = mg_clock_on(r->cc_smmu);
}

// Runs the gated sequence. `have` is a bitmask of mapped windows (1 << MG_WIN_*); a missing window makes its block MG_NOMAP.
static inline void mg_run(const struct mg_io *io, unsigned mode, unsigned have, struct mg_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    r->mode = mode;
    r->smr_first = 0xffffffffu;
    if (!mode) {r->gcc = MG_SKIPPED_MODE; return;}
    if (!(have & (1u << MG_WIN_GCC_GPU))) {r->gcc = MG_NOMAP; return;}
    // GCC: the page family the touch and UFS drivers already read on this SoC.
    if (mg_rd(io, r, MG_WIN_GCC_GPU, MG_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    mg_rd(io, r, MG_WIN_GCC_GPU, MG_GCC_QMIP_GPU_AHB, &r->qmip_ahb);
    mg_rd(io, r, MG_WIN_GCC_GPU, MG_GCC_GPU_GEMNOC_GFX, &r->gemnoc);
    if (have & (1u << MG_WIN_GCC_VOTE)) mg_rd(io, r, MG_WIN_GCC_VOTE, MG_GCC_GPLL0_VOTE, &r->gpll0);
    r->cfg_ahb_after = r->cfg_ahb;
    if (mode >= 2 && !mg_enabled(r->cfg_ahb)) {   // the one write of the GCC probe for this branch; mode 1 writes nothing
        if (!mg_wr(io, r, MG_WIN_GCC_GPU, MG_GCC_GPU_CFG_AHB, r->cfg_ahb | MG_CBCR_ENABLE)) {
            r->enabled_ahb = 1;
            mg_delay(io, 10);
            mg_rd(io, r, MG_WIN_GCC_GPU, MG_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        }
    }
    r->gcc = MG_DONE;
    // GPUCC needs gcc_gpu_cfg_ahb_clk.
    if (!(have & (1u << MG_WIN_GPUCC))) r->cc = MG_NOMAP;
    else if (!mg_enabled(r->cfg_ahb_after)) r->cc = MG_SKIPPED_PREREQ;
    else {mg_read_gpucc(io, r); r->cc = MG_DONE;}
    r->cx_before = r->cx_gdscr;
    r->smmu_before = r->cc_smmu;
    if (mode >= 3 && r->cc == MG_DONE && !mg_gdsc_on(r->cx_hwctrl) && !r->io_error) mg_power_cx(io, r);
    int cx = r->cc == MG_DONE && mg_gdsc_on(r->cx_hwctrl);
    if (mode >= 3 && cx && !r->io_error) {
        mg_cx_clocks(io, r);
        mg_rd(io, r, MG_WIN_GPUCC, MG_CC_AHB_CLK, &r->cc_ahb);   // the words after the sequence
        mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GMU_CLK, &r->cc_cx_gmu);
    }
    // gx_clkctl sits in the CX power domain (its DT power-domains is gpucc 0) and is reached through the GPU AHB clock.
    if (!(have & (1u << MG_WIN_GXCC))) r->gx = MG_NOMAP;
    else if (!cx || !mg_clock_on(r->cc_ahb)) r->gx = MG_SKIPPED_PREREQ;
    else {
        mg_rd(io, r, MG_WIN_GXCC, MG_GX_GDSCR, &r->gx_gdscr);
        mg_rd(io, r, MG_WIN_GXCC, MG_GX_CFG_GDSCR, &r->gx_cfg);
        r->gx = MG_DONE;
    }
    // The GPU SMMU needs CX and its vote clock. Global space 0 only (on the apps SMMU, space 1 aborts from EL1).
    if (!(have & (1u << MG_WIN_SMMU))) r->smmu = MG_NOMAP;
    else if (!cx || !mg_clock_on(r->cc_smmu)) r->smmu = MG_SKIPPED_PREREQ;
    else {
        mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_SCR0, &r->scr0);
        mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_ID0, &r->id0);
        mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_ID1, &r->id1);
        mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_ID2, &r->id2);
        mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_GFSR, &r->gfsr);
        unsigned n = mg_smmu_num_smr(r->id0);
        if (n > MG_SMMU_MAX_SMR) n = MG_SMMU_MAX_SMR;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t smr = 0;
            if (mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_SMR(i), &smr)) break;
            if (!(smr & (1u << 31))) continue;
            ++r->smr_valid;
            if (r->smr_first == 0xffffffffu) {
                r->smr_first = i; r->smr_first_val = smr;
                mg_rd(io, r, MG_WIN_SMMU, MG_SMMU_S2CR(i), &r->s2cr_first_val);
            }
        }
        r->smmu = MG_DONE;
    }
    // The GMU CX words need CX, the GPU AHB clock and the GMU clock; nothing here turns the GMU clock on.
    if (!(have & (1u << MG_WIN_GMU))) r->gmu = MG_NOMAP;
    else if (!cx || !mg_clock_on(r->cc_ahb) || !mg_clock_on(r->cc_cx_gmu)) r->gmu = MG_SKIPPED_PREREQ;
    else {
        mg_rd(io, r, MG_WIN_GMU, MG_GMU_GFX_PWR_CLK_STATUS, &r->gmu_pwr_clk);
        mg_rd(io, r, MG_WIN_GMU, MG_GMU_RPMH_POWER_STATE, &r->gmu_rpmh);
        mg_rd(io, r, MG_WIN_GMU, MG_GMU_CM3_SYSRESET, &r->gmu_sysreset);
        mg_rd(io, r, MG_WIN_GMU, MG_GMU_CM3_FW_INIT_RESULT, &r->gmu_fw_init);
        mg_rd(io, r, MG_WIN_GMU, MG_GMU_AO_COUNTER_LO, &r->gmu_ao);
        r->gmu = MG_DONE;
    }
    // The RBBM words are in the GX domain.
    if (!(have & (1u << MG_WIN_GPU))) r->gpu = MG_NOMAP;
    else if (r->gx != MG_DONE || !mg_gx_on(r->gx_gdscr, r->gx_cfg)) r->gpu = MG_SKIPPED_PREREQ;
    else {
        mg_rd(io, r, MG_WIN_GPU, MG_RBBM_STATUS, &r->rbbm_status);
        mg_rd(io, r, MG_WIN_GPU, MG_RBBM_GFX_STATUS, &r->rbbm_gfx_status);
        mg_rd(io, r, MG_WIN_GPU, MG_RBBM_GBIF_HALT, &r->gbif_halt);
        r->gpu = MG_DONE;
    }
}

// Mode 4: the GMU clocks a6xx_gmu_resume relies on, in Linux order (the gpucc probe configures PLL0; resume sets the GMU
// root to 200 MHz, which the shared RCG rounds up to its 475 MHz PLL0_OUT_EVEN entry, and the hub root to 150 MHz =
// GPLL0_OUT_MAIN_DIV / 2; clk_bulk_prepare_enable then turns the DT gmu clocks on). Steps:
//   1 GPLL0 GPU votes (gcc_gpu_gpll0_clk_src / _div_clk_src, 0x52000 bits 15 and 16, BRANCH_HALT_DELAY)
//   2 clk_lucid_evo_pll_configure for gpu_cc_pll0 (taycan_eko_t = lucid_evo registers) with gpu_cc_pll0_config, only if
//     PLL0 is not already running: L 0x31 | cal_l 0x48 << 16, alpha 0x7aaa, CONFIG_CTL 0x25c400e7 / U 0x0a8062e0 /
//     U1 0xf51dea20, USER_CTL 0x408 (PLL0_OUT_EVEN = /2) / U 0x2; OUTCTRL off, OPMODE STANDBY, RESET_N
//   3 alpha_pll_lucid_evo_enable: RESET_N, OPMODE RUN, LOCK_DET within 1500 us, USER_CTL outputs, OUTCTRL
//   4 gpu_cc_hub_clk_src 150 MHz (GPLL0_OUT_MAIN_DIV sel 6, div 2 -> 3, HW_CLK_CTRL) and 5 gpu_cc_gmu_clk_src 475 MHz
//     (PLL0_OUT_EVEN sel 2, div 1 -> 0, HW_CLK_CTRL), each by clk_rcg2_shared_enable: ROOT_EN, wait ROOT_OFF clear,
//     CFG, CMD_UPDATE until it clears, ROOT_EN off
//   6 the GMU branches (cxo, cx_gmu, hub_cx_int, hub_aon; ahb is on from mode 3), waiting for cx_gmu to run
#define MG_PLL_MODE 0x00
#define MG_PLL_OPMODE 0x04
#define MG_PLL_L_VAL 0x10
#define MG_PLL_ALPHA 0x14
#define MG_PLL_USER_CTL 0x18
#define MG_PLL_USER_CTL_U 0x1c
#define MG_PLL_CONFIG_CTL 0x20
#define MG_PLL_CONFIG_CTL_U 0x24
#define MG_PLL_CONFIG_CTL_U1 0x28
#define MG_PLL_OUTCTRL (1u << 0)
#define MG_PLL_RESET_N (1u << 2)
#define MG_PLL_LOCK_DET (1u << 31)
#define MG_PLL_RUN 1u
#define MG_PLL_OUT_MASK 7u
#define MG_PLL_VOTE_RUN (1u << 25)        // LUCID_EVO_ENABLE_VOTE_RUN: an FSM-voted PLL is only voted, never driven
#define MG_RCG_UPDATE (1u << 0)
#define MG_RCG_ROOT_EN (1u << 1)
#define MG_RCG_ROOT_OFF (1u << 31)
#define MG_RCG_CFG_MASK (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))   // hid 5 bits, SRC_SEL, MODE, HW_CLK_CTRL
#define MG_HUB_CFG ((6u << 8) | 3u | (1u << 20))
#define MG_GMU_CFG ((2u << 8) | 0u | (1u << 20))

struct mg_gmu_clocks {
    unsigned step;                       // last step started (1..6), 7 when all passed
    int failed;                          // 1 timeout at `step`, 2 I/O error
    unsigned lock_polls, hub_polls, gmu_polls, branch_polls;
    uint32_t gpll0, pll0_mode, pll0_opmode, pll0_l, pll0_user, hub_cmd, hub_cfg, gmu_cmd, gmu_cfg, cx_gmu, cxo, hub_cx_int, hub_aon;
};
static inline int mg_pll_running(uint32_t mode, uint32_t opmode) {return (opmode & MG_PLL_RUN) && (mode & MG_PLL_OUTCTRL);}
static inline int mg_set_enable(const struct mg_io *io, struct mg_result *r, unsigned window, uint32_t off) {
    uint32_t v = 0;
    if (mg_rd(io, r, window, off, &v)) return -1;
    return (v & MG_CBCR_ENABLE) ? 0 : mg_wr(io, r, window, off, v | MG_CBCR_ENABLE);
}
// 0 done, 1 timeout, -1 I/O.
static inline int mg_rcg_shared_enable(const struct mg_io *io, struct mg_result *r, uint32_t rcgr, uint32_t cfg, unsigned *polls) {
    uint32_t cmd = 0, old = 0;
    if (mg_rd(io, r, MG_WIN_GPUCC, rcgr, &cmd) || mg_wr(io, r, MG_WIN_GPUCC, rcgr, cmd | MG_RCG_ROOT_EN)) return -1;
    int timeout = 1;
    for (unsigned i = 0; i < 500; ++i) {
        if (mg_rd(io, r, MG_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & MG_RCG_ROOT_OFF)) {timeout = 0; break;}
        mg_delay(io, 1);
    }
    if (timeout) return 1;
    if (mg_rd(io, r, MG_WIN_GPUCC, rcgr + 4, &old) || mg_wr(io, r, MG_WIN_GPUCC, rcgr + 4, (old & ~MG_RCG_CFG_MASK) | cfg)) return -1;
    if (mg_rd(io, r, MG_WIN_GPUCC, rcgr, &cmd) || mg_wr(io, r, MG_WIN_GPUCC, rcgr, cmd | MG_RCG_UPDATE)) return -1;
    timeout = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++*polls;
        if (mg_rd(io, r, MG_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & MG_RCG_UPDATE)) {timeout = 0; break;}
        mg_delay(io, 1);
    }
    if (timeout) return 1;
    return mg_wr(io, r, MG_WIN_GPUCC, rcgr, cmd & ~MG_RCG_ROOT_EN) ? -1 : 0;
}
static inline void mg_gmu_clocks_on(const struct mg_io *io, struct mg_result *r, struct mg_gmu_clocks *o) {
    __builtin_memset(o, 0, sizeof *o);
    uint32_t v = 0, mode = 0, opmode = 0;
    int e;
    o->step = 1;
    if (r->cc != MG_DONE || !mg_gdsc_on(r->cx_hwctrl) || r->io_error) {o->failed = 1; return;}
    if (mg_rd(io, r, MG_WIN_GCC_VOTE, MG_GCC_GPLL0_VOTE, &v)) goto io;
    if ((v & MG_GPLL0_VOTES) != MG_GPLL0_VOTES) {
        if (mg_wr(io, r, MG_WIN_GCC_VOTE, MG_GCC_GPLL0_VOTE, v | MG_GPLL0_VOTES)) goto io;
        mg_delay(io, 10);
    }
    o->step = 2;
    if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode) || mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_OPMODE, &opmode) ||
        mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_USER_CTL, &v)) goto io;
    if (v & MG_PLL_VOTE_RUN) {o->failed = 1; goto out;}   // an FSM-voted PLL belongs to another master's sequence
    if (!mg_pll_running(mode, opmode)) {
        static const uint32_t off[] = {MG_PLL_L_VAL, MG_PLL_ALPHA, MG_PLL_CONFIG_CTL, MG_PLL_CONFIG_CTL_U, MG_PLL_CONFIG_CTL_U1,
                                       MG_PLL_USER_CTL, MG_PLL_USER_CTL_U};
        static const uint32_t val[] = {0x31u | 0x48u << 16, 0x7aaa, 0x25c400e7, 0x0a8062e0, 0xf51dea20, 0x408, 0x2};
        for (unsigned i = 0; i < sizeof off / sizeof off[0]; ++i)
            if (mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + off[i], val[i])) goto io;
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, mode & ~MG_PLL_OUTCTRL) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_OPMODE, 0) ||
            mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, mode | MG_PLL_RESET_N)) goto io;
        o->step = 3;
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, mode | MG_PLL_RESET_N) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_OPMODE, MG_PLL_RUN)) goto io;
        e = 1;
        for (unsigned i = 0; i < 1500; ++i) {
            ++o->lock_polls;
            if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode)) goto io;
            if (mode & MG_PLL_LOCK_DET) {e = 0; break;}
            mg_delay(io, 1);
        }
        if (e) {o->failed = 1; goto out;}
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_USER_CTL, &v) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_USER_CTL, v | MG_PLL_OUT_MASK) ||
            mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &mode) ||
            mg_wr(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, mode | MG_PLL_OUTCTRL)) goto io;
    }
    o->step = 4;
    e = mg_rcg_shared_enable(io, r, MG_CC_HUB_RCGR, MG_HUB_CFG, &o->hub_polls);
    if (e) {o->failed = e < 0 ? 2 : 1; goto out;}
    o->step = 5;
    e = mg_rcg_shared_enable(io, r, MG_CC_GMU_RCGR, MG_GMU_CFG, &o->gmu_polls);
    if (e) {o->failed = e < 0 ? 2 : 1; goto out;}
    o->step = 6;
    if (mg_set_enable(io, r, MG_WIN_GPUCC, MG_CC_CXO_CLK) || mg_set_enable(io, r, MG_WIN_GPUCC, MG_CC_CX_GMU_CLK) ||
        mg_set_enable(io, r, MG_WIN_GPUCC, MG_CC_HUB_CX_INT_CLK) || mg_set_enable(io, r, MG_WIN_GPUCC, MG_CC_HUB_AON_CLK)) goto io;
    e = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++o->branch_polls;
        if (mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GMU_CLK, &o->cx_gmu)) goto io;
        if (mg_clock_on(o->cx_gmu)) {e = 0; break;}
        mg_delay(io, 1);
    }
    if (e) {o->failed = 1; goto out;}
    o->step = 7;
    goto out;
io:
    o->failed = 2;
out:
    mg_rd(io, r, MG_WIN_GCC_VOTE, MG_GCC_GPLL0_VOTE, &o->gpll0);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_MODE, &o->pll0_mode);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_OPMODE, &o->pll0_opmode);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_L_VAL, &o->pll0_l);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_PLL0 + MG_PLL_USER_CTL, &o->pll0_user);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_RCGR, &o->hub_cmd);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_RCGR + 4, &o->hub_cfg);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_GMU_RCGR, &o->gmu_cmd);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_GMU_RCGR + 4, &o->gmu_cfg);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CX_GMU_CLK, &o->cx_gmu);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_CXO_CLK, &o->cxo);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_CX_INT_CLK, &o->hub_cx_int);
    mg_rd(io, r, MG_WIN_GPUCC, MG_CC_HUB_AON_CLK, &o->hub_aon);
}

static inline const char *mg_state_text(enum mg_state s) {
    return s == MG_DONE ? "ok" : s == MG_SKIPPED_PREREQ ? "skip" : s == MG_SKIPPED_MODE ? "off" : s == MG_NOMAP ? "nomap" : "-";
}
#endif
