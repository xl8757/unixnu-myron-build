// SPDX-License-Identifier: MIT
// The diagnostic page (BootProgress.c, from sheng) and NativeHidInput read counters of sheng's touch, CPU frequency, UFS
// log and Adreno drivers. myron links none of them, and the static kernel link refuses an undefined weak reference, so
// they are defined here, zero: their rows show nothing, and NativeHidInput sees no touch contacts. The display counters
// come from LegacyDisplayProbe.cpp and NativeGOPPresenter.cpp, the HID counters from HidInputProbe.cpp, the process rows
// from BootProcessProbe.c and BootUiTraceProbe.c. The Adreno cache counters native_adreno_cache_stats and
// native_adreno_reassert_ticks come from NativeAdrenoCpu.cpp, which the GPU observation (MyronGpuPower.cpp) links, and the
// submission counters native_adreno_coherence, native_adreno_wc_distrusted and native_adreno_wc_audit from its
// NativeAdrenoSubmit.cpp (the run mode, myron-gpu=10/11). The line audit's two counters stay sheng's GpuPowerProbe.cpp's.
#include <stdint.h>

char native_touch_rows[4][120];
volatile uint32_t native_touch_loop_step, native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile unsigned native_touch_stage_req;
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
void sheng_cpufreq_poll(void) {}
void boot_log_tick(unsigned seconds) { (void)seconds; }
volatile uint32_t native_adreno_audit_attr;
volatile uint64_t native_adreno_audit_ticks;
