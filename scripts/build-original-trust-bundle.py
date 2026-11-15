#!/usr/bin/env python3
"""The original static/System trust-cache bundle of the payload (--trust-cache), from the two IPSW members.

  build-original-trust-bundle.py [OUT_IPSW_BUILD_DIR] [OUTPUT]

UniXNU's scripts/build-original-system-trust.py reads a local full IPSW and a separately extracted static trust cache.
This reads the members `unixnu ipsw fetch StaticTrustCache Cryptex1,SystemTrustCache` extracts into
out/ipsw/21H461/ (Firmware/044-76040-038.dmg.trustcache and Firmware/044-76034-038.dmg.trustcache, both IM4P), takes
their payloads and writes the same bundle: <u32 2><u32 12><u32 12 + len(static)>, then the two unchanged version-2
modules. The module pins are the ones of build-original-system-trust.py; the bundle must have the first hash of
board.toml [payload] trust_cache_sha256. Plain Python, runs anywhere.
"""
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEMBERS = ('044-76040-038.dmg.trustcache', '044-76034-038.dmg.trustcache')   # StaticTrustCache, Cryptex1,SystemTrustCache
MODULES = ('da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2',
           'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651')
BUNDLE = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def der(data, at):
    if at + 2 > len(data):
        raise ValueError('Truncated DER')
    tag, size = data[at:at + 2]
    at += 2
    if size & 128:
        count = size & 127
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError('Invalid DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER exceeds original input')
    return tag, data[at:at + size], at + size


def payload(original):
    tag, sequence, end = der(original, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, data, cursor = der(sequence, cursor)
        fields.append((kind, data))
    if tag != 0x30 or end != len(original) or [kind for kind, _ in fields] != [0x16, 0x16, 0x16, 4] or fields[0][1] != b'IM4P':
        raise SystemExit('Require an original trust-cache IM4P')
    return fields[3][1]


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / 'UniXNU/out/ipsw/21H461'
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'build/original-static-system.bundle'
    modules = [payload((source / name).read_bytes()) for name in MEMBERS]
    for module, digest in zip(modules, MODULES):
        if (hashlib.sha256(module).hexdigest() != digest or struct.unpack_from('<I', module)[0] != 2 or
                len(module) != 24 + struct.unpack_from('<I', module, 20)[0] * 24):
            raise SystemExit('Require complete exact original version-2 trust module')
    static, system = modules
    bundle = struct.pack('<3I', 2, 12, 12 + len(static)) + static + system
    if hashlib.sha256(bundle).hexdigest() != BUNDLE:
        raise SystemExit(f'bundle {hashlib.sha256(bundle).hexdigest()} is not the allow-listed {BUNDLE}')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bundle)
    print(f'{output}: {len(bundle)} bytes, sha256 {BUNDLE} (two unchanged original modules)')


if __name__ == '__main__':
    main()
