// SPDX-License-Identifier: BSD-2-Clause-Patent
// The HFI start-up tables of myron's Adreno 840 GMU: see MyronHfi.h.
#include "MyronHfi.h"

// base0.dts qcom,gpu-pwrlevels-0..3, the levels common to every bin (frequency in kHz, qcom,level, qcom,bus-freq), ascending
// as a6xx_gmu_build_freq_table lists them after the synthetic "off" level.
// base0.dts qcom,gpu-pwrlevels-1 (qcom,speed-bin = 0xfc, the fuse myron reads): frequency, qcom,level, qcom,bus-freq (the
// index into mhf_bw_kbps), ascending.
const struct mhf_level mhf_gpu_levels[MHF_GPU_LEVELS] = {
    {0, 0, 0}, {160000, 0x32, 1}, {191000, 0x33, 2}, {222000, 0x34, 2}, {282000, 0x36, 2}, {342000, 0x38, 2}, {382000, 0x3c, 2},
    {422000, 0x40, 3}, {461000, 0x4c, 3}, {500000, 0x50, 3}, {539000, 0x60, 3}, {578000, 0x80, 3}, {646000, 0x90, 5},
    {726000, 0xc0, 5}, {826000, 0xe0, 7}, {902000, 0x100, 7}, {967000, 0x140, 7}, {1050000, 0x1a0, 9}, {1200000, 0x1c0, 11},
};
// qcom,gmu-freq-table <475000000 0x38 700000000 0x80>, after "off".
const struct mhf_level mhf_gmu_levels[MHF_GMU_LEVELS] = {{0, 0, 0}, {475000, 0x38, 0}, {700000, 0x80, 0}};
// qcom,bus-table-ddr, kB/s.
const uint32_t mhf_bw_kbps[MHF_BWS] = {0x0, 0x209a8e, 0x50a524, 0x5caf6a, 0x65ce03, 0x7cb163, 0xa3140c, 0xbdf5c2, 0xdbb3e5, 0xfbc520,
                                       0x11ce903, 0x13ddf1f};

static uint16_t u16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t u32(const uint8_t *p) {return u16(p) | (uint32_t)u16(p + 2) << 16;}

// a6xx_gmu_rpmh_arc_votes_init: pindex is the first primary level >= the frequency's level (none: refused), sindex the first
// secondary level >= it, else the last nonzero one; vote = primary level << 16 | sindex << 8 | pindex.
static int arc_votes(const struct mcd_resource *pri, const struct mcd_resource *sec, const struct mhf_level *levels, unsigned n,
                     uint32_t *votes, int *failed) {
    unsigned pn = pri->aux_bytes / 2, sn = sec->aux_bytes / 2;
    if (!pri->aux || !sec->aux || !pn || !sn) return -1;
    for (unsigned i = 0; i < n; ++i) {
        unsigned level = levels[i].level, pindex = 0, sindex = 0, j;
        for (j = 0; j < pn; ++j) if (u16(pri->aux + 2 * j) >= level) {pindex = j; break;}
        if (j == pn) {*failed = (int)level; return -2;}
        for (j = 0; j < sn; ++j) {
            uint16_t s = u16(sec->aux + 2 * j);
            if (s >= level) {sindex = j; break;}
            if (s) sindex = j;
        }
        votes[i] = (uint32_t)u16(pri->aux + 2 * pindex) << 16 | sindex << 8 | pindex;
    }
    return 0;
}

int mhf_build(const struct mcd_result *db, struct mhf_tables *t) {
    __builtin_memset(t, 0, sizeof *t);
    const unsigned need = (1u << MCD_GFX) | (1u << MCD_MX) | (1u << MCD_CX) | (1u << MCD_SH0) | (1u << MCD_MC0) | (1u << MCD_ACV) |
                          (1u << MCD_CN0);
    if (!db || db->status || (db->found & need) != need) return t->status = MHF_DB;
    // GX: gfx.lvl with GMxC as the secondary rail when the command DB has it (a6xx_gmu_rpmh_votes_init), else MX.
    const struct mcd_resource *gx_sec = (db->found & (1u << MCD_GMXC)) ? &db->r[MCD_GMXC] : &db->r[MCD_MX];
    unsigned gpu_levels = MHF_GPU_LEVELS;
    int e = arc_votes(&db->r[MCD_GFX], gx_sec, mhf_gpu_levels, gpu_levels, t->gx, &t->failed_level);
    if (e == -2 && t->failed_level) {
        // A command DB whose gfx rail stops below the table's top level: offer the levels it does have (the boot stays up,
        // and myron-gpu-perf cannot select a level that was not built).
        unsigned fit = 0;
        while (fit < gpu_levels && mhf_gpu_levels[fit].level < (unsigned)t->failed_level) ++fit;
        if (fit >= 2) {gpu_levels = fit; e = arc_votes(&db->r[MCD_GFX], gx_sec, mhf_gpu_levels, gpu_levels, t->gx, &t->failed_level);}
    }
    if (e) return t->status = e == -2 ? MHF_GFX_LEVEL : MHF_GX_SECONDARY;
    e = arc_votes(&db->r[MCD_CX], &db->r[MCD_MX], mhf_gmu_levels, MHF_GMU_LEVELS, t->cx, &t->failed_level);
    if (e) return t->status = e == -2 ? MHF_CX_LEVEL : MHF_CX_SECONDARY;
    // a6xx_gmu_rpmh_dep_votes_init: MX for every GX level, "off" fixed.
    const struct mcd_resource *mx = &db->r[MCD_MX];
    unsigned mn = mx->aux_bytes / 2;
    if (!mn) return t->status = MHF_MX_LEVEL;
    t->dep[0] = 0xffffffffu;
    for (unsigned i = 1; i < gpu_levels; ++i) {
        unsigned j, index = 0;
        for (j = 0; j < mn; ++j) if (u16(mx->aux + 2 * j) >= mhf_gpu_levels[i].level) {index = j; break;}
        if (j == mn) {t->failed_level = mhf_gpu_levels[i].level; return t->status = MHF_MX_LEVEL;}
        t->dep[i] = (0x3fffu << 14) | index << 8 | 0xff;
    }
    // a6xx_gmu_rpmh_bw_votes_init: struct bcm_db {le32 unit; le16 width; u8 vcd; u8 reserved}; a840 SH0 buswidth 16, MC0
    // buswidth 4, ACV fixed with perfmode BIT(2), which A8XX votes on every nonzero bandwidth; A7XX/A8XX vote x and y.
    static const unsigned names[MHF_BCMS] = {MCD_SH0, MCD_MC0, MCD_ACV};
    static const uint32_t buswidth[MHF_BCMS] = {16, 4, 0};
    for (unsigned b = 0; b < MHF_BCMS; ++b) {
        const struct mcd_resource *r = &db->r[names[b]];
        if (!r->aux || r->aux_bytes < 8 || !u32(r->aux)) return t->status = MHF_BCM_AUX;
        t->bcm_unit[b] = u32(r->aux); t->bcm_width[b] = u16(r->aux + 4); t->bcm_vcd[b] = r->aux[6];
    }
    for (unsigned i = 0; i < MHF_BWS; ++i) {
        for (unsigned b = 0; b < MHF_BCMS; ++b) {
            unsigned commit = b == MHF_BCMS - 1 || t->bcm_vcd[b] != t->bcm_vcd[b + 1];
            if (!mhf_bw_kbps[i]) {t->ib[i][b] = MHF_BCM_CMD(commit, 0, 0, 0); continue;}
            if (b == 2) {t->ib[i][b] = MHF_BCM_CMD(commit, 1, 0, 1u << 2); continue;}
            uint64_t peak = (uint64_t)mhf_bw_kbps[i] * t->bcm_width[b] / buswidth[b];
            peak = peak * 1000 / t->bcm_unit[b];
            uint32_t vote = peak < 1 ? 1 : peak > 0x3fff ? 0x3fff : (uint32_t)peak;
            t->ib[i][b] = MHF_BCM_CMD(commit, 1, vote, vote);
        }
    }
    // a8xx_hfi_send_perf_table: version, type, then the GX entry {count, stride 3, {gx, dep, kHz}...} and the CX entry
    // {count, stride 2, {cx, kHz}...}.
    uint32_t *p = t->perf;
    *p++ = 0; *p++ = MHF_TABLE_GPU_PERF;
    *p++ = gpu_levels; *p++ = 3;
    for (unsigned i = 0; i < gpu_levels; ++i) {*p++ = t->gx[i]; *p++ = t->dep[i]; *p++ = mhf_gpu_levels[i].khz;}
    *p++ = MHF_GMU_LEVELS; *p++ = 2;
    for (unsigned i = 0; i < MHF_GMU_LEVELS; ++i) {*p++ = t->cx[i]; *p++ = mhf_gmu_levels[i].khz;}
    // a6xx_generate_bw_table into struct a6xx_hfi_msg_bw_table without its header: bw_level_num, cnoc_cmds_num, ddr_cmds_num,
    // cnoc_wait_bitmask, ddr_wait_bitmask, cnoc_cmds_addrs[6], cnoc_cmds_data[2][6], ddr_cmds_addrs[8], ddr_cmds_data[16][8].
    uint32_t *w = t->bw;
    w[0] = MHF_BWS; w[1] = 1; w[2] = MHF_BCMS;
    w[3] = 1;                                            // CN0 commits
    for (unsigned b = 0; b < MHF_BCMS; ++b) if (t->ib[0][b] & (1u << 30)) w[4] |= 1u << b;
    w[5] = db->r[MCD_CN0].address;                       // cnoc_cmds_addrs[0]
    w[11] = MHF_BCM_CMD(1, 0, 0, 0);                     // cnoc_cmds_data[0][0]
    w[17] = MHF_BCM_CMD(1, 1, 0, 1);                     // cnoc_cmds_data[1][0]
    for (unsigned b = 0; b < MHF_BCMS; ++b) w[23 + b] = db->r[names[b]].address;   // ddr_cmds_addrs
    for (unsigned i = 0; i < MHF_BWS; ++i) for (unsigned b = 0; b < MHF_BCMS; ++b) w[31 + 8 * i + b] = t->ib[i][b];
    t->perf_index = gpu_levels - 1;
    t->bw_index = mhf_gpu_levels[gpu_levels - 1].bw;
    return t->status = MHF_OK;
}
