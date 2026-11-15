#!/usr/bin/env python3
"""Bound the Metal command-buffer completion wait in the upstream perf/gpu-smoothness frontend (myron stage 80).

The scanout worker's path is GpuScanoutClient::serveOne -> gpupresent::Presenter::present -> [commands waitUntilCompleted].
The upstream frontend waits forever on the bridge completion (nmb_command_buffer_wait(handle, 0)) and then forever on the
handler group. One lost completion parks the worker, COMPLETE_SWAP never runs, and the display's 5 s SwapWait fires.
This makes both waits 1 s; on timeout the caller sees a non-completed status, present() returns kGpuFailed, and the display
falls back. Stage 79 did this to the pre-smoothness frontend; this adapts the same edit to the new one.

  myron-bounded-completion.py [drivers/NativeMetal/NativeBridgeMetalFrontend.mm]
"""
import sys
from pathlib import Path

p = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('drivers/NativeMetal/NativeBridgeMetalFrontend.mm')
s = p.read_text()
if 'gWaitTimeouts' in s:
    print('PASS: bounded completion wait already present in', p)
    raise SystemExit(0)

g_old = 'static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;'
g_new = 'static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted,gWaitTimeouts;'
w_old = '''- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 const uint32_t cpuStart=threadCpuUs();
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
'''
w_new = '''- (void)waitUntilCompleted {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 // myron (stage 79/80): bounded. A completion the bridge never delivers must not park the scanout worker; on timeout the
 // caller sees a non-completed status and the display falls back. One second is far above a scanout blit (~11 ms).
 if(!self.failedLocally) {
  if(nmb_command_buffer_wait(_handle,1000)!=0) {
   gWaitTimeouts++;
   NBLOG(@"native-metal: completion wait timed out after 1000 ms (status %llu)",(unsigned long long)nmb_command_buffer_status(_handle));
  }
 }
 const uint32_t cpuStart=threadCpuUs();
 if(dispatch_group_wait(_handlers,dispatch_time(DISPATCH_TIME_NOW,(int64_t)1000*1000*1000))!=0) {
  gWaitTimeouts++;
  NBLOG(@"native-metal: completion handlers timed out (bridge did not deliver the completion)");
 }
'''
r_old = ('row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),'
         'ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));')
r_new = ('row("J completed wait n=%llu total=%llums max=%llums t=%llu",(unsigned long long)gTimeWaitCompleted.count.load(),'
         'ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()),(unsigned long long)gWaitTimeouts.load());')

for old, new, what in ((g_old, g_new, 'globals'), (w_old, w_new, 'waitUntilCompleted'), (r_old, r_new, 'J row')):
    if s.count(old) != 1:
        raise SystemExit(f'{what}: expected exactly one match, found {s.count(old)}; review the frontend')
    s = s.replace(old, new)
p.write_text(s)
print('PASS: bounded completion wait in', p)
