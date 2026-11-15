// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lookup of the GPU resources in myron's AOP command DB (reserved-memory aop_cmd_db_region@81c60000, 0x20000 bytes), as
// Linux drivers/soc/qcom/cmd-db.c reads it and sheng's ShengGmuCommandDb.c checks it: the header (version, magic
// 0x0c0330db), up to eight slave headers, 24-byte entries per slave, and the aux data each entry points to. The Adreno 840
// needs the ARC levels gfx.lvl, mx.lvl, cx.lvl and (when present) gmxc.lvl, and the BCMs SH0, MC0, ACV and CN0. Read only;
// the caller passes a CPU copy. Host-tested by MyronCmdDbTests.c.
#ifndef MYRON_CMD_DB_H
#define MYRON_CMD_DB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MCD_BASE 0x81c60000ULL
#define MCD_BYTES 0x20000u
enum {MCD_GFX = 0, MCD_MX, MCD_CX, MCD_GMXC, MCD_SH0, MCD_MC0, MCD_ACV, MCD_CN0, MCD_RESOURCES};
struct mcd_resource {
    uint32_t address;                    // the TCS address (slave id in bits 18:16: 3 ARC, 5 BCM)
    const uint8_t *aux;                  // into the caller's copy
    uint32_t aux_bytes;
};
struct mcd_result {
    int status;                          // 0 ok, 1 argument, 2 magic, 3 corrupt, 4 duplicate, 5 missing (all but gmxc required)
    uint32_t version, slaves, entries, found;   // found: bit per MCD_* resource
    struct mcd_resource r[MCD_RESOURCES];
};
int mcd_parse(const uint8_t *data, size_t bytes, struct mcd_result *out);
// The ARC level list of an ARC resource (u16 levels, ending at the first zero); returns the count, at most `capacity`.
unsigned mcd_arc_levels(const struct mcd_resource *r, uint16_t *levels, unsigned capacity);
#ifdef __cplusplus
}
#endif
#endif
