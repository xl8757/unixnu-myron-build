#!/usr/bin/env python3
"""Inject a VA=PA framebuffer TTE into start.s after all patches are applied.

boot_progress C code writes 0xfc800000 with the MMU on; without a TTE for that
VA the stores fault. Doing this here (string replace, not a patch hunk) avoids
line-number drift from the many earlier patches that also touch start.s.
Idempotent: no-ops if the marker is already present.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
MARKER = '/* myron framebuffer VA=PA map */'
if MARKER in text:
    print('framebuffer map already present; skip')
    sys.exit(0)

anchor = '\tcreate_bootstrap_mapping x14, x15, x5, x3, x2, x9, x10, x11, x12, x13\n'
if text.count(anchor) != 1:
    raise SystemExit(f'expected 1 anchor, found {text.count(anchor)}')

# Correct VA=PA mapping: 0xfc800000 is at L1 index 3 (VA>>30 & 0x1ff),
# L2 index 484 ((VA>>21)&0x1ff). x2 is the allocator's next-free L2 table
# pointer (create_bootstrap_mapping advances it by PGBYTES each time); x25 is
# the TTBR0 L1 table base. We must write BOTH an L1 table descriptor pointing
# at a fresh L2 table AND the L2 block entry. Writing only an L2 index off x25
# corrupts the L1 table (the earlier bug that caused an immediate reboot).
inject = anchor + '\n\t' + MARKER + '\n' + '\n'.join([
    '\tMOV64\tx10, 0xfc800000',
    '\tMOV64\tx11, 0x701',
    '\torr\t\tx11, x11, x10',
    '\tand\t\tx12, x10, #(ARM_TT_L2_INDEX_MASK)',
    '\tlsr\t\tx12, x12, #(ARM_TT_L2_SHIFT)',
    '\tstr\t\tx11, [x2, x12, lsl #TTE_SHIFT]',
    '\tlsr\t\tx13, x10, #30',
    '\tand\t\tx13, x13, #0x1ff',
    '\torr\t\tx14, x2, #3',
    '\tstr\t\tx14, [x25, x13, lsl #TTE_SHIFT]',
    '\tadd\t\tx2, x2, #1, lsl #12',
    '\tdsb\t\tish',
]) + '\n'
path.write_text(text.replace(anchor, inject, 1))
print('injected framebuffer VA=PA map into', path)
