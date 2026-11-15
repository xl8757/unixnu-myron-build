#!/usr/bin/env python3
"""Extract the stock Canoe base DTB of myron from vendor_boot.img for `unixnu bootimg --dtb`.

The stock boot image is header v4 and keeps its SoC DTBs in vendor_boot; the board
overlay (myron, board-id 8) is the dtbo partition, which the stock ABL applies over
the DTB it selects. An appended DTB reaches the ABL's legacy path ("Single appended
DTB found"), so the boot image carries the base DTB whose qcom,msm-id matches the
SoC: 0x294 (canoe, soc_id 660) or 0x295 (canoep, soc_id 661), revision 1 or 2.
Pure Python: no dtc needed. Output: build/myron/canoe.dtb (ignored).
"""
from pathlib import Path
import argparse
import struct

ROOT = Path(__file__).resolve().parents[1]
FDT_MAGIC = b'\xd0\x0d\xfe\xed'


def vendor_dtb(image):
    if image[:8] != b'VNDRBOOT':
        raise SystemExit('Not a vendor_boot image')
    version, page = struct.unpack_from('<II', image, 8)
    if version < 3:
        raise SystemExit(f'vendor_boot header v{version}: expected v3 or v4')
    ramdisk_size, = struct.unpack_from('<I', image, 24)
    header_size, dtb_size = struct.unpack_from('<II', image, 28 + 2048 + 4 + 16)
    align = lambda value: (value + page - 1) // page * page
    start = align(header_size) + align(ramdisk_size)
    return image[start:start + dtb_size]


def root_property(blob, wanted):
    """Value of a property of the root node of one flattened DT."""
    off_struct, off_strings = struct.unpack_from('>II', blob, 8)
    strings = blob[off_strings:]
    at = off_struct
    depth = 0
    while True:
        token, = struct.unpack_from('>I', blob, at)
        at += 4
        if token == 1:
            end = blob.index(b'\0', at)
            at = (end + 4) & ~3
            depth += 1
        elif token == 3:
            length, name = struct.unpack_from('>II', blob, at)
            at += 8
            key = strings[name:strings.index(b'\0', name)].decode()
            if depth == 1 and key == wanted:
                return blob[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 2:
            depth -= 1
            if depth == 0:
                return None
        elif token == 9:
            return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vendor_boot', type=Path, help='vendor_boot.img of the stock fastboot package')
    parser.add_argument('--soc-id', type=lambda value: int(value, 0), default=660, help='/sys/devices/soc0/soc_id (660 canoe, 661 canoep)')
    parser.add_argument('--revision', type=int, choices=(1, 2), default=2, help='/sys/devices/soc0/revision major')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/myron/canoe.dtb')
    args = parser.parse_args()
    if args.soc_id not in (0x294, 0x295):
        parser.error('--soc-id must be 660 (canoe) or 661 (canoep)')
    wanted = (args.soc_id, args.revision << 16)
    table = vendor_dtb(args.vendor_boot.read_bytes())
    found = None
    at = 0
    while at + 8 <= len(table) and table[at:at + 4] == FDT_MAGIC:
        size, = struct.unpack_from('>I', table, at + 4)
        blob = table[at:at + size]
        ids = root_property(blob, 'qcom,msm-id') or b''
        pairs = [struct.unpack_from('>II', ids, i) for i in range(0, len(ids) - 7, 8)]
        model = (root_property(blob, 'model') or b'').rstrip(b'\0').decode()
        if wanted in pairs:
            if found is not None:
                raise SystemExit(f'More than one base DTB matches msm-id {wanted[0]:#x}/{wanted[1]:#x}')
            found = (blob, model)
        at += size
    if found is None:
        raise SystemExit(f'No base DTB matches msm-id {wanted[0]:#x}/{wanted[1]:#x}')
    blob, model = found
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f'{args.output}: {model} ({len(blob)} bytes, msm-id {wanted[0]:#x}/{wanted[1]:#x})')


if __name__ == '__main__':
    main()
