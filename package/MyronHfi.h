// SPDX-License-Identifier: BSD-2-Clause-Patent
// The HFI start-up tables of myron's Adreno 840 GMU, built as Linux mainline builds them for A8XX
// (a6xx_gmu_rpmh_arc_votes_init / _dep_votes_init / _bw_votes_init, a8xx_hfi_send_perf_table, a6xx_generate_bw_table;
// myron-work/gpu/GEN8-HFI-GX-POWERON.md) from the AOP command DB (MyronCmdDb.h) and myron's stock GPU levels
// (base0.dts qcom,gpu-pwrlevels, qcom,gmu-freq-table, qcom,bus-table-ddr). All 18 GPU levels of speed bin 1 are offered,
// 160..1200 MHz: myron's fuse reads 0xfc, which is that bin's qcom,speed-bin (the four bins differ only at the high levels).
// bin 1 also gives each level its RPMh level and DDR index. myron-gpu-perf picks the voted level (default: the highest).
// No device access; host-tested by MyronHfiTests.c.
#ifndef MYRON_HFI_H
#define MYRON_HFI_H
#include <stdint.h>
#include "MyronCmdDb.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MHF_MSG_BW_TABLE 3
#define MHF_MSG_START 10
#define MHF_MSG_CORE_FW_START 14
#define MHF_MSG_TABLE 15
#define MHF_MSG_GX_BW_PERF_VOTE 30
#define MHF_TABLE_GPU_PERF 1
#define MHF_GPU_LEVELS 19                 // off + 18 (speed bin 1: 160..1200 MHz)
#define MHF_GMU_LEVELS 3                  // off + 475 MHz + 700 MHz
#define MHF_BWS 12                        // qcom,bus-table-ddr, off included
#define MHF_BCMS 3                        // SH0, MC0, ACV (a6xx_catalog.c a840 bcms), in that order
#define MHF_PERF_WORDS (6 + 3 * MHF_GPU_LEVELS + 2 * MHF_GMU_LEVELS)   // the table message without its header
#define MHF_BW_WORDS 159                  // struct a6xx_hfi_msg_bw_table (640 bytes) without its header
#define MHF_BCM_CMD(commit, valid, x, y) \
    (((uint32_t)(commit) << 30) | ((uint32_t)(valid) << 29) | (((uint32_t)(x) & 0x3fff) << 14) | ((uint32_t)(y) & 0x3fff))

struct mhf_level {uint32_t khz; uint16_t level; uint16_t bw;};
extern const struct mhf_level mhf_gpu_levels[MHF_GPU_LEVELS];
extern const struct mhf_level mhf_gmu_levels[MHF_GMU_LEVELS];
extern const uint32_t mhf_bw_kbps[MHF_BWS];

enum {MHF_OK = 0, MHF_DB, MHF_GFX_LEVEL, MHF_GX_SECONDARY, MHF_MX_LEVEL, MHF_CX_LEVEL, MHF_CX_SECONDARY, MHF_BCM_AUX};
struct mhf_tables {
    int status, failed_level;
    uint32_t gx[MHF_GPU_LEVELS], dep[MHF_GPU_LEVELS], cx[MHF_GMU_LEVELS];
    uint32_t bcm_unit[MHF_BCMS], bcm_width[MHF_BCMS], bcm_vcd[MHF_BCMS];
    uint32_t ib[MHF_BWS][MHF_BCMS];
    uint32_t perf[MHF_PERF_WORDS];        // payloads as AdrenoHfiExchange sends them (the kit adds the header)
    uint32_t bw[MHF_BW_WORDS];
    uint32_t perf_index, bw_index;        // the initial GX vote: the highest offered level and its DDR index
};
int mhf_build(const struct mcd_result *db, struct mhf_tables *t);
#ifdef __cplusplus
}
#endif
#endif
