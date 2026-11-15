"""Read a flattened device tree: node walk and reserved-memory extents.

Two callers need the same read of a DTB blob: `extract-firmware-dtb.py`, which
prints what a vendor_boot carries, and `check-memory-map.py`, which compares
those reservations with the UEFI memory map. Both used to parse the structure
themselves, which is the kind of drift that makes one of them quietly wrong.

Nothing here is device tree *semantics*: no address-cell inference, no overlay
merging. It reads the reservations this board's firmware DTB declares and stops
there.
"""
from __future__ import annotations

import struct

DTB_MAGIC = 0xd00dfeed

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


class FdtError (ValueError):
    """A blob that is not a usable flattened device tree."""


def walk(blob: bytes):
    """Yield (path, {property: bytes}) once per node of a flattened device tree."""
    if len(blob) < 40:
        raise FdtError('shorter than a flattened device tree header')
    (_, _, off_struct, off_strings) = struct.unpack_from('>5I', blob, 0)[:4]
    if off_struct >= len(blob) or off_strings >= len(blob):
        raise FdtError('implausible flattened device tree header offsets')
    stack: list[str] = []
    current: tuple[str, dict] | None = None
    at = off_struct

    while at + 4 <= len(blob):
        token, = struct.unpack_from('>I', blob, at)
        at += 4
        if token == FDT_BEGIN_NODE:
            if current is not None:
                yield current
            end = blob.index(b'\0', at)
            name = blob[at:end].decode('utf-8', 'replace')
            at = (end + 1 + 3) & ~3
            stack.append(name)
            current = ('/' + '/'.join(stack[1:]), {})
        elif token == FDT_END_NODE:
            if current is not None:
                yield current
                current = None
            if stack:
                stack.pop()
        elif token == FDT_PROP:
            length, nameoff = struct.unpack_from('>II', blob, at)
            at += 8
            value = blob[at:at + length]
            at = (at + length + 3) & ~3
            end = blob.index(b'\0', off_strings + nameoff)
            prop = blob[off_strings + nameoff:end].decode('utf-8', 'replace')
            if current is not None:
                current[1][prop] = value
        elif token == FDT_NOP:
            continue
        else:
            break
    if current is not None:
        yield current


def reg_extent(value: bytes) -> tuple[int, int] | None:
    """(address, size) from a `reg` property, or None when it is not one cell pair.

    This board's trees use #address-cells = #size-cells = 2 and every
    reservation is a single extent. A property that is not exactly one
    (address, size) pair returns None rather than being truncated to fit.
    """
    words = struct.unpack('>%dI' % (len(value) // 4), value[:len(value) // 4 * 4])
    if len(words) == 4:
        return ((words[0] << 32) | words[1], (words[2] << 32) | words[3])
    if len(words) == 2:
        return (words[0], words[1])
    return None


class Reservation:
    """One /reserved-memory child."""

    __slots__ = ('name', 'address', 'size')

    def __init__(self, name: str, address: int | None, size: int | None) -> None:
        self.name = name
        self.address = address
        self.size = size

    @property
    def fixed(self) -> bool:
        return self.address is not None and self.size is not None

    @property
    def end(self) -> int:
        return self.address + self.size

    def __str__(self) -> str:
        # The width keeps the listing this prints byte-identical to what
        # device-inputs.md records, so a rerun can be diffed against it.
        if not self.fixed:
            return f'{self.name:28s} (size only, no reg)'
        return f'{self.name:28s} 0x{self.address:08x} / 0x{self.size:x}'


def reservations(blob: bytes) -> list[Reservation]:
    """The /reserved-memory children of one tree, in declaration order."""
    out: list[Reservation] = []
    for path, props in walk(blob):
        if not path.startswith('/reserved-memory/'):
            continue
        name = path.rsplit('/', 1)[1]
        extent = reg_extent(props['reg']) if 'reg' in props else None
        # A node with no `reg` is allocated at runtime; its `size` says how much
        # but not where, so it carries no extent to check.
        out.append(Reservation(name, *(extent if extent else (None, None))))
    return out
