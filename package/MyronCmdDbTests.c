// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of MyronCmdDb.c: cc -std=c11 -Wall -Wextra -Werror MyronCmdDbTests.c MyronCmdDb.c -o t && ./t
#include <stdio.h>
#include <string.h>
#include "MyronCmdDb.h"

static int failures;
#define CHECK(c) do {if (!(c)) {printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures;}} while (0)

static uint8_t db[MCD_BYTES];
static void p16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void p32(uint8_t *p, uint32_t v) {p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16));}
static void entry(uint8_t *at, const char *id, uint32_t address, uint16_t len, uint16_t offset) {
    memset(at, 0, 24);
    memcpy(at, id, strlen(id) < 8 ? strlen(id) : 8);
    p32(at + 16, address); p16(at + 20, len); p16(at + 22, offset);
}
// Two slaves as the AOP lays them out: ARC (3) with gfx/mx/cx/gmxc and a filler, BCM (5) with SH0/MC0/ACV/CN0.
static void build(int with_gmxc) {
    memset(db, 0, sizeof db);
    p32(db, 0x00010000); p32(db + 4, 0x0c0330db);
    uint8_t *arc = db + 8, *bcm = db + 24;
    p16(arc, 3); p16(arc + 2, 0x000); p16(arc + 4, 0x400); p16(arc + 6, with_gmxc ? 5 : 4);
    p16(bcm, 5); p16(bcm + 2, 0x200); p16(bcm + 4, 0x500); p16(bcm + 6, 4);
    uint8_t *data = db + 144;
    entry(data + 0, "ddr.lvl", 0x30000, 8, 0x00);
    entry(data + 24, "gfx.lvl", 0x30010, 12, 0x10);
    entry(data + 48, "mx.lvl", 0x30020, 8, 0x20);
    entry(data + 72, "cx.lvl", 0x30030, 8, 0x30);
    if (with_gmxc) entry(data + 96, "gmxc.lvl", 0x30040, 8, 0x40);
    const uint16_t gfx[] = {0, 0x30, 0x40, 0x80, 0x100, 0}, mx[] = {0, 0x40, 0x80, 0x100};
    for (unsigned i = 0; i < 6; ++i) p16(data + 0x400 + 0x10 + 2 * i, gfx[i]);
    for (unsigned i = 0; i < 4; ++i) p16(data + 0x400 + 0x20 + 2 * i, mx[i]);
    entry(data + 0x200, "SH0", 0x50000, 8, 0x00);
    entry(data + 0x218, "MC0", 0x50004, 8, 0x08);
    entry(data + 0x230, "ACV", 0x50008, 8, 0x10);
    entry(data + 0x248, "CN0", 0x5000c, 8, 0x18);
}

static void test_parse(void) {
    struct mcd_result r;
    build(1);
    CHECK(mcd_parse(db, sizeof db, &r) == 0 && r.slaves == 2 && r.entries == 9 && r.found == 0xff);
    CHECK(r.r[MCD_GFX].address == 0x30010 && r.r[MCD_GFX].aux_bytes == 12 && r.r[MCD_CN0].address == 0x5000c);
    uint16_t levels[16];
    unsigned n = mcd_arc_levels(&r.r[MCD_GFX], levels, 16);
    CHECK(n == 5 && levels[0] == 0 && levels[1] == 0x30 && levels[4] == 0x100);   // the leading zero is kept, the pad dropped
    CHECK(mcd_arc_levels(&r.r[MCD_MX], levels, 16) == 4 && levels[3] == 0x100);
    build(0);
    CHECK(mcd_parse(db, sizeof db, &r) == 0 && r.found == (0xffu & ~(1u << MCD_GMXC)));   // gmxc is optional
}

static void test_refusals(void) {
    struct mcd_result r;
    build(1);
    db[4] = 0;
    CHECK(mcd_parse(db, sizeof db, &r) == 2);
    build(1);
    memcpy(db + 144 + 48, "xx.lvl\0\0", 8);                       // mx.lvl gone
    CHECK(mcd_parse(db, sizeof db, &r) == 5);
    build(1);
    memcpy(db + 144 + 0, "gfx.lvl\0", 8);                         // gfx.lvl twice
    CHECK(mcd_parse(db, sizeof db, &r) == 4);
    build(1);
    p32(db + 144 + 24 + 16, 0x50010);                             // gfx.lvl claims the BCM slave
    CHECK(mcd_parse(db, sizeof db, &r) == 3);
    build(1);
    p16(db + 144 + 0x248 + 22, 0xfff0);                           // CN0 aux past the end
    CHECK(mcd_parse(db, 0x1000, &r) == 3);
    CHECK(mcd_parse(NULL, sizeof db, &r) == 1 && mcd_parse(db, 100, &r) == 1);
}

int main(void) {
    test_parse();
    test_refusals();
    if (failures) {printf("%d failures\n", failures); return 1;}
    printf("MyronCmdDb: all tests passed\n");
    return 0;
}
