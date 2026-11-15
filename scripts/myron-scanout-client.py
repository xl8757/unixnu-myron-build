#!/usr/bin/env python3
"""Stage 99's userspace half: the scan-out worker survives a failed call (myron, 2026-10-26).

`GpuScanoutClient.h` (NativeMetal, the tree's 39ba232 checkout) returns false from `serveOne` on any `WAIT_SWAP` error
other than a timeout and on any `COMPLETE_SWAP` error. The worker's loop is `while (client.serveOne(500))`
(`NativeMetalBridgePhase.h`), so one transient failure — a refused mapping, or a frame the kernel's new watchdog has
already taken back — ended the GPU hand-off for the life of backboardd, and the display fell back to the CPU copy
until a respring.

This script makes both error paths non-fatal: the frame is counted as a failure, a failed wait sleeps 1 ms so a dead
connection cannot spin the thread, and serving continues. The kernel side (myron-scanout-watchdog.py) guarantees that
the display keeps retiring frames meanwhile. Anchors are from NativeMetal 39ba232; every edit is idempotent and each
anchor is asserted exactly once.
"""
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path.home() / "xnu2").resolve()
CLIENT = ROOT / "UniXNU/drivers/NativeMetal/GpuScanoutClient.h"

changes = []


def edit(path, old, new, marker):
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


edit(CLIENT,
     "    // One wait (at most timeoutMs) and, when the display driver offered a frame, one GPU copy and its completion. Returns false on a\n"
     "    // protocol error that makes further serving pointless.\n",
     "    // One wait (at most timeoutMs) and, when the display driver offered a frame, one GPU copy and its completion. A failed\n"
     "    // call is counted and serving continues: treating it as fatal ended the hand-off for the process's lifetime.\n",
     "treating it as fatal ended the hand-off")

edit(CLIENT,
     "        if (status) {\n"
     "            if ((unsigned)status == 0xe00002d6u /* kIOReturnTimeout */) {++stats_->timeouts; return true;}\n"
     "            ++stats_->failures;\n"
     "            stats_->error = status;\n"
     "            return false;\n"
     "        }\n",
     "        if (status) {\n"
     "            if ((unsigned)status == 0xe00002d6u /* kIOReturnTimeout */) {++stats_->timeouts; return true;}\n"
     "            // myron: a failed call (a refused mapping, a swap the kernel has taken back) is one lost frame, not the end of\n"
     "            // the hand-off. The kernel's watchdog gives the display its CPU path meanwhile; keep serving so the hand-off\n"
     "            // resumes as soon as the kernel offers a frame again.\n"
     "            ++stats_->failures;\n"
     "            stats_->error = status;\n"
     "            usleep(1000);\n"
     "            return true;\n"
     "        }\n",
     "a refused mapping, a swap the kernel has taken back")

edit(CLIENT,
     "        if (completed) {++stats_->failures; stats_->error = completed; return false;}\n",
     "        // myron: likewise a completion the kernel no longer holds (it abandoned the frame) is one lost frame, not fatal.\n"
     "        if (completed) {++stats_->failures; stats_->error = completed;}\n",
     "a completion the kernel no longer holds")

for line in changes:
    print(line)
print("stage 99 userspace changes complete (%d)" % len(changes))
