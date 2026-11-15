#!/usr/bin/env python3
"""Stage 99's source changes for myron's GPU scan-out hand-off, applied in place under a ~/xnu2 tree.

Why (2026-10-26, from the SWAP STALL page and the source):

 * The display driver offers a finished frame to the render server's scan-out worker through
   `NativeGOPPresenter::offer` (`sheng-fb-gpu=1`). Exactly one offer may be outstanding, and the swap on the display
   side is retired only when the worker answers `NGP_METHOD_COMPLETE_SWAP` (`completeSwap` -> `native_legacy_gpu_complete`
   -> `finishGpuSwap`).
 * The worker's loop is `while (client.serveOne(500))` (`NativeMetalBridgePhase.h`). `serveOne` returns false on any
   `WAIT_SWAP` error other than a timeout and on any `COMPLETE_SWAP` error (`GpuScanoutClient.h`), so one transient
   failure — or a completion the kernel no longer recognises — ends the scan-out worker for the life of the process.
 * If the worker is gone (or wedged inside its Metal copy), the offered frame is never completed, `swap.offered` stays
   true forever, every later offer is refused, and IOMFB's `swap_wait_gated` waits for a swap that will never retire:
   its 5 s timeout is exactly the panic/page this port keeps hitting (offers = done + 1, ring retired).

This script makes the kernel side self-healing, without touching the Metal submodule:

 1. `NativeGOPPresenter::watchdog()` runs on its own kernel thread every 200 ms. A frame that has been offered for
    longer than `staleNs` (default 3 s, `myron-gop-stale-ms` to change it, 500..20000) is taken away from the display:
    its mapping is kept alive for a late completion (one stale frame at a time), the display is told the swap failed
    (`kIOReturnTimeout`), which makes it use its own CPU copy for that frame and retire it, and `gpuClient` is cleared
    so offers are refused until a client waits again. A dead worker then leaves a working, CPU-copied display instead
    of a frozen one; a live worker re-arms the hand-off on its next wait.
 2. `completeSwap` treats a late completion of the abandoned sequence as success (and frees the kept mapping), so the
    worker no longer exits when the kernel already gave up on that frame.
 3. `abandonSwap` (client teardown) also frees the stale slot.
 4. `native_gop_gpu_stale` counts the rescues; the SWAP STALL page and the slot-345 panic text show it.

Every edit is idempotent and each anchor is asserted exactly once.
"""
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path.home() / "xnu2").resolve()
PRESENTER = ROOT / "UniXNU/drivers/NativeMobileDisplay/NativeGOPPresenter.cpp"
GPU_POWER = ROOT / "UniXNU-myron/package/MyronGpuPower.cpp"
PROBE = ROOT / "UniXNU-myron/package/LegacyDisplayProbe.cpp"

changes = []


def edit(path, old, new, marker):
    """Replace `old` once (or leave the file alone when `marker` is already there)."""
    if not path.exists():
        raise SystemExit("missing %s" % path)
    text = path.read_text()
    if marker in text:
        changes.append("already: %s (%s)" % (path.name, marker))
        return
    if text.count(old) != 1:
        raise SystemExit("%s: anchor found %d times, expected 1:\n%s" % (path.name, text.count(old), old[:300]))
    path.write_text(text.replace(old, new, 1))
    changes.append("edited: %s" % path.name)


# ---------------------------------------------------------------- NativeGOPPresenter.cpp

edit(PRESENTER,
     "#include <kern/clock.h>\n",
     "#include <kern/clock.h>\n#include <kern/thread.h>\n",
     "include <kern/thread.h>")

edit(PRESENTER,
     "volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned;\n",
     "volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned;\n"
     "/* a frame the hand-off watchdog took away because it stopped making progress (a lost completion, a dead client) */\n"
     "volatile uint64_t native_gop_gpu_stale;\n",
     "native_gop_gpu_stale;")

edit(PRESENTER,
     "class NativeGOPClient;\nclass NativeGOPPresenter;\n",
     "static void handoffWatchdogThread(void *,wait_result_t);\nclass NativeGOPClient;\nclass NativeGOPPresenter;\n",
     "static void handoffWatchdogThread(void *,wait_result_t);")

edit(PRESENTER,
     "            uint64_t stride;uint32_t width,height;} swap={};\n",
     "            uint64_t stride;uint32_t width,height;uint64_t offeredAt;} swap={};\n",
     "uint64_t offeredAt")

edit(PRESENTER,
     "    uint64_t swapSequence=0;\n",
     "    uint64_t swapSequence=0;\n"
     "    // A frame the watchdog took away stays mapped until the client's late completion frees it (one at a time), so\n"
     "    // nothing is unmapped under a worker that is still inside its Metal copy.\n"
     "    uint64_t staleSequence=0,staleNs=3000000000ull;\n"
     "    IOMemoryDescriptor *staleMemory=nullptr;\n"
     "    IOMemoryMap *staleMap=nullptr;\n",
     "staleSequence=0")

edit(PRESENTER,
     "    bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);\n",
     "    bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);\n"
     "    void watchdog();   // rescues a frame the scan-out worker stopped making progress on\n",
     "void watchdog();")

edit(PRESENTER,
     '    {unsigned v=0;PE_parse_boot_argn("sheng-fb-gpu",&v,sizeof v);gpuMode=(v==1);}\n',
     '    {unsigned v=0;PE_parse_boot_argn("sheng-fb-gpu",&v,sizeof v);gpuMode=(v==1);}\n'
     "    if(gpuMode) {\n"
     "        unsigned ms=0;\n"
     '        if(PE_parse_boot_argn("myron-gop-stale-ms",&ms,sizeof ms) && ms>=500 && ms<=20000) staleNs=(uint64_t)ms*1000000ull;\n'
     "    }\n",
     "myron-gop-stale-ms")

edit(PRESENTER,
     "    online=true;native_gop_ready=1;if(gpuMode)gHandoff=this;registerService();return true;\n",
     "    online=true;native_gop_ready=1;\n"
     "    if(gpuMode) {\n"
     "        gHandoff=this;\n"
     "        thread_t thread=nullptr;\n"
     "        if(kernel_thread_start(handoffWatchdogThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);\n"
     "    }\n"
     "    registerService();return true;\n",
     "handoffWatchdogThread,nullptr")

edit(PRESENTER,
     "        swap={true,false,++swapSequence,display,memory,nullptr,stride,width,height};\n",
     "        swap={};swap.offered=true;swap.sequence=++swapSequence;swap.display=display;swap.memory=memory;\n"
     "        swap.stride=stride;swap.width=width;swap.height=height;swap.offeredAt=mach_absolute_time();\n",
     "swap.offeredAt=mach_absolute_time()")

edit(PRESENTER,
     "    if(!swap.offered || !swap.delivered || swap.sequence!=sequence) {IOLockUnlock(gate);return kIOReturnBadArgument;}\n",
     "    if(!swap.offered || !swap.delivered || swap.sequence!=sequence) {\n"
     "        // A late completion of a frame the watchdog abandoned: free its mapping and answer success, so the scan-out\n"
     "        // worker keeps serving instead of treating the frame as a protocol error and leaving the hand-off.\n"
     "        if(sequence && sequence==staleSequence) {\n"
     "            IOMemoryMap *map=staleMap;IOMemoryDescriptor *memory=staleMemory;\n"
     "            staleMap=nullptr;staleMemory=nullptr;staleSequence=0;\n"
     "            IOLockUnlock(gate);\n"
     "            OSSafeReleaseNULL(map);if(memory)memory->release();\n"
     "            return kIOReturnSuccess;\n"
     "        }\n"
     "        IOLockUnlock(gate);return kIOReturnBadArgument;\n"
     "    }\n",
     "A late completion of a frame the watchdog abandoned")

edit(PRESENTER,
     "    gpuClient=false;\n"
     "    auto done=swap;swap={};\n"
     "    IOLockUnlock(gate);\n",
     "    gpuClient=false;\n"
     "    auto done=swap;swap={};\n"
     "    IOMemoryMap *smap=staleMap;IOMemoryDescriptor *smemory=staleMemory;staleMap=nullptr;staleMemory=nullptr;staleSequence=0;\n"
     "    IOLockUnlock(gate);\n"
     "    OSSafeReleaseNULL(smap);if(smemory)smemory->release();\n",
     "IOMemoryMap *smap=staleMap")

edit(PRESENTER,
     "bool NativeGOPPresenter::start(IOService *provider) {\n",
     "// Every 200 ms on its own kernel thread: take away a frame the display offered whose GPU copy neither completed nor\n"
     "// failed within staleNs. Its mapping stays valid for the client's late completion; the display is told the swap\n"
     "// failed, so its own path retires that frame; offers are refused (gpuClient=0) until a client waits again, so a\n"
     "// scan-out worker that died or wedged leaves a working CPU-copied display instead of a swap that never retires.\n"
     "void NativeGOPPresenter::watchdog() {\n"
     "    if(!gate)return;\n"
     "    void *display=nullptr;\n"
     "    IOMemoryMap *oldMap=nullptr;\n"
     "    IOMemoryDescriptor *oldMemory=nullptr;\n"
     "    IOLockLock(gate);\n"
     "    if(swap.offered) {\n"
     "        uint64_t ns=0;\n"
     "        absolutetime_to_nanoseconds(mach_absolute_time()-swap.offeredAt,&ns);\n"
     "        if(ns>=staleNs) {\n"
     "            display=swap.display;\n"
     "            oldMap=staleMap;oldMemory=staleMemory;\n"
     "            staleMap=swap.map;staleMemory=swap.memory;staleSequence=swap.sequence;\n"
     "            swap={};\n"
     "            gpuClient=false;\n"
     "            native_gop_gpu_stale=native_gop_gpu_stale+1;\n"
     "        }\n"
     "    }\n"
     "    IOLockUnlock(gate);\n"
     "    if(oldMap)oldMap->release();\n"
     "    if(oldMemory)oldMemory->release();\n"
     "    if(display)native_legacy_gpu_complete(display,kIOReturnTimeout);\n"
     "}\n"
     "bool NativeGOPPresenter::start(IOService *provider) {\n",
     "void NativeGOPPresenter::watchdog()")

edit(PRESENTER,
     "OSDefineMetaClassAndStructors(NativeGOPClient,IOUserClient)\n",
     "OSDefineMetaClassAndStructors(NativeGOPClient,IOUserClient)\n"
     "// The hand-off watchdog thread (GPU mode only): stops when the presenter is stopped (gHandoff cleared).\n"
     "static void handoffWatchdogThread(void *,wait_result_t) {\n"
     "    for(;;) {\n"
     "        IOSleep(200);\n"
     "        NativeGOPPresenter *presenter=gHandoff;\n"
     "        if(!presenter)break;\n"
     "        presenter->watchdog();\n"
     "    }\n"
     "}\n",
     "// The hand-off watchdog thread")

# ---------------------------------------------------------------- MyronGpuPower.cpp (the SWAP STALL row)

edit(GPU_POWER,
     "extern volatile uint64_t native_gop_gpu_abandoned __attribute__((weak_import));\n",
     "extern volatile uint64_t native_gop_gpu_abandoned __attribute__((weak_import));\n"
     "extern volatile uint64_t native_gop_gpu_stale __attribute__((weak_import));\n",
     "native_gop_gpu_stale __attribute__")

edit(GPU_POWER,
     '"SWAP wait >5s | offers %llu done %llu fail %llu abandoned %llu | worker waits %llu t/o %llu | retired %u refused %u/%x",\n',
     '"SWAP wait >5s | offers %llu done %llu fail %llu abandoned %llu stale %llu | worker waits %llu t/o %llu | retired %u refused %u/%x",\n',
     "abandoned %llu stale %llu")

edit(GPU_POWER,
     "                u64(&native_gop_gpu_abandoned), u64(waits), u64(waits ? waits + 1 : nullptr), u32(&native_legacy_retirement_calls),\n",
     "                u64(&native_gop_gpu_abandoned), u64(&native_gop_gpu_stale), u64(waits), u64(waits ? waits + 1 : nullptr),\n"
     "                u32(&native_legacy_retirement_calls),\n",
     "u64(&native_gop_gpu_stale)")

# ---- accelerator client records: why a process's connection was refused (diagnostic, 2026-10-26)

edit(GPU_POWER,
     "extern MyronClientText native_adreno_client_text[4];\n",
     "extern MyronClientText native_adreno_client_text[4];\n"
     "// The Adreno driver's per-client open records (NativeAdrenoPublish.cpp) and the last window overflow: which process was\n"
     "// refused a GPU connection, why, and how often. Diagnostic only; the layouts match the driver's.\n"
     "struct MyronClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};\n"
     "extern MyronClientRecord native_adreno_client_log[8];\n"
     "extern volatile uint32_t native_adreno_window_overflow[7][3];\n",
     "native_adreno_client_log[8]")

edit(GPU_POWER,
     '    pageRow(row++, "OPEN %.16s %.16s %.16s | START %.16s %.16s %.16s", d.opener[0], d.opener[1], d.opener[2], d.starter[0],\n'
     '            d.starter[1], d.starter[2]);\n',
     '    pageRow(row++, "OPEN %.16s %.16s %.16s | START %.16s %.16s %.16s", d.opener[0], d.opener[1], d.opener[2], d.starter[0],\n'
     '            d.starter[1], d.starter[2]);\n'
     '    {\n'
     '        // myron: the last mount failure of the accelerator (1 no free slot, 2 the core open, 3 the window scope) and the\n'
     '        // process whose connections were refused most often, with its opens, refusals, closes, live slot and last refusal.\n'
     '        unsigned worst = 0;\n'
     '        for (unsigned i = 1; i < 8; ++i) if (native_adreno_client_log[i].refused > native_adreno_client_log[worst].refused) worst = i;\n'
     '        const MyronClientRecord &c = native_adreno_client_log[worst];\n'
     '        pageRow(row++, "GM mount m%u e%x slot %u n%u | ovf %u/%u/%u", native_adreno_mount_fail[0], native_adreno_mount_fail[1],\n'
     '                native_adreno_mount_fail[2], native_adreno_mount_fail[3], native_adreno_window_overflow[0][0],\n'
     '                native_adreno_window_overflow[0][1], native_adreno_window_overflow[0][2]);\n'
     '        pageRow(row++, "GC %.12s o%u r%u c%u slot %u lr%x", c.name, c.opens, c.refused, c.closes, c.slot, c.last_refusal);\n'
     '    }\n',
     "GM mount m%u")

# ---------------------------------------------------------------- LegacyDisplayProbe.cpp (the slot-345 panic text)

edit(PROBE,
     'extern "C" volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned;\n',
     'extern "C" volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned,native_gop_gpu_stale;\n',
     "native_gop_gpu_abandoned,native_gop_gpu_stale;")

edit(PROBE,
     'fail=%llu abandoned=%llu waits=%llu/%llu "\n',
     'fail=%llu abandoned=%llu stale=%llu waits=%llu/%llu "\n',
     "abandoned=%llu stale=%llu")

edit(PROBE,
     "native_gop_gpu_failures,native_gop_gpu_abandoned,native_gop_wait_stats[0],native_gop_wait_stats[1],native_legacy_retirement_calls,",
     "native_gop_gpu_failures,native_gop_gpu_abandoned,native_gop_gpu_stale,native_gop_wait_stats[0],native_gop_wait_stats[1],native_legacy_retirement_calls,",
     "native_gop_gpu_abandoned,native_gop_gpu_stale,native_gop_wait_stats")

for line in changes:
    print(line)
print("stage 99 source changes complete (%d)" % len(changes))
