#!/usr/bin/env python3
"""Extract the DXE drivers out of a Qualcomm UEFI firmware volume.

`build firmware` needs twenty vendor UEFI DXE drivers under
`Binaries/nx769j/QcomPkg/Drivers/…`. They are not redistributable, so every
build machine collects them from the device's own firmware package. This script
does that collection from the image that carries them.

Qualcomm ships the UEFI firmware as an ELF with an EFI firmware volume embedded
in it. The volume is a sequence of FFS files; each carries a UI section holding
the driver name and a PE32 section holding the driver itself. The names are the
ones `Include/DXE.inc` refers to -- `EnvDxe`, `UFSDxe`, `RpmhDxe` and the rest
-- so extracting by name is what makes them match.

Read-only against the firmware image: nothing outside --out is written.

Usage:
    scripts/extract-uefi-drivers.py --image uefi.img            # list
    scripts/extract-uefi-drivers.py --image uefi.img --out DIR  # extract
"""
from __future__ import annotations

import argparse
import hashlib
import lzma
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
DEVICE = HERE.parents[1]

FVH = b'_FVH'


def efi_guid(text: str) -> bytes:
    """The on-disk layout: Data1/2/3 little endian, Data4 as written."""
    parts = text.split('-')
    return (struct.pack('<IHH', int(parts[0], 16), int(parts[1], 16), int(parts[2], 16))
            + bytes.fromhex(parts[3]) + bytes.fromhex(parts[4]))


# EFI_LZMA_CUSTOM_DECOMPRESS, and the gzip one this board also uses. Note the
# on-disk layout when reading a GUID out of a hex dump: Data1, Data2 and Data3
# are little endian, so `e91f301d79be5343…` is 1D301FE9-BE79-4353-… and not the
# other way round.
LZMA_CUSTOM_DECOMPRESS = efi_guid('EE4E5898-3914-4259-9D6E-DC7BD79403CF')
GZIP_CUSTOM_DECOMPRESS = efi_guid('1D301FE9-BE79-4353-91C2-D23BC959AE0C')

FFS_FILE_HEADER2 = 0x00  # size field 0xFFFFFF means a large-file header follows
SECTION_GUID_DEFINED = 0x02
SECTION_PE32 = 0x10
SECTION_TE = 0x12
SECTION_DEPEX = 0x13
SECTION_UI = 0x15
SECTION_VERSION = 0x16
SECTION_RAW = 0x19

# What board.toml [firmware] and Include/DXE.inc expect under Binaries/nx769j.
# The UI section names in the image do not always match the INF names the DSC
# refers to -- the INF is a wrapper written around the prebuilt module -- so a
# name that differs is mapped rather than guessed at by prefix.
UI_TO_INF = {
    'EnvDxeEnhanced': 'EnvDxe',
    'ChipInfo': 'ChipInfoDxe',
    'PlatformInfoDxeDriver': 'PlatformInfoDxe',
    'DALSys': 'DALSYSDxe',
    'HALIOMMU': 'HALIOMMUDxe',
    'HWIODxeDriver': 'HWIODxe',
    'I2C': 'I2CDxe',
    'DALTLMM': 'TLMMDxe',
    'PmicDxe': 'PmicDxeLa',
    'SPMI': 'SPMIDxe',
    'SPI': 'SPIDxe',
    'XhciPciEmulation': 'XhciPciEmulationDxe',
    'GlinkDxe': 'GLinkDxe',
}
def inf_name(ui: str) -> str:
    """The INF name the DSC uses, given the module's UI section name."""
    return UI_TO_INF.get(ui, ui)


def referenced_modules(path: Path) -> list[tuple[str, str]]:
    """The (directory, INF name) pairs the platform's includes actually name.

    APRIORI.inc, DXE.inc and RAW.inc all pull from Binaries/<device>, and
    APRIORI.inc names one that DXE.inc has commented out. Reading one of the
    three under-counts the set. Some modules are in a family directory that
    differs from the INF name -- `TzDxe/ScmDxeCompat.inf` beside
    `TzDxe/TzDxeLA.inf`, and `PmicDxe/PmicDxeLa.inf` -- so the pair is kept
    rather than assumed equal.
    """
    here = path if path.is_dir() else path.parent
    includes = sorted(here.glob('*.inc')) if here.is_dir() else [path]
    if not includes:
        fail(f'no Include/*.inc under {here}; cannot tell which modules the firmware needs')
    out: list[tuple[str, str]] = []
    for include in includes:
        for line in include.read_text(encoding='utf-8').splitlines():
            stripped = line.strip()
            if not stripped.startswith('INF Binaries/'):
                continue
            parts = stripped.split('INF ', 1)[1].strip().split('/')
            entry = (parts[-2], parts[-1][:-4] if parts[-1].endswith('.inf') else parts[-1])
            if entry not in out:
                out.append(entry)
    return out


def referenced_raw_files(path: Path) -> list[str]:
    """The RawFiles/ names the platform's includes embed.

    Panel descriptions sit behind `!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1`, which
    this board sets to 0, so most of the list is not built. The unguarded ones
    are what the firmware actually needs; the guarded ones are reported as
    unreferenced rather than required.
    """
    here = path if path.is_dir() else path.parent
    out: list[str] = []
    for include in sorted(here.glob('*.inc')):
        guarded = 0
        for line in include.read_text(encoding='utf-8').splitlines():
            stripped = line.strip()
            if stripped.startswith('!if'):
                guarded += 1
                continue
            if stripped.startswith('!endif'):
                guarded = max(0, guarded - 1)
                continue
            if guarded or not stripped.startswith('SECTION RAW'):
                continue
            marker = 'RawFiles/'
            if marker in stripped:
                name = stripped.split(marker, 1)[1].strip()
                if name not in out:
                    out.append(name)
    return out


INF_TEMPLATE = '''[Defines]
  INF_VERSION                    = 0x0001001B
  BASE_NAME                      = {base_name}
  FILE_GUID                      = {file_guid}
  MODULE_TYPE                    = DXE_DRIVER
  VERSION_STRING                 = 1.0
  ENTRY_POINT                    = EfiEntry

[Binaries.AARCH64]
{binaries}
[Depex]
  TRUE
'''


def file_guid(name: str) -> str:
    """A stable GUID per module, so a rebuild produces the same wrapper.

    EDK II identifies a module in the firmware volume by this, so it has to be
    unique; it does not have to be the one Device-Binaries picked, and deriving
    it from the name keeps the output reproducible.
    """
    import uuid
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f'UniXNU-nx769j:{name}')).upper()


def write_module(root: Path, directory: str, inf: str, entry: dict) -> Path:
    """One driver directory: the INF wrapper, the module and its depex."""
    out = root / directory
    out.mkdir(parents=True, exist_ok=True)
    (out / f'{inf}.efi').write_bytes(entry['payload'])
    binaries = [f'  PE32|{inf}.efi|*']
    if entry['depex']:
        (out / f'{inf}.depex').write_bytes(entry['depex'])
        binaries.insert(0, f'  DXE_DEPEX|{inf}.depex|*')
    (out / f'{inf}.inf').write_text(INF_TEMPLATE.format(
        base_name=entry['name'], file_guid=file_guid(inf),
        binaries='\n'.join(binaries) + '\n'))
    return out


def fail(message: str) -> None:
    raise SystemExit('extract-uefi-drivers: ' + message)


def align(value: int, to: int = 4) -> int:
    return (value + to - 1) & ~(to - 1)


def find_volume(image: bytes) -> tuple[int, int]:
    """Offset and length of the first EFI firmware volume in an image."""
    at = image.find(FVH)
    if at < 0:
        fail('no EFI firmware volume: the _FVH signature is absent')
    base = at - 40  # zero vector (16) + FvNameGuid (16) + FvLength (8)
    if base < 0:
        fail('the _FVH signature is too early in the image to be a volume header')
    length, = struct.unpack_from('<Q', image, base + 32)
    if length <= 0 or base + length > len(image):
        fail(f'implausible firmware volume length {length:#x} at {base:#x}')
    return base, length


def files(image: bytes, base: int, length: int):
    """Yield (offset, name GUID bytes, type, size, body) for each FFS file.

    Free space is erased flash -- 0xFF when erase polarity is set -- and can sit
    between files, so a run of it is skipped rather than taken as end of volume.
    """
    header_length, = struct.unpack_from('<H', image, base + 48)
    at = base + header_length
    end = base + length
    while at + 24 <= end:
        if image[at] == 0xFF:
            at += 1
            continue
        name = image[at:at + 16]
        if name == b'\0' * 16:
            break
        file_type = image[at + 18]
        size = int.from_bytes(image[at + 20:at + 23], 'little')
        header = 24
        if size == 0xFFFFFF:
            if at + 32 > end:
                break
            size, = struct.unpack_from('<Q', image, at + 24)
            header = 32
        if size < header or at + size > end or file_type not in range(0x01, 0x10):
            at += 8
            continue
        yield at, name, file_type, size, image[at + header:at + size]
        at = align(at + size, 8)


def ui_name(data: bytes) -> str:
    """UI sections are UTF-16LE; some vendors write ASCII."""
    try:
        return data.decode('utf-16-le').split('\0')[0]
    except UnicodeDecodeError:
        return data.split(b'\0')[0].decode('utf-8', 'replace')


def sections(body: bytes):
    """Yield (type, data) for each section of an FFS file body."""
    at = 0
    while at + 4 <= len(body):
        size = int.from_bytes(body[at:at + 3], 'little')
        section_type = body[at + 3]
        if size < 4 or at + size > len(body):
            break
        yield section_type, body[at + 4:at + size]
        at = align(at + size, 4)


def decompress(payload: bytes, section_guid: bytes) -> bytes | None:
    """Unwrap a GUID_DEFINED section, or return None if it is not readable."""
    import gzip
    attempts = [
        ('gzip', lambda: gzip.decompress(payload)),
        ('lzma', lambda: lzma.decompress(payload)),
    ]
    # The GUID says which, but the header bytes say it more reliably when a
    # vendor reuses one; try both and take the first that works.
    if section_guid == LZMA_CUSTOM_DECOMPRESS:
        attempts.reverse()
    for _, attempt in attempts:
        try:
            return attempt()
        except Exception:
            continue
    return None


def collect(image: bytes, base: int | None = None, length: int | None = None,
            depth: int = 0) -> list[dict]:
    """Every FFS file of a volume, descending into compressed nested volumes.

    Qualcomm ships the DXE drivers inside firmware volumes that are themselves
    LZMA-compressed sections of an outer volume, so a flat walk finds only the
    SEC and the platform configuration. `UEFI_LZMA_CUSTOM_DECOMPRESS` names the
    only compression this board uses.
    """
    if base is None or length is None:
        base, length = find_volume(image)
    out = []
    for offset, guid, file_type, size, body in files(image, base, length):
        name, payload, depex, nested = '', b'', b'', []
        for section_type, data in sections(body):
            if section_type == SECTION_UI:
                name = ui_name(data)
            elif section_type == SECTION_DEPEX and not depex:
                depex = data
            elif section_type in (SECTION_PE32, SECTION_TE, SECTION_RAW) and not payload:
                payload = data
            elif section_type == SECTION_GUID_DEFINED and len(data) >= 20:
                section_guid = data[:16]
                data_offset, _ = struct.unpack_from('<HH', data, 16)
                if section_guid in (LZMA_CUSTOM_DECOMPRESS, GZIP_CUSTOM_DECOMPRESS) and data_offset >= 4:
                    inner = decompress(data[data_offset - 4:], section_guid)
                    if inner is None:
                        continue
                    if depth < 4:
                        # The decompressed block is not always the FV at offset
                        # zero -- this board prefixes it -- so locate the volume
                        # by its signature the way find_volume does.
                        mark = inner.find(FVH)
                        if mark >= 40:
                            inner_base = mark - 40
                            inner_length, = struct.unpack_from('<Q', inner, inner_base + 32)
                            if 0 < inner_length <= len(inner) - inner_base:
                                nested = collect(inner, inner_base, inner_length, depth + 1)
        out.append({'offset': offset, 'guid': guid.hex(), 'type': file_type,
                    'size': size, 'name': name, 'payload': payload,
                    'depex': depex, 'nested': nested})
    return out


def flatten(entries: list[dict]) -> list[dict]:
    for entry in entries:
        yield entry
        yield from flatten(entry['nested'])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, required=True,
                        help='the firmware image carrying the volume (for example uefi.img)')
    parser.add_argument('--out', type=Path, default=None,
                        help='write each driver here as <name>.efi')
    parser.add_argument('--layout', type=Path, default=None,
                        help='write the Binaries/<device>/QcomPkg/Drivers/ tree here, '
                             'each module with its INF wrapper and depex')
    parser.add_argument('--dxe-inc', type=Path,
                        default=DEVICE / 'firmware/Platforms/ZTE/nx769jPkg/Include',
                        help='the DSC include directory naming the modules to recover')
    parser.add_argument('--require', action='store_true',
                        help='fail unless every expected driver is present')
    args = parser.parse_args()

    if not args.image.is_file():
        fail(f'{args.image} not found')
    image = args.image.read_bytes()
    outer = collect(image)
    entries = list(flatten(outer))
    base, length = find_volume(image)
    nested = len(entries) - len(outer)
    print(f'{args.image.name}: firmware volume at {base:#x}, {length:#x} bytes')
    print(f'  {len(entries)} FFS files ({len(outer)} outer, {nested} in nested volumes), '
          f'{sum(1 for e in entries if e["payload"])} with a body')

    modules = referenced_modules(args.dxe_inc)
    wanted = {inf for _, inf in modules}
    by_inf = {inf_name(e['name']): e for e in entries if e['name']}

    drivers = [e for e in entries if e['payload'] and e['name']]
    print(f'  {len(drivers)} named modules in the image, {len(modules)} referenced by {args.dxe_inc.name}')
    for e in sorted(drivers, key=lambda e: inf_name(e['name'])):
        shown = inf_name(e['name'])
        renamed = f'  (UI name {e["name"]})' if shown != e['name'] else ''
        marker = '' if shown in wanted else '   (not referenced)'
        print(f'    {shown:24s} {len(e["payload"]):7d} bytes  type {e["type"]:#04x}{marker}{renamed}')

    found = [(d, i) for d, i in modules if i in by_inf and by_inf[i]['payload']]
    missing = [(d, i) for d, i in modules if i not in by_inf]
    bodiless = [(d, i) for d, i in modules if i in by_inf and not by_inf[i]['payload']]
    print(f'\n  referenced by {args.dxe_inc.name}: {len(found)} recovered, '
          f'{len(missing)} not in the image, {len(bodiless)} without a module body')
    for d, i in missing:
        print(f'    missing {d}/{i}.inf')
    for d, i in bodiless:
        print(f'    no body  {d}/{i}.inf')
    if (missing or bodiless) and args.require:
        fail(f'{len(missing) + len(bodiless)} referenced module(s) could not be recovered')

    if args.out is not None:
        args.out.mkdir(parents=True, exist_ok=True)
        for _, name in found:
            (args.out / f'{name}.efi').write_bytes(by_inf[name]['payload'])
        print(f'  wrote {len(found)} modules to {args.out}')

    if args.layout is not None:
        root = args.layout / 'Binaries/nx769j/QcomPkg/Drivers'
        for directory, name in found:
            write_module(root, directory, name, by_inf[name])
        print(f'  laid out {len(found)} modules under {root}')

        raw_root = args.layout / 'Binaries/nx769j/RawFiles'
        by_name = {e['name']: e for e in entries if e['name']}
        wanted = referenced_raw_files(args.dxe_inc)
        missing, written = [], 0
        for name in wanted:
            entry = by_name.get(name)
            if entry is None or not entry['payload']:
                missing.append(name)
                continue
            raw_root.mkdir(parents=True, exist_ok=True)
            (raw_root / name).write_bytes(entry['payload'])
            written += 1
        print(f'  laid out {written} raw files under {raw_root}')
        for name in missing:
            print(f'    missing {name}')
        if missing and args.require:
            fail(f'{len(missing)} referenced raw file(s) are not in this image')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
