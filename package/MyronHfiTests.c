// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of MyronHfi.c: cc -std=c11 -Wall -Wextra -Werror MyronHfiTests.c MyronHfi.c -o t && ./t
#include <stdio.h>
#include <string.h>
#include "MyronHfi.h"

static int failures;
#define CHECK(c) do {if (!(c)) {printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures;}} while (0)

// gfx.lvl as myron-gpu=4 read its first 12 levels; the rest, mx/cx/gmxc and the BCM aux data are stand-ins.
static uint8_t gfx[40], gmxc[32], mx[26], cx[26], sh0[8], mc0[8], acv[8];
static void put16(uint8_t *a, unsigned n, const uint16_t *v) {for (unsigned i = 0; i < n; ++i) {a[2 * i] = (uint8_t)v[i]; a[2 * i + 1] = (uint8_t)(v[i] >> 8);}}
static void bcm(uint8_t *a, uint32_t unit, uint16_t width, uint8_t vcd) {
    a[0] = (uint8_t)unit; a[1] = (uint8_t)(unit >> 8); a[2] = (uint8_t)(unit >> 16); a[3] = (uint8_t)(unit >> 24);
    a[4] = (uint8_t)width; a[5] = (uint8_t)(width >> 8); a[6] = vcd; a[7] = 0;
}
static struct mcd_result db(int with_gmxc) {
    static const uint16_t g[17] = {0, 0x32, 0x33, 0x34, 0x36, 0x38, 0x3c, 0x40, 0x4c, 0x50, 0x80, 0x90, 0xc0, 0x100, 0x140, 0x1a0, 0x1c0};
    static const uint16_t gm[16] = {0, 0x30, 0x34, 0x38, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1a0, 0x1c0, 0, 0, 0, 0};
    static const uint16_t m[13] = {0, 0x10, 0x30, 0x38, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1c0, 0x1f0, 0};
    static const uint16_t c[13] = {0, 0x10, 0x30, 0x38, 0x40, 0x60, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1c0, 0};
    put16(gfx, 17, g); put16(gmxc, 16, gm); put16(mx, 13, m); put16(cx, 13, c);
    bcm(sh0, 1000, 16, 0); bcm(mc0, 1000, 4, 1); bcm(acv, 1000, 4, 1);
    struct mcd_result r;
    memset(&r, 0, sizeof r);
    r.found = 0xff & ~(with_gmxc ? 0u : 1u << MCD_GMXC);
    r.r[MCD_GFX] = (struct mcd_resource){0x30010, gfx, 34};
    r.r[MCD_MX] = (struct mcd_resource){0x30020, mx, 26};
    r.r[MCD_CX] = (struct mcd_resource){0x30030, cx, 26};
    if (with_gmxc) r.r[MCD_GMXC] = (struct mcd_resource){0x30040, gmxc, 32};
    r.r[MCD_SH0] = (struct mcd_resource){0x50004, sh0, 8};
    r.r[MCD_MC0] = (struct mcd_resource){0x50000, mc0, 8};
    r.r[MCD_ACV] = (struct mcd_resource){0x50068, acv, 8};
    r.r[MCD_CN0] = (struct mcd_resource){0x50030, NULL, 0};
    return r;
}

static void test_tables(void) {
    struct mcd_result d = db(1);
    static struct mhf_tables t;
    CHECK(mhf_build(&d, &t) == MHF_OK);
    // GX: off, then 160 MHz @ 0x32 = gfx index 1, gmxc first >= 0x32 is index 2 (0x34).
    CHECK(t.gx[0] == 0 && t.gx[1] == (0x32u << 16 | 2 << 8 | 1));
    CHECK(t.gx[7] == (0x40u << 16 | 4 << 8 | 7));                 // 422 MHz @ 0x40: gfx 7, gmxc 4
    CHECK(t.dep[0] == 0xffffffff && t.dep[1] == ((0x3fffu << 14) | 3 << 8 | 0xff));   // mx first >= 0x32 is 0x38 at 3
    CHECK(t.dep[7] == ((0x3fffu << 14) | 4 << 8 | 0xff));
    CHECK(t.cx[0] == 0 && t.cx[1] == (0x38u << 16 | 3 << 8 | 3) && t.cx[2] == (0x80u << 16 | 5 << 8 | 6));
    // BCMs: SH0 (vcd 0) commits, MC0 and ACV share vcd 1 so only ACV commits.
    CHECK(t.ib[0][0] == MHF_BCM_CMD(1, 0, 0, 0) && t.ib[0][1] == MHF_BCM_CMD(0, 0, 0, 0) && t.ib[0][2] == MHF_BCM_CMD(1, 0, 0, 0));
    uint32_t v1 = 0x209a8e;                                          // SH0: bw * 16 / 16 * 1000 / 1000, clamped to 0x3fff
    CHECK(t.ib[1][0] == MHF_BCM_CMD(1, 1, 0x3fff, 0x3fff) && v1 > 0x3fff);
    CHECK(t.ib[1][2] == MHF_BCM_CMD(1, 1, 0, 4));                    // ACV perfmode on every nonzero bandwidth
    bcm(sh0, 0x186a0, 16, 0);                                        // unit 100000: 2136718 * 1000 / 100000 = 21367 -> clamped
    bcm(mc0, 0x989680, 4, 1);                                        // unit 10000000: 2136718 * 4 / 4 * 1000 / 1e7 = 213
    CHECK(mhf_build(&d, &t) == MHF_OK && t.ib[1][1] == MHF_BCM_CMD(0, 1, 213, 213));
    // Perf table: version, type, GX entry, CX entry.
    CHECK(t.gx[18] == (0x1c0u << 16 | 11 << 8 | 18));              // 1200 MHz @ 0x1c0: gfx 18, gmxc 11
    CHECK(t.dep[18] == ((0x3fffu << 14) | 10 << 8 | 0xff));         // mx first >= 0x1c0 is 0x1c0 at 10
    CHECK(t.perf[0] == 0 && t.perf[1] == 1 && t.perf[2] == 19 && t.perf[3] == 3);
    CHECK(t.perf[4] == t.gx[0] && t.perf[5] == 0xffffffff && t.perf[6] == 0 && t.perf[4 + 3 * 18 + 2] == 1200000);
    CHECK(t.perf[61] == 3 && t.perf[62] == 2 && t.perf[63] == t.cx[0] && t.perf[65] == 475000 && t.perf[67] == 700000);
    CHECK(MHF_PERF_WORDS == 69 && (1 + MHF_PERF_WORDS) * 4 == 3 * 4 + 2 * 8 + 8 * 3 * 19 + 3 * 2 * 4);
    // BW table at the a6xx_hfi_msg_bw_table offsets.
    CHECK(t.bw[0] == 12 && t.bw[1] == 1 && t.bw[2] == 3 && t.bw[3] == 1 && t.bw[4] == 5);   // SH0 and ACV commit
    CHECK(t.bw[5] == 0x50030 && t.bw[11] == 0x40000000 && t.bw[17] == 0x60000001);
    CHECK(t.bw[23] == 0x50004 && t.bw[24] == 0x50000 && t.bw[25] == 0x50068 && t.bw[26] == 0);
    CHECK(t.bw[31 + 8 * 1 + 1] == t.ib[1][1] && t.bw[31 + 8 * 11 + 2] == t.ib[11][2] && t.bw[158] == 0 && (1 + MHF_BW_WORDS) * 4 == 640);
    CHECK(t.perf_index == 18 && t.bw_index == 11);
}

static void test_refusals(void) {
    static struct mhf_tables t;
    struct mcd_result d = db(0);
    CHECK(mhf_build(&d, &t) == MHF_OK);                              // no GMxC: MX is the GX secondary
    CHECK(t.gx[1] == (0x32u << 16 | 3 << 8 | 1));                   // mx first >= 0x32 is 0x38 at 3
    d = db(1);
    d.r[MCD_GFX].aux_bytes = 14;                                     // gfx.lvl up to 0x3c only: 422 MHz has no level
    CHECK(mhf_build(&d, &t) == MHF_GFX_LEVEL && t.failed_level == 0x40);
    d = db(1);
    d.r[MCD_SH0].aux_bytes = 4;
    CHECK(mhf_build(&d, &t) == MHF_BCM_AUX);
    d = db(1);
    d.status = 5;
    CHECK(mhf_build(&d, &t) == MHF_DB);
    d = db(1);
    d.found &= ~(1u << MCD_CN0);
    CHECK(mhf_build(&d, &t) == MHF_DB);
    CHECK(mhf_build(NULL, &t) == MHF_DB);
}

int main(void) {
    test_tables();
    test_refusals();
    if (failures) {printf("%d failures\n", failures); return 1;}
    printf("MyronHfi: all tests passed\n");
    return 0;
}
