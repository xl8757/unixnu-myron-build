// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cold start of myron's Adreno 840 GMU: see MyronGmu.h.
#include "MyronGmu.h"

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static int in_range(uint32_t address, uint32_t bytes, uint32_t base, uint32_t size) {
    return address >= base && address - base < size && bytes <= size - (address - base);
}

int mgm_plan(const uint8_t *firmware, size_t bytes, const struct mgm_config *c, struct mgm_block *blocks, unsigned capacity,
             unsigned *count, unsigned *small) {
    *count = 0;
    *small = 0;
    if (!firmware || !c) return -1;
    unsigned n = 0;
    for (size_t at = 0; at < bytes;) {
        if (bytes - at < 16) return -1;
        uint32_t address = le32(firmware + at), size = le32(firmware + at + 4);
        size_t payload = at + 16;
        if (size > bytes - payload) return -1;
        at = payload + size;
        if (!size) continue;
        if (size < 4) {++*small; continue;}
        if (size & 3) return -1;
        struct mgm_block b = {address, size, (uint32_t)payload, 0};
        if (in_range(address, size, 0, MGM_TCM_BYTES)) b.dest = MGM_DEST_ITCM;
        else if (in_range(address, size, MGM_DTCM_BASE, MGM_TCM_BYTES)) b.dest = MGM_DEST_DTCM;
        else if (in_range(address, size, c->icache.iova, c->icache.bytes)) b.dest = MGM_DEST_ICACHE;
        else if (in_range(address, size, c->dummy.iova, c->dummy.bytes)) b.dest = MGM_DEST_DUMMY;
        else return -1;                  // Linux only logs an unmatched block; here it refuses the image
        if (n == capacity) return -1;
        blocks[n++] = b;
    }
    *count = n;
    return n ? 0 : -1;
}

static int rd(const struct mgm_io *io, unsigned w, uint32_t off, uint32_t *v) {return io->read32(io->cookie, w, off, v);}
static int wr(const struct mgm_io *io, unsigned w, uint32_t off, uint32_t v) {return io->write32(io->cookie, w, off, v);}
static int grd(const struct mgm_io *io, uint32_t dword, uint32_t *v) {return rd(io, MGM_WIN_GMU, MGM_GMU(dword), v);}
static int gwr(const struct mgm_io *io, uint32_t dword, uint32_t v) {return wr(io, MGM_WIN_GMU, MGM_GMU(dword), v);}
static void barrier(const struct mgm_io *io) {if (io->barrier) io->barrier(io->cookie);}
static void delay(const struct mgm_io *io, unsigned us) {if (io->delay_us) io->delay_us(io->cookie, us);}

// The TCM register (ITCM/DTCM) or buffer word a block word lands in.
static uint32_t tcm_dword(const struct mgm_block *b, uint32_t at) {
    return b->dest == MGM_DEST_ITCM ? MGM_CM3_ITCM_START + (b->address + at) / 4 : MGM_CM3_DTCM_START + (b->address - MGM_DTCM_BASE + at) / 4;
}
static volatile uint32_t *buffer_word(const struct mgm_config *c, const struct mgm_block *b, uint32_t at) {
    const struct mgm_memory *m = b->dest == MGM_DEST_ICACHE ? &c->icache : &c->dummy;
    return m->cpu + (b->address - m->iova + at) / 4;
}
static int faulted(const struct mgm_io *io, struct mgm_result *r) {
    if (grd(io, MGM_AO_HOST_INTERRUPT_STATUS, &r->ao_status) || grd(io, MGM_GMU2HOST_INTR_INFO, &r->gmu2host)) return -1;
    return (r->ao_status & MGM_GMU_AO_FATAL) || (r->gmu2host & MGM_CM3_FAULT);
}
static enum mgm_status fail(const struct mgm_io *io, struct mgm_result *r, enum mgm_status status) {
    if (r->phase >= 6) {gwr(io, MGM_CM3_SYSRESET, 1); barrier(io);}   // back into reset
    return r->status = status;
}

enum mgm_status mgm_cold_start(const struct mgm_io *io, const struct mgm_config *c, struct mgm_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    if (!io || !io->read32 || !io->write32 || !c || !c->hfi_init || !c->icache.cpu || !c->dummy.cpu || !c->log.cpu || !c->hfi.cpu ||
        c->icache.iova != 0x4000 || c->icache.bytes != 0xffc000 || c->dummy.iova != 0x60000000 || c->dummy.bytes != 0x4000 ||
        c->log.bytes != 0x4000 || (c->log.iova & 0xfff) || c->hfi.bytes != 0x4000 || (c->hfi.iova & 0xfff))
        return r->status = MGM_ARGUMENT;
    // 1: the GMU must be stopped: CM3 in reset, no stale HFI ready bit. Linux cold-boots when TCM retention is off.
    r->phase = 1;
    struct mgm_block blocks[MGM_MAX_BLOCKS];
    unsigned count = 0;
    if (mgm_plan(c->firmware, c->firmware_bytes, c, blocks, MGM_MAX_BLOCKS, &count, &r->small)) return r->status = MGM_FIRMWARE;
    r->blocks = count;
    if (grd(io, MGM_CM3_SYSRESET, &r->sysreset) || grd(io, MGM_HFI_CTRL_STATUS, &r->hfi_before) ||
        rd(io, MGM_WIN_CXMISC, MGM_CX_TCM_RET_CNTL, &r->tcm_ret) || rd(io, MGM_WIN_RSCC, MGM_RSC_STATUS0, &r->rsc_status) ||
        rd(io, MGM_WIN_RSCC, MGM_RSCC_SEQ_BUSY, &r->seq_busy) || grd(io, MGM_RSCC_CONTROL_ACK, &r->rscc_ack))
        return r->status = MGM_IO;
    if (r->sysreset != 1) return r->status = MGM_NOT_IN_RESET;
    if (r->hfi_before & 1) return r->status = MGM_STALE_HFI;
    // a6xx_gmu_resume before fw_start: clear every AO interrupt, unmask only the fatal ones.
    if (gwr(io, MGM_AO_HOST_INTERRUPT_CLR, 0xffffffffu) || gwr(io, MGM_AO_HOST_INTERRUPT_MASK, ~MGM_GMU_AO_FATAL)) return fail(io, r, MGM_IO);
    // 2: veto for FAL10.
    r->phase = 2;
    if (gwr(io, MGM_CX_FALNEXT_INTF, 1) || gwr(io, MGM_CX_FAL_INTF, 1)) return fail(io, r, MGM_IO);
    // 3: the buffers start zeroed, the HFI table is built while the GMU is stopped, then the firmware blocks.
    r->phase = 3;
    const struct mgm_memory *zero[] = {&c->icache, &c->dummy, &c->log};
    for (unsigned m = 0; m < 3; ++m) for (uint32_t i = 0; i < zero[m]->bytes / 4; ++i) zero[m]->cpu[i] = 0;
    if (c->debug.cpu) for (uint32_t i = 0; i < c->debug.bytes / 4; ++i) c->debug.cpu[i] = 0;
    if (c->hfi_init(c->hfi_cookie, c->hfi.cpu, c->hfi.bytes, c->hfi.iova)) return fail(io, r, MGM_HFI_INIT);
    for (unsigned i = 0; i < count; ++i) {
        const struct mgm_block *b = blocks + i;
        for (uint32_t at = 0; at < b->bytes; at += 4) {
            uint32_t word = le32(c->firmware + b->payload_offset + at);
            if (b->dest == MGM_DEST_ITCM || b->dest == MGM_DEST_DTCM) {
                if (gwr(io, tcm_dword(b, at), word)) return fail(io, r, MGM_IO);
            } else *buffer_word(c, b, at) = word;
        }
        if (b->dest == MGM_DEST_ITCM) r->itcm_bytes += b->bytes;
        else if (b->dest == MGM_DEST_DTCM) r->dtcm_bytes += b->bytes;
        else r->memory_bytes += b->bytes;
    }
    barrier(io);
    // 4: every word read back before the CM3 may run it.
    r->phase = 4;
    for (unsigned i = 0; i < count; ++i) {
        const struct mgm_block *b = blocks + i;
        for (uint32_t at = 0; at < b->bytes; at += 4) {
            uint32_t want = le32(c->firmware + b->payload_offset + at), got = 0;
            if (b->dest == MGM_DEST_ITCM || b->dest == MGM_DEST_DTCM) {
                if (grd(io, tcm_dword(b, at), &got)) return fail(io, r, MGM_IO);
            } else got = *buffer_word(c, b, at);
            if (got != want) {
                r->mismatch_dword = b->dest <= MGM_DEST_DTCM ? tcm_dword(b, at) : b->address + at;
                r->mismatch_got = got; r->mismatch_want = want;
                return fail(io, r, MGM_READBACK);
            }
        }
    }
    // 5: a6xx_gmu_fw_start's registers (A8XX values), a6xx_gmu_power_config.
    r->phase = 5;
    if (gwr(io, MGM_CM3_FW_INIT_RESULT, 0) || gwr(io, MGM_CM3_BOOT_CONFIG, 2) || gwr(io, MGM_HFI_QTBL_ADDR, c->hfi.iova) ||
        gwr(io, MGM_HFI_QTBL_INFO, 1) || gwr(io, MGM_AHB_FENCE_RANGE_0, MGM_FENCE_RANGE) || gwr(io, MGM_CM3_CFG, 0x4052) ||
        gwr(io, MGM_GENERAL_10, MGM_GMU_CHIPID) || gwr(io, MGM_GENERAL_8, (c->log.iova & 0xfffff000u) | (c->log.bytes / 0x1000 - 1)))
        return fail(io, r, MGM_IO);
    for (unsigned i = 0; i < 4; ++i) if (wr(io, MGM_WIN_GPU, MGM_GBIF_QSB_SIDE(i), MGM_GBIF_QSB)) return fail(io, r, MGM_IO);
    if (wr(io, MGM_WIN_GPU, MGM_GBIF_CX_CONFIG, MGM_GBIF_CX) || wr(io, MGM_WIN_GPU, MGM_GBIF_CX_CONFIG, MGM_GBIF_CX) ||
        gwr(io, MGM_MRC_GBIF_QOS_CTRL, 0x33) || gwr(io, MGM_SYS_BUS_CONFIG, 1) || gwr(io, MGM_ICACHE_CONFIG, 1) ||
        gwr(io, MGM_DCACHE_CONFIG, 1))
        return fail(io, r, MGM_IO);
    // a6xx_gmu_start: the firmware version in the DTCM picks the ready test.
    if (grd(io, MGM_CM3_DTCM_START + 0xff8, &r->version)) return fail(io, r, MGM_IO);
    r->mask = r->version <= 0x20010004u ? 0xffffffffu : 0x1ffu;
    r->ready = r->version <= 0x20010004u ? 0xbabefaceu : 0x100u;
    if (gwr(io, MGM_CM3_SYSRESET, 1) || gwr(io, MGM_GENERAL_9, 0)) return fail(io, r, MGM_IO);
    barrier(io);
    // 6: release the CM3, wait up to 100 ms for the firmware (gmu_poll_timeout 100 us steps).
    r->phase = 6;
    if (gwr(io, MGM_CM3_SYSRESET, 0)) return fail(io, r, MGM_IO);
    barrier(io);
    for (;;) {
        ++r->fw_polls;
        int f = faulted(io, r);
        if (f < 0 || grd(io, MGM_CM3_FW_INIT_RESULT, &r->fw_init)) return fail(io, r, MGM_IO);
        if (f) return fail(io, r, MGM_FAULT);
        if ((r->fw_init & r->mask) == r->ready) break;
        if (r->fw_polls >= 1000) {grd(io, MGM_PWR_CLK_STATUS, &r->pwr_clk); return fail(io, r, MGM_FW_TIMEOUT);}
        delay(io, 100);
    }
    // 7: a6xx_gmu_hfi_start: HFI_CTRL_INIT, wait up to 10 ms for HFI_CTRL_STATUS bit 0.
    r->phase = 7;
    if (gwr(io, MGM_HFI_CTRL_INIT, 1)) return fail(io, r, MGM_IO);
    barrier(io);
    for (;;) {
        ++r->hfi_polls;
        int f = faulted(io, r);
        if (f < 0 || grd(io, MGM_HFI_CTRL_STATUS, &r->hfi_status)) return fail(io, r, MGM_IO);
        if (f) return fail(io, r, MGM_FAULT);
        if (r->hfi_status & 1) break;
        if (r->hfi_polls >= 100) return fail(io, r, MGM_HFI_TIMEOUT);
        delay(io, 100);
    }
    r->phase = 8;
    grd(io, MGM_PWR_CLK_STATUS, &r->pwr_clk);
    return r->status = MGM_OK;
}
