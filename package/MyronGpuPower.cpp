// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of myron's Adreno 840 (facts and sequences: MyronGpuBoard.h, MyronGmu.h, MyronCmdDb.h), after
// sheng's GpuPowerProbe.cpp. `myron-gpu=<n>`, each panicking with every word it read:
//   1  read only: the GCC GPU branches and GPLL0 votes; GPUCC (the CX power domain, the GMU/SMMU branches, the always-on
//      branches, the GMU and hub roots, PLL0) when gcc_gpu_cfg_ahb_clk is enabled; gx_clkctl's GX power domain, the GPU SMMU
//      global space 0, the GMU CX status words and the RBBM status words, each only when what it needs reads on
//   2  as 1, plus the one GCC write Linux's probe makes: gcc_gpu_cfg_ahb_clk enabled if its enable bit is clear
//   3  as 2, plus: if CX reads off, gdsc.c's power-on of gpu_cc_cx_gdsc; with CX on, the GPUCC critical branches,
//      gpu_cc_ahb_clk and the SMMU vote clock, so gx_clkctl and the SMMU can be read. GX and the GMU are never written.
//   4  as 3, plus what a6xx_gmu_resume does before the GMU firmware: the GMU clocks (mg_gmu_clocks_on), then the SCM call
//      that opens the CX/GMU registers to the kernel (qcom_scm_gpu_init_regs(ALWAYS_EN | TSENSE), as a6xx_gmu_secure_init
//      makes it); then read only: the GMU, RSCC and CX_MISC state, the first SMMU stream slots and the GPU resources
//      of the AOP command DB
//   5  as 4, plus the GMU cold start: the GMU's own SMMU context (NativeAdrenoGmuContextBringup: context bank 1, ASID 1,
//      stream slot 2, stream 5 from the gmu node's iommus), its buffers, then mgm_cold_start (firmware, start registers, CM3
//      release, HFI ready). The GPU itself (GX) is not powered.
//   6  as 5, plus the HFI start-up and the GX power-on (a6xx_hfi_start's non-legacy path and a6xx_gmu_resume's first vote):
//      the GPU performance table and the bandwidth table (MyronHfi.c, from the command DB and the stock GPU levels),
//      CORE_FW_START and START, the GX vote for the highest offered level (422 MHz) and its DDR level, then the OOB GPU_SET
//      handshake that keeps GX up (as hw_init takes it), the wait for GMU_PWR_CLK_STATUS to report GX on, and the first RBBM
//      reads. The panic report then gives modes 1-5 one summary line.
//   7  as 6, plus the rest of a8xx hw_init that needs no GPU memory, in its order (MyronA840HwInit.h, generated from Linux):
//      GBIF halt release, SECVID TSB off, GMEM ranges and UCHE trap bases, UBWC, the non-context list per pipe, hang
//      detection, CP protect, APRIV and the CP/RBBM interrupt masks; then the written registers read back, the speed-bin
//      and slice fuses, and the GPU status words. From here the report gives mode 6 one line too.
//   8  as 7, plus the command processor: the GPU's own SMMU context and buffers, the SQE/AQE microcode, the ring and its
//      rptr shadows, CP_SQE_CNTL, a8xx_cp_init through the ring and its idle wait, then one CP_MEM_WRITE and a timestamp
//      fence.
//   9  as 8, plus the zap shader (PAS 13 through TrustZone into gpu_microcode_region) and the CP's exit from secure mode
//      (CP_SET_SECURE_MODE 0 with a fence), GMEM protection, and a second fenced CP_MEM_WRITE.
//  10  the run mode: 1-7, then the command processor through NativeAdreno's own objects on the A8XX register table
//      (NativeAdrenoChipA8xx): the ring and CP_ME_INIT, a fenced CP_MEM_WRITE, the zap shader and the secure exit, the GPU
//      interrupt (SPI 300) through the A8XX decoder, the always-on timer, then native_adreno_publish and two rounds through
//      the published service as a client makes them (allocate, map, a framed submission, wait, read back); then the report.
//  11  as 10 without the panic: the result goes to the diagnostic page's GPU rows and the boot continues with the GPU up and
//      its service published.
// `myron-gpu-delay=<s>` waits that long first (default 0). With mode 11, `myron-gpu-metal=1` then publishes the Metal accelerator
// (NativeAdreno's staged AGXAccelerator; the plugin bundle itself comes from the GPU addon, sheng-gpu-addon=1), and
// `myron-gpu-report=<s>` panics <s> seconds after the run with what the Metal side reached (who opened the driver, the plugin's
// progress markers and result lines), and `myron-gpu-hang=<s>` panics as soon as the GPU has had work outstanding for <s> seconds
// without progress, with where both CP threads stand. `myron-gpu-perf=<1..18>` picks the GPU level voted with GX (default
// 18 = 1200 MHz on speed bin 1), `myron-gpu-bw=<1..11>` the DDR level (default that level's own).
// `myron-gpu-hang-page=1` shows either report as a live two-page view on the boot-progress page instead of panicking.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/thread.h>
#include <kern/debug.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <stdarg.h>
extern "C" {
#include "MyronGpuBoard.h"
#include "MyronGmu.h"
#include "MyronCmdDb.h"
#include "MyronHfi.h"
#include "AdrenoHfi.h"
#include "AdrenoPM4.h"
volatile uint32_t myron_gpu_stage;
// gen80200_{gmu,sqe,aqe,zap}, embedded by build-native-legacy-display.py from the device repository's ignored .cache/firmware.
extern const uint8_t myron_gmu_firmware[], myron_sqe_firmware[], myron_aqe_firmware[], myron_zap_firmware[];
extern const uint64_t myron_gmu_firmware_size, myron_sqe_firmware_size, myron_aqe_firmware_size, myron_zap_firmware_size;
// NativeAdrenoPublish.cpp hands user space's boot-log export to this hook (sheng's BootLog.c keeps it); myron has no log
// export, so it refuses every piece. A definition is required: the kernel's static link resolves no weak import.
int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -1;}
// The diagnostic page's extension rows (BootProgress.c); the run mode's result goes to the GPU rows 4-7.
extern char boot_progress_ext[8][48];
extern char boot_progress_ext_wide[2][112];
// ArmCpu: attach a GIC SPI to the boot core (readback: group | enabled << 1 | edge << 2 | priority << 8).
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
int get_preemption_level(void);
// The Metal accelerator: NativeAdreno's staged AGXAccelerator (build-native-adreno-accelerator.py; its static initializer is out of
// __mod_init_func and runs here, after IOGPUFamily has loaded, because the parent metaclass does not exist before that).
void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
IOService *native_adreno_accelerator_allocate(void);
unsigned native_adreno_accelerator_instance_size(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern kmod_info_t g_kernel_kmod_info;
// The GPU addon's install and its three union mounts (patch zzy-native-gpu-addon-mount, kern_exec.c): stage 5 = mounted and the
// addon's own entry visible through it; status = the last error.
extern volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status,
    native_gpu_addon3_stage, native_gpu_addon3_status, native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same,
    native_gpu_addon_failed, native_gpu_addon_error;
// What the processes that reached the driver did (NativeAdrenoPublish.cpp, which defines these in a .cpp): the same layouts, read
// only, as sheng's GpuPowerProbe.cpp views them.
struct MyronMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector,
        last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls,
        marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight,
        inflight_since_calls, forced_unmaps;
    char opener[3][17], reader[3][17], name_reader[17], starter[3][17], report[64], report2[64];
    uint32_t grants;
};
extern MyronMetalDiag native_adreno_metal_diag;
struct MyronOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
extern MyronOpenRecord native_adreno_open_log[8];
extern volatile uint32_t native_adreno_open_log_count;
struct MyronClientText {char name[20]; char text[64];};
extern MyronClientText native_adreno_client_text[4];
extern volatile uint32_t native_adreno_mount_fail[4];
struct MyronAsker {char name[20]; uint32_t reads;};
extern MyronAsker native_adreno_metal_askers[12];
extern char native_adreno_metal_rows[48][96];
// The live debug page (BootProgress.c): rows 0-46 of at most 99 visible columns, and the switch that makes the page show them.
extern char myron_debug_rows[48][120];
extern volatile unsigned myron_debug_on;
// NativeAdreno's BO pool (NativeAdrenoMemory.cpp): the size the board asks for, the reservation's status, and [0] pool MiB, [1] BOs
// from the pool, [2] BOs that fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans.
extern volatile uint32_t native_adreno_pool_mib, native_adreno_pool_status, native_adreno_pool_stats[8];
// The display's GPU hand-off (NativeGOPPresenter.cpp) and the legacy display's retirements (LegacyDisplayProbe.cpp), for the swap-stall page:
// frames offered to the render server's scanout worker, copies it completed or failed, frames abandoned, its waits for a frame ([0] calls,
// [1] timeouts), and the swaps retired and refused offers. Weak: a kernel without the display archive still links.
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_completions __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_failures __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_abandoned __attribute__((weak_import));
extern volatile uint64_t native_gop_wait_stats[7] __attribute__((weak_import));
extern volatile uint32_t native_legacy_retirement_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_gpu_fallbacks __attribute__((weak_import));
extern volatile uint32_t native_legacy_gpu_last_status __attribute__((weak_import));
}
#include "MyronA840HwInit.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMemory.h"
extern "C" { extern volatile uint32_t native_adreno_io_coherent; extern volatile uint32_t native_adreno_flush_imports; }
// The CPU thermal controller's GPU level cap (MyronCpuPerf.c): 0 = none, otherwise the highest DCVS level allowed.
extern "C" volatile uint32_t myron_gpu_thermal_top;
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" IOService *native_adreno_published_service();
extern "C" uint32_t native_adreno_publish_stats(uint32_t *closed);
// A throwaway uncached kernel mapping of one physical page (the hang report reads DRAM as the GPU does), only of pages XNU manages
// (pmap_valid_page = pa_valid): boot-myron-32's first report took a synchronous external abort reading the page the GPU table named.
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
extern "C" boolean_t pmap_valid_page(uint32_t pn);
extern "C" uint64_t phystokv(uint64_t pa);   // XNU's linear map of a page it manages

namespace {
struct Window {uint64_t phys, length; IOMemoryMap *map; uintptr_t va;};

Window gWindows[MG_WINDOWS] = {
    {MG_GCC_GPU_PAGE, 0x1000, nullptr, 0},
    {MG_GCC_VOTE_PAGE, 0x1000, nullptr, 0},
    {MG_GPUCC_BASE, MG_GPUCC_SIZE, nullptr, 0},
    {MG_GXCC_PAGE, 0x1000, nullptr, 0},
    {MG_SMMU_BASE, MG_SMMU_GR0_SIZE, nullptr, 0},
    {MG_GMU_PAGE, MG_GMU_WINDOW, nullptr, 0},
    {MG_GPU_BASE, MG_GPU_WINDOW, nullptr, 0},
};
Window gGmuWindows[MGM_WINDOWS] = {
    {MGM_GMU_BASE, MGM_GMU_SIZE, nullptr, 0},
    {MGM_RSCC_BASE, MGM_RSCC_SIZE, nullptr, 0},
    {MGM_GPU_BASE, MGM_GPU_SIZE, nullptr, 0},
    {MGM_CXMISC_BASE, MGM_CXMISC_SIZE, nullptr, 0},
};

uintptr_t mapOne(Window &w, IOOptionBits cache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(w.phys, w.length);
    if (!memory) return 0;
    w.map = memory->map(kIOMapAnywhere | cache);
    memory->release();
    w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
    return w.va;
}
// Mapping a window reads nothing; a block is only touched once its prerequisites read on.
unsigned mapWindows(Window *windows, unsigned count) {
    unsigned have = 0;
    for (unsigned i = 0; i < count; ++i) if (mapOne(windows[i], kIOMapInhibitCache)) have |= 1u << i;
    return have;
}

int mmio(Window *windows, unsigned count, unsigned base, unsigned window, uint32_t offset, uint32_t *out, const uint32_t *in) {
    if (window >= count || !windows[window].va || offset + 4 > windows[window].length || (offset & 3)) return -1;
    myron_gpu_stage = (base + window + 1) << 24 | offset | (in ? 1u << 23 : 0);   // a faulting access leaves itself here
    volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(windows[window].va + offset);
    if (in) {*reg = *in; __asm__ volatile("dsb sy" ::: "memory");}
    else *out = *reg;
    return 0;
}
int rd(void *, unsigned window, uint32_t offset, uint32_t *out) {return mmio(gWindows, MG_WINDOWS, 0, window, offset, out, nullptr);}
int wr(void *, unsigned window, uint32_t offset, uint32_t value) {return mmio(gWindows, MG_WINDOWS, 0, window, offset, nullptr, &value);}
int grd(void *, unsigned window, uint32_t offset, uint32_t *out) {return mmio(gGmuWindows, MGM_WINDOWS, 8, window, offset, out, nullptr);}
int gwr(void *, unsigned window, uint32_t offset, uint32_t value) {return mmio(gGmuWindows, MGM_WINDOWS, 8, window, offset, nullptr, &value);}
void delayUs(void *, unsigned us) {IODelay(us);}
void barrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}

// SIP SMC as qcom_scm-smc.c makes it (sheng's scmCall): x0 function, x1 arginfo, x2.. arguments; QCOM_SCM_INTERRUPTED (1)
// resumes with x0 = 1 and the returned x6, QCOM_SCM_V2_EBUSY (-12) retries after 30 ms, at most 20 times.
struct ScmRes {int64_t a0; uint64_t a1; unsigned resumes;};
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1 = 0, uint64_t p2 = 0) {
    uint64_t first = fn, a6 = 0;
    unsigned resume = 0, busy = 0;
    while (resume < 10000) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}
        return {int64_t(x0), x1, resume};
    }
    return {-1000, 0, resume};
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, QCOM_SCM_SVC_GPU 0x28 << 8 | QCOM_SCM_SVC_GPU_INIT_REGS 0x1); one value argument,
// QCOM_SCM_GPU_ALWAYS_EN_REQ | QCOM_SCM_GPU_TSENSE_EN_REQ.
constexpr uint64_t kScmGpuInitRegs = 0x42000000u | (0x28u << 8) | 0x01u;
constexpr uint64_t kGpuInitRegsArg = (1u << 0) | (1u << 3);

// The GMU's SMMU context: bank 1 / ASID 1 / SMR+S2CR slot 2 as on sheng; stream 5 from myron's gmu iommus.
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlot = 2, kGmuSid = 5;
constexpr uint64_t kSmmuWindow = 0x40000;   // kgsl-smmu@3da0000 reg size: global spaces and 21 context banks

struct Mode4 {
    ScmRes scm;
    mg_gmu_clocks clocks;
    uint32_t smr[4], s2cr[4];
    uint32_t pwr_clk, rpmh, sysreset, fw_init, hfi, ao_count, version, rsc_status, seq_busy, rscc_ack, tcm_ret, ao_status;
    unsigned gmu_have;
    mcd_result db;
    uint16_t gfx[16], mx[16], cx[16], gmxc[16];
    unsigned gfx_n, mx_n, cx_n, gmxc_n;
    int db_mapped;
};
struct Mode5 {
    int ran, s2cr_fixed;
    uint32_t s2cr_before;
    NativeAdrenoGmuContextReport ctx;
    IOReturn ctx_status, payload_status;
    NativeAdrenoGmuPayload payloads[5];
    unsigned payload_maps;
    mgm_result gmu;
};
struct Mode6 {
    int ran;
    unsigned step;                       // 1 tables, 2 perf, 3 bw, 4 core start, 5 start, 6 GX vote, 7 OOB, 8 GX wait, 9 GX on
    unsigned sent;                       // bit per message slot that was sent
    mhf_tables t;
    AdrenoHfiStatus hs[5];
    uint32_t fw_error[5];
    size_t reply_words[5];
    int oob_ok;
    uint32_t oob_info, pwr_before, pwr_after, rpmh, gx_gdscr, gx_cfg, rbbm, rbbm_gfx, gbif_halt;
    unsigned oob_polls, gx_polls;
};
// Mode 7 (register init, no GPU memory).
struct Mode7 {
    int ran;
    unsigned writes, io_errors;          // GPU register writes made, accesses refused by the window check
    uint32_t gbif_halt, rbbm_gbif_halt;  // the read-backs hw_init makes after releasing the halts
    unsigned checked, matched;           // written registers read back / equal to what was written
    uint32_t bad_reg, bad_got, bad_want; // the first that differs: dword offset | pipe << 20
    uint32_t bad2_reg, bad2_got, bad2_want;   // and the second
    uint32_t status, int0, cp_int;       // RBBM_STATUS, RBBM_INT_0_STATUS, CP_INTERRUPT_STATUS_GLOBAL afterwards
    uint32_t fuse_freq, fuse_value, slices;   // CX_MISC SW_FUSE_FREQ_LIMIT_STATUS, SW_FUSE_VALUE, SLICE_ENABLE_FINAL
};
// Mode 8 (command processor).
struct Mode8 {
    int ran;
    unsigned step;   // 1 stream slots, 2 context, 3 buffers, 4 SQE buffer, 5 microcode, 6 ring + SQE start, 7 CP init sent,
                     // 8 CP init consumed, 9 mem write sent, 10 mem write and fence seen
    uint32_t s2cr_before[2];
    unsigned s2cr_fixed, views;
    NativeAdrenoGmuContextReport ctx;
    IOReturn ctx_status, buf_status, sqe_alloc, sqe_map;
    NativeAdrenoGpuBuffer buf[5];
    uint64_t sqe_phys;
    uint32_t sqe_bad, aqe_bad;           // first differing byte + 1 of the microcode read back
    uint32_t actlr, actlr_set;           // context bank 0 ACTLR as found, and read back after myron-gpu-actlr
    uint32_t rptr0, wptr0, wptr;
    unsigned init_polls, fence_polls;
    int init_idle;
    uint32_t rptr, shadow, bv_rptr, status, int0, cp_int, pipe_int, hw_fault, sqe_status, protect_status;
    uint32_t fence, scratch, scratch_reg;
    uint32_t fsr, far_lo, far_hi, fsynr0, gfsr;
};
// Mode 9 (zap shader and secure-mode exit).
struct Mode9 {
    int ran, stage;  // as sheng's zapAndSecure: <0 refused at that check; 3 init_image, 4 mem_setup, 5 copy, 6 auth, 7 secure
                     // fence, 8 second write, 9 done
    int64_t init, mem, auth;
    unsigned resumes[3], polls, polls2;
    uint32_t fence, scratch, status, int0, cp_int, fsr;
};
Mode4 gM4;
Mode5 gM5;
Mode6 gM6;
Mode7 gM7;
Mode8 gM8;
Mode9 gM9;
Window gSmmu = {MG_SMMU_BASE, kSmmuWindow, nullptr, 0};   // the whole kgsl-smmu window, mapped by mode 5
alignas(8) uint8_t gCmdDb[MCD_BYTES];
AdrenoHfi gHfi;

int hfiNotify(void *) {
    uint32_t one = 1;
    return mmio(gGmuWindows, MGM_WINDOWS, 8, MGM_WIN_GMU, MGM_GMU(MGM_HOST2GMU_INTR_SET), nullptr, &one);
}
int hfiInit(void *, volatile uint32_t *shared, uint32_t bytes, uint32_t iova) {
    return AdrenoHfiInitializeStopped(&gHfi, shared, bytes, iova, nullptr, hfiNotify) != AdrenoHfiSuccess;
}

void mode4(const mg_io &io, mg_result &r, unsigned have) {
    Mode4 &m = gM4;
    mg_gmu_clocks_on(&io, &r, &m.clocks);
    m.scm = scmCall(kScmGpuInitRegs, 1, kGpuInitRegsArg);   // after the clocks, as a6xx_gmu_resume orders them
    if (have & (1u << MG_WIN_SMMU) && r.smmu == MG_DONE)
        for (unsigned i = 0; i < 4; ++i) {rd(nullptr, MG_WIN_SMMU, MG_SMMU_SMR(i), &m.smr[i]); rd(nullptr, MG_WIN_SMMU, MG_SMMU_S2CR(i), &m.s2cr[i]);}
    m.gmu_have = mapWindows(gGmuWindows, MGM_WINDOWS);
    if (m.clocks.step == 7 && m.gmu_have == (1u << MGM_WINDOWS) - 1) {
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS), &m.pwr_clk);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS + 1), &m.rpmh);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_CM3_SYSRESET), &m.sysreset);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_CM3_FW_INIT_RESULT), &m.fw_init);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_HFI_CTRL_STATUS), &m.hfi);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(0x1f840), &m.ao_count);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_CM3_DTCM_START + 0xff8), &m.version);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_RSCC_CONTROL_ACK), &m.rscc_ack);
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_AO_HOST_INTERRUPT_STATUS), &m.ao_status);
        grd(nullptr, MGM_WIN_RSCC, MGM_RSC_STATUS0, &m.rsc_status);
        grd(nullptr, MGM_WIN_RSCC, MGM_RSCC_SEQ_BUSY, &m.seq_busy);
        grd(nullptr, MGM_WIN_CXMISC, MGM_CX_TCM_RET_CNTL, &m.tcm_ret);
    }
    // The command DB is ordinary memory the AOP filled; it is copied with aligned word reads.
    Window db = {MCD_BASE, MCD_BYTES, nullptr, 0};
    if (mapOne(db, kIOMapInhibitCache)) {
        m.db_mapped = 1;
        const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(db.va);
        uint32_t *dst = reinterpret_cast<uint32_t *>(gCmdDb);
        for (uint32_t i = 0; i < MCD_BYTES / 4; ++i) dst[i] = src[i];
        mcd_parse(gCmdDb, MCD_BYTES, &m.db);
        m.gfx_n = mcd_arc_levels(&m.db.r[MCD_GFX], m.gfx, 16);
        m.mx_n = mcd_arc_levels(&m.db.r[MCD_MX], m.mx, 16);
        m.cx_n = mcd_arc_levels(&m.db.r[MCD_CX], m.cx, 16);
        m.gmxc_n = mcd_arc_levels(&m.db.r[MCD_GMXC], m.gmxc, 16);
    }
}

void gmuProgress(uint32_t step) {myron_gpu_stage = 0x50000000u | step;}

int hfiWaitOnce(void *) {IODelay(100); return 0;}
// One HFI command and its acknowledgement through the kit (as a6xx_hfi_send_msg / a6xx_hfi_wait_for_ack: 1 s), then the
// message-queue interrupt bit cleared as a6xx_hfi_wait_for_ack clears it.
bool exchange(unsigned slot, unsigned id, const uint32_t *payload, size_t words) {
    Mode6 &m = gM6;
    uint32_t reply[16];
    myron_gpu_stage = 0x60000000u | id << 8 | slot;
    m.sent |= 1u << slot;
    m.hs[slot] = AdrenoHfiExchange(&gHfi, id, payload, words, 10000, hfiWaitOnce, reply, 16, &m.reply_words[slot], &m.fw_error[slot]);
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_GMU2HOST_INTR_CLR), 1);
    return m.hs[slot] == AdrenoHfiSuccess;
}

void mode6() {
    Mode6 &m = gM6;
    if (gM5.gmu.status != MGM_OK) return;
    m.ran = 1;
    m.step = 1;
    if (mhf_build(&gM4.db, &m.t)) return;
    m.step = 2;
    if (!exchange(0, MHF_MSG_TABLE, m.t.perf, MHF_PERF_WORDS)) return;
    m.step = 3;
    if (!exchange(1, MHF_MSG_BW_TABLE, m.t.bw, MHF_BW_WORDS)) return;
    m.step = 4;
    static const uint32_t handle = 0;
    if (!exchange(2, MHF_MSG_CORE_FW_START, &handle, 1)) return;
    m.step = 5;
    if (!exchange(3, MHF_MSG_START, nullptr, 0)) return;
    // The rest of a6xx_gmu_resume after a6xx_hfi_start: the CM3 fault interrupt unmasked (the line itself stays off in the
    // GIC), then a6xx_gmu_set_initial_freq, whose a6xx_hfi_set_freq votes GX: GX_BW_PERF_VOTE {ack_type 1 (blocking), perf
    // index, bandwidth index}. The OOB below comes later in Linux, from hw_init; the GMU acknowledges it only after this vote
    // (myron-gpu=6 of 2026-10-22 asked first and got no acknowledgement).
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_GMU2HOST_INTR_CLR), 0xffffffffu);
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_GMU2HOST_INTR_MASK), ~MGM_CM3_FAULT);
    m.step = 6;
    grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS), &m.pwr_before);
    // myron-gpu-perf=<1..18> votes that GPU level (1 = 160 MHz, 18 = 1200 MHz; default the highest) and takes its DDR index.
    // myron-gpu-bw=<1..11> then overrides the DDR level (11 is about 21 GB/s). The UI renders in system memory, so the DDR
    // level bounds every frame.
    unsigned bw = 0, perf = 0;
    if (PE_parse_boot_argn("myron-gpu-perf", &perf, sizeof perf) && perf >= 1 && perf <= m.t.perf_index) {
        m.t.perf_index = perf;
        m.t.bw_index = mhf_gpu_levels[perf].bw;
    }
    if (PE_parse_boot_argn("myron-gpu-bw", &bw, sizeof bw) && bw >= 1 && bw < MHF_BWS) m.t.bw_index = bw;
    const uint32_t vote[3] = {1, m.t.perf_index, m.t.bw_index};
    if (!exchange(4, MHF_MSG_GX_BW_PERF_VOTE, vote, 3)) return;
    // hw_init's a6xx_gmu_set_oob(GMU_OOB_GPU_SET), non-legacy bits: request 30, acknowledgement 31 (100 us steps; Linux waits
    // 10 ms, this waits up to 100 ms and records how long it took).
    m.step = 7;
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_HOST2GMU_INTR_SET), 1u << 30);
    for (;;) {
        ++m.oob_polls;
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_GMU2HOST_INTR_INFO), &m.oob_info);
        if (m.oob_info & (1u << 31)) break;
        if (m.oob_polls >= 1000) {grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS), &m.pwr_after); return;}
        IODelay(100);
    }
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_GMU2HOST_INTR_CLR), 1u << 31);
    m.oob_ok = 1;
    // a8xx_gmu_gx_is_on: neither GX_HM_GDSC_POWER_OFF nor GX_HM_CLK_OFF (bits 0 and 1); up to 100 ms.
    m.step = 8;
    for (;;) {
        ++m.gx_polls;
        grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS), &m.pwr_after);
        if (!(m.pwr_after & 3)) break;
        if (m.gx_polls >= 1000) break;
        IODelay(100);
    }
    grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_PWR_CLK_STATUS + 1), &m.rpmh);
    rd(nullptr, MG_WIN_GXCC, MG_GX_GDSCR, &m.gx_gdscr);
    rd(nullptr, MG_WIN_GXCC, MG_GX_CFG_GDSCR, &m.gx_cfg);
    if (m.pwr_after & 3) return;
    // GX is up and held by the OOB: the GPU block's own status words (gen8_reg.h, dwords from the GPU base).
    m.step = 9;
    grd(nullptr, MGM_WIN_GPU, MG_RBBM_STATUS, &m.rbbm);
    grd(nullptr, MGM_WIN_GPU, MG_RBBM_GFX_STATUS, &m.rbbm_gfx);
    grd(nullptr, MGM_WIN_GPU, MG_RBBM_GBIF_HALT, &m.gbif_halt);
}

void mode5(mg_result &r) {
    Mode5 &m = gM5;
    const Mode4 &p = gM4;
    // The context needs CX, the SMMU vote clock and the GMU clocks running, an SMMU with no stream mapped, and the GMU
    // stopped; every condition is the reading of this same boot.
    if (r.smmu != MG_DONE || r.smr_valid || p.clocks.step != 7 || p.gmu_have != (1u << MGM_WINDOWS) - 1 || p.sysreset != 1) return;
    m.ran = 1;
    Window &smmu = gSmmu;
    if (!mapOne(smmu, kIOMapInhibitCache)) {m.ctx_status = kIOReturnNoMemory; return;}
    // The kit attaches only to a slot whose S2CR faults; an unused slot (SMR invalid) is set to FAULT first.
    volatile uint32_t *s2cr = reinterpret_cast<volatile uint32_t *>(smmu.va + MG_SMMU_S2CR(kGmuSlot));
    m.s2cr_before = *s2cr;
    if (((m.s2cr_before >> 16) & 3) != 2 && !(p.smr[kGmuSlot] & (1u << 31))) {
        *s2cr = 0x20000;
        __asm__ volatile("dsb sy" ::: "memory");
        m.s2cr_fixed = 1;
    }
    m.ctx_status = NativeAdrenoGmuContextBringup(reinterpret_cast<volatile uint8_t *>(smmu.va), kSmmuWindow, kGmuBank, kGmuAsid,
                                                 kGmuSlot, kGmuSid, gmuProgress, &m.ctx);
    if (m.ctx_status) return;
    m.payload_status = NativeAdrenoGmuContextPayloads(m.payloads);
    if (m.payload_status) return;
    // The GMU maps its buffers Normal non-cacheable; the CPU gets a write-combined view of each.
    mgm_memory memory[5] = {};
    static Window views[5];
    for (unsigned i = 0; i < 5; ++i) {
        views[i] = {m.payloads[i].physical, m.payloads[i].bytes, nullptr, 0};
        if (!mapOne(views[i], kIOMapWriteCombineCache)) return;
        ++m.payload_maps;
        memory[i] = {reinterpret_cast<volatile uint32_t *>(views[i].va), uint32_t(m.payloads[i].bytes), uint32_t(m.payloads[i].iova)};
    }
    mgm_io io = {nullptr, grd, gwr, barrier, delayUs};
    mgm_config config = {myron_gmu_firmware, size_t(myron_gmu_firmware_size), memory[0], memory[1], memory[2], memory[3], memory[4],
                         hfiInit, nullptr};
    myron_gpu_stage = 0x51000000u;
    mgm_cold_start(&io, &config, &m.gmu);
}

// GPU registers through the cold-start GPU window (dword offsets of MyronA840HwInit.h). Pipe registers are reached through
// CP_APERTURE_CNTL_HOST, as a8xx_write_pipe / a8xx_read_pipe_slice do (slice 0).
uint32_t gpuRd(uint32_t reg) {
    uint32_t v = 0xdeadbeef;
    if (grd(nullptr, MGM_WIN_GPU, reg * 4, &v)) ++gM7.io_errors;
    return v;
}
void gpuWr(uint32_t reg, uint32_t value) {
    ++gM7.writes;
    if (gwr(nullptr, MGM_WIN_GPU, reg * 4, value)) ++gM7.io_errors;
}
void gpuWr64(uint32_t reg, uint64_t value) {gpuWr(reg, uint32_t(value)); gpuWr(reg + 1, uint32_t(value >> 32));}
void aperture(unsigned pipe) {gpuWr(A840_CP_APERTURE_CNTL_HOST, A840_APERTURE_PIPE(pipe));}
void pipeWr(unsigned pipe, uint32_t reg, uint32_t value) {aperture(pipe); gpuWr(reg, value);}
uint32_t pipeRd(unsigned pipe, uint32_t reg) {aperture(pipe); return gpuRd(reg);}

// Mode 7. a8xx_gpu.c hw_init from after a6xx_gmu_set_oob(GPU_SET) to just before adreno_hw_init, line for line. Left out:
// the GMU power and busy counter selects, the TPL1 bicubic weights (texture filtering) and, later, hwcg (no A840 table in
// Linux); none is on the command processor's path.
constexpr uint64_t kGmemRangeMin = 0x4000000;            // SZ_64M
constexpr uint64_t kUcheTrapBase = 0x1fffffffff000ull;   // a6xx_gpu_init's uche_trap_base for A7XX and later
void mode7() {
    Mode7 &m = gM7;
    if (gM6.step != 9 || (gM6.pwr_after & 3)) return;
    m.ran = 1;
    aperture(A840_PIPE_NONE);
    gpuWr(A840_GBIF_HALT, 0);
    m.gbif_halt = gpuRd(A840_GBIF_HALT);
    gpuWr(A840_RBBM_GBIF_HALT, 0);
    m.rbbm_gbif_halt = gpuRd(A840_RBBM_GBIF_HALT);
    gpuWr(A840_RBBM_SECVID_TSB_CNTL, 0);
    gpuWr64(A840_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    gpuWr(A840_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
    gpuWr(A840_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffffu);
    gpuWr64(A840_UCHE_CCHE_GC_GMEM_RANGE_MIN, kGmemRangeMin);
    gpuWr64(A840_SP_HLSQ_GC_GMEM_RANGE_MIN, kGmemRangeMin);
    gpuWr64(A840_UCHE_TRAP_BASE, kUcheTrapBase);
    gpuWr64(A840_UCHE_WRITE_THRU_BASE, kUcheTrapBase);
    gpuWr64(A840_UCHE_CCHE_TRAP_BASE, kUcheTrapBase);
    gpuWr64(A840_UCHE_CCHE_WRITE_THRU_BASE, kUcheTrapBase);
    gpuWr(A840_RBBM_PERFCTR_CNTL, 1);
    gpuWr(A840_RBBM_SLICE_PERFCTR_CNTL, 1);
    gpuWr(A840_CP_PERFCTR_CP_SEL(0), 1);
    // a8xx_set_ubwc_config (kaanapali: UBWC 6.0, highest bank bit 16).
    pipeWr(A840_PIPE_BV, A840_GRAS_NC_MODE_CNTL, A840_UBWC_GRAS_NC_MODE_CNTL);
    pipeWr(A840_PIPE_BR, A840_GRAS_NC_MODE_CNTL, A840_UBWC_GRAS_NC_MODE_CNTL);
    pipeWr(A840_PIPE_BR, A840_RB_CCU_NC_MODE_CNTL, A840_UBWC_RB_CCU_NC_MODE_CNTL);
    pipeWr(A840_PIPE_BR, A840_RB_CMP_NC_MODE_CNTL, A840_UBWC_RB_CMP_NC_MODE_CNTL);
    aperture(A840_PIPE_NONE);
    gpuWr(A840_SP_NC_MODE_CNTL, A840_UBWC_SP_NC_MODE_CNTL);
    gpuWr(A840_TPL1_NC_MODE_CNTL, A840_UBWC_TPL1_NC_MODE_CNTL);
    gpuWr(A840_UCHE_CLIENT_PF, (1u << 7) | 0x1);
    // a8xx_nonctxt_config: every pipe from NONE to DDE_BV but LPAC, each entry on the pipes its mask names.
    for (unsigned pipe = A840_PIPE_NONE; pipe <= A840_PIPE_DDE_BV; ++pipe) {
        if (pipe == A840_PIPE_LPAC) continue;
        aperture(pipe);
        for (unsigned i = 0; i < MYRON_A840_NONCTXT_COUNT; ++i)
            if (MyronA840Nonctxt[i][2] & (1u << pipe)) gpuWr(MyronA840Nonctxt[i][0], MyronA840Nonctxt[i][1]);
    }
    aperture(A840_PIPE_NONE);
    gpuWr(A840_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0xcfffff);
    gpuWr(A840_RBBM_SLICE_INTERFACE_HANG_INT_CNTL, 1u << 30);
    // a8xx_set_cp_protect.
    pipeWr(A840_PIPE_BR, A840_CP_PROTECT_CNTL_PIPE, MYRON_A8XX_CP_PROTECT_CNTL);
    pipeWr(A840_PIPE_BV, A840_CP_PROTECT_CNTL_PIPE, MYRON_A8XX_CP_PROTECT_CNTL);
    aperture(A840_PIPE_NONE);
    uint32_t final_cfg = 0;
    for (unsigned i = 0; i < MYRON_A840_PROTECT_COUNT; ++i)
        if (MyronA840Protect[i]) {gpuWr(A840_CP_PROTECT_GLOBAL(i), MyronA840Protect[i]); final_cfg = MyronA840Protect[i];}
    pipeWr(A840_PIPE_BR, A840_CP_PROTECT_PIPE(15), final_cfg);
    pipeWr(A840_PIPE_BV, A840_CP_PROTECT_PIPE(15), final_cfg);
    aperture(A840_PIPE_NONE);
    pipeWr(A840_PIPE_BR, A840_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE, 1);
    for (unsigned pipe = A840_PIPE_BR; pipe <= A840_PIPE_DDE_BV; ++pipe) {
        if (pipe == A840_PIPE_LPAC) continue;
        aperture(pipe);
        gpuWr(A840_CP_APRIV_CNTL_PIPE, pipe == A840_PIPE_BR ? MYRON_A8XX_BR_APRIV_MASK : MYRON_A8XX_APRIV_MASK);
        gpuWr(A840_CP_INTERRUPT_STATUS_MASK_PIPE, MYRON_A8XX_CP_INTERRUPT_STATUS_MASK_PIPE);
        gpuWr(A840_CP_HW_FAULT_STATUS_MASK_PIPE, MYRON_A8XX_CP_HW_FAULT_STATUS_MASK_PIPE);
    }
    aperture(A840_PIPE_NONE);
    // The masks only reach the GIC through SPI 300, which nothing has enabled: the status words below are polled.
    gpuWr(A840_CP_INTERRUPT_STATUS_MASK_GLOBAL, MYRON_A8XX_CP_GLOBAL_INT_MASK);
    gpuWr(A840_RBBM_INT_0_MASK, MYRON_A8XX_INT_MASK);
    // Read back what was written, the protect table and the interrupt masks first; the non-context list last, since a
    // chicken bit may read back differently.
    auto check = [&](unsigned pipe, uint32_t reg, uint32_t want) {
        const uint32_t got = pipeRd(pipe, reg);
        ++m.checked;
        if (got == want) ++m.matched;
        else if (!m.bad_reg) {m.bad_reg = reg | pipe << 20; m.bad_got = got; m.bad_want = want;}
        else if (!m.bad2_reg) {m.bad2_reg = reg | pipe << 20; m.bad2_got = got; m.bad2_want = want;}
    };
    for (unsigned i = 0; i < MYRON_A840_PROTECT_COUNT; ++i) check(A840_PIPE_NONE, A840_CP_PROTECT_GLOBAL(i), MyronA840Protect[i]);
    check(A840_PIPE_NONE, A840_RBBM_INT_0_MASK, MYRON_A8XX_INT_MASK);
    check(A840_PIPE_NONE, A840_CP_INTERRUPT_STATUS_MASK_GLOBAL, MYRON_A8XX_CP_GLOBAL_INT_MASK);
    check(A840_PIPE_NONE, A840_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffffu);
    check(A840_PIPE_NONE, A840_UCHE_TRAP_BASE, uint32_t(kUcheTrapBase));
    check(A840_PIPE_NONE, A840_UCHE_TRAP_BASE + 1, uint32_t(kUcheTrapBase >> 32));
    check(A840_PIPE_NONE, A840_UCHE_CCHE_GC_GMEM_RANGE_MIN, uint32_t(kGmemRangeMin));
    check(A840_PIPE_NONE, A840_UCHE_CLIENT_PF, (1u << 7) | 0x1);
    check(A840_PIPE_NONE, A840_SP_NC_MODE_CNTL, A840_UBWC_SP_NC_MODE_CNTL);
    check(A840_PIPE_NONE, A840_TPL1_NC_MODE_CNTL, A840_UBWC_TPL1_NC_MODE_CNTL);
    check(A840_PIPE_BR, A840_CP_PROTECT_CNTL_PIPE, MYRON_A8XX_CP_PROTECT_CNTL);
    check(A840_PIPE_BR, A840_CP_PROTECT_PIPE(15), final_cfg);
    check(A840_PIPE_BR, A840_CP_APRIV_CNTL_PIPE, MYRON_A8XX_BR_APRIV_MASK);
    check(A840_PIPE_BV, A840_CP_APRIV_CNTL_PIPE, MYRON_A8XX_APRIV_MASK);
    check(A840_PIPE_BR, A840_GRAS_NC_MODE_CNTL, A840_UBWC_GRAS_NC_MODE_CNTL);
    check(A840_PIPE_BR, A840_RB_CCU_NC_MODE_CNTL, A840_UBWC_RB_CCU_NC_MODE_CNTL);
    check(A840_PIPE_BR, A840_RB_CMP_NC_MODE_CNTL, A840_UBWC_RB_CMP_NC_MODE_CNTL);
    for (unsigned i = 0; i < MYRON_A840_NONCTXT_COUNT; ++i) {
        // hw_init rewrites the hang-detect word below the list, and GMEM protection holds only after the secure exit.
        const uint32_t pipes = MyronA840Nonctxt[i][2], reg = MyronA840Nonctxt[i][0];
        if (reg == A840_RBBM_INTERFACE_HANG_INT_CNTL || reg == A840_RB_GC_GMEM_PROTECT) continue;
        check(pipes & 1 ? A840_PIPE_NONE : pipes & 2 ? A840_PIPE_BR : A840_PIPE_BV, MyronA840Nonctxt[i][0], MyronA840Nonctxt[i][1]);
    }
    aperture(A840_PIPE_NONE);
    m.status = gpuRd(A840_RBBM_STATUS);
    m.int0 = gpuRd(A840_RBBM_INT_0_STATUS);
    m.cp_int = gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL);
    grd(nullptr, MGM_WIN_CXMISC, A840_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS * 4, &m.fuse_freq);
    grd(nullptr, MGM_WIN_CXMISC, A840_CX_MISC_SW_FUSE_VALUE * 4, &m.fuse_value);
    grd(nullptr, MGM_WIN_CXMISC, A840_CX_MISC_SLICE_ENABLE_FINAL * 4, &m.slices);
}

// Mode 8. The command processor as a6xx_ucode_load and the rest of hw_init start it. The GPU's own SMMU context comes from
// NativeAdrenoGpuContextBringup: context bank 0 (arm-smmu-qcom gives the GPU bank 0), ASID 2, stream slots 0/1 for myron's
// gfx3d_user and gfx3d_lpac streams 0 and 1 (gfx3d_secure, stream 2, stays unmatched and faults), with its ring, memptrs,
// power-up list and scratch pages. The AQE microcode goes into the context's firmware buffer; the SQE microcode, too big
// for it on gen8, into a buffer of its own. Both drop their first dword, as adreno_fw_create_bo copies them.
constexpr unsigned kGpuBank = 0, kGpuAsid = 2;
constexpr uint64_t kSqeIova = 0x400000;          // below the GMEM range and clear of the context's buffers
constexpr size_t kSqeBytes = 0x1e000;
constexpr uint32_t kRbCntl = (2u << 8) | 12u;    // MSM_GPU_RB_CNTL_DEFAULT: BLKSZ log2(32 / 8), BUFSZ log2(32 KiB / 8)
constexpr uint32_t kRingMask = GpuRingBytes / 4 - 1;
constexpr uint32_t kMeInitMask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 6) | (1u << 8);   // a8xx_cp_init
constexpr uint32_t kCacheClean = 0x31, kEventWriteEnabled = 1u << 27;
constexpr uint32_t kScratchMagic = 0x5e9a8400u, kScratchMagic2 = 0x5ec0de00u;
// adreno_pm4.xml opcodes.
enum : uint32_t {kPm4WaitMemWrites = 0x12, kPm4ThreadControl = 0x17, kPm4MemWrite = 0x3d, kPm4EventWrite7 = 0x46,
                 kPm4MeInit = 0x48, kPm4SetSecureMode = 0x66};
NativeAdrenoMemory gSqe;
volatile uint32_t *gGpuCpu[5];                   // write-combined views of sqe(aqe here), ring, memptrs, pwrup, scratch
uint32_t gWptr;
void gpuCtxProgress(uint32_t step) {myron_gpu_stage = 0x80000000u | step;}

void ringPut(uint32_t word) {gGpuCpu[1][gWptr & kRingMask] = word; ++gWptr;}
bool pkt7(uint32_t opcode, const uint32_t *payload, uint32_t count) {
    uint32_t header = 0;
    if (AdrenoPM4Type7(opcode, count, &header) != AdrenoPM4Success) return false;
    ringPut(header);
    for (uint32_t i = 0; i < count; ++i) ringPut(payload[i]);
    return true;
}
bool pkt4(uint32_t reg, uint32_t value) {
    uint32_t header = 0;
    if (AdrenoPM4Type4(reg, 1, &header) != AdrenoPM4Success) return false;
    ringPut(header);
    ringPut(value);
    return true;
}
// a6xx_flush: the ring words are Normal non-cacheable on both sides, so ordering them before the WPTR write is enough.
void ringKick() {
    __asm__ volatile("dsb sy" ::: "memory");
    gpuWr(A840_CP_RB_WPTR, gWptr & kRingMask);
}
// A CACHE_CLEAN timestamp into memptrs+4 (msm_rbmemptrs.fence) after a CP_WAIT_MEM_WRITES, then the wait for it (100 us
// steps, up to 200 ms).
bool fenceAndWait(uint32_t seqno, unsigned &polls) {
    const uint64_t at = gM8.buf[2].iova + 4;
    const uint32_t event[4] = {kCacheClean | kEventWriteEnabled, uint32_t(at), uint32_t(at >> 32), seqno};
    if (!pkt7(kPm4WaitMemWrites, nullptr, 0) || !pkt7(kPm4EventWrite7, event, 4)) return false;
    ringKick();
    for (polls = 1;; ++polls) {
        if (gGpuCpu[2][1] == seqno) return true;
        if (polls >= 2000) return false;
        IODelay(100);
    }
}
size_t bank0() {
    const uint32_t id1 = *reinterpret_cast<volatile uint32_t *>(gSmmu.va + 0x24);
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank = page << (((id1 >> 28) & 7) + 1);
    return bank + 0x1000 <= kSmmuWindow ? bank : 0;
}
uint32_t smmuRd(size_t offset) {return gSmmu.va && offset ? *reinterpret_cast<volatile uint32_t *>(gSmmu.va + offset) : 0xdead;}
void cpSnapshot(Mode8 &m) {
    m.rptr = gpuRd(A840_CP_RB_RPTR);
    m.shadow = gGpuCpu[2][0];
    m.bv_rptr = gGpuCpu[2][2];
    m.status = gpuRd(A840_RBBM_STATUS);
    m.int0 = gpuRd(A840_RBBM_INT_0_STATUS);
    m.cp_int = gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL);
    m.pipe_int = pipeRd(A840_PIPE_BR, A840_CP_INTERRUPT_STATUS_PIPE);
    m.hw_fault = gpuRd(A840_CP_HW_FAULT_STATUS_PIPE);
    m.sqe_status = gpuRd(A840_CP_SQE_STATUS_PIPE);
    m.protect_status = gpuRd(A840_CP_PROTECT_STATUS_PIPE);
    aperture(A840_PIPE_NONE);
    const size_t b = bank0();
    m.fsr = smmuRd(b ? b + 0x58 : 0);
    m.far_lo = smmuRd(b ? b + 0x60 : 0);
    m.far_hi = smmuRd(b ? b + 0x64 : 0);
    m.fsynr0 = smmuRd(b ? b + 0x68 : 0);
    m.gfsr = smmuRd(0x48);
}

// Mode 8's first half, shared with the run mode: the GPU's SMMU context and buffers, the SQE buffer and both microcode
// images read back; true once all of it is in place.
bool cpPrepare() {
    Mode8 &m = gM8;
    if (!gM7.ran || gM7.io_errors || !gSmmu.va) return false;
    m.ran = 1;
    // 1. Stream slots 0 and 1 unused (SMR invalid) and faulting, as the kit requires.
    m.step = 1;
    for (unsigned i = 0; i < 2; ++i) {
        volatile uint32_t *smr = reinterpret_cast<volatile uint32_t *>(gSmmu.va + MG_SMMU_SMR(i));
        volatile uint32_t *s2cr = reinterpret_cast<volatile uint32_t *>(gSmmu.va + MG_SMMU_S2CR(i));
        m.s2cr_before[i] = *s2cr;
        if (*smr & (1u << 31)) return false;
        if (((m.s2cr_before[i] >> 16) & 3) != 2) {*s2cr = 0x20000; __asm__ volatile("dsb sy" ::: "memory"); m.s2cr_fixed |= 1u << i;}
    }
    m.step = 2;
    m.ctx_status = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(gSmmu.va), kSmmuWindow, kGpuBank, kGpuAsid,
                                                 0, 0, 1, 1, gpuCtxProgress, &m.ctx);
    if (m.ctx_status) return false;
    const size_t b = bank0();
    m.actlr = smmuRd(b ? b + 0x4 : 0);
    // sheng's countermeasure (GpuPowerProbe.cpp gpuSetActlr): the bootloader leaves the bank's ACTLR at 0x3, CPRE on, and MMU-500 errata
    // 841119/826419 let that next-page prefetcher keep stale translations after a TLB invalidation. Linux clears CPRE in every bank, and
    // sheng sets the Adreno bank to 0x301 (CMTLB | PREFETCH_DEEP, CPRE off), as Linux's driver left it, before any GPU buffer is mapped,
    // then invalidates the bank's TLB (TLBIALL +0x618, TLBSYNC +0x7f0, TLBSTATUS +0x7f4). With CPRE on (myron to boot-myron-30) the GPU
    // read freed surfaces' pages after their reuse (Settings labels showing SpringBoard's) and hung in the same draw each time.
    // myron-gpu-actlr=<value> sets another value; 0 leaves the bootloader's.
    unsigned actlr = 0x301;
    PE_parse_boot_argn("myron-gpu-actlr", &actlr, sizeof actlr);
    m.actlr_set = m.actlr;
    if (b && actlr) {
        volatile uint32_t *cb = reinterpret_cast<volatile uint32_t *>(gSmmu.va + b);
        cb[0x4 / 4] = actlr;
        __asm__ volatile("dsb sy" ::: "memory");
        cb[0x618 / 4] = 0;
        cb[0x7f0 / 4] = 0;
        for (unsigned i = 0; i < 1000 && (cb[0x7f4 / 4] & 1); ++i) IODelay(1);
        m.actlr_set = cb[0x4 / 4];
    }
    m.step = 3;
    m.buf_status = NativeAdrenoGpuContextBuffers(m.buf);
    if (m.buf_status) return false;
    static Window views[5];
    for (unsigned i = 0; i < 5; ++i) {
        views[i] = {m.buf[i].physical, m.buf[i].bytes, nullptr, 0};
        if (!mapOne(views[i], kIOMapWriteCombineCache)) return false;
        gGpuCpu[i] = reinterpret_cast<volatile uint32_t *>(views[i].va);
        ++m.views;
    }
    m.step = 4;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    if (!vm) return false;
    m.sqe_alloc = gSqe.allocate(kSqeBytes, m.ctx.output_bits, true, kSqeBytes);
    if (m.sqe_alloc || gSqe.spanCount() != 1) return false;
    uint64_t handle = 0;
    m.sqe_map = vm->map(&gSqe, 0, kSqeIova, 5, 0, 2, &handle);   // read + execute, as the kit maps its SQE buffer
    if (m.sqe_map) return false;
    m.sqe_phys = gSqe.spans()[0].physical;
    static Window sqe;
    sqe = {m.sqe_phys, kSqeBytes, nullptr, 0};
    if (!mapOne(sqe, kIOMapWriteCombineCache)) return false;
    // 2. The microcode without its first dword, read back in full.
    m.step = 5;
    if (myron_sqe_firmware_size < 8 || myron_sqe_firmware_size - 4 > kSqeBytes || myron_aqe_firmware_size < 8 ||
        myron_aqe_firmware_size - 4 > m.buf[0].bytes) return false;
    volatile uint8_t *sqeCpu = reinterpret_cast<volatile uint8_t *>(sqe.va), *aqeCpu = reinterpret_cast<volatile uint8_t *>(gGpuCpu[0]);
    for (uint64_t i = 4; i < myron_sqe_firmware_size; ++i) sqeCpu[i - 4] = myron_sqe_firmware[i];
    for (uint64_t i = 4; i < myron_aqe_firmware_size; ++i) aqeCpu[i - 4] = myron_aqe_firmware[i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint64_t i = 4; i < myron_sqe_firmware_size && !m.sqe_bad; ++i) if (sqeCpu[i - 4] != myron_sqe_firmware[i]) m.sqe_bad = uint32_t(i - 3);
    for (uint64_t i = 4; i < myron_aqe_firmware_size && !m.aqe_bad; ++i) if (aqeCpu[i - 4] != myron_aqe_firmware[i]) m.aqe_bad = uint32_t(i - 3);
    if (m.sqe_bad || m.aqe_bad) return false;
    // The power-up list stays empty (all lengths 0), as Linux's is at the first CP_ME_INIT: a8xx_patch_pwrup_reglist runs
    // after it. The ring, memptrs and scratch start zeroed.
    for (unsigned i = 1; i < 5; ++i) for (size_t w = 0; w < m.buf[i].bytes / 4; ++w) gGpuCpu[i][w] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}

void mode8() {
    if (!cpPrepare()) return;
    Mode8 &m = gM8;
    // 3. adreno_hw_init's ring reset, then the microcode bases, the ring, the rptr shadows (memptrs+0, bv_rptr at +8) and the
    // SQE start.
    m.step = 6;
    gpuWr64(A840_CP_SQE_INSTR_BASE, kSqeIova);
    gpuWr64(A840_CP_AQE_INSTR_BASE_0, m.buf[0].iova);
    gpuWr64(A840_CP_RB_BASE, m.buf[1].iova);
    gpuWr(A840_CP_RB_CNTL, kRbCntl);
    gpuWr64(A840_CP_RB_RPTR_ADDR, m.buf[2].iova);
    gpuWr64(A840_CP_RB_RPTR_ADDR_BV, m.buf[2].iova + 8);
    m.rptr0 = gpuRd(A840_CP_RB_RPTR);
    m.wptr0 = gpuRd(A840_CP_RB_WPTR);
    if (m.rptr0 != m.wptr0 || m.rptr0 > kRingMask) return;   // the CP would run whatever lies between them
    gWptr = m.wptr0;
    gpuWr(A840_CP_SQE_CNTL, 1);
    // 4. a8xx_cp_init: concurrent binning off, then CP_ME_INIT (multiple contexts, error detection, default reset state, no
    // perfcounter save/restore, the register list with its lock), then a8xx_idle: rptr caught up, RBBM idle.
    m.step = 7;
    const uint32_t thread = 1u << 27;
    const uint32_t init[7] = {kMeInitMask, 0x00000003, 0x20000000, 0x00000002, uint32_t(m.buf[3].iova), uint32_t(m.buf[3].iova >> 32),
                              1u << 31};
    if (!pkt7(kPm4ThreadControl, &thread, 1) || !pkt7(kPm4MeInit, init, 7)) return;
    m.wptr = gWptr & kRingMask;
    ringKick();
    for (m.init_polls = 1;; ++m.init_polls) {
        const uint32_t rptr = gpuRd(A840_CP_RB_RPTR), status = gpuRd(A840_RBBM_STATUS);
        if (rptr == m.wptr && gGpuCpu[2][0] == m.wptr && !(status & ~1u)) {m.init_idle = 1; break;}
        if (m.init_polls >= 2000) break;
        IODelay(100);
    }
    cpSnapshot(m);
    if (m.rptr != m.wptr) return;
    // 5. The first command: CP_SCRATCH_GLOBAL(2) and a CP_MEM_WRITE into the scratch page, then the timestamp fence.
    m.step = 8;
    const uint64_t at = m.buf[4].iova;
    const uint32_t write[3] = {uint32_t(at), uint32_t(at >> 32), kScratchMagic};
    if (!pkt4(A840_CP_SCRATCH_GLOBAL(2), 1) || !pkt7(kPm4MemWrite, write, 3)) return;
    m.step = 9;
    const bool fenced = fenceAndWait(1, m.fence_polls);
    cpSnapshot(m);
    m.fence = gGpuCpu[2][1];
    m.scratch = gGpuCpu[4][0];
    m.scratch_reg = gpuRd(A840_CP_SCRATCH_GLOBAL(2));
    if (fenced && m.scratch == kScratchMagic) m.step = 10;   // the CP ran a command and wrote memory through its own context
}

// Mode 9. zap_shader_load_mdt / qcom_mdt_load as sheng's zapAndSecure makes them, into myron's gpu_microcode_region (the
// zap-shader memory-region of the kgsl node): PAS 13 init_image with the ELF header and the hash segment, mem_setup over
// the relocatable span, the segment copy, auth_and_reset; then, only if TrustZone accepted it, hw_init's CP_SET_SECURE_MODE 0
// with a fence, RB_GC_GMEM_PROTECT (after the exit, as hw_init orders it) and a second fenced CP_MEM_WRITE.
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}   // SIP, QCOM_SCM_SVC_PIL
constexpr uint32_t kGpuPasId = 13;                                                    // GPU_PAS_ID
constexpr uint64_t kZapPhys = 0x9959a000, kZapBytes = 0x2000;                         // gpu_microcode_region@9959a000
NativeAdrenoMemory gZapMetadata;
uint32_t rd32le(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t rd16le(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
int64_t scmResult(const ScmRes &r) {return r.a0 ? r.a0 : int64_t(r.a1);}

// The TrustZone half of mode 9, shared with the run mode: true once PAS 13 authenticated the zap shader.
bool zapLoad() {
    Mode9 &m = gM9;
    m.ran = 1;
    const uint8_t *fw = myron_zap_firmware;
    const uint64_t size = myron_zap_firmware_size;
    // mdt_header_valid / qcom_mdt_read_metadata: ELF32, 32-byte program headers inside the file, at least two, the first not
    // PT_LOAD, a hash segment (segment type 2 in p_flags bits 26:24).
    m.stage = 1;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) {m.stage = -1; return false;}
    const uint32_t phoff = rd32le(fw + 28);
    const unsigned phent = rd16le(fw + 42), phnum = rd16le(fw + 44);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) {m.stage = -2; return false;}
    auto ph = [&](unsigned i, unsigned field) {return rd32le(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) {m.stage = -3; return false;}
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) {m.stage = -4; return false;}
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    if (lo >= hi || hi - lo > kZapBytes) {m.stage = -5; return false;}
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) {m.stage = -6; return false;}
    // Metadata: physically contiguous, page aligned, pushed to the point of coherency (Linux: dma_alloc_coherent).
    m.stage = 2;
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    if (gZapMetadata.allocate(metaBytes, 40, true, metaBytes) || gZapMetadata.spanCount() != 1) {m.stage = -7; return false;}
    uint8_t *meta = static_cast<uint8_t *>(gZapMetadata.mapping());
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    if (gZapMetadata.publishForDevice(0, metaBytes, true)) {m.stage = -8; return false;}
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    m.stage = 3;
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, gZapMetadata.spans()[0].physical);
    m.init = scmResult(r);
    m.resumes[0] = r.resumes;
    if (m.init) return false;
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        m.stage = 4;
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        m.mem = scmResult(r);
        m.resumes[1] = r.resumes;
        if (m.mem) return false;
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address); the CPU view is
    // gone before TrustZone takes the region.
    m.stage = 5;
    {
        Window region = {kZapPhys, kZapBytes, nullptr, 0};
        if (!mapOne(region, kIOMapWriteCombineCache)) {m.stage = -9; return false;}
        volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(region.va);
        const uint64_t base = relocate ? lo : kZapPhys;
        bool fits = true;
        for (unsigned i = 0; i < phnum && fits; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t to = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || to + mem > kZapBytes || file > mem || off + file > size) {fits = false; break;}
            for (uint64_t k = 0; k < file; ++k) dst[to + k] = fw[off + k];
            for (uint64_t k = file; k < mem; ++k) dst[to + k] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        region.map->release();
        if (!fits) {m.stage = -10; return false;}
    }
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    m.stage = 6;
    r = scmCall(kScmPil(0x05), 1, kGpuPasId);
    m.auth = scmResult(r);
    m.resumes[2] = r.resumes;
    if (m.auth) return false;
    return true;
}

void mode9() {
    if (gM8.step != 10 || !zapLoad()) return;
    Mode9 &m = gM9;
    m.stage = 7;
    static const uint32_t secure = 0;
    bool fenced = pkt7(kPm4SetSecureMode, &secure, 1) && fenceAndWait(2, m.polls);
    if (fenced) {
        pipeWr(A840_PIPE_BR, A840_RB_GC_GMEM_PROTECT, A840_GMEM_PROTECT);
        aperture(A840_PIPE_NONE);
        m.stage = 8;
        const uint64_t at = gM8.buf[4].iova + 4;
        const uint32_t write[3] = {uint32_t(at), uint32_t(at >> 32), kScratchMagic2};
        fenced = pkt7(kPm4MemWrite, write, 3) && fenceAndWait(3, m.polls2);
    }
    m.fence = gGpuCpu[2][1];
    m.scratch = gGpuCpu[4][1];
    m.status = gpuRd(A840_RBBM_STATUS);
    m.int0 = gpuRd(A840_RBBM_INT_0_STATUS);
    m.cp_int = gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL);
    const size_t b = bank0();
    m.fsr = smmuRd(b ? b + 0x58 : 0);
    if (m.stage == 8 && fenced && m.scratch == kScratchMagic2) m.stage = 9;   // out of secure mode and still running commands
}

// Modes 10 and 11, the run mode: after mode 7's registers and cpPrepare's buffers and microcode, the command processor through
// NativeAdreno's own ring, CP start, interrupt decoder and timer on the A8XX register table, the zap shader, the GPU interrupt,
// the timer and the published service with two client rounds through it, as sheng's gpuStart and publishAndWrapperTest make
// them for the A740.
struct Mode10 {
    int ran;
    unsigned step;   // 1 ring attached, 2 CP init fenced, 3 first write seen, 4 zap and secure exit, 5 interrupt, 6 timer,
                     // 7 submit attached, 8 published, 9 client rounds run, 10 both rounds read back
    IOReturn ring_attach, cp_init, irq_mask, irq_attach, timer, submit, publish;
    int init_wait, write_wait, secure_wait, nop_wait;   // NativeAdrenoRingFence
    uint32_t init_fence, init_seen, write_seen, scratch, secure_seen, irq_readback, irq_before, irq_after, irq_seen;
    uint64_t irq_router, ticks0, ticks1;
    int rounds[2], round_ok, round_steps, round_err_at;
    IOReturn round_err;
    uint32_t mem_status, opened, closed, status, int0, cp_int, fsr, gfsr;
};
Mode10 gM10;
NativeAdrenoRing gRing;
NativeAdrenoCpInit gCpInit;
NativeAdrenoIrq gIrq;
NativeAdrenoTimer gTimer;
NativeAdrenoSubmit gSubmit;
unsigned gIbThread;   // myron-gpu-ib-thread: 0 per job (continuation jobs on BR), 1 BR, 3 BOTH
volatile bool gIrqAttached;
volatile uint32_t gIrqCount, gIrqEmpty, gIrqSeen;
// The A840 as a client sees it: the KGSL chip id with its three slices (0x44050a31, as a8xx_gpu_get_slice_info makes it and Mesa
// names the Adreno 840), 18 MiB of GMEM at the UCHE GMEM window hw_init programs, the trap base, a client window above every
// kernel buffer and the GMEM window inside the 2 GiB aperture, and kaanapali's UBWC 6.0 (highest bank bit 16, swizzle levels 2
// and 3, eight-channel macrotiles).
const NativeAdrenoAbiDeviceInfo gDeviceInfo = {0x44050a31u, 18u << 20, kGmemRangeMin, kUcheTrapBase, 0x20000000ull, 0x60000000ull,
                                               16, 6, 1, 6, NativeAdrenoDeviceClientIova, 0};

int ringWrite(void *, uint32_t reg, uint32_t value) {return gwr(nullptr, MGM_WIN_GPU, reg * 4, value);}
int ringRead(void *, uint32_t reg, uint32_t *value) {return grd(nullptr, MGM_WIN_GPU, reg * 4, value);}
int ringBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The ring, memptrs and command buffers are Normal non-cacheable on both sides (GPU PTEs, CPU write-combined): ordering only.
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t ringNow(void *) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
// Once the GPU line is attached, a waiter that may block sleeps until the next GPU interrupt or 100 us; otherwise it spins 20 us.
int ringIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
    } else {
        IODelay(20);
    }
    return 0;
}
const NativeAdrenoRingIo gRingIo{nullptr, ringWrite, ringRead, ringBarrier};
const NativeAdrenoRingCache gRingCache{nullptr, ringPublish, ringAcquire};
const NativeAdrenoRingClock gClock{nullptr, ringNow, ringIdle};
int timerRead(void *, uint32_t reg, uint32_t *value) {return grd(nullptr, MGM_WIN_GPU, reg * 4, value);}
const NativeAdrenoTimeIo gTimerIo{nullptr, timerRead};
// The GPU interrupt (level): NativeAdrenoIrq reads RBBM_INT_0_STATUS and clears it before the EOI. A source it cannot clear would
// keep the boot core in this handler, so a long run of empty reads masks the GPU's interrupts instead, as sheng's handler does.
void gpuIrq(void *) {
    NativeAdrenoIrqResult r{};
    const bool handled = gIrq.handle(&r) == kIOReturnSuccess;
    gIrqSeen = gIrqSeen | r.status;
    gIrqEmpty = handled && r.status ? 0 : gIrqEmpty + 1;
    if (gIrqEmpty > 100000) gwr(nullptr, MGM_WIN_GPU, A840_RBBM_INT_0_MASK * 4, 0);
    gIrqCount = gIrqCount + 1;
    thread_wakeup(event_t(&gIrqCount));
}
// One plain kernel submission (the words, CP_SCRATCH_GLOBAL(2) and a CACHE_CLEAN timestamp) and the wait for its fence.
bool ringFence(const uint32_t *words, size_t count, int &result, uint32_t *seen) {
    uint32_t fence = 0;
    if (gRing.submit(words, count, &fence)) return false;
    result = gRing.wait(fence, &gClock, 200000, seen);
    return result == NativeAdrenoRingFenceComplete;
}
// ---- IO coherence probe, adapted from sheng's GpuPowerProbe.cpp coherenceProbe ----
// A740 on sheng and the A840 here both sit behind a dma-coherent GPU SMMU, and Linux maps cached BOs with IOMMU_CACHE
// (write-back, inner shareable). The probe imports 16 KiB of kernel memory that way and proves both directions against DRAM:
//   read:  the CPU stores new values over lines DRAM still holds old (counted dirty through an uncached mapping), the GPU
//          copies the page, and the copy DRAM holds must be the new values, which only the CPU's caches had;
//   write: the CPU keeps clean lines of a page it read, the GPU copies other values over it, and plain cached loads (no
//          invalidate) must return the GPU's values.
// Only both proofs set native_adreno_io_coherent: imported memory is then mapped write-back inner-shareable (read-only
// imports fully; GPU-written ones stay non-coherent per the upstream import patch) and publishMapped() stops cleaning every
// non-coherent mapping on every submission (about 60 ms a frame as measured upstream).
// Row: H d<dirty lines of 64> a<attempts> r<GPU read new>/<old> w<CPU saw GPU>/<DRAM> s<status> k<stage> -> on/off/skip
char myron_gpu_coherence_row[120];
uint32_t myron_gpu_coherence[8];
bool myronGpuCopyPage(uint64_t src, uint64_t dst) {
    uint32_t words[8] = {};
    if (AdrenoPM4Type7(0x75, 5, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x12, 0, &words[6]) != AdrenoPM4Success) return false;
    words[1] = 1024; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst); words[5] = uint32_t(dst >> 32);
    int result = 0;
    return ringFence(words, 7, result, nullptr);
}
void myronCoherenceProbe(NativeAdrenoVm *vm) {
    uint32_t *k = myron_gpu_coherence;
    unsigned wanted = 1;
    PE_parse_boot_argn("myron-gpu-coherent", &wanted, sizeof wanted);
    if (!wanted || !vm) {
        snprintf(myron_gpu_coherence_row, sizeof myron_gpu_coherence_row, "H skip (arg %u vm %d)", wanted, vm != nullptr);
        return;
    }
    constexpr uint64_t kIova = 0x18000000;
    constexpr size_t kBytes = 4 * 4096;
    // Kernel data, page aligned, not IOMallocAligned: the IOMalloc pages on myron took the CPU stores straight through
    // (the DRAM read already held the new values), so the dirty-line precondition could never hold.
    static uint32_t probeBuffer[4 * 1024] __attribute__((aligned(4096)));
    uint8_t *buf = reinterpret_cast<uint8_t *>(probeBuffer);
    NativeAdrenoMemory mem;
    uint64_t handle = 0;
    IOReturn st = mem.importTask(kernel_task, uint64_t(uintptr_t(buf)), kBytes, 40, kBytes);
    k[0] = 1;
    if (!st) {k[0] = 2; st = vm->map(&mem, 0, kIova, 3, 1, 3, &handle);}
    uint64_t phys[4] = {};
    for (size_t j = 0; !st && j < mem.spanCount(); ++j)
        for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096) phys[(mem.spans()[j].offset + at) / 4096] = mem.spans()[j].physical + at;
    bool pass = false;
    if (!st) {
        k[0] = 3;
        volatile uint32_t *S = reinterpret_cast<volatile uint32_t *>(buf), *D = S + 1024, *R = S + 2048;
        // Cache-maintenance self-check, CPU paths only, no io mapping: A, clean it into DRAM, B (dirty), discard the line,
        // read the word back. A means the discard worked (so a store that survives it was never cached); B means the
        // maintenance calls do not reach this buffer at all. q1 = maintenance works, q2 = it does not, q3 = neither value.
        S[0] = 0xA5A5A5A5u;
        (void)NativeAdrenoMaintainOwnedPages(buf, 4096, NativeAdrenoCleanInvalidate);
        S[0] = 0xA5A5A5A5u;
        (void)NativeAdrenoMaintainOwnedPages(buf, 4096, NativeAdrenoClean);
        S[0] = 0x5A5A5A5Au;
        (void)NativeAdrenoMaintainOwnedPages(buf, 4096, NativeAdrenoInvalidate);
        __asm__ volatile("dsb sy" ::: "memory");
        k[1] = S[0] == 0xA5A5A5A5u ? 1 : S[0] == 0x5A5A5A5Au ? 2 : 3;
        for (unsigned i = 0; i < 1024; ++i) {S[i] = 0x11110000u | i; D[i] = 0; R[i] = 0x44440000u | i;}
        // Old values into DRAM, R into DRAM, D zero, and every line of the buffer dropped from the CPU caches.
        st = NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
        if (!st) {
            // Read direction: S exists only in the CPU caches now. The GPU copies S over D; the loads below find D's lines
            // absent and fetch D from memory, so 0x2222 there means the GPU read the CPU's dirty lines.
            for (unsigned i = 0; i < 1024; ++i) S[i] = 0x22220000u | i;
            __asm__ volatile("dsb sy" ::: "memory");
            if (myronGpuCopyPage(kIova, kIova + 4096)) {
                k[0] = 4;
                for (unsigned i = 0; i < 1024; ++i) {
                    const uint32_t v = D[i];
                    k[2] += v == (0x22220000u | i);
                    k[3] += v == (0x11110000u | i);
                }
                // Write direction: D's lines hold 0x3333 in the CPU caches, the GPU copies R (0x4444) over it; plain loads
                // returning 0x4444 mean the GPU's writes reach the CPU's caches.
                for (unsigned i = 0; i < 1024; ++i) D[i] = 0x33330000u | i;
                __asm__ volatile("dsb sy" ::: "memory");
                if (myronGpuCopyPage(kIova + 8192, kIova + 12288)) {
                    k[0] = 5;
                    for (unsigned i = 0; i < 1024; ++i) {
                        const uint32_t v = D[i];
                        k[4] += v == (0x44440000u | i);
                        k[5] += v == (0x33330000u | i);
                    }
                }
            }
        }
        /* Both directions are required. myron's CPU stores reach DRAM (the GPU sees them) but the GPU's writes do not
         * reach the CPU's caches (a plain load after a GPU copy still returned the CPU's value), so the coherent import
         * mode would skip the acquisition invalidate the port still needs. Measured: r1024/0 w0/1024 -> off. */
        pass = k[0] == 5 && k[2] == 1024 && k[4] == 1024;
    }
    k[6] = uint32_t(st);
    if (handle) (void)vm->unmap(handle);
    // Whatever the result, no line of these pages stays in a CPU cache (a stale clean copy would outlive the free).
    (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
    (void)mem.release();
    k[7] = pass;
    native_adreno_io_coherent = pass ? 1 : 0;
    snprintf(myron_gpu_coherence_row, sizeof myron_gpu_coherence_row, "H q%u r%u/%u w%u/%u s%x k%u -> %s", k[1], k[2], k[3], k[4], k[5], k[6], k[0],
             pass ? "on" : "off");
}

// One external-method call through the published service's user client, as a user-space caller's arrives.
IOReturn clientCall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
// Sheng's wrapperRound: the path a client takes, minus the Mach trap. newUserClient (a second one is refused while it is open),
// the device info, two buffer objects mapped at fixed GPU addresses and through clientMemoryForType into this task, an
// indirect buffer of three CP_MEM_WRITEs submitted (NativeAdrenoSubmit's framed submission on the A8XX ring) and waited for,
// the CPU's read-back, the timestamp, then clientClose (drain, unmap, free). Returns the matching dwords (of 4) or -1.
// The open type is the Adreno Metal plugin's, the only one NativeAdrenoPublish.cpp serves since cedcaa2 (it refuses every other
// with kIOReturnUnsupported; boot-myron-22's rounds opened with sheng's type 0 and were refused). The kernel task has its own single
// client slot, so a second open is still refused while one is live.
constexpr UInt32 kAdrenoPluginOpenType = 0x4147;
int clientRound(IOService *service) {
    Mode10 &m = gM10;
    auto step = [&](IOReturn s) {
        const int index = m.round_steps++;
        if (!s) ++m.round_ok;
        else if (!m.round_err) {m.round_err = s; m.round_err_at = index;}
        return !s;
    };
    IOUserClient *uc = nullptr;
    if (!step(service->newUserClient(kernel_task, nullptr, kAdrenoPluginOpenType, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(service->newUserClient(kernel_task, nullptr, kAdrenoPluginOpenType, nullptr, &second) == kIOReturnBusy ? 0 : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(clientCall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(clientCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == gDeviceInfo.chip_id && info.gmem_bytes == gDeviceInfo.gmem_bytes ? 0 : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, target{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(clientCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(clientCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &target))) {
        const uint64_t ibIova = 0x20004000, targetIova = 0x20005000;
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTarget{target.handle, targetIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply mr{};
        NativeAdrenoAbiBoCpuMap cpuIb{ib.handle}, cpuTarget{target.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTarget{};
        if (step(clientCall(uc, NativeAdrenoMethodBoMap, &mapIb, &mr)) && step(clientCall(uc, NativeAdrenoMethodBoMap, &mapTarget, &mr)) &&
            step(clientCall(uc, NativeAdrenoMethodBoCpuMap, &cpuIb, &crIb)) &&
            step(clientCall(uc, NativeAdrenoMethodBoCpuMap, &cpuTarget, &crTarget))) {
            IOOptionBits options[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTarget.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn s = uc->clientMemoryForType(tokens[i], &options[i], &desc[i]);
                if (!m.mem_status) m.mem_status = uint32_t(s);
                if (s || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, options[i]);
                if (!maps[i]) {mapped = false; if (!m.mem_status) m.mem_status = 0xbad;}
            }
            step(mapped ? 0 : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0a840c0d, 0xfeed0840, 0x600d0000u | uint32_t(m.round_steps)};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(kPm4MemWrite, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(targetIova + i * 4); words[i * 4 + 2] = uint32_t(targetIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ibIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? 0 : kIOReturnInternalError) && step(clientCall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(clientCall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(clientCall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *map : maps) if (map) map->release();
    for (auto *d : desc) if (d) d->release();
    step(uc->clientClose());
    uc->release();
    return matched;
}

void runSteps() {
    Mode10 &m = gM10;
    // The BO pool, which NativeAdrenoGpuContextBringup reserves while long contiguous runs remain: every client's buffers come from one
    // block whose pages never go back to the VM and take the write-combine attribute once. sheng's NativeAdrenoService sets its size
    // from sheng-gpu-pool (1536 MiB); myron did not link that, so boot-myron-28 had no pool: each BO changed its own pages' attribute,
    // and a freed BO whose CPU view a client still had mapped handed mapped pages back to the VM (pmap_assert_free). XNU has 4.75 GiB
    // here, so 1024 MiB by default (sheng's peak use was 782 MiB); a size that cannot be had is halved down to 16 MiB.
    unsigned pool = 1024;
    PE_parse_boot_argn("sheng-gpu-pool", &pool, sizeof pool);
    native_adreno_pool_mib = pool;
    if (!cpPrepare()) return;
    m.ran = 1;
    const Mode8 &p = gM8;
    // adreno_hw_init's ring through NativeAdrenoRing on the A8XX table (base, control, rptr shadow at memptrs+0, WPTR and the fence
    // scratch), then what the ring object does not program: the microcode bases and the BV rptr shadow at memptrs+8.
    if (gRing.useChip(&NativeAdrenoChipA8xx)) return;
    // The user words' thread, myron-gpu-ib-thread: 0 (default) per job, through NativeAdrenoSubmit::setContinuationThread below; 1 BR
    // for every job; 3 BOTH for every job (a7xx_submit). boot-myron-37's debug page showed a job that began inside a Turnip command buffer
    // (row-by-row 2D blits of the scanout copy) run its blits on BV under BOTH while BR waited for idle; boot-myron-38, with every job
    // on BR, stopped before the Apple logo because BV never saw Turnip's command-buffer prologue.
    PE_parse_boot_argn("myron-gpu-ib-thread", &gIbThread, sizeof gIbThread);
    if (gIbThread != 1 && gIbThread != 3) gIbThread = 0;
    if (gRing.setUserThread(gIbThread == 1 ? 1 : 3)) return;
    m.ring_attach = gRing.attach(&gRingIo, &gRingCache, const_cast<uint32_t *>(gGpuCpu[1]), p.buf[1].bytes, p.buf[1].iova,
                                 const_cast<uint32_t *>(gGpuCpu[2]), p.buf[2].bytes, p.buf[2].iova);
    if (m.ring_attach) return;
    m.step = 1;
    gpuWr64(A840_CP_SQE_INSTR_BASE, kSqeIova);
    gpuWr64(A840_CP_AQE_INSTR_BASE_0, p.buf[0].iova);
    gpuWr64(A840_CP_RB_RPTR_ADDR_BV, p.buf[2].iova + 8);
    gpuWr(A840_CP_SQE_CNTL, 1);
    // a8xx_cp_init through NativeAdrenoCpInit, which reads back the SQE base written above, before CP_SQE_CNTL.
    const NativeAdrenoCpInitConfig config{kSqeIova, p.buf[3].iova, 1};
    if (gCpInit.useChip(&NativeAdrenoChipA8xx)) return;
    m.cp_init = gCpInit.run(&gRingIo, &gRing, &config, &m.init_fence);
    if (m.cp_init) return;
    m.init_wait = gRing.wait(m.init_fence, &gClock, 200000, &m.init_seen);
    if (m.init_wait) return;
    m.step = 2;
    // The first command after it: a fenced CP_MEM_WRITE into the scratch page.
    volatile uint32_t *scratch = gGpuCpu[4];
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t write[4] = {};
    if (AdrenoPM4Type7(kPm4MemWrite, 3, &write[0]) != AdrenoPM4Success) return;
    write[1] = uint32_t(p.buf[4].iova); write[2] = uint32_t(p.buf[4].iova >> 32); write[3] = kScratchMagic;
    if (!ringFence(write, 4, m.write_wait, &m.write_seen)) return;
    m.scratch = scratch[0];
    if (m.scratch != kScratchMagic) return;
    m.step = 3;
    // The zap shader through TrustZone, then CP_SET_SECURE_MODE 0 and GMEM protection, in hw_init's order.
    if (!zapLoad()) return;
    uint32_t secure[2] = {};
    if (AdrenoPM4Type7(kPm4SetSecureMode, 1, &secure[0]) != AdrenoPM4Success) return;
    if (!ringFence(secure, 2, m.secure_wait, &m.secure_seen)) return;
    pipeWr(A840_PIPE_BR, A840_RB_GC_GMEM_PROTECT, A840_GMEM_PROTECT);
    aperture(A840_PIPE_NONE);
    m.step = 4;
    // The GPU interrupt: the A8XX decoder and its enable set (read back), then SPI 300 (INTID 332, level) on the boot core. The
    // fences above left CP_CACHE_FLUSH_TS set, so a working line fires at once; one more fence (a CP_NOP) must fire it again.
    if (gIrq.useChip(&NativeAdrenoChipA8xx) || gIrq.attach(&gRingIo)) return;
    uint32_t programmed = 0;
    m.irq_mask = gIrq.programMask(&programmed);
    if (m.irq_mask) return;
    m.irq_attach = armcpu_spi_attach(332, false, gpuIrq, nullptr, &m.irq_readback, &m.irq_router);
    gIrqAttached = m.irq_attach == kIOReturnSuccess;
    IOSleep(20);
    m.irq_before = gIrqCount;
    uint32_t nop = 0;
    if (AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success) ringFence(&nop, 1, m.nop_wait, nullptr);
    IOSleep(20);
    m.irq_after = gIrqCount;
    m.irq_seen = gIrqSeen;
    m.step = 5;
    // The always-on counter (19.2 MHz) through the A8XX table, twice, 10 ms apart.
    if (gTimer.useChip(&NativeAdrenoChipA8xx)) return;
    m.timer = gTimer.attach(&gTimerIo, NativeAdrenoTimeCp);
    if (m.timer) return;
    gTimer.read(&m.ticks0);
    IOSleep(10);
    gTimer.read(&m.ticks1);
    m.step = 6;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    // Before any client maps an import: prove IO coherence and, if both directions hold, switch imports to coherent mappings.
    myronCoherenceProbe(vm);
    // Per-submission clean/invalidate of every imported page: NativeMetal already flushes the surfaces a command buffer uses
    // (flushSurfaces at commit and completion; ios-pandora 0195 dropped the kernel loop for the same reason). Off by default,
    // myron-gpu-flushall=1 restores it.
    unsigned flushImports = 0;
    PE_parse_boot_argn("myron-gpu-flushall", &flushImports, sizeof flushImports);
    native_adreno_flush_imports = flushImports ? 1 : 0;
    m.submit = vm ? gSubmit.attach(vm, &gRing) : kIOReturnNotReady;
    if (m.submit) return;
    gSubmit.setContinuationThread(gIbThread == 0);
    m.step = 7;
    m.publish = native_adreno_publish(vm, &gSubmit, &gRing, &gClock, &gTimer, &gDeviceInfo, 31, 40);
    IOService *service = m.publish ? nullptr : native_adreno_published_service();
    if (!service) return;
    m.step = 8;
    m.rounds[0] = clientRound(service);
    m.rounds[1] = clientRound(service);   // the client slot must recycle; the close unmapped the round's addresses
    m.opened = native_adreno_publish_stats(&m.closed);
    m.step = 9;
    if (m.rounds[0] == 4 && m.rounds[1] == 4) m.step = 10;
}

// The GPU rows of the diagnostic page (47 characters each), for the run mode that does not panic.
void runRow(unsigned slot, const char *text) {
    unsigned i = 0;
    for (; i < 47 && text[i]; ++i) boot_progress_ext[slot][i] = text[i];
    for (; i < 47; ++i) boot_progress_ext[slot][i] = ' ';
    boot_progress_ext[slot][47] = 0;
}
// GPU frequency scaling (DCVS), ported from Data A Live's pandora 0198 for the same Adreno 840. The GMU's CX power counter 0
// counts XO (19.2 MHz) ticks while the GPU is busy (Linux a8xx hw_init: GPU_GMU_AO_GPU_CX_BUSY_MASK 0xff000000,
// POWER_COUNTER_SELECT_XOCLK_0 countable 5, POWER_COUNTER_ENABLE). Every 16 ms:
//   - the frequency that would have kept the window 50 % busy;
//   - up at once to the lowest level at or above 1.1 x that; to the top after a window over 85 % busy or 50 ms busy
//     without a break;
//   - down one level only after 4 windows in a row (one more right after a step up) that would have needed at most
//     0.8 x the next lower level; windows without work do not count;
//   - 80 ms without work: back to the initial level 3 (222 MHz);
//   - work after 50 ms without a submission: a floor of max(2 x the current frequency, 578 MHz) for 50 ms;
//   - bandwidth: the level's device-tree bus-freq index, in the same GX_BW_PERF_VOTE.
// myron-gpu-dcvs=1 enables it (the fixed myron-gpu-perf vote remains the default); myron-gpu-dcvs-max caps the level
// (default 18) and myron-gpu-dcvs-min sets a floor (default 1, e.g. 8 = 461 MHz for a DDR-bound UI). The CPU thermal
// controller (MyronCpuPerf.c) caps it through myron_gpu_thermal_top, and the cap wins over the floor.
static int gDcvsWait;
static uint32_t gDcvsLevel, gDcvsChanges;   /* 0 = DCVS off, the fixed myron-gpu-perf vote is in effect */

static uint64_t gpuDcvsBusyTicks() {
    uint32_t hi = 0, lo = 0, hi2 = 0;
    if (grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_XO0_H), &hi)) return 0;
    if (grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_XO0_L), &lo)) return 0;
    if (grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_XO0_H), &hi2)) return 0;
    if (hi2 != hi && grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_XO0_L), &lo)) return 0;
    return uint64_t(hi2) << 32 | lo;
}

static bool gpuDcvsVote(uint32_t level) {
    uint32_t vote[3] = {1, level, mhf_gpu_levels[level].bw};
    return exchange(4, MHF_MSG_GX_BW_PERF_VOTE, vote, 3);
}

static uint32_t gpuDcvsLevelFor(uint64_t hz, uint32_t top) {
    uint32_t l = 1;
    while (l < top && uint64_t(mhf_gpu_levels[l].khz) * 1000 < hz) ++l;
    return l;
}

static void gpuDcvsThread(void *, wait_result_t) {
    uint32_t topMax = MHF_GPU_LEVELS - 1, arg = 0, minLevel = 1;
    if (PE_parse_boot_argn("myron-gpu-dcvs-max", &arg, sizeof arg) && arg >= 1 && arg < MHF_GPU_LEVELS) topMax = arg;
    arg = 0;
    if (PE_parse_boot_argn("myron-gpu-dcvs-min", &arg, sizeof arg) && arg >= 1 && arg <= topMax) minLevel = arg;
    const uint32_t start = topMax >= 3 ? 3 : 1;
    uint32_t cur = minLevel > start ? minLevel : start;
    gDcvsLevel = cur;
    // The busy counter: the GPU's CX busy mask, XOCLK_0 as the source, then enable.
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_AO_GPU_CX_BUSY_MASK), 0xff000000u);
    uint32_t sel = 0;
    if (!grd(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_SELECT_XO0), &sel))
        gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_SELECT_XO0), (sel & ~0xffu) | (1u << 5));
    gwr(nullptr, MGM_WIN_GMU, MGM_GMU(MGM_POWER_COUNTER_ENABLE), 1);
    if (gpuDcvsVote(cur)) gDcvsLevel = cur;
    IOLog("myron GPU DCVS: on, levels %u..%u (%u..%u MHz), every 16 ms, from %u MHz\n", minLevel, topMax,
          mhf_gpu_levels[minLevel].khz / 1000, mhf_gpu_levels[topMax].khz / 1000, mhf_gpu_levels[cur].khz / 1000);
    uint64_t boostTicks = 0;
    nanoseconds_to_absolutetime(50ull * NSEC_PER_MSEC, &boostTicks);
    uint64_t lastBusy = gpuDcvsBusyTicks(), lastT = mach_absolute_time(), idleSince = lastT;
    uint64_t runNs = 0, boostUntil = 0, boostHz = 0;
    uint64_t winBusyNs = 0, winNs = 0, atLevel[MHF_GPU_LEVELS] = {};
    uint32_t down = 0, hold = 0, changes = 0, failed = 0, boosts = 0;
    uint32_t lastSubmitted = gRing.lastSubmitted();
    uint64_t lastSubmitT = 0;
    for (;;) {
        uint64_t deadline;
        clock_interval_to_deadline(16, NSEC_PER_MSEC, &deadline);
        (void)assert_wait_deadline(event_t(&gDcvsWait), THREAD_UNINT, deadline);
        thread_block(THREAD_CONTINUE_NULL);
        const uint64_t busy = gpuDcvsBusyTicks(), t = mach_absolute_time();
        uint64_t ns = 0, sinceSubmit = 0;
        absolutetime_to_nanoseconds(t - lastT, &ns);
        if (!ns) continue;
        const uint64_t busyDelta = (busy - lastBusy) * 625 / 12;
        const uint64_t busyNs = busyDelta < ns ? busyDelta : ns;
        lastBusy = busy;
        lastT = t;
        const uint32_t submitted = gRing.lastSubmitted();
        if (submitted != lastSubmitted) {
            if (lastSubmitT) {
                absolutetime_to_nanoseconds(t - lastSubmitT, &sinceSubmit);
                if (sinceSubmit >= 50ull * NSEC_PER_MSEC) {
                    boostHz = 2ull * uint64_t(mhf_gpu_levels[cur].khz) * 1000;
                    if (boostHz < 578000000ull) boostHz = 578000000ull;
                    boostUntil = t + boostTicks;
                    boosts++;
                }
            }
            lastSubmitT = t;
            lastSubmitted = submitted;
        }
        const bool pending = int32_t(submitted - gRing.completedFence()) > 0;
        const bool active = busyNs > 0 || pending;
        uint32_t top = topMax;
        const uint32_t cap = __atomic_load_n(&myron_gpu_thermal_top, __ATOMIC_RELAXED);
        if (cap && cap < top) top = cap < 1 ? 1 : cap;
        uint64_t floor = t < boostUntil ? gpuDcvsLevelFor(boostHz, top) : minLevel;
        if (floor > top) floor = top;
        const uint64_t curHz = uint64_t(mhf_gpu_levels[cur].khz) * 1000;
        const uint64_t need = busyNs * curHz * 2 / ns;
        runNs = busyNs * 100 >= ns * 95 ? runNs + ns : 0;
        uint32_t want = cur;
        if (cur > top) {
            want = top;
            down = 0;
        } else if (busyNs * 100 > ns * 85 || runNs >= 50ull * NSEC_PER_MSEC) {
            want = top;
        } else {
            const uint32_t up = gpuDcvsLevelFor(need * 11 / 10, top);
            if (up > cur) {
                want = up;
            } else if (active) {
                if (cur > 1 && need * 10 <= uint64_t(mhf_gpu_levels[cur - 1].khz) * 1000 * 8) {
                    if (++down >= 4 + hold) want = cur - 1;
                } else {
                    down = 0;
                }
            } else {
                uint64_t idleNs = 0;
                absolutetime_to_nanoseconds(t - idleSince, &idleNs);
                if (idleNs >= 80ull * NSEC_PER_MSEC && cur != start && cur > minLevel) want = start > minLevel ? start : minLevel;
            }
        }
        if (active) idleSince = t;
        if (want < floor) want = floor;
        if (want != cur) {
            if (gpuDcvsVote(want)) {
                hold = want > cur ? 1 : 0;
                cur = want;
                gDcvsLevel = cur;
                changes++;
                gDcvsChanges++;
            } else {
                failed++;
            }
            down = 0;
        }
        winBusyNs += busyNs;
        winNs += ns;
        atLevel[cur] += ns;
        if (winNs >= 10ull * NSEC_PER_SEC) {
            char used[200];
            int n = 0;
            used[0] = 0;
            for (uint32_t l = 1; l < MHF_GPU_LEVELS && n >= 0 && n < (int)sizeof used; ++l) {
                if (atLevel[l] * 100 / winNs >= 1)
                    n += snprintf(used + n, sizeof used - n, " %u:%llu%%", mhf_gpu_levels[l].khz / 1000, atLevel[l] * 100 / winNs);
                atLevel[l] = 0;
            }
            IOLog("myron GPU DCVS: busy %llu%%, MHz:time%s, %u changes, %u boosts, %u failed, now %u MHz\n",
                  winBusyNs * 100 / winNs, used, changes, boosts, failed, mhf_gpu_levels[cur].khz / 1000);
            winBusyNs = winNs = 0;
            changes = failed = boosts = 0;
        }
    }
}

static void gpuDcvsStart() {
    unsigned on = 0;
    if (!PE_parse_boot_argn("myron-gpu-dcvs", &on, sizeof on) || on != 1) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuDcvsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

void run10(unsigned mode) {
    Mode10 &m = gM10;
    runSteps();
    m.status = gpuRd(A840_RBBM_STATUS);
    m.int0 = gpuRd(A840_RBBM_INT_0_STATUS);
    m.cp_int = gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL);
    const size_t b = gSmmu.va ? bank0() : 0;
    m.fsr = smmuRd(b ? b + 0x58 : 0);
    m.gfsr = gSmmu.va ? smmuRd(0x48) : 0xdead;
    if (mode != 11) return;
    if (m.step >= 8) gpuDcvsStart();   /* myron-gpu-dcvs=1: scale the GPU from here on */
    char row[64];
    snprintf(row, sizeof row, "GPU run s%u ring%x cp%x/%d zap%d sec%d", m.step, unsigned(m.ring_attach), unsigned(m.cp_init),
             m.init_wait, gM9.stage, m.secure_wait);
    runRow(4, row);
    snprintf(row, sizeof row, "GPU irq%x %u>%u seen%x tmr%x", unsigned(m.irq_attach), m.irq_before, m.irq_after, m.irq_seen,
             unsigned(m.timer));
    runRow(5, row);
    snprintf(row, sizeof row, "GPU pub%x r%d/%d ok%d/%d e%x@%d o%u/%u", unsigned(m.publish), m.rounds[0], m.rounds[1], m.round_ok,
             m.round_steps, unsigned(m.round_err), m.round_err_at, m.opened, m.closed);
    runRow(6, row);
    snprintf(row, sizeof row, "GPU st%08x int%08x fsr%x", m.status, m.int0, m.fsr);
    runRow(7, row);
    if (myron_gpu_coherence_row[0]) {
        unsigned i = 0;
        for (; i < 111 && myron_gpu_coherence_row[i]; ++i) boot_progress_ext_wide[0][i] = myron_gpu_coherence_row[i];
        boot_progress_ext_wide[0][i] = 0;
    }
}

// The Metal accelerator (myron-gpu-metal=1, after a run that published the GPU service), sheng's publishMetal: the original
// IOGPUFamily kext is loaded so that the IOAcceleratorES metaclass exists, the staged AGXAccelerator registers its class between
// preModLoad and postModLoad, and one instance goes on the platform expert with the keys the original Metal discovery reads
// (MetalPluginName -> /System/Library/Extensions/NativeAdrenoMetal.bundle, which the GPU addon mounts; MetalPluginClassName) and
// the processes patch zzw-native-adreno-sandbox-grant admits in place of the sandbox verdict. No Apple GPU identity is claimed.
// Stages: 1 kext load attempted, 2 parent metaclass found, 3 own class registered, 4 object created and initialised, 5 published.
struct Metal {uint32_t stage, registered; int kext, status;};
Metal gMetal = {0, 0, -99, 0};
void publishMetal() {
    Metal &m = gMetal;
    m.stage = 1;
    m.kext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (m.kext) {m.status = m.kext; return;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {m.status = kIOReturnNotFound; return;}
    m.stage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {m.status = kIOReturnNoResources; return;}
    native_adreno_accelerator_static_init();
    m.status = OSMetaClass::postModLoad(handle);
    if (m.status) return;
    if (native_adreno_accelerator_instance_size() != 136) {m.status = kIOReturnInvalid; return;}
    m.stage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {m.status = kIOReturnNoMemory; return;}
    if (!accelerator->init()) {accelerator->release(); m.status = kIOReturnError; return;}
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    // The poster extensions that render the wallpapers (Aegir, Emoji, Collections, Pride, ...) are separate processes;
    // without their names here their Metal device open is refused by MACF before any rendering. The boot argument lets a
    // variant add them without a rebuild; the default keeps the render server and its known clients.
    static char sandboxClients[768] = "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard";
    (void)PE_parse_boot_argn("myron-gpu-clients", sandboxClients, sizeof sandboxClients);
    accelerator->setProperty("NativeSandboxGrantProcess", sandboxClients);
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    m.stage = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); m.status = kIOReturnNotAttached; return;}
    accelerator->registerService();
    m.registered = 1;
    m.stage = 5;
}

// A panic report's text; append() is printf into it, and once it is full the rest is dropped.
struct ReportText {char text[4096]; size_t n;};
__attribute__((format(printf, 2, 3))) void append(ReportText &out, const char *format, ...) {
    if (out.n >= sizeof out.text) return;
    va_list args;
    va_start(args, format);
    const int k = vsnprintf(out.text + out.n, sizeof out.text - out.n, format, args);
    va_end(args);
    if (k > 0) out.n += size_t(k);
}
// The Metal side, as both mode-11 reports show it: the accelerator, the addon's install and mounts, the driver's counters (opens,
// registry reads, the plugin's progress markers, its result lines, the newest opens, the processes that asked for MetalPluginName)
// and the bridge's telemetry rows. Brief (the hang report) leaves out what is settled once the UI is up.
void metalLines(ReportText &out, bool brief) {
    const Metal &a = gMetal;
    const MyronMetalDiag &d = native_adreno_metal_diag;
    if (!brief) {
        append(out, "ACC s%u/%x kext %x reg %u\n", a.stage, unsigned(a.status), unsigned(a.kext), a.registered);
        append(out, "ADD %d.%x %d.%x %d.%x files %d w%d =%d x%d e%d\n", native_gpu_addon_stage, unsigned(native_gpu_addon_status),
               native_gpu_addon2_stage, unsigned(native_gpu_addon2_status), native_gpu_addon3_stage,
               unsigned(native_gpu_addon3_status), native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same,
               native_gpu_addon_failed, native_gpu_addon_error);
    }
    append(out, "DRV o%u b%u bad%u c%u g%u p%u n%u fo%u/%x calls%u l%u/%x\n", d.opens, d.busy, d.bad, d.closes, d.grants,
           d.property_reads, d.name_reads, d.foreign_opens, d.foreign_type, d.calls, d.last_selector, d.last_status & 0xfff);
    append(out, "POOL %u/%u st %x bo %u fb %u use %u peak %u sc %u/%u\n", native_adreno_pool_stats[0], native_adreno_pool_mib,
           native_adreno_pool_status, native_adreno_pool_stats[1], native_adreno_pool_stats[2], native_adreno_pool_stats[3],
           native_adreno_pool_stats[4], native_adreno_pool_stats[5], native_adreno_pool_stats[6]);
    if (!brief) {
        append(out, "DRV ext%u/%u/%x rep%u inf%u att%x mnt%u/%x/%u/%u\n", d.ext_entries, d.ext_selector, d.ext_status & 0xfff,
               d.reports, d.inflight, d.attach_status, native_adreno_mount_fail[0], native_adreno_mount_fail[1],
               native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
        append(out, "OPEN %.16s %.16s %.16s\n", d.opener[0], d.opener[1], d.opener[2]);
        append(out, "READ %.16s %.16s name %.16s\n", d.reader[0], d.reader[1], d.name_reader);
        append(out, "START %.16s %.16s %.16s\n", d.starter[0], d.starter[1], d.starter[2]);
    }
    // The plugin's progress markers, oldest first (1 started, 2 symbols, 3 opened, 1xxx probe, 2000/2001 mailbox, 3xxx step done,
    // 4xxx failed, 5xxx reported, 6000.. the Turnip phase, 6fff/6ffe its result, 4f01 a failed open with its kern_return_t halves).
    char marks[64] = {};
    const unsigned total = d.marks_total, shown = total < 10 ? total : 10;
    for (unsigned i = 0, at = 0; i < shown && at < sizeof marks - 6; ++i)
        at += unsigned(snprintf(marks + at, sizeof marks - at, " %x", d.marks[(total - shown + i) & 15]));
    append(out, "K%u%s\n", total, marks);
    append(out, "R %.63s\n", d.report[0] ? d.report : "-");
    append(out, "T %.63s\n", d.report2[0] ? d.report2 : "-");
    if (!brief) {
        for (const MyronClientText &c : native_adreno_client_text)
            if (c.name[0]) append(out, "C %.10s %.53s\n", c.name, c.text);
        const unsigned opens = native_adreno_open_log_count;
        for (unsigned i = 0; i < 3 && i < opens; ++i) {
            const MyronOpenRecord &o = native_adreno_open_log[(opens - 1 - i) & 7];
            append(out, "L %.16s t%x m%x r%x a%x f%x\n", o.name, o.type, o.macf, o.res, o.attach, o.foreign);
        }
        char askers[72] = {};
        for (unsigned i = 0, at = 0; i < 4 && native_adreno_metal_askers[i].name[0] && at < sizeof askers - 16; ++i)
            at += unsigned(snprintf(askers + at, sizeof askers - at, " %.10s:%u", native_adreno_metal_askers[i].name,
                                    native_adreno_metal_askers[i].reads));
        append(out, "A%s\n", askers[0] ? askers : " -");
    }
    for (unsigned i = 0, rows = 0; i < 48 && rows < (brief ? 2u : 6u); ++i)
        if (native_adreno_metal_rows[i][0]) {append(out, "W%u %.62s\n", i, native_adreno_metal_rows[i]); ++rows;}
}
// Both reports start at panic line 25 (report() explains why) and keep within its 66 columns.
const char kLow[] = "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";

// The Metal report (myron-gpu-report=<s>, mode 11): <s> seconds after the run, a panic with what the user-space side reached and
// the GPU status words.
[[noreturn]] void metalReport(unsigned seconds) {
    static ReportText out;
    const Mode10 &r = gM10;
    append(out, "%sMYRON metal t+%us run s%u pub %x irq %u seen %08x bw %u\n", kLow, seconds, r.step, unsigned(r.publish),
           gIrqCount, gIrqSeen, gM6.t.bw_index);
    metalLines(out, false);
    const size_t b = gSmmu.va ? bank0() : 0;
    append(out, "GPU st %08x int %08x cp %08x fsr %x gfsr %x actlr %x\n", gpuRd(A840_RBBM_STATUS), gpuRd(A840_RBBM_INT_0_STATUS),
           gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL), smmuRd(b ? b + 0x58 : 0), gSmmu.va ? smmuRd(0x48) : 0xdead, smmuRd(b ? b + 0x4 : 0));
    panic("%s", out.text);
    __builtin_unreachable();
}

// The CP registers of the hang report beyond MyronA840HwInit.h (KGSL gen8_reg.h, Linux a6xx.xml A8XX): RBBM_STATUS1, the BV read
// pointer, and per pipe (through the aperture) the SQE status, both indirect-buffer positions and the RB/IB1 prefetch queues.
constexpr uint32_t kA8xxRbbmStatus1 = 0x013, kA8xxCpRbRptrBv = 0x80a, kA8xxCpSqeStatusPipe = 0x8bb, kA8xxCpIb1BasePipe = 0x900,
                   kA8xxCpIb1RemPipe = 0x902, kA8xxCpIb2BasePipe = 0x904, kA8xxCpIb2RemPipe = 0x906,
                   kA8xxCpRoqRbStatusPipe = 0x920, kA8xxCpRoqIb1StatusPipe = 0x921;
// The GPU's own progress, read without the aperture so that it can be sampled beside the running driver: the fence the CP last wrote
// to memptrs+4 and the BV fence at +12 (NativeAdrenoRing's framed fence), the fence it last started (CP_SCRATCH_GLOBAL(2), written at
// the top of every fence), both read pointers and the write pointer; submitted is the newest fence the ring handed out.
struct Progress {uint32_t done, bv_done, started, rptr, rptr_bv, wptr, submitted;};
Progress progress() {
    Progress p{};
    if (gGpuCpu[2]) {p.done = gGpuCpu[2][1]; p.bv_done = gGpuCpu[2][3];}
    p.started = gpuRd(A840_CP_SCRATCH_GLOBAL(2));
    p.rptr = gpuRd(A840_CP_RB_RPTR);
    p.rptr_bv = gpuRd(kA8xxCpRbRptrBv);
    p.wptr = gpuRd(A840_CP_RB_WPTR);
    p.submitted = gRing.lastSubmitted();
    return p;
}
bool moved(const Progress &a, const Progress &b) {
    return a.done != b.done || a.bv_done != b.bv_done || a.started != b.started || a.rptr != b.rptr || a.rptr_bv != b.rptr_bv;
}
// A load whose abort names itself: the tag (site << 56 | detail) in x16 and the physical address in x17, set in the same asm block as the
// load, so a synchronous external abort's register dump says which read it was. boot-myron-32 to -34 aborted at page offset 0x20-0x24
// of some page of the walk, through both the device mapping and the linear map; the registers did not say which page.
uint64_t taggedLoad64(uintptr_t va, uint64_t tag, uint64_t pa) {
    uint64_t v;
    asm volatile("mov x16, %2\n\tmov x17, %3\n\tldr %0, [%1]" : "=r"(v) : "r"(va), "r"(tag), "r"(pa) : "x16", "x17", "memory");
    return v;
}
uint32_t taggedLoad32(uintptr_t va, uint64_t tag, uint64_t pa) {
    uint32_t v;
    asm volatile("mov x16, %2\n\tmov x17, %3\n\tldr %w0, [%1]" : "=r"(v) : "r"(va), "r"(tag), "r"(pa) : "x16", "x17", "memory");
    return v;
}
// One aligned 64-bit word of physical memory through a fresh uncached mapping (sheng's physRead64), only of RAM XNU manages; tag as above.
bool physRead64(uint64_t pa, uint64_t *out, uint64_t tag) {
    /* pmap_valid_page's argument is a 32-bit page number: refuse a descriptor whose high bits would truncate into a valid page
     * (boot-myron-39's GPU-table walk aborted with an external abort on exactly such a load). */
    if ((pa >> 12) != (uint32_t)(pa >> 12) || !pmap_valid_page(uint32_t(pa >> 12))) return false;
    const vm_offset_t va = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!va) return false;
    *out = taggedLoad64(va + (pa & 0xff8), tag, pa);
    ml_io_unmap(va, 0x1000);
    return true;
}
// sheng's gpuWalk: the GPU context's TTBR0 table (bank 0 TTBR0 +0x20, TCR +0x30 with T0SZ in bits 5:0, 4 KiB granule) walked in software
// for one GPU address. This SMMU has no translation operations, so it shows what the table says, not what the TLB holds. Returns the
// leaf descriptor (0 when absent) and the physical address. Table reads are tagged 0x1L (L = level) in x16's top byte.
uint64_t gpuWalk(uint64_t va, uint64_t *pa) {
    *pa = 0;
    const size_t b = gSmmu.va ? bank0() : 0;
    if (!b) return 0;
    const uint64_t ttbr = (uint64_t(smmuRd(b + 0x24)) << 32) | smmuRd(b + 0x20);
    const unsigned bits = 64 - (smmuRd(b + 0x30) & 0x3f);
    if (bits < 25 || bits > 48) return 0;
    uint64_t table = ttbr & 0x0000fffffffff000ull, desc = 0;
    for (int level = 4 - int((bits - 12 + 8) / 9); level <= 3; ++level) {
        const unsigned shift = 12 + 9 * unsigned(3 - level);
        const uint64_t tag = (uint64_t(0x10 | level) << 56) | (va & 0xffffffffffull);
        if (!physRead64(table + ((va >> shift) & 0x1ff) * 8, &desc, tag) || !(desc & 1)) return 0;
        if (level == 3 || !(desc & 2)) {
            *pa = (desc & 0x0000fffffffff000ull & ~((uint64_t(1) << shift) - 1)) | (va & ((uint64_t(1) << shift) - 1));
            return desc;
        }
        table = desc & 0x0000fffffffff000ull;
    }
    return 0;
}
// count dwords from GPU address va0 at the physical pages the GPU's table names (each page through gpuWalk). mode 1 (default) reads them
// through XNU's linear map (phystokv, tag 0x21), mode 2 through the uncached device mapping (tag 0x22); the low bits of the tag are the
// GPU address. myron-gpu-hang-words picks the mode; 0 walks nothing.
bool gpuWords(uint64_t va0, uint32_t *w, unsigned count, unsigned mode) {
    if (mode != 1 && mode != 2) return false;
    uint64_t pageVa = ~0ull, pagePa = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t va = va0 + 4ull * i;
        if ((va & ~0xfffull) != pageVa) {
            pageVa = va & ~0xfffull;
            if (!gpuWalk(pageVa, &pagePa) || (pagePa >> 12) != (uint32_t)(pagePa >> 12) ||
                !pmap_valid_page(uint32_t(pagePa >> 12))) return false;
        }
        const uint64_t pa = pagePa + (va & 0xffc), tag = (uint64_t(0x20 | mode) << 56) | (va & 0xffffffffffull);
        if (mode == 1) {
            w[i] = taggedLoad32(uintptr_t(phystokv(pa)), tag, pa);
        } else {
            uint64_t v = 0;
            if (!physRead64(pa & ~7ull, &v, tag)) return false;
            w[i] = uint32_t((va & 4) ? v >> 32 : v);
        }
    }
    return true;
}
// The hang report (myron-gpu-hang=<s>): where both CP threads stand when the GPU stopped, the ring and submission counters, the
// interrupt, fault and protection status of each pipe, the SMMU fault registers, then the brief Metal lines.
[[noreturn]] void hangReport(const Progress &p, unsigned stillMs) {
    static ReportText out;
    const NativeAdrenoSubmitStats s = gSubmit.stats();
    append(out, "%sMYRON gpu hang %ums: fence %u/%u bv %u started %u\n", kLow, stillMs, p.done, p.submitted, p.bv_done, p.started);
    append(out, "RING rptr %x bv %x wptr %x shadow %x/%x out %u\n", p.rptr, p.rptr_bv, p.wptr, gGpuCpu[2] ? gGpuCpu[2][0] : 0,
           gGpuCpu[2] ? gGpuCpu[2][2] : 0, gRing.outstanding());
    append(out, "SUB s%u c%u to%u poison%u peak%u held%u irq %u bw %u\n", s.submissions, s.completed, s.timeouts, s.poisoned_jobs,
           unsigned(s.peak_jobs), unsigned(s.references_held), gIrqCount, gM6.t.bw_index);
    // Per pipe through the aperture: only here, as the last thing before the panic, never while sampling.
    static const unsigned pipes[2] = {A840_PIPE_BR, A840_PIPE_BV};
    uint64_t fetched[2] = {};
    for (unsigned k = 0; k < 2; ++k) {
        const unsigned pipe = pipes[k];
        const char *name = pipe == A840_PIPE_BR ? "BR" : "BV";
        const uint64_t ib1 = pipeRd(pipe, kA8xxCpIb1BasePipe) | uint64_t(pipeRd(pipe, kA8xxCpIb1BasePipe + 1)) << 32;
        fetched[k] = ib1;
        const uint64_t ib2 = pipeRd(pipe, kA8xxCpIb2BasePipe) | uint64_t(pipeRd(pipe, kA8xxCpIb2BasePipe + 1)) << 32;
        append(out, "%s ib1 %llx/%x ib2 %llx/%x sqe %x\n", name, (unsigned long long)ib1, pipeRd(pipe, kA8xxCpIb1RemPipe),
               (unsigned long long)ib2, pipeRd(pipe, kA8xxCpIb2RemPipe), pipeRd(pipe, kA8xxCpSqeStatusPipe));
        append(out, "%s roq %x/%x int %x hw %x prot %x\n", name, pipeRd(pipe, kA8xxCpRoqRbStatusPipe),
               pipeRd(pipe, kA8xxCpRoqIb1StatusPipe), pipeRd(pipe, A840_CP_INTERRUPT_STATUS_PIPE),
               pipeRd(pipe, A840_CP_HW_FAULT_STATUS_PIPE), pipeRd(pipe, A840_CP_PROTECT_STATUS_PIPE));
    }
    aperture(A840_PIPE_NONE);
    // The command words each thread stopped in. CP_IB1_BASE advances as the CP fetches (the prefetch queue runs a few dwords ahead of
    // the packet in execution): the GPU table's entry and physical address for it, the word there as the kernel's mapping of the buffer
    // reads it (kv; NativeAdrenoVm::peekWords), then the 15 dwords before it and 3 after at the physical pages the GPU's table names
    // (gpuWords, read path from myron-gpu-hang-words, default 1). Row labels are dword offsets from the fetch pointer. boot-myron-31 read
    // zeros through the kernel's mapping alone.
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    unsigned wordsMode = 1;
    PE_parse_boot_argn("myron-gpu-hang-words", &wordsMode, sizeof wordsMode);
    for (unsigned k = 0; k < 2; ++k) {
        const char *name = k ? "BV" : "BR";
        uint64_t pa = 0;
        const uint64_t desc = wordsMode ? gpuWalk(fetched[k], &pa) : 0;
        uint32_t kv = 0;
        const bool kernel = vm && !vm->peekWords(fetched[k], &kv, 1);
        // "!" after the physical address: not RAM XNU manages (its words are not read).
        append(out, "%s pte %llx pa %llx%s kv %s%08x\n", name, (unsigned long long)desc, (unsigned long long)pa,
               desc && !pmap_valid_page(uint32_t(pa >> 12)) ? "!" : "", kernel ? "" : "-", kv);
        uint32_t w[18] = {};
        if (fetched[k] < 15 * 4 || !gpuWords(fetched[k] - 15 * 4, w, 18, wordsMode)) {
            append(out, "%s words -\n", name);
            continue;
        }
        for (unsigned row = 0; row < 3; ++row)
            append(out, "%s%+d %08x %08x %08x %08x %08x %08x\n", name, int(row * 6) - 15, w[row * 6], w[row * 6 + 1],
                   w[row * 6 + 2], w[row * 6 + 3], w[row * 6 + 4], w[row * 6 + 5]);
    }
    append(out, "GPU st %08x/%08x int %08x cp %08x\n", gpuRd(A840_RBBM_STATUS), gpuRd(kA8xxRbbmStatus1),
           gpuRd(A840_RBBM_INT_0_STATUS), gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL));
    const size_t b = gSmmu.va ? bank0() : 0;
    append(out, "SMMU fsr %x far %08x%08x syn %08x gfsr %x actlr %x\n", smmuRd(b ? b + 0x58 : 0), smmuRd(b ? b + 0x64 : 0),
           smmuRd(b ? b + 0x60 : 0), smmuRd(b ? b + 0x68 : 0), gSmmu.va ? smmuRd(0x48) : 0xdead, smmuRd(b ? b + 0x4 : 0));
    metalLines(out, true);
    panic("%s", out.text);
    __builtin_unreachable();
}
// The live debug page (myron-gpu-hang-page=1): instead of a panic, the hang or the timed report shows two pages on the boot-progress page,
// refreshed every second and alternating every 8 s, and the system keeps running (reboot to leave). Page 1 is the state, page 2 the
// command words from kernel views that cannot abort: the ring around both read pointers (the CP_INDIRECT_BUFFER packets with the
// buffers' addresses and sizes) and the words around each thread's IB1 fetch point and BR's IB2 through the buffers' own kernel
// mappings. The GPU-table walk (myron-gpu-hang-words=1/2, tagged loads) runs only from the second round of page 2 on, after both pages
// have been shown once, so an abort there still leaves the page readable.
__attribute__((format(printf, 2, 3))) void pageRow(unsigned row, const char *format, ...) {
    if (row >= 47) return;
    char text[120];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    text[99] = 0;
    strlcpy(myron_debug_rows[row], text, sizeof myron_debug_rows[row]);
}
struct PipeState {uint64_t ib1, ib2; uint32_t ib1_rem, ib2_rem, sqe, roq_rb, roq_ib1, intr, hw, prot;};
void readPipes(PipeState out[2]) {
    static const unsigned pipes[2] = {A840_PIPE_BR, A840_PIPE_BV};
    for (unsigned k = 0; k < 2; ++k) {
        const unsigned pipe = pipes[k];
        PipeState &s = out[k];
        s.ib1 = pipeRd(pipe, kA8xxCpIb1BasePipe) | uint64_t(pipeRd(pipe, kA8xxCpIb1BasePipe + 1)) << 32;
        s.ib2 = pipeRd(pipe, kA8xxCpIb2BasePipe) | uint64_t(pipeRd(pipe, kA8xxCpIb2BasePipe + 1)) << 32;
        s.ib1_rem = pipeRd(pipe, kA8xxCpIb1RemPipe);
        s.ib2_rem = pipeRd(pipe, kA8xxCpIb2RemPipe);
        s.sqe = pipeRd(pipe, kA8xxCpSqeStatusPipe);
        s.roq_rb = pipeRd(pipe, kA8xxCpRoqRbStatusPipe);
        s.roq_ib1 = pipeRd(pipe, kA8xxCpRoqIb1StatusPipe);
        s.intr = pipeRd(pipe, A840_CP_INTERRUPT_STATUS_PIPE);
        s.hw = pipeRd(pipe, A840_CP_HW_FAULT_STATUS_PIPE);
        s.prot = pipeRd(pipe, A840_CP_PROTECT_STATUS_PIPE);
    }
    aperture(A840_PIPE_NONE);
}
// Set by myron_gpu_swap_stall_pages: the pages are up because a display swap never retired, not because the GPU stopped.
volatile unsigned gSwapStall;
// Page 1: the state.
void pageState(const Progress &p, unsigned since, unsigned stillMs, bool hang) {
    const NativeAdrenoSubmitStats s = gSubmit.stats();
    const MyronMetalDiag &d = native_adreno_metal_diag;
    PipeState pipe[2];
    readPipes(pipe);
    const size_t b = gSmmu.va ? bank0() : 0;
    unsigned row = 0;
    pageRow(row++, "MYRON GPU %s  page 1/2 state  t+%us  (%ums without progress)  dcvs %u/%u  refreshed every second",
            gSwapStall ? "SWAP STALL" : hang ? "HANG" : "REPORT", since, stillMs, gDcvsLevel, gDcvsChanges);
    if (gSwapStall) {
        auto u64 = [](const volatile uint64_t *v) { return v ? (unsigned long long)*v : 0ull; };
        auto u32 = [](const volatile uint32_t *v) { return v ? unsigned(*v) : 0u; };
        const volatile uint64_t *waits = native_gop_wait_stats;
        pageRow(row++, "SWAP wait >5s | offers %llu done %llu fail %llu abandoned %llu | worker waits %llu t/o %llu | retired %u refused %u/%x",
                u64(&native_gop_gpu_offers), u64(&native_gop_gpu_completions), u64(&native_gop_gpu_failures),
                u64(&native_gop_gpu_abandoned), u64(waits), u64(waits ? waits + 1 : nullptr), u32(&native_legacy_retirement_calls),
                u32(&native_legacy_gpu_fallbacks), u32(&native_legacy_gpu_last_status));
    }
    pageRow(row++, "fence %u/%u bv %u started %u | ring rptr %x bv %x wptr %x shadow %x/%x out %u", p.done, p.submitted,
            p.bv_done, p.started, p.rptr, p.rptr_bv, p.wptr, gGpuCpu[2] ? gGpuCpu[2][0] : 0, gGpuCpu[2] ? gGpuCpu[2][2] : 0,
            gRing.outstanding());
    pageRow(row++, "submit s%u c%u timeouts %u poison %u peak %u held %u | irq %u seen %08x | ib thread %s both %u br %u | bw %u actlr %x",
            s.submissions, s.completed, s.timeouts, s.poisoned_jobs, unsigned(s.peak_jobs), unsigned(s.references_held), gIrqCount,
            gIrqSeen, gIbThread == 0 ? "job" : gIbThread == 1 ? "BR" : "BOTH", native_adreno_thread_jobs[0], native_adreno_thread_jobs[1],
            gM6.t.bw_index, smmuRd(b ? b + 0x4 : 0));
    for (unsigned k = 0; k < 2; ++k)
        pageRow(row++, "%s ib1 %llx/%x ib2 %llx/%x sqe %x roq %x/%x int %x hw %x prot %x", k ? "BV" : "BR",
                (unsigned long long)pipe[k].ib1, pipe[k].ib1_rem, (unsigned long long)pipe[k].ib2, pipe[k].ib2_rem, pipe[k].sqe,
                pipe[k].roq_rb, pipe[k].roq_ib1, pipe[k].intr, pipe[k].hw, pipe[k].prot);
    pageRow(row++, "GPU st %08x/%08x int %08x cp %08x | SMMU fsr %x far %08x%08x syn %08x gfsr %x", gpuRd(A840_RBBM_STATUS),
            gpuRd(kA8xxRbbmStatus1), gpuRd(A840_RBBM_INT_0_STATUS), gpuRd(A840_CP_INTERRUPT_STATUS_GLOBAL), smmuRd(b ? b + 0x58 : 0),
            smmuRd(b ? b + 0x64 : 0), smmuRd(b ? b + 0x60 : 0), smmuRd(b ? b + 0x68 : 0), gSmmu.va ? smmuRd(0x48) : 0xdead);
    pageRow(row++, "POOL %u/%u st %x bo %u fb %u use %u peak %u sc %u/%u | ADD %d.%x %d.%x files %d w%d", native_adreno_pool_stats[0],
            native_adreno_pool_mib, native_adreno_pool_status, native_adreno_pool_stats[1], native_adreno_pool_stats[2],
            native_adreno_pool_stats[3], native_adreno_pool_stats[4], native_adreno_pool_stats[5], native_adreno_pool_stats[6],
            native_gpu_addon_stage, unsigned(native_gpu_addon_status), native_gpu_addon2_stage, unsigned(native_gpu_addon2_status),
            native_gpu_addon_files, native_gpu_addon_written);
    pageRow(row++, "DRV opens %u busy %u bad %u closes %u grants %u props %u names %u calls %u last %u/%x ext %u/%u/%x",
            d.opens, d.busy, d.bad, d.closes, d.grants, d.property_reads, d.name_reads, d.calls, d.last_selector,
            d.last_status & 0xfff, d.ext_entries, d.ext_selector, d.ext_status & 0xfff);
    pageRow(row++, "OPEN %.16s %.16s %.16s | START %.16s %.16s %.16s", d.opener[0], d.opener[1], d.opener[2], d.starter[0],
            d.starter[1], d.starter[2]);
    char marks[100] = {};
    const unsigned total = d.marks_total, shown = total < 16 ? total : 16;
    for (unsigned i = 0, at = 0; i < shown && at < sizeof marks - 6; ++i)
        at += unsigned(snprintf(marks + at, sizeof marks - at, " %x", d.marks[(total - shown + i) & 15]));
    pageRow(row++, "K%u%s", total, marks);
    pageRow(row++, "R %.63s", d.report[0] ? d.report : "-");
    pageRow(row++, "T %.63s", d.report2[0] ? d.report2 : "-");
    for (const MyronClientText &c : native_adreno_client_text)
        pageRow(row++, "C %.16s %.64s", c.name[0] ? c.name : "-", c.text);
    const unsigned opens = native_adreno_open_log_count;
    for (unsigned i = 0; i < 3; ++i) {
        if (i >= opens) {pageRow(row++, "L -"); continue;}
        const MyronOpenRecord &o = native_adreno_open_log[(opens - 1 - i) & 7];
        pageRow(row++, "L %.16s type %x macf %x res %x attach %x foreign %x", o.name, o.type, o.macf, o.res, o.attach, o.foreign);
    }
    pageRow(row++, "# render server telemetry (native_adreno_metal_rows)");
    for (unsigned i = 0; i < 48 && row < 47; ++i)
        if (native_adreno_metal_rows[i][0]) pageRow(row++, "W%-2u %.94s", i, native_adreno_metal_rows[i]);
    while (row < 47) pageRow(row++, "%s", "");
}
// Eight dwords per row from a kernel-owned buffer at a GPU address, through its own kernel mapping ("--" where none covers it).
void pageWords(unsigned &row, uint64_t va, int first, unsigned rows) {
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    for (unsigned r = 0; r < rows; ++r) {
        const int offset = first + int(r) * 8;
        uint32_t w[8] = {};
        const uint64_t at = va + uint64_t(int64_t(offset) * 4);
        if (!va || !vm || vm->peekWords(at, w, 8)) {pageRow(row++, "%+04d --", offset); continue;}
        pageRow(row++, "%+04d %08x %08x %08x %08x %08x %08x %08x %08x", offset, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}
// The size (dwords) of the indirect buffer at GPU address ib whose CP_INDIRECT_BUFFER packet (0x70bf8003, address lo, hi, size) lies in the
// ring before read pointer rptr, looking back at most 512 dwords; 0 when not found.
uint32_t ringIbSize(uint32_t rptr, uint64_t ib) {
    const uint32_t ringDwords = uint32_t(gM8.buf[1].bytes / 4);
    if (!gGpuCpu[1] || !ringDwords || !ib) return 0;
    for (uint32_t back = 4; back <= 512; ++back) {
        const uint32_t at = (rptr + ringDwords - back) % ringDwords;
        if (gGpuCpu[1][at] != 0x70bf8003u) continue;
        const uint64_t base = gGpuCpu[1][(at + 1) % ringDwords] | uint64_t(gGpuCpu[1][(at + 2) % ringDwords]) << 32;
        if (base == ib) return gGpuCpu[1][(at + 3) % ringDwords] & 0xfffff;
    }
    return 0;
}
// Page 2: the command words. CP_IB1_BASE is the buffer's start (boot-myron-36), so each thread's position is start + (size - remaining)
// dwords, the size from its CP_INDIRECT_BUFFER packet in the ring; the words from 40 before to 16 after it, through the buffer's kernel
// mapping (the CP stops a little before the position: the prefetch queue holds what it fetched ahead). Then the stuck submission's indirect
// buffers as the ring lists them. walk = myron-gpu-hang-words (0 none, 1 linear map, 2 device mapping) once allowed.
void pageCommands(const Progress &p, unsigned since, unsigned walk) {
    PipeState pipe[2];
    readPipes(pipe);
    unsigned row = 0;
    pageRow(row++, "MYRON GPU page 2/2 commands  t+%us  (kernel views; GPU-table walk %s)", since,
            walk ? "below" : "off: myron-gpu-hang-words=1 or 2");
    const uint32_t rptrs[2] = {p.rptr, p.rptr_bv};
    for (unsigned k = 0; k < 2; ++k) {
        const uint32_t size = ringIbSize(rptrs[k], pipe[k].ib1);
        const uint32_t done = size >= pipe[k].ib1_rem ? size - pipe[k].ib1_rem : 0;
        pageRow(row++, "# %s in IB1 %llx size %x rem %x -> at dword %x; words from -40 (kernel mapping)", k ? "BV" : "BR",
                (unsigned long long)pipe[k].ib1, size, pipe[k].ib1_rem, done);
        pageWords(row, pipe[k].ib1 + 4ull * done, -40, 7);
    }
    pageRow(row++, "# BR IB2 %llx rem %x: its first words", (unsigned long long)pipe[0].ib2, pipe[0].ib2_rem);
    pageWords(row, pipe[0].ib2, 0, 2);
    // The stuck submission's indirect buffers: from 160 dwords before the older read pointer to the write pointer.
    const uint32_t ringDwords = uint32_t(gM8.buf[1].bytes / 4);
    if (gGpuCpu[1] && ringDwords) {
        const uint32_t from = (p.rptr_bv + ringDwords - 160) % ringDwords;
        pageRow(row++, "# ring IBs from %x to wptr %x (BR rptr %x, BV rptr %x): address/size", from, p.wptr, p.rptr, p.rptr_bv);
        char line[100] = {};
        unsigned used = 0, listed = 0;
        for (uint32_t at = from; at != p.wptr && listed < 40 && row < 46; at = (at + 1) % ringDwords) {
            if (gGpuCpu[1][at] != 0x70bf8003u) continue;
            const uint64_t base = gGpuCpu[1][(at + 1) % ringDwords] | uint64_t(gGpuCpu[1][(at + 2) % ringDwords]) << 32;
            const int n = snprintf(line + used, sizeof line - used, " %x:%llx/%x", at, (unsigned long long)base,
                                   gGpuCpu[1][(at + 3) % ringDwords] & 0xfffff);
            if (n > 0) used += unsigned(n);
            ++listed;
            if (used > 70) {pageRow(row++, "%s", line); used = 0; line[0] = 0;}
        }
        if (used) pageRow(row++, "%s", line);
    }
    if (walk) {
        for (unsigned k = 0; k < 2 && row < 46; ++k) {
            uint64_t pa = 0;
            const uint64_t desc = gpuWalk(pipe[k].ib1, &pa);
            uint32_t w[8] = {};
            const bool read = desc && gpuWords(pipe[k].ib1, w, 8, walk);
            pageRow(row++, "%s table pte %llx pa %llx%s", k ? "BV" : "BR", (unsigned long long)desc, (unsigned long long)pa,
                    desc && !pmap_valid_page(uint32_t(pa >> 12)) ? " (not XNU RAM)" : "");
            if (read) pageRow(row++, "+0 %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    while (row < 47) pageRow(row++, "%s", "");
}
[[noreturn]] void debugPages(unsigned stillMs, bool hang) {
    unsigned walk = 0;
    PE_parse_boot_argn("myron-gpu-hang-words", &walk, sizeof walk);
    if (walk > 2) walk = 0;
    for (unsigned i = 0; i < 47; ++i) myron_debug_rows[i][0] = 0;
    myron_debug_on = 1;
    for (unsigned since = 0, cycle = 0;; ++cycle) {
        for (unsigned page = 0; page < 2; ++page) {
            for (unsigned s = 0; s < 8; ++s, ++since) {
                const Progress now = progress();
                if (page == 0) pageState(now, since, stillMs, hang);
                else pageCommands(now, since, cycle ? walk : 0);
                IOSleep(1000);
            }
        }
    }
}
// Mode 11's watch (myron-gpu-report=<s>, myron-gpu-hang=<s>): every 100 ms the GPU's progress. Work outstanding while none of it
// moves for <hang> seconds is a hang, reported at once; after <report> seconds the timed report.
void watch(unsigned seconds, unsigned hang) {
    unsigned pages = 0;
    PE_parse_boot_argn("myron-gpu-hang-page", &pages, sizeof pages);
    Progress last = progress();
    unsigned still = 0;
    for (unsigned tick = 1;; ++tick) {
        IOSleep(100);
        if (seconds && tick >= seconds * 10) {
            if (pages == 1) debugPages(0, false);
            metalReport(seconds);
        }
        const Progress now = progress();
        still = now.done != now.submitted && !moved(now, last) ? still + 1 : 0;
        if (hang && still >= hang * 10) {
            if (pages == 1) debugPages(still * 100, true);
            hangReport(now, still * 100);
        }
        last = now;
    }
}

// One status pair of an HFI message: transport status / firmware error, or "-" when it was never sent.
const char *hfiPair(unsigned slot, char (&out)[16]) {
    if (!(gM6.sent & (1u << slot))) return "-";
    snprintf(out, sizeof out, "%u/%x", unsigned(gM6.hs[slot]), gM6.fw_error[slot]);
    return out;
}

[[noreturn]] void report(const mg_result &r, unsigned have) {
    static char text[6144];
    int n;
    // From mode 7 the report starts at panic line 25 (y 1384 of the 2608-pixel panel; xnu-panic-screen.c draws 42-pixel lines
    // from y 334 and never scrolls): the boot-progress monitor keeps redrawing its rows 0-47 (y 0-1343) from the other cores
    // after this panic, and painted over the middle of the first mode 7 report. Its lines stay within the 66 columns there.
    static const char low[] = "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";
    const bool compact = r.mode >= 7 && gM6.step == 9;   // modes 1-6 reached GX on: one line says so
    if (r.mode >= 6) {
        // The modes 1-5 blocks do not fit on the panel together with the mode 6 rows: one line carries their outcome.
        const Mode4 &a = gM4;
        const Mode5 &b = gM5;
        n = snprintf(text, sizeof text, "%sMYRON gpu stage %u (windows %02x io %d)\n", r.mode >= 7 ? low : "\n\n\n\n\n\n", r.mode, have,
            r.io_error);
        if (compact) n += snprintf(text + n, sizeof text - n, "GX on: fw %08x/%u oob %u pwr %08x->%08x rbbm %08x\n", b.gmu.fw_init,
            b.gmu.fw_polls, gM6.oob_polls, gM6.pwr_before, gM6.pwr_after, gM6.rbbm);
        else n += snprintf(text + n, sizeof text - n,
            "BASE cx %08x smmu %s smr %u clk %u/%d scm %lld cmddb %d/%02x ctx %x/%u gmu %u/%u fw %08x hfi %08x polls %u\n",
            r.cx_hwctrl, mg_state_text(r.smmu), r.smr_valid, a.clocks.step, a.clocks.failed,
            (long long)a.scm.a0, a.db.status, a.db.found, unsigned(b.ctx_status), b.ctx.step, b.gmu.status, b.gmu.phase, b.gmu.fw_init,
            b.gmu.hfi_status, b.gmu.fw_polls);
    } else n = snprintf(text, sizeof text,
        // The diagnostic page paints its magenta band over the first lines of the panic text: start below it.
        "\n\n\n\n\n\nMYRON gpu stage %u (windows %02x io %d)\n"
        "GCC %s cfg_ahb %08x->%08x%s qmip %08x gemnoc %08x gpll0 %08x\n"
        "GPUCC %s cx %08x hw %08x ahb %08x cx_gmu %08x cxo %08x hub %08x memnoc %08x gx_gmu %08x smmu %08x\n"
        "aon %08x %08x %08x %08x rcg gmu %08x/%08x hub %08x/%08x pll0 %08x %08x l %08x\n"
        "CX before %08x smmu before %08x polls %u/%u powered %d clocks %d\n"
        "GX %s gdscr %08x cfg %08x\n"
        "SMMU %s scr0 %08x id %08x %08x %08x gfsr %08x smr valid %u first %u %08x s2cr %08x\n"
        "GMU %s pwrclk %08x rpmh %08x sysreset %08x fwinit %08x ao %08x\n"
        "GPU %s rbbm %08x gfx %08x gbif_halt %08x\n",
        r.mode, have, r.io_error,
        mg_state_text(r.gcc), r.cfg_ahb, r.cfg_ahb_after, r.enabled_ahb ? " (set)" : "", r.qmip_ahb, r.gemnoc, r.gpll0,
        mg_state_text(r.cc), r.cx_gdscr, r.cx_hwctrl, r.cc_ahb, r.cc_cx_gmu, r.cc_cxo, r.cc_hub_cx_int, r.cc_memnoc, r.cc_gx_gmu,
        r.cc_smmu,
        r.aon_rscc_xo, r.aon_cxo, r.aon_rscc_hub, r.aon_hub, r.gmu_cmd, r.gmu_cfg, r.hub_cmd, r.hub_cfg, r.pll0_mode, r.pll0_opmode,
        r.pll0_l,
        r.cx_before, r.smmu_before, r.cx_polls, r.clock_polls, r.powered_cx, r.clocks_on,
        mg_state_text(r.gx), r.gx_gdscr, r.gx_cfg,
        mg_state_text(r.smmu), r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.smr_valid, r.smr_first, r.smr_first_val, r.s2cr_first_val,
        mg_state_text(r.gmu), r.gmu_pwr_clk, r.gmu_rpmh, r.gmu_sysreset, r.gmu_fw_init, r.gmu_ao,
        mg_state_text(r.gpu), r.rbbm_status, r.rbbm_gfx_status, r.gbif_halt);
    if (r.mode >= 4 && r.mode < 6 && n > 0 && size_t(n) < sizeof text) {
        const Mode4 &m = gM4;
        const mg_gmu_clocks &c = m.clocks;
        n += snprintf(text + n, sizeof text - n,
            "SCM gpu_init_regs a0 %lld a1 %llx resumes %u\n"
            "CLK step %u fail %d gpll0 %08x pll0 %08x/%08x l %08x user %08x polls %u/%u/%u/%u\n"
            "CLK hub %08x/%08x gmu %08x/%08x cx_gmu %08x cxo %08x hub_cx %08x hub_aon %08x\n"
            "SMR %08x %08x %08x %08x S2CR %08x %08x %08x %08x\n"
            "GMU win %x pwrclk %08x rpmh %08x sysreset %08x fwinit %08x hfi %08x ao %08x ver %08x\n"
            "RSC status %08x busy %08x ack %08x tcm_ret %08x aoint %08x\n"
            "CMDDB map %d st %d ver %08x slaves %u entries %u found %02x gfx %u mx %u cx %u gmxc %u\n"
            "CMDDB sh0 %05x mc0 %05x acv %05x cn0 %05x gfx.lvl %x %x %x %x %x %x %x %x %x %x %x %x\n",
            (long long)m.scm.a0, (unsigned long long)m.scm.a1, m.scm.resumes,
            c.step, c.failed, c.gpll0, c.pll0_mode, c.pll0_opmode, c.pll0_l, c.pll0_user, c.lock_polls, c.hub_polls, c.gmu_polls,
            c.branch_polls,
            c.hub_cmd, c.hub_cfg, c.gmu_cmd, c.gmu_cfg, c.cx_gmu, c.cxo, c.hub_cx_int, c.hub_aon,
            m.smr[0], m.smr[1], m.smr[2], m.smr[3], m.s2cr[0], m.s2cr[1], m.s2cr[2], m.s2cr[3],
            m.gmu_have, m.pwr_clk, m.rpmh, m.sysreset, m.fw_init, m.hfi, m.ao_count, m.version,
            m.rsc_status, m.seq_busy, m.rscc_ack, m.tcm_ret, m.ao_status,
            m.db_mapped, m.db.status, m.db.version, m.db.slaves, m.db.entries, m.db.found, m.gfx_n, m.mx_n, m.cx_n, m.gmxc_n,
            m.db.r[MCD_SH0].address, m.db.r[MCD_MC0].address, m.db.r[MCD_ACV].address, m.db.r[MCD_CN0].address,
            m.gfx[0], m.gfx[1], m.gfx[2], m.gfx[3], m.gfx[4], m.gfx[5], m.gfx[6], m.gfx[7], m.gfx[8], m.gfx[9], m.gfx[10], m.gfx[11]);
    }
    if (r.mode == 5 && n > 0 && size_t(n) < sizeof text) {
        const Mode5 &m = gM5;
        const NativeAdrenoGmuContextReport &x = m.ctx;
        const mgm_result &g = m.gmu;
        n += snprintf(text + n, sizeof text - n,
            "CTX ran %d s2cr %08x fixed %d st %x step %u kit %u/%u id2 %x oas %u prog p%uw%ur%ue%u route p%uw%uv%u\n"
            "CTX T %016llx S %08x C %08x R %08x M %08x mism %x spans %u pay %x maps %u\n"
            "MEM %llx/%llx %llx/%llx %llx/%llx %llx/%llx %llx/%llx\n"
            "GMU st %u ph %u blocks %u small %u itcm %x dtcm %x mem %x ver %08x mask %x ready %x\n"
            "GMU before reset %x hfi %x tcm %x rsc %x busy %x ack %x\n"
            "GMU fw_init %08x polls %u hfi %08x polls %u ao %08x g2h %08x pwrclk %08x mism %x %08x/%08x\n",
            m.ran, m.s2cr_before, m.s2cr_fixed, unsigned(m.ctx_status), x.step, x.program_status, x.route_status, x.id2,
            x.output_bits, x.program_phase, x.program_writes, x.program_readback_valid, x.program_enabled, x.route_phase,
            x.route_writes, x.routes_verified,
            (unsigned long long)x.ttbr0, x.sctlr, x.cbar, x.s2cr, x.smr, x.mismatch, x.spans, unsigned(m.payload_status), m.payload_maps,
            (unsigned long long)m.payloads[0].physical, (unsigned long long)m.payloads[0].iova,
            (unsigned long long)m.payloads[1].physical, (unsigned long long)m.payloads[1].iova,
            (unsigned long long)m.payloads[2].physical, (unsigned long long)m.payloads[2].iova,
            (unsigned long long)m.payloads[3].physical, (unsigned long long)m.payloads[3].iova,
            (unsigned long long)m.payloads[4].physical, (unsigned long long)m.payloads[4].iova,
            g.status, g.phase, g.blocks, g.small, g.itcm_bytes, g.dtcm_bytes, g.memory_bytes, g.version, g.mask, g.ready,
            g.sysreset, g.hfi_before, g.tcm_ret, g.rsc_status, g.seq_busy, g.rscc_ack,
            g.fw_init, g.fw_polls, g.hfi_status, g.hfi_polls, g.ao_status, g.gmu2host, g.pwr_clk, g.mismatch_dword, g.mismatch_got,
            g.mismatch_want);
    }
    if (r.mode >= 7 && n > 0 && size_t(n) < sizeof text) {
        const Mode7 &m = gM7;
        n += snprintf(text + n, sizeof text - n,
            "HW ran %d wr %u io %u halt %08x/%08x rb %u/%u\n"
            "HW bad %05x=%08x want %08x, %05x=%08x want %08x\n"
            "HW st %08x int %08x cp %08x\n"
            "HW fuse freq %08x value %08x slices %08x\n",
            m.ran, m.writes, m.io_errors, m.gbif_halt, m.rbbm_gbif_halt, m.matched, m.checked,
            m.bad_reg, m.bad_got, m.bad_want, m.bad2_reg, m.bad2_got, m.bad2_want,
            m.status, m.int0, m.cp_int,
            m.fuse_freq, m.fuse_value, m.slices);
    }
    if (r.mode >= 8 && n > 0 && size_t(n) < sizeof text) {
        // The context and microcode lines (cpPrepare, every mode from 8); the CP lines only for mode 8's own ring.
        const Mode8 &m = gM8;
        const NativeAdrenoGmuContextReport &x = m.ctx;
        n += snprintf(text + n, sizeof text - n,
            "CTX %d s%u st %x kit %u/%u step %u p%ue%u r%uv%u oas %u spans %u\n"
            "CTX s2cr %08x %08x fix %x actlr %x>%x buf %x views %u\n"
            "SQE %x/%x phys %llx bad %x/%x\n",
            m.ran, m.step, unsigned(m.ctx_status), x.program_status, x.route_status, x.step, x.program_phase, x.program_enabled,
            x.route_phase, x.routes_verified, x.output_bits, x.spans,
            m.s2cr_before[0], m.s2cr_before[1], m.s2cr_fixed, m.actlr, m.actlr_set, unsigned(m.buf_status), m.views,
            unsigned(m.sqe_alloc), unsigned(m.sqe_map), (unsigned long long)m.sqe_phys, m.sqe_bad, m.aqe_bad);
        if (n > 0 && size_t(n) < sizeof text && myron_gpu_coherence_row[0])
            n += snprintf(text + n, sizeof text - n, "%s\n", myron_gpu_coherence_row);
        if (r.mode <= 9 && n > 0 && size_t(n) < sizeof text) n += snprintf(text + n, sizeof text - n,
            "CP step %u rw0 %x/%x wptr %x polls %u/%u idle %d\n"
            "CP rptr %x shadow %x bv %x fence %x scratch %08x reg %x\n"
            "CP st %08x int %08x cp %08x pipe %08x\n"
            "CP fault %08x sqe %08x prot %08x gfsr %08x\n"
            "SMMU fsr %08x far %08x%08x syn %08x\n",
            m.step, m.rptr0, m.wptr0, m.wptr, m.init_polls, m.fence_polls, m.init_idle,
            m.rptr, m.shadow, m.bv_rptr, m.fence, m.scratch, m.scratch_reg,
            m.status, m.int0, m.cp_int, m.pipe_int,
            m.hw_fault, m.sqe_status, m.protect_status, m.gfsr,
            m.fsr, m.far_hi, m.far_lo, m.fsynr0);
    }
    if (r.mode >= 9 && n > 0 && size_t(n) < sizeof text) {
        // The TrustZone line (zapLoad, modes 9-11); the secure-exit lines only for mode 9's own ring.
        const Mode9 &m = gM9;
        n += snprintf(text + n, sizeof text - n, "ZAP %d stage %d init %lld/%u mem %lld/%u auth %lld/%u\n",
            m.ran, m.stage, (long long)m.init, m.resumes[0], (long long)m.mem, m.resumes[1], (long long)m.auth, m.resumes[2]);
        if (r.mode == 9 && n > 0 && size_t(n) < sizeof text) n += snprintf(text + n, sizeof text - n,
            "ZAP polls %u/%u fence %x scratch %08x st %08x int %08x\n"
            "ZAP cp %08x fsr %08x\n",
            m.polls, m.polls2, m.fence, m.scratch, m.status, m.int0,
            m.cp_int, m.fsr);
    }
    if (r.mode >= 10 && n > 0 && size_t(n) < sizeof text) {
        const Mode10 &m = gM10;
        n += snprintf(text + n, sizeof text - n,
            "RUN %d s%u ring %x cp %x/%d/%x wr %d %08x sec %d/%x\n"
            "RUN irq %x/%x rb %x %u>%u seen %08x tmr %x %llx>%llx\n"
            "RUN sub %x pub %x r %d/%d ok %d/%d e %x@%d m %x o %u/%u\n"
            "RUN st %08x int %08x cp %08x fsr %08x gfsr %08x\n",
            m.ran, m.step, unsigned(m.ring_attach), unsigned(m.cp_init), m.init_wait, m.init_seen, m.write_wait, m.scratch,
            m.secure_wait, m.secure_seen,
            unsigned(m.irq_mask), unsigned(m.irq_attach), m.irq_readback, m.irq_before, m.irq_after, m.irq_seen, unsigned(m.timer),
            (unsigned long long)m.ticks0, (unsigned long long)m.ticks1,
            unsigned(m.submit), unsigned(m.publish), m.rounds[0], m.rounds[1], m.round_ok, m.round_steps, unsigned(m.round_err),
            m.round_err_at, m.mem_status, m.opened, m.closed,
            m.status, m.int0, m.cp_int, m.fsr, m.gfsr);
    }
    if ((r.mode == 6 || (r.mode >= 7 && !compact)) && n > 0 && size_t(n) < sizeof text) {
        const Mode6 &m = gM6;
        const mhf_tables &t = m.t;
        char p0[16], p1[16], p2[16], p3[16], p4[16];
        n += snprintf(text + n, sizeof text - n,
            "HFI ran %d step %u build %d lvl %x gx %08x %08x %08x dep %08x cx %08x %08x %08x\n"
            "HFI bcm unit %x %x %x width %u %u %u vcd %u %u %u ib3 %08x %08x %08x wait %x\n"
            "HFI perf %s bw %s core %s start %s vote %s skipped %u hdr %08x\n"
            "GX oob %d info %08x polls %u pwrclk %08x->%08x polls %u rpmh %08x gdscr %08x cfg %08x\n"
            "GPU rbbm %08x gfx %08x gbif_halt %08x\n",
            m.ran, m.step, t.status, t.failed_level, t.gx[1], t.gx[4], t.gx[7], t.dep[7], t.cx[0], t.cx[1], t.cx[2],
            t.bcm_unit[0], t.bcm_unit[1], t.bcm_unit[2], t.bcm_width[0], t.bcm_width[1], t.bcm_width[2], t.bcm_vcd[0], t.bcm_vcd[1],
            t.bcm_vcd[2], t.ib[3][0], t.ib[3][1], t.ib[3][2], t.bw[4],
            hfiPair(0, p0), hfiPair(1, p1), hfiPair(2, p2), hfiPair(3, p3), hfiPair(4, p4), gHfi.skipped, gHfi.skipped_header,
            m.oob_ok, m.oob_info, m.oob_polls, m.pwr_before, m.pwr_after, m.gx_polls, m.rpmh, m.gx_gdscr, m.gx_cfg,
            m.rbbm, m.rbbm_gfx, m.gbif_halt);
    }
    panic("%s", text);
    __builtin_unreachable();
}

void gpuThread(void *argument, wait_result_t) {
    unsigned mode = static_cast<unsigned>(reinterpret_cast<uintptr_t>(argument)), delay = 0;
    if (PE_parse_boot_argn("myron-gpu-delay", &delay, sizeof delay) && delay && delay <= 600) IOSleep(delay * 1000);
    unsigned have = mapWindows(gWindows, MG_WINDOWS);
    mg_io io = {nullptr, rd, wr, delayUs};
    static mg_result result;
    mg_run(&io, mode, have, &result);
    if (mode >= 4 && result.cc == MG_DONE && mg_gdsc_on(result.cx_hwctrl) && !result.io_error) {
        mode4(io, result, have);
        if (mode >= 5) mode5(result);
        if (mode >= 6) mode6();
        if (mode >= 7) mode7();
        if (mode == 8 || mode == 9) mode8();
        if (mode == 9) mode9();
    }
    if (mode >= 10) run10(mode);
    if (mode == 11) {
        // The GPU stays up and published; the result is in the diagnostic page's GPU rows. The accelerator only on top of a run
        // that published the service (step 8 on), so the plugin never finds an accelerator without a GPU behind it.
        unsigned metal = 0, seconds = 0, hang = 0;
        if (PE_parse_boot_argn("myron-gpu-metal", &metal, sizeof metal) && metal == 1 && gM10.step >= 8) publishMetal();
        if (!PE_parse_boot_argn("myron-gpu-report", &seconds, sizeof seconds) || seconds > 600) seconds = 0;
        if (!PE_parse_boot_argn("myron-gpu-hang", &hang, sizeof hang) || hang > 60 || gM10.step < 1) hang = 0;
        if (seconds || hang) watch(seconds, hang);
        return;
    }
    report(result, have);
}
}  // namespace

// IOMFB's swap_wait_gated waited more than 5 s for a swap that never retired and called the platform's slot 345 on its way to its own panic
// (LegacyDisplayProbe.cpp, native_legacy_backend_guard). With myron-gpu-hang-page=1 and the GPU up, the GPU's live debug pages take the
// panel instead, headed by the display hand-off counters, and the waiting thread stays here: the rest of the system keeps running and the
// render server's telemetry rows keep updating (reboot to leave). False when the GPU never came up; the caller panics then.
extern "C" bool myron_gpu_swap_stall_pages(void) {
    if (gM10.step < 8) return false;
    gSwapStall = 1;
    debugPages(0, false);
}

extern "C" void myron_gpu_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("myron-gpu", &mode, sizeof mode) || mode < 1 || mode > 11) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, reinterpret_cast<void *>(static_cast<uintptr_t>(mode)), &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
