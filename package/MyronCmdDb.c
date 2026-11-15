// SPDX-License-Identifier: BSD-2-Clause-Patent
// GPU resources of myron's AOP command DB: see MyronCmdDb.h.
#include "MyronCmdDb.h"

static uint16_t u16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t u32(const uint8_t *p) {return u16(p) | (uint32_t)u16(p + 2) << 16;}
static int fits(size_t at, size_t len, size_t size) {return at <= size && len <= size - at;}

int mcd_parse(const uint8_t *data, size_t bytes, struct mcd_result *out) {
    __builtin_memset(out, 0, sizeof *out);
    if (!data || bytes < 144) return out->status = 1;
    out->version = u32(data);
    if (u32(data + 4) != 0x0c0330dbu) return out->status = 2;
    // cmd_db_header: version, magic, eight rsc_hdr {slv_id, header_offset, data_offset, cnt, version, reserved[3]}, checksum,
    // reserved; data follows at 144. The slave list ends at the first zero slave id.
    static const char names[MCD_RESOURCES][8] = {"gfx.lvl", "mx.lvl", "cx.lvl", {'g','m','x','c','.','l','v','l'}, "SH0", "MC0", "ACV", "CN0"};
    for (unsigned s = 0; s < 8; ++s) {
        const uint8_t *h = data + 8 + 16 * s;
        unsigned slave = u16(h), count = u16(h + 6);
        if (!slave) break;
        size_t entries = 144u + u16(h + 2), aux = 144u + u16(h + 4);
        if (!fits(entries, (size_t)count * 24, bytes) || aux > bytes) return out->status = 3;
        ++out->slaves;
        out->entries += count;
        for (unsigned e = 0; e < count; ++e) {
            const uint8_t *entry = data + entries + 24 * e;   // id[8], priority[2], addr, len, offset
            for (unsigned n = 0; n < MCD_RESOURCES; ++n) {
                unsigned k = 0;
                while (k < 8 && entry[k] == (uint8_t)names[n][k]) ++k;
                if (k != 8) continue;
                uint32_t address = u32(entry + 16);
                size_t len = u16(entry + 20), at = aux + u16(entry + 22);
                if (out->found & (1u << n)) return out->status = 4;
                if (!address || ((address >> 16) & 7) != slave || !fits(at, len, bytes)) return out->status = 3;
                if (((address >> 16) & 7) != (n < MCD_SH0 ? 3u : 5u)) return out->status = 3;
                out->found |= 1u << n;
                out->r[n] = (struct mcd_resource){address, data + at, (uint32_t)len};
            }
        }
    }
    const unsigned required = ((1u << MCD_RESOURCES) - 1) & ~(1u << MCD_GMXC);
    if ((out->found & required) != required) return out->status = 5;
    return out->status = 0;
}

unsigned mcd_arc_levels(const struct mcd_resource *r, uint16_t *levels, unsigned capacity) {
    unsigned n = 0;
    if (!r || !r->aux) return 0;
    for (uint32_t at = 0; at + 2 <= r->aux_bytes && n < capacity; at += 2) {
        uint16_t level = u16(r->aux + at);
        if (!level && n) break;
        levels[n++] = level;
    }
    return n;
}
