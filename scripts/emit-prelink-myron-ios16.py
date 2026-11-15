#!/usr/bin/env python3
"""Run the stock XnuPrelink emit_prelink.py for the 20H392 kernelcache.

NX769J's emit-prelink-nx769j.py adds one entry to MATH_LITERAL_POOLS: the Libm
IEEE-754 coefficient table, pinned by VA + size + sha256, measured on the iOS
16.5 (20F66) kernelcache. Those exact constants do not apply here.

What this wrapper adds is measured, not guessed. In 20H392 the kit's own
instruction_inventory() rejected the Libm constant table at
0xFFFFFFF005B8C1CC with "External PC-relative encoding ... no instruction
rewrite supported": the segment com.apple.kec.Libm __TEXT_EXEC
(0xFFFFFFF005B8B000, 0x39E4 bytes) is entirely data, and some of its words
happen to decode as ADRP. Evidence that it is data and not code:

  * 99.0% of its 4-byte words are finite IEEE-754 floats in a plausible range;
  * 16-byte periodic records of (coefficient, tag) repeat inside it;
  * only 35% of its bytes are printable, so it is not a string table either;
  * the five pools the stock list already whitelists for this module match
    inside this very segment (0xFFFFFFF005B8B410, +0xC964, +0xD2C8, +0xD8EC,
    +0xE0F0), so it is the same Libm coefficient table, just with more entries
    in this build.

Four sub-ranges are the ones that trip the check. They are pinned by the same
prefix + exact-sha256 scheme, so a different kernelcache can never match them.
Verified offline with work/verify_libm_pools.py: stock pools reproduce the
ValueError, these four yield pc_relative_encodings_preserved = 236910 with the
13 known inline-data ranges reported. Only the inventory report changes; the
relocation itself still comes from _PrelinkLinkKASLROffsets.

Usage: invoked by [kernel.prelink] tool, receives the kit's own argv.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kits/XnuPrelink"

# com.apple.kec.Libm __TEXT_EXEC @ 0xFFFFFFF005B8B000 (0x39E4 bytes):
# (va, size, prefix, sha256) of the inline coefficient data that decodes as ADRP.
LIBM_INLINE_DATA = (
    (0xFFFFFFF005B8C1CC, 52, "485f9df0",
     "a9a6e0fc38df0ead5b312222ef2627503b44282bb03bbd85fe335d403e1509b1"),
    (0xFFFFFFF005B8C29C, 36, "12595670",
     "16e966a4150c3f040798e9233001155c612ef397ba134d0ca11f2f503af21d20"),
    (0xFFFFFFF005B8C490, 20, "b54eb2b7",
     "b5f6dc1815166be1321ca44186936643617225b1099318965df62c1c41bae371"),
    (0xFFFFFFF005B8C4D4, 28, "130c5496",
     "e56005d35ef1157c4b440f6a440e17be30a0eaeb2caaebdac7c9b73759feb0fd"),
)


def main() -> int:
    entry = KIT / "emit_prelink.py"
    if not entry.is_file():
        raise SystemExit(f"emit-prelink-myron-ios16: {entry} missing; run 'unixnu tree' first")
    sys.path.insert(0, str(KIT))
    spec = importlib.util.spec_from_file_location("emit_prelink", entry)
    if spec is None or spec.loader is None:
        raise SystemExit("emit-prelink-myron-ios16: cannot load emit_prelink.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["emit_prelink"] = module
    spec.loader.exec_module(module)
    module.MATH_LITERAL_POOLS = module.MATH_LITERAL_POOLS + tuple(
        (prefix, size, digest) for _, size, prefix, digest in LIBM_INLINE_DATA)
    print(f"emit-prelink-myron-ios16: {len(LIBM_INLINE_DATA)} measured 20H392 Libm "
          f"inline-data ranges added to MATH_LITERAL_POOLS", flush=True)
    return module.main()


if __name__ == "__main__":
    raise SystemExit(main())
