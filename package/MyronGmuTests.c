// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of MyronGmu.c: cc -std=c11 -Wall -Wextra -Werror MyronGmuTests.c MyronGmu.c -o t && ./t [gen80200_gmu.bin]
// With the firmware path it also plans the real image (kept out of every repository).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MyronGmu.h"

static int failures;
#define CHECK(c) do {if (!(c)) {printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures;}} while (0)

static const uint32_t sizes[MGM_WINDOWS] = {MGM_GMU_SIZE, MGM_RSCC_SIZE, MGM_GPU_SIZE, MGM_CXMISC_SIZE};
struct fake {
    uint32_t *w[MGM_WINDOWS];
    unsigned writes, sysreset_writes, ready_after;
    int itcm_dead, fw_dead, hfi_dead;
    unsigned fault_at_poll;
    unsigned polls;
};
static uint32_t *greg(struct fake *f, uint32_t dword) {return &f->w[MGM_WIN_GMU][MGM_GMU(dword) / 4];}
static int fake_rd(void *cookie, unsigned w, uint32_t off, uint32_t *v) {
    struct fake *f = cookie;
    if (w >= MGM_WINDOWS || off + 4 > sizes[w] || (off & 3)) return -1;
    if (w == MGM_WIN_GMU && off == MGM_GMU(MGM_CM3_FW_INIT_RESULT) && *greg(f, MGM_CM3_SYSRESET) == 0 && !f->fw_dead) {
        if (++f->polls == f->fault_at_poll) *greg(f, MGM_AO_HOST_INTERRUPT_STATUS) = 1;   // watchdog bite
        if (f->polls >= f->ready_after) *greg(f, MGM_CM3_FW_INIT_RESULT) = 0xabcd0100;     // low 9 bits 0x100
    }
    *v = f->w[w][off / 4];
    return 0;
}
static int fake_wr(void *cookie, unsigned w, uint32_t off, uint32_t v) {
    struct fake *f = cookie;
    if (w >= MGM_WINDOWS || off + 4 > sizes[w] || (off & 3)) return -1;
    ++f->writes;
    if (w == MGM_WIN_GMU && off >= MGM_GMU(MGM_CM3_ITCM_START) && off < MGM_GMU(MGM_CM3_ITCM_START) + MGM_TCM_BYTES && f->itcm_dead) return 0;
    f->w[w][off / 4] = v;
    if (w == MGM_WIN_GMU && off == MGM_GMU(MGM_CM3_SYSRESET)) ++f->sysreset_writes;
    if (w == MGM_WIN_GMU && off == MGM_GMU(MGM_HFI_CTRL_INIT) && v == 1 && !f->hfi_dead) *greg(f, MGM_HFI_CTRL_STATUS) = 1;
    return 0;
}

// A firmware with the structure of gen80200_gmu.bin: five type descriptors, two single bytes at ITCM 0, extent descriptors,
// then the ITCM image, an icache image at 0x4000 and the two DTCM tails, the second carrying the version word at DTCM + 0xff8.
static size_t put(uint8_t *out, size_t at, uint32_t address, uint32_t size, uint32_t type, uint32_t value, const uint8_t *data) {
    uint32_t h[4] = {address, size, type, value};
    memcpy(out + at, h, 16);
    if (size) memcpy(out + at + 16, data, size);
    return at + 16 + size;
}
static size_t synthetic(uint8_t *out) {
    static uint8_t itcm[0x1fc], icache[0x800], dtcm1[0x28], dtcm2[0x18];
    for (unsigned i = 0; i < sizeof itcm; ++i) itcm[i] = (uint8_t)(i * 7 + 1);
    for (unsigned i = 0; i < sizeof icache; ++i) icache[i] = (uint8_t)(i * 13 + 3);
    for (unsigned i = 0; i < sizeof dtcm1; ++i) dtcm1[i] = (uint8_t)i;
    memset(dtcm2, 0xff, sizeof dtcm2);
    uint32_t version = 0x50030019;
    memcpy(dtcm2 + 4, &version, 4);   // 0x10007fdc + 4 = DTCM + 0xff8 dwords
    const uint8_t one = 7;
    size_t at = 0;
    for (uint32_t t = 2; t <= 6; ++t) at = put(out, at, 0, 0, t, 0x50000000 + t, NULL);
    at = put(out, at, 0, 1, 2, 0, &one);
    at = put(out, at, 0, 1, 3, 0, &one);
    at = put(out, at, 0x4000, 0, 1, sizeof icache, NULL);
    at = put(out, at, 0x60000000, 0, 7, 0x1000, NULL);
    at = put(out, at, 0, sizeof itcm, 0xefbeadde, 0xefbeadde, itcm);
    at = put(out, at, 0x4000, sizeof icache, 0xefbeadde, 0xefbeadde, icache);
    at = put(out, at, 0x10007f98, sizeof dtcm1, 0xefbeadde, 0xefbeadde, dtcm1);
    return put(out, at, 0x10007fdc, sizeof dtcm2, 0xefbeadde, 0xefbeadde, dtcm2);
}

static int hfi_calls;
static int fake_hfi(void *cookie, volatile uint32_t *shared, uint32_t bytes, uint32_t iova) {
    (void)cookie;
    ++hfi_calls;
    return !(shared && bytes == 0x4000 && iova);
}
static uint8_t firmware[0x2000];
static size_t firmware_bytes;
static struct mgm_config config(void) {
    static uint32_t *icache, dummy[0x1000], debug[0x1c00], log[0x1000], hfi[0x1000];
    if (!icache) icache = calloc(0xffc000 / 4, 4);
    struct mgm_config c = {firmware, firmware_bytes, {icache, 0xffc000, 0x4000}, {dummy, 0x4000, 0x60000000},
                           {debug, 0x7000, 0x60400000}, {log, 0x4000, 0x60404000}, {hfi, 0x4000, 0x60408000}, fake_hfi, NULL};
    return c;
}
static struct fake *fresh(struct fake *f) {
    for (unsigned i = 0; i < MGM_WINDOWS; ++i) {
        if (!f->w[i]) f->w[i] = calloc(sizes[i] / 4, 4);
        memset(f->w[i], 0, sizes[i]);
    }
    uint32_t *keep[MGM_WINDOWS];
    memcpy(keep, f->w, sizeof keep);
    memset(f, 0, sizeof *f);
    memcpy(f->w, keep, sizeof keep);
    *greg(f, MGM_CM3_SYSRESET) = 1;
    f->ready_after = 3;
    return f;
}
static struct mgm_io io_of(struct fake *f) {return (struct mgm_io){f, fake_rd, fake_wr, NULL, NULL};}

static void test_layout(void) {
    CHECK(MGM_GMU(MGM_CM3_SYSRESET) == 0x47000 && MGM_GMU_BASE + MGM_GMU(MGM_CM3_SYSRESET) == 0x3d7e000);
    CHECK(MGM_GMU_BASE + MGM_GMU(MGM_CM3_ITCM_START) == 0x3d6d000 && MGM_GMU_BASE + MGM_GMU(MGM_CM3_DTCM_START + 0xff8) == 0x3d74fe0);
    CHECK(MGM_GMU_BASE + MGM_GMU(MGM_AHB_FENCE_RANGE_0) == 0x3d8ec44 && MGM_GMU(MGM_AHB_FENCE_RANGE_0) + 4 <= MGM_GMU_SIZE);
    CHECK(MGM_GMU_BASE + MGM_GMU(MGM_GENERAL_10) == 0x3d7e728 && MGM_GMU_BASE + MGM_GMU(MGM_CX_FALNEXT_INTF) == 0x3d7dfb4);
    CHECK(MGM_GPU_BASE + MGM_GBIF_QSB_SIDE(3) == 0x3d0f018 && MGM_RSCC_BASE + MGM_RSCC_SEQ_BUSY == 0x3d50404);
    CHECK(MGM_CXMISC_BASE + MGM_CX_TCM_RET_CNTL == 0x3d9e0e4 && MGM_FENCE_RANGE == 0x80c808c0);
}

static void test_plan(void) {
    struct mgm_config c = config();
    struct mgm_block b[MGM_MAX_BLOCKS];
    unsigned n = 0, small = 0;
    CHECK(mgm_plan(firmware, firmware_bytes, &c, b, MGM_MAX_BLOCKS, &n, &small) == 0 && n == 4 && small == 2);
    CHECK(b[0].dest == MGM_DEST_ITCM && b[0].bytes == 0x1fc && b[1].dest == MGM_DEST_ICACHE && b[1].address == 0x4000);
    CHECK(b[2].dest == MGM_DEST_DTCM && b[3].dest == MGM_DEST_DTCM && b[3].address == 0x10007fdc);
    CHECK(mgm_plan(firmware, firmware_bytes - 1, &c, b, MGM_MAX_BLOCKS, &n, &small) != 0 && n == 0);   // truncated
    CHECK(mgm_plan(firmware, firmware_bytes, &c, b, 3, &n, &small) != 0);                             // no room
    uint8_t odd[64];
    size_t at = put(odd, 0, 0x30000000, 8, 0, 0, (const uint8_t *)"abcdefgh");   // outside every region
    CHECK(mgm_plan(odd, at, &c, b, MGM_MAX_BLOCKS, &n, &small) != 0);
}

static void test_cold_start(void) {
    static struct fake f;
    fresh(&f);
    struct mgm_io io = io_of(&f);
    struct mgm_config c = config();
    struct mgm_result r;
    hfi_calls = 0;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_OK && r.phase == 8 && hfi_calls == 1);
    CHECK(r.blocks == 4 && r.small == 2 && r.itcm_bytes == 0x1fc && r.dtcm_bytes == 0x40 && r.memory_bytes == 0x800);
    CHECK(r.version == 0x50030019 && r.mask == 0x1ff && r.ready == 0x100 && (r.fw_init & 0x1ff) == 0x100 && r.fw_polls == 3);
    CHECK(r.hfi_status == 1 && r.hfi_polls == 1 && r.sysreset == 1);
    CHECK(*greg(&f, MGM_CM3_SYSRESET) == 0 && f.sysreset_writes == 2);
    CHECK(*greg(&f, MGM_GENERAL_10) == 0x08020100 && *greg(&f, MGM_AHB_FENCE_RANGE_0) == 0x80c808c0 && *greg(&f, MGM_CM3_CFG) == 0x4052);
    CHECK(*greg(&f, MGM_GENERAL_8) == (0x60404000u | 3) && *greg(&f, MGM_HFI_QTBL_ADDR) == 0x60408000 && *greg(&f, MGM_HFI_QTBL_INFO) == 1);
    CHECK(*greg(&f, MGM_CM3_BOOT_CONFIG) == 2 && *greg(&f, MGM_CX_FAL_INTF) == 1 && *greg(&f, MGM_CX_FALNEXT_INTF) == 1);
    CHECK(*greg(&f, MGM_MRC_GBIF_QOS_CTRL) == 0x33 && *greg(&f, MGM_SYS_BUS_CONFIG) == 1 && *greg(&f, MGM_ICACHE_CONFIG) == 1);
    CHECK(f.w[MGM_WIN_GPU][MGM_GBIF_QSB_SIDE(0) / 4] == 0x00071e20 && f.w[MGM_WIN_GPU][MGM_GBIF_CX_CONFIG / 4] == 0x20023000);
    CHECK(*greg(&f, MGM_AO_HOST_INTERRUPT_MASK) == ~0x29u);
    // The images landed: ITCM word 1, the icache's first word, the version word in the DTCM.
    uint32_t w1;
    memcpy(&w1, firmware + 0x50 + 0x11 + 0x11 + 0x20 + 16 + 4, 4);   // past 5 descriptors, 2 single bytes, 2 descriptors
    CHECK(*greg(&f, MGM_CM3_ITCM_START + 1) == w1);
    CHECK(*greg(&f, MGM_CM3_DTCM_START + 0xff8) == 0x50030019);
    CHECK(c.icache.cpu[0] != 0 && c.icache.cpu[0x800 / 4] == 0);
    // Nothing in RSCC or CX_MISC is written on A8XX.
    for (uint32_t i = 0; i < MGM_RSCC_SIZE / 4; ++i) if (f.w[MGM_WIN_RSCC][i]) {CHECK(0); break;}
    for (uint32_t i = 0; i < MGM_CXMISC_SIZE / 4; ++i) if (f.w[MGM_WIN_CXMISC][i]) {CHECK(0); break;}
}

static void test_refusals(void) {
    static struct fake f;
    struct mgm_io io = io_of(&f);
    struct mgm_config c = config();
    struct mgm_result r;
    // A GMU that is not in reset is left alone.
    fresh(&f);
    *greg(&f, MGM_CM3_SYSRESET) = 0;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_NOT_IN_RESET && f.writes == 0);
    fresh(&f);
    *greg(&f, MGM_HFI_CTRL_STATUS) = 1;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_STALE_HFI && f.writes == 0);
    // ITCM writes that do not stick: refused at the readback, the CM3 never leaves reset.
    fresh(&f);
    f.itcm_dead = 1;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_READBACK && r.phase == 4 && f.sysreset_writes == 0);
    CHECK(r.mismatch_dword == MGM_CM3_ITCM_START && *greg(&f, MGM_CM3_SYSRESET) == 1);
    // Firmware that never reports ready: 1000 polls, then back into reset.
    fresh(&f);
    f.fw_dead = 1;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_FW_TIMEOUT && r.fw_polls == 1000 && *greg(&f, MGM_CM3_SYSRESET) == 1);
    // A watchdog bite while waiting: a fault, back into reset.
    fresh(&f);
    f.fault_at_poll = 1;
    f.ready_after = 5;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_FAULT && r.ao_status == 1 && *greg(&f, MGM_CM3_SYSRESET) == 1);
    // HFI never ready.
    fresh(&f);
    f.hfi_dead = 1;
    CHECK(mgm_cold_start(&io, &c, &r) == MGM_HFI_TIMEOUT && r.phase == 7 && r.hfi_polls == 100);
    // Buffers that do not follow the a8xx GMU layout.
    fresh(&f);
    struct mgm_config bad = c;
    bad.icache.iova = 0x8000;
    CHECK(mgm_cold_start(&io, &bad, &r) == MGM_ARGUMENT && f.writes == 0);
}

static void test_real(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {printf("FAIL cannot open %s\n", path); ++failures; return;}
    static uint8_t data[0x40000];
    size_t bytes = fread(data, 1, sizeof data, file);
    fclose(file);
    struct mgm_config c = config();
    struct mgm_block b[MGM_MAX_BLOCKS];
    unsigned n = 0, small = 0;
    CHECK(mgm_plan(data, bytes, &c, b, MGM_MAX_BLOCKS, &n, &small) == 0 && n == 4 && small == 2);
    CHECK(b[0].dest == MGM_DEST_ITCM && b[0].address == 0 && b[0].bytes == 0x1fc);
    CHECK(b[1].dest == MGM_DEST_ICACHE && b[1].address == 0x4000 && b[1].bytes == 0x1b098);
    CHECK(b[2].dest == MGM_DEST_DTCM && b[2].address == 0x10007f98 && b[2].bytes == 0x28);
    CHECK(b[3].dest == MGM_DEST_DTCM && b[3].address == 0x10007fdc && b[3].bytes == 0x18);
    uint32_t version;
    memcpy(&version, data + b[3].payload_offset + 4, 4);
    CHECK(version == 0x50030019);
    printf("real firmware %s: %zu bytes, %u blocks, version %08x\n", path, bytes, n, version);
}

int main(int argc, char **argv) {
    firmware_bytes = synthetic(firmware);
    test_layout();
    test_plan();
    test_cold_start();
    test_refusals();
    if (argc > 1) test_real(argv[1]);
    if (failures) {printf("%d failures\n", failures); return 1;}
    printf("MyronGmu: all tests passed\n");
    return 0;
}
