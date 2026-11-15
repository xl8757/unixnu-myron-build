#!/usr/bin/env python3
"""Check the UEFI memory map against the reservations this board declares.

`Library/MemoryMapLib/MemoryMapLib.c` is the file that decides which memory the
firmware may write to. Reserving the wrong range lets it overwrite what the
bootloader, TrustZone, the modem or a coprocessor owns, and the failure is a
corrupted boot rather than a build error. So the table is checked, not reviewed.

Two sources describe what is already taken:

  * the firmware device tree, whose /reserved-memory nodes are the bootloader's
    and TrustZone's own reservations -- `scripts/extract-firmware-dtb.py` pulls
    the tree out of `vendor_boot.img`
  * `board.toml` `[[bootimg.dtb_node]]`, which adds reservations to the boot
    image's device tree, currently the contiguous splash region

The map must cover every fixed-address reservation exactly once, and no two of
its entries may overlap. A descriptor is also allowed to cover more than one
reservation: nx729jPkg groups the device tree's fine-grained nodes into coarser
UEFI descriptors and this map follows it.

Regions UEFI lays out for itself -- FD Reserved, UEFI Stack, DXE Heap and the
rest -- appear in neither source, so they are named here and reported rather
than rejected.

Usage:
    scripts/check-memory-map.py --dtb Resources/DTBs/nx769j.dtb
    scripts/check-memory-map.py --dtb nx769j.dtb --map MemoryMapLib.c -v
"""
from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fdt  # noqa: E402  the scripts directory is not a package

# The UEFI's own DDR layout, in the order the map declares it. Anything else
# inside the DRAM window has to be explained by a reservation.
UEFI_REGIONS = (
    'FD Reserved', 'UEFI FD', 'UEFI FD Reserved', 'Info Blk', 'MMU PageTables',
    'Log Buffer', 'UEFI Stack', 'SEC Heap', 'Sched Heap', 'FV Region',
    'UEFI RESV', 'Kernel', 'DXE Heap',
)

# Matches one `gMemoryDescriptor[]` entry. The fields after the length are
# HobOption, ResourceType, ResourceAttribute, MemoryType and ArmAttribute; only
# ResourceType is read here, to tell the DDR descriptors the reservation check
# runs on from the register regions that are MMIO and live outside DRAM.
DESCRIPTOR = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,\s*(?P<address>0x[0-9A-Fa-f]+|\d+)\s*,'
    r'\s*(?P<length>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<hob>\w+)\s*,\s*(?P<resource>\w+)\s*,')

MMIO = 'MMAP_IO'
class Region:
    __slots__ = ('name', 'address', 'length', 'source', 'resource')

    def __init__(self, name: str, address: int, length: int, source: str,
                 resource: str = 'SYS_MEM') -> None:
        self.name = name
        self.address = address
        self.length = length
        self.source = source
        self.resource = resource

    @property
    def end(self) -> int:
        return self.address + self.length

    @property
    def dram(self) -> bool:
        """True when this is a DDR descriptor rather than a register region."""
        return self.resource != MMIO

    def covers(self, other: 'Region') -> bool:
        return self.address <= other.address and other.end <= self.end

    def overlaps(self, other: 'Region') -> bool:
        return self.address < other.end and other.address < self.end

    def __str__(self) -> str:
        return f'{self.name:20s} 0x{self.address:08x} + 0x{self.length:x}'


def fail(message: str) -> None:
    raise SystemExit('check-memory-map: ' + message)


def map_regions(path: Path) -> list[Region]:
    """The descriptors of MemoryMapLib.c, in table order."""
    if not path.is_file():
        fail(f'{path} not found')
    regions = []
    for match in DESCRIPTOR.finditer(path.read_text(encoding='utf-8')):
        regions.append(Region(match.group('name'),
                              int(match.group('address'), 0),
                              int(match.group('length'), 0),
                              'map',
                              match.group('resource')))
    if not regions:
        fail(f'no memory descriptors found in {path}')
    return regions


def dtb_reservations(path: Path) -> list[Region]:
    """The fixed-address reservations of the firmware device tree."""
    if not path.is_file():
        fail(f'{path} not found')
    blob = path.read_bytes()
    if int.from_bytes(blob[:4], 'big') != fdt.DTB_MAGIC:
        fail(f'{path} is not a flattened device tree')
    try:
        found = fdt.reservations(blob)
    except fdt.FdtError as error:
        fail(f'{path} cannot be read: {error}')
    regions = []
    for reservation in found:
        if reservation.fixed:
            regions.append(Region(reservation.name, reservation.address,
                                  reservation.size, 'dtb'))
    return regions


def boot_image_reservations(path: Path) -> list[Region]:
    """Reservations `[[bootimg.dtb_node]]` adds to the boot image's device tree.

    The cell order is the one the node is written in: address cells then size
    cells, each 64-bit and given as two hex strings.
    """
    if not path.is_file():
        fail(f'{path} not found')
    board = tomllib.loads(path.read_text(encoding='utf-8'))
    regions = []
    for node in board.get('bootimg', {}).get('dtb_node', []):
        name = node.get('path', '?').rsplit('/', 1)[-1]
        for prop in node.get('props', []):
            if prop.get('name') != 'reg':
                continue
            cells = [int(value, 0) for value in prop.get('value', [])]
            if len(cells) != 4:
                fail(f'bootimg.dtb_node {name} has a reg of {len(cells)} cells, expected 4')
            address = (cells[0] << 32) | cells[1]
            length = (cells[2] << 32) | cells[3]
            regions.append(Region(name, address, length, 'boot image'))
    return regions


def audit(descriptors: list[Region], reservations: list[Region],
          dram: Region) -> tuple[list[str], list[Region], list[Region]]:
    """Compare a map with the reservations, and say what is wrong.

    Returns the problems, the descriptors that cover a reservation, and the
    descriptors that cover nothing and are not UEFI's own layout. Separating it
    from the argument handling is what makes the comparison testable without a
    device tree on disk.
    """
    problems: list[str] = []

    # The register regions are MMIO and deliberately live outside DRAM. Every
    # DDR descriptor, on the other hand, has to sit inside the DRAM window: one
    # reaching past it would reserve or allocate where the board has no RAM.
    in_dram = [region for region in descriptors if region.dram]
    for region in in_dram:
        if not dram.covers(region):
            problems.append(f'{region} is not inside the DRAM window '
                            f'0x{dram.address:x}..0x{dram.end:x}')

    for index, first in enumerate(in_dram):
        for second in in_dram[index + 1:]:
            if first.overlaps(second):
                problems.append(f'descriptors overlap: {first} and {second}')

    for reservation in sorted(reservations, key=lambda region: region.address):
        owners = [region for region in in_dram if region.covers(reservation)]
        if not owners:
            problems.append(f'reservation not covered by the map: {reservation}')
        elif len(owners) > 1:
            problems.append('reservation covered more than once: ' +
                            f'{reservation} by ' + ', '.join(str(owner) for owner in owners))

    claimed = [region for region in in_dram if any(region.covers(r) for r in reservations)]
    unexplained = [region for region in in_dram
                   if region not in claimed and region.name not in UEFI_REGIONS]
    for region in unexplained:
        problems.append(f'{region} covers no reservation and is not a UEFI region')

    return problems, claimed, unexplained


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent.parent
    parser.add_argument('--dtb', type=Path, required=True, help='the firmware device tree')
    parser.add_argument('--map', type=Path,
                        default=here / 'firmware/Platforms/ZTE/nx769jPkg/Library/MemoryMapLib/MemoryMapLib.c',
                        help='the memory map source to check')
    parser.add_argument('--board', type=Path, default=here / 'board.toml',
                        help='the board configuration holding [[bootimg.dtb_node]]')
    parser.add_argument('--dram-base', type=lambda value: int(value, 0), default=0x80000000)
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), default=0x80000000)
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='also list every descriptor and every free band')
    args = parser.parse_args()

    dram = Region('DRAM', args.dram_base, args.dram_size, 'argument')
    descriptors = map_regions(args.map)
    reservations = dtb_reservations(args.dtb) + boot_image_reservations(args.board)
    problems, claimed, unexplained = audit(descriptors, reservations, dram)
    in_dram = [region for region in descriptors if region.dram]

    if args.verbose:
        print(f'memory map: {args.map}')
        for region in descriptors:
            if not region.dram:
                kind = 'mmio'
            elif region in claimed:
                kind = 'reserved'
            elif region.name in UEFI_REGIONS:
                kind = 'uefi'
            else:
                kind = 'other'
            print(f'  [{kind:8s}] {region}')
        free = free_bands(dram, in_dram)
        print(f'  free within DRAM: {len(free)} band(s)')
        for band in free:
            print(f'    0x{band.address:08x} + 0x{band.length:x} ({band.length // 0x100000} MiB)')

    print(f'check-memory-map: {len(descriptors)} descriptors, '
          f'{len(reservations)} fixed reservations, '
          f'{len(claimed)} covering one, {len(unexplained)} unexplained')
    for problem in problems:
        print(f'  {problem}')
    if problems:
        print(f'check-memory-map: {len(problems)} problem(s)')
        return 1
    print(f'check-memory-map: {args.map.name} covers every reservation and overlaps nothing')
    return 0


def free_bands(dram: Region, descriptors: list[Region]) -> list[Region]:
    """The parts of the DRAM window no descriptor claims."""
    bands, at = [], dram.address
    for region in sorted(descriptors, key=lambda region: region.address):
        if region.address > at:
            bands.append(Region('free', at, region.address - at, 'computed'))
        at = max(at, region.end)
    if at < dram.end:
        bands.append(Region('free', at, dram.end - at, 'computed'))
    return bands


if __name__ == '__main__':
    raise SystemExit(main())
