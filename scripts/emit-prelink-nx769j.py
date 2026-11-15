#!/usr/bin/env python3
"""Run emit_prelink.py with this kernelcache's Libm literal pools known.

`kits/XnuPrelink/emit_prelink.py` walks every selected module's __TEXT_EXEC,
decodes each word as a possible PC-relative encoding, and refuses to continue
when one targets something outside the selected modules: it cannot tell an
instruction from a constant, so it errs toward reporting rather than guessing.

It distinguishes the two by a catalogue of known literal pools -- prefix, byte
length and a sha256 over the whole range -- and deliberately requires complete
identity, so a prefix alone never excludes code. `MATH_LITERAL_POOLS` is that
catalogue, and it is per kernelcache: the entries there belong to the iPadOS
17.7.11 image, whose Libm tables sit elsewhere.

This wrapper adds the pools that the iOS 16.5 kernelcache actually has and then
runs the tool unchanged. It is a thin adapter, not a second implementation: the
scan, the plan and the emit all come from the kit.

Why the range below is a literal pool and not code
--------------------------------------------------
`com.apple.kec.Libm` keeps its coefficient tables in __TEXT_EXEC rather than in
a data segment. The range is bounded by function boundaries on both sides: two
`ret` instructions immediately before it, and `stp x29,x30,[sp,#-16]!` followed
by `mov x29,sp` immediately after, which is the prologue of the next function.
Its contents are IEEE-754 single and double constants -- +inf, 8.0, 4.0, pi/2,
and long runs of repeated coefficient groups. A handful of its words happen to
decode as ADRP or BL, which is what makes the tool raise.

The wrapper re-derives that boundary from the image it is given and refuses to
run if it does not hold, so the entry cannot quietly start covering code if the
input changes. Loosening the tool's test instead would let real code be
classified as data and drop a genuine external encoding without a word.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'kits/XnuPrelink'

# (start, size, prefix bytes, sha256 of the range) for the Libm coefficient
# table that follows the last function before it in the iOS 16.5 kernelcache.
# Everything except the digest is re-verified against the image at run time.
LIBM_POOL_VA = 0xFFFFFFF005B8E054
LIBM_POOL_SIZE = 0x4CC
LIBM_POOL_SHA256 = '73df5b757c53532583e3686b7d93df11ec1f5dd8c201d05502654a7812cc385f'

RET = 0xD65F03C0
PROLOGUE_PAIR = (0xA9BF7BFD, 0x910003FD)  # stp x29,x30,[sp,#-16]! ; mov x29,sp


def fail(message: str) -> None:
    raise SystemExit('emit-prelink-nx769j: ' + message)


def load_kit():
    """Import emit_prelink and native_prelink from the kit directory."""
    if not (KIT / 'emit_prelink.py').is_file():
        fail(f'{KIT}/emit_prelink.py missing; run "unixnu tree" first')
    sys.path.insert(0, str(KIT))
    spec = importlib.util.spec_from_file_location('emit_prelink', KIT / 'emit_prelink.py')
    if spec is None or spec.loader is None:
        fail(f'cannot load {KIT}/emit_prelink.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules['emit_prelink'] = module
    spec.loader.exec_module(module)
    return module


def verify_pool(original: bytes, np) -> str:
    """Re-derive the pool boundary from the image and return its 16-byte prefix."""
    image = np.macho(original)
    segments = image['segments']

    def read(address: int, length: int) -> bytes:
        offset = np.backed_offset(segments, address, length, len(original))
        return original[offset:offset + length]

    pool = read(LIBM_POOL_VA, LIBM_POOL_SIZE)
    digest = hashlib.sha256(pool).hexdigest()
    if digest != LIBM_POOL_SHA256:
        fail(f'the Libm pool at {LIBM_POOL_VA:#x} hashes to {digest}, expected {LIBM_POOL_SHA256}; '
             'this is not the iOS 16.5 kernelcache the entry was measured from')

    # The word before the pool must be a function's last instruction, and the
    # two after it must begin the next function. Either failing means the range
    # no longer sits between two functions and could contain code.
    before, = struct.unpack('<I', read(LIBM_POOL_VA - 4, 4))
    if before != RET:
        fail(f'{LIBM_POOL_VA - 4:#x} is {before:#010x}, not the ret that should end the preceding function')
    prologue = struct.unpack('<2I', read(LIBM_POOL_VA + LIBM_POOL_SIZE, 8))
    if prologue != PROLOGUE_PAIR:
        fail(f'{LIBM_POOL_VA + LIBM_POOL_SIZE:#x} is {prologue}, not the prologue that should begin the next function')

    # No function of its own inside: no ret, and no stp x29,x30,[sp,#-N]!.
    for offset in range(0, len(pool), 4):
        word, = struct.unpack_from('<I', pool, offset)
        if word == RET:
            fail(f'{LIBM_POOL_VA + offset:#x} is a ret inside the supposed literal pool')
        if word & 0xFFE07FFF == 0xA9A07BFD:
            fail(f'{LIBM_POOL_VA + offset:#x} is a frame prologue inside the supposed literal pool')
    return pool[:16].hex()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--prelinked', type=Path, required=True,
                        help='the original kernelcache, to verify the literal pool against')
    args, rest = parser.parse_known_args()
    del rest

    emit = load_kit()
    np = sys.modules['native_prelink']
    if not args.prelinked.is_file():
        fail(f'{args.prelinked} missing')
    prefix = verify_pool(args.prelinked.read_bytes(), np)

    emit.MATH_LITERAL_POOLS = (*emit.MATH_LITERAL_POOLS, (prefix, LIBM_POOL_SIZE, LIBM_POOL_SHA256))
    print(f'emit-prelink-nx769j: Libm literal pool {LIBM_POOL_VA:#x}+{LIBM_POOL_SIZE:#x} verified and known')
    return emit.main()


if __name__ == '__main__':
    raise SystemExit(main())
