#!/usr/bin/env python3
"""Read the APFS volume UUIDs out of a raw disk image.

`board.toml [payload.adt] root_match_volume_uuid` is the one payload input that
is not derivable from the configuration alone. The ADT generator writes it to
`/chosen/root-matching` as an IOKit dictionary, and XNU mounts the IOMedia whose
UUID matches, so a wrong value boots the firmware and then finds no root.

UniXNU's userdata guide says to attach the OS image and read the Volume UUID
with `diskutil apfs list`. This does the same read without macOS: it finds the
APFS containers in a raw disk image, then the volume superblock of each, and
prints the volume UUID the way `diskutil` would spell it.

It takes the raw disk, not the .dmg. `./unixnu build userdata` writes
`userdata-root.img`, which holds the OS and SystemOS windows as raw disks at the
offsets `userdata-root.json` records, so that is the usual input. The windows
are exactly what gets flashed, which is also what the kernel will see.

What it reads
-------------
An APFS container superblock is `NXSB` at offset 0x20 of block 0, and a volume
superblock is `APSB` at the same offset of its own block. `apfs_vol_uuid` is the
16 bytes at 0xF0 of the volume superblock. A container keeps several copies of
its volume superblock through the checkpoint area, so the same volume appears
more than once; every copy of one volume carries the same UUID and the output
says how many copies each was seen in.

Volume superblocks are found by scanning rather than by walking the object map.
That reads the whole image once and needs no APFS tree code, and it cannot
silently report the wrong volume: the magic, the block alignment and the UUID
version and variant nibbles are all checked.

Usage:
    scripts/extract-root-volume-uuid.py userdata-root.img
    scripts/extract-root-volume-uuid.py userdata-root.img --json
"""
from __future__ import annotations

import argparse
import json
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

NXSB = b'NXSB'
APSB = b'APSB'
MAGIC_OFFSET = 0x20
BLOCK_SIZE_OFFSET = 0x24
BLOCK_COUNT_OFFSET = 0x28
CONTAINER_UUID_OFFSET = 0x48
VOL_UUID_OFFSET = 0xF0
# obj_phys_t: o_cksum, o_oid, o_xid, then o_type. OBJECT_TYPE_NX_SUPERBLOCK and
# OBJECT_TYPE_FS, each with OBJ_STORAGETYPE_PHYSICAL in the high half.
NX_TYPE = 0x80000001
FS_TYPE = 0x0000000d
TYPE_OFFSET = 0x18
SCAN = 32 << 20
CARRY = 8
# How far into a window its own container may start. The OS image puts a short
# header in front of the container, so the container is not at the window's
# first byte; anything much deeper is a file inside the volume.
WINDOW_SLACK = 0x10000


def fail(message: str) -> None:
    raise SystemExit('extract-root-volume-uuid: ' + message)


@dataclass
class Container:
    base: int
    block_size: int
    block_count: int


@dataclass
class Volume:
    container: int
    block: int
    value: uuid.UUID
    copies: int = 0


def magic_offsets(path: Path) -> tuple[list[int], list[int]]:
    """Where the container and volume superblock magics sit, in one pass."""
    containers: list[int] = []
    volumes: list[int] = []
    with path.open('rb') as image:
        at = 0
        carry = b''
        while True:
            data = image.read(SCAN)
            if not data:
                break
            window = carry + data
            base = at - len(carry)
            for magic, sink in ((NXSB, containers), (APSB, volumes)):
                found = window.find(magic)
                while found >= 0:
                    sink.append(base + found)
                    found = window.find(magic, found + 1)
            carry = window[-CARRY:]
            at += len(data)
    return containers, volumes


def read_blocks(path: Path, offsets: list[int]) -> dict[int, bytes]:
    """The header bytes of the block holding each offset, read once per offset."""
    found: dict[int, bytes] = {}
    with path.open('rb') as image:
        for offset in sorted(set(offsets)):
            if offset < 0:
                continue
            image.seek(offset)
            found[offset] = image.read(VOL_UUID_OFFSET + 16)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path, help='a raw disk image, such as userdata-root.img')
    parser.add_argument('--json', action='store_true', help='print the result as JSON')
    parser.add_argument('--report', type=Path, default=None,
                        help='the userdata-root.json naming each window; '
                             'defaults to one beside the image')
    parser.add_argument('--window', action='append', default=[],
                        metavar='NAME=START+LENGTH',
                        help='name a window in the image, so each volume is attributed to one')
    args = parser.parse_args()

    if not args.image.is_file():
        fail(f'{args.image} not found')

    windows = []
    report_path = args.report or args.image.with_suffix('.json')
    if report_path.is_file():
        # `build userdata` records where each window landed, and that is the
        # attribution the choice of a root volume needs.
        for window in json.loads(report_path.read_text(encoding='utf-8')).get('windows', []):
            windows.append((window['name'], window['offset'], window['size']))
    for window in args.window:
        name, _, extent = window.partition('=')
        start, _, length = extent.partition('+')
        try:
            windows.append((name, int(start, 0), int(length, 0)))
        except ValueError:
            fail(f'--window {window} is not NAME=START+LENGTH')
    if not windows:
        print(f'{report_path}: no window report, volumes are unattributed', file=sys.stderr)

    container_magics, volume_magics = magic_offsets(args.image)

    # A container superblock sits at block 0 of its container, so the container
    # starts 0x20 bytes before the magic. A container also keeps copies of that
    # superblock in its checkpoint area, and a string in a file can read as the
    # magic too, so the candidates are grouped by container UUID and the lowest
    # block of each group is the container. The block size check rules out the
    # strings.
    headers = read_blocks(args.image, [offset - MAGIC_OFFSET for offset in container_magics])
    groups: dict[bytes, Container] = {}
    for offset in container_magics:
        block = headers.get(offset - MAGIC_OFFSET)
        if block is None or len(block) < CONTAINER_UUID_OFFSET + 16:
            continue
        block_size = int.from_bytes(block[BLOCK_SIZE_OFFSET:BLOCK_SIZE_OFFSET + 4], 'little')
        block_count = int.from_bytes(block[BLOCK_COUNT_OFFSET:BLOCK_COUNT_OFFSET + 8], 'little')
        object_type = int.from_bytes(block[TYPE_OFFSET:TYPE_OFFSET + 4], 'little')
        if object_type != NX_TYPE or block_size not in (4096, 8192, 16384, 65536) or not block_count:
            continue
        identity = block[CONTAINER_UUID_OFFSET:CONTAINER_UUID_OFFSET + 16]
        base = offset - MAGIC_OFFSET
        known = groups.get(identity)
        if known is None:
            groups[identity] = Container(base, block_size, block_count)
        elif base < known.base:
            known.base = base
    containers = sorted(groups.values(), key=lambda container: container.base)

    # A volume superblock belongs to the container whose block grid it is on.
    volumes: list[Volume] = []
    blocks = read_blocks(args.image, [offset - MAGIC_OFFSET for offset in volume_magics])
    for offset in volume_magics:
        start = offset - MAGIC_OFFSET
        block = blocks.get(start)
        if block is None or len(block) < VOL_UUID_OFFSET + 16:
            continue
        if int.from_bytes(block[TYPE_OFFSET:TYPE_OFFSET + 4], 'little') != FS_TYPE:
            continue
        for container in containers:
            within = start - container.base
            if within < 0 or within % container.block_size:
                continue
            raw = block[VOL_UUID_OFFSET:VOL_UUID_OFFSET + 16]
            value = uuid.UUID(bytes=raw)
            # A UUIDv4 carries its version and variant in fixed nibbles. A block
            # that only happens to contain the magic usually fails here, which is
            # what keeps a false positive out of the answer.
            if value.version != 4 or raw[8] & 0xC0 != 0x80:
                continue
            existing = next((volume for volume in volumes if volume.value == value), None)
            if existing is None:
                volumes.append(Volume(container.base, within, value, 1))
            else:
                existing.copies += 1
            break

    if not containers:
        print(f'{args.image}: no APFS container found', file=sys.stderr)
    report = []
    for container in containers:
        origin = next(((name, start, length) for name, start, length in windows
                       if start <= container.base < start + length), None)
        name = origin[0] if origin else None
        # A window holds one container at its start. A container found deeper
        # inside a window is a disk image the volume carries as a file, not the
        # volume's own container.
        placement = ''
        if origin is not None:
            placement = f'  window {name}'
            if container.base - origin[1] >= WINDOW_SLACK:
                placement += ' (embedded, not the window container)'
        span = container.block_size * container.block_count
        print(f'container at 0x{container.base:x}, {span} bytes in {container.block_count} '
              f'{container.block_size}-byte blocks{placement}')
        for volume in (v for v in volumes if v.container == container.base):
            print(f'  volume {volume.value}  in {volume.copies} superblock copy/copies')
            report.append({'window': name, 'container': hex(container.base),
                           'block': hex(volume.block), 'volume_uuid': str(volume.value).upper(),
                           'copies': volume.copies})
        if not any(volume.container == container.base for volume in volumes):
            print('  no volume superblock found in this container', file=sys.stderr)

    if args.json:
        print(json.dumps(report, indent=2))
    elif not volumes:
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
