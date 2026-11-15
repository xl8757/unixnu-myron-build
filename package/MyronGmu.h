// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cold start of myron's Adreno 840 GMU (gen8_2_1, gmu_chipid 0x8020100, gen80200_gmu.bin), after Linux mainline
// a6xx_gmu_fw_start / a6xx_gmu_fw_load / a6xx_gmu_start / a6xx_gmu_hfi_start on their A8XX paths and cross-checked against
// the stock KGSL module (gen8_gmu_register_config, gen8_gmu_device_start); the derivation is in
// myron-work/gpu/GEN8-GMU-COLDSTART.md. Differences from the A740 sequence of sheng: no RSC/PDC programming (the AOP owns
// it on A8XX; a first cold boot does not even run the RSC wake handshake), no TCM retention write, the A8XX FAL interface
// registers, the A840 fence range, GBIF values and chip id, and a firmware whose version selects the 0x1ff/0x100 ready test.
// No device access here: MyronGpuPower.cpp passes the MMIO windows and the GMU memory. Host-tested by MyronGmuTests.c.
#ifndef MYRON_GMU_H
#define MYRON_GMU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Windows (DT): the GMU block (qcom,gmu@3d37000, 0x68000), RSCC (GPU base + 0x50000), the GPU block (GBIF) and CX_MISC.
enum {MGM_WIN_GMU = 0, MGM_WIN_RSCC = 1, MGM_WIN_GPU = 2, MGM_WIN_CXMISC = 3, MGM_WINDOWS = 4};
#define MGM_GMU_BASE 0x03d37000ULL
#define MGM_GMU_SIZE 0x68000ULL
#define MGM_RSCC_BASE 0x03d50000ULL
#define MGM_RSCC_SIZE 0x10000ULL
#define MGM_GPU_BASE 0x03d00000ULL
#define MGM_GPU_SIZE 0x40000ULL
#define MGM_CXMISC_BASE 0x03d9e000ULL
#define MGM_CXMISC_SIZE 0x2000ULL
// GMU registers are dword offsets from the GPU base; the GMU window starts 0x37000 bytes into it.
#define MGM_GMU(dword) ((uint32_t)((dword) * 4u - 0x37000u))

#define MGM_CM3_ITCM_START 0x1b400
#define MGM_CM3_DTCM_START 0x1c400
#define MGM_ICACHE_CONFIG 0x1f400
#define MGM_DCACHE_CONFIG 0x1f401
#define MGM_SYS_BUS_CONFIG 0x1f40f
#define MGM_MRC_GBIF_QOS_CTRL 0x1f50b
#define MGM_PWR_CLK_STATUS 0x1f7e8
#define MGM_CX_FAL_INTF 0x1f7ec
#define MGM_CX_FALNEXT_INTF 0x1f7ed
#define MGM_CM3_SYSRESET 0x1f800
#define MGM_CM3_BOOT_CONFIG 0x1f801
#define MGM_CM3_FW_INIT_RESULT 0x1f81c
#define MGM_CM3_CFG 0x1f82d
#define MGM_HFI_CTRL_STATUS 0x1f980
#define MGM_HFI_QTBL_INFO 0x1f984
#define MGM_HFI_QTBL_ADDR 0x1f985
#define MGM_HFI_CTRL_INIT 0x1f986
#define MGM_GMU2HOST_INTR_CLR 0x1f991
#define MGM_GMU2HOST_INTR_INFO 0x1f992
#define MGM_GMU2HOST_INTR_MASK 0x1f993
#define MGM_HOST2GMU_INTR_SET 0x1f994
#define MGM_GENERAL_8 0x1f9c8
#define MGM_GENERAL_9 0x1f9c9
#define MGM_GENERAL_10 0x1f9ca
#define MGM_AO_HOST_INTERRUPT_CLR 0x23b04
#define MGM_AO_HOST_INTERRUPT_STATUS 0x23b05
#define MGM_AO_HOST_INTERRUPT_MASK 0x23b06
#define MGM_RSCC_CONTROL_ACK 0x23b08
#define MGM_AHB_FENCE_RANGE_0 0x23b11
// The a8xx busy counter (Linux a8xx hw_init / a6xx_gmu_get_counter): which busy signals count, the XOCLK_0 source
// (countable 5) and the 64-bit counter itself. Used by MyronGpuPower.cpp's DCVS.
#define MGM_AO_GPU_CX_BUSY_MASK 0x23b0e
#define MGM_POWER_COUNTER_ENABLE 0x1fc10
#define MGM_POWER_COUNTER_SELECT_XO0 0x1fc40
#define MGM_POWER_COUNTER_XO0_L 0x1fca0
#define MGM_POWER_COUNTER_XO0_H 0x1fca1
// The GPU block (byte offsets): GBIF_CX_CONFIG 0x3c00, GBIF_QSB_SIDE0..3 0x3c03..0x3c06 (dwords).
#define MGM_GBIF_CX_CONFIG (0x3c00 * 4)
#define MGM_GBIF_QSB_SIDE(n) ((0x3c03 + (n)) * 4)
// RSCC (byte offsets): GPU_RSCC_RSC_STATUS0_DRV0 0x4, RSCC_SEQ_BUSY_DRV0 0x101 (dwords). CX_MISC: TCM_RET_CNTL 0x39.
#define MGM_RSC_STATUS0 (0x004 * 4)
#define MGM_RSCC_SEQ_BUSY (0x101 * 4)
#define MGM_CX_TCM_RET_CNTL (0x39 * 4)

#define MGM_GMU_CHIPID 0x08020100u        // a6xx_catalog.c gmu_chipid of chip id 0x44050a01
#define MGM_FENCE_RANGE (0x80000000u | (0x32u << 18) | 0x8c0u)
#define MGM_GBIF_QSB 0x00071e20u          // a840_gbif
#define MGM_GBIF_CX 0x20023000u
#define MGM_GMU_AO_FATAL 0x29u            // A6XX_GMU_IRQ_MASK: WDOG_BITE | FENCE_ERR | HOST_AHB_BUS_ERROR
#define MGM_CM3_FAULT (1u << 23)          // A6XX_HFI_IRQ_MASK
#define MGM_DTCM_BASE 0x10004000u         // a8xx dtcm_base
#define MGM_TCM_BYTES 0x4000u

struct mgm_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*barrier)(void *cookie);
    void (*delay_us)(void *cookie, unsigned us);
};
// One GMU buffer: its CPU view (uncached or write-combined; the GMU reads it through its SMMU context) and GMU address.
struct mgm_memory {volatile uint32_t *cpu; uint32_t bytes, iova;};
struct mgm_config {
    const uint8_t *firmware;
    size_t firmware_bytes;
    struct mgm_memory icache, dummy, debug, log, hfi;
    // Initializes the HFI queue table in the stopped GMU's hfi buffer (AdrenoHfiInitializeStopped); 0 on success.
    int (*hfi_init)(void *cookie, volatile uint32_t *shared, uint32_t bytes, uint32_t iova);
    void *hfi_cookie;
};

// The real payload blocks of the firmware, as a6xx_gmu_fw_load routes them by address: ITCM and DTCM by register, the rest into
// the GMU buffer whose GMU address range holds it. Blocks of size 0 are metadata; blocks of fewer than four bytes are not
// written (the only ones, two single bytes at ITCM address 0, are overwritten by the ITCM image that starts there).
enum {MGM_DEST_ITCM = 1, MGM_DEST_DTCM = 2, MGM_DEST_ICACHE = 3, MGM_DEST_DUMMY = 4};
struct mgm_block {uint32_t address, bytes, payload_offset, dest;};
#define MGM_MAX_BLOCKS 16
int mgm_plan(const uint8_t *firmware, size_t bytes, const struct mgm_config *c, struct mgm_block *blocks, unsigned capacity,
             unsigned *count, unsigned *small);

enum mgm_status {MGM_OK = 0, MGM_ARGUMENT, MGM_FIRMWARE, MGM_NOT_IN_RESET, MGM_STALE_HFI, MGM_IO, MGM_READBACK, MGM_HFI_INIT,
                 MGM_FAULT, MGM_FW_TIMEOUT, MGM_HFI_TIMEOUT};
struct mgm_result {
    unsigned phase;                      // 1 checks, 2 FAL, 3 load, 4 readback, 5 registers, 6 CM3 released, 7 FW up, 8 HFI up
    enum mgm_status status;
    unsigned blocks, small, itcm_bytes, dtcm_bytes, memory_bytes;
    uint32_t sysreset, hfi_before, tcm_ret, rsc_status, seq_busy, rscc_ack;   // before anything is written
    uint32_t version, mask, ready;
    uint32_t fw_init, hfi_status, ao_status, gmu2host, pwr_clk;
    unsigned fw_polls, hfi_polls;
    uint32_t mismatch_dword, mismatch_got, mismatch_want;
};
// The whole cold start up to HFI_CTRL_STATUS ready. On any failure after the CM3 left reset it is put back into reset.
enum mgm_status mgm_cold_start(const struct mgm_io *io, const struct mgm_config *c, struct mgm_result *r);

#ifdef __cplusplus
}
#endif
#endif
