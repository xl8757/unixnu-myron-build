#!/usr/bin/env python3
"""Read the UniXNU prelink inputs out of an iOS restore image.

Fills the fields an OS profile cannot know without the IPSW:

  ipsw.chip_id / ipsw.board_id        BuildManifest BuildIdentities
  ipsw.sha256                         member file hashes
  kernel.prelink.kernelcache_sha256   the decompressed arm64 kernelcache
  kernel.prelink.early_security_init  not emitted: needs a measured __TEXT_EXEC address
  kernel.prelink.roots                kext bundle identifiers in the kernelcache

The kernelcache pipeline mirrors scripts/unixnu/ipsw.py exactly: the IMG4
payload, LZFSE decode, then the single 0x0100000c fat slice. Hashing the whole
fat image gives a different digest and would fail the board pin check.

Read-only. Prints to stdout and writes nothing to the image.

Usage:
  python3 scripts/extract-ipsw-pins.py IMAGE.ipsw [--device-class d22ap]

Requires pyliblzfse (`pip install pyliblzfse`) for the kernelcache.
"""
from __future__ import annotations

import argparse
import hashlib
import plistlib
import re
import struct
import sys
import zipfile
from pathlib import Path

CPU_TYPE_ARM64 = 0x0100000c
MH_MAGIC_64 = b'\xcf\xfa\xed\xfe'
FAT_MAGIC = b'\xca\xfe\xba\xbe'


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def der(data: bytes, at: int) -> tuple[int, int, int]:
    """(tag, content offset, content length) of one DER element."""
    tag = data[at]
    at += 1
    length = data[at]
    at += 1
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, length


def im4p_payload(data: bytes) -> bytes:
    """The payload of an IM4P (DER: IM4P, type, version, data, ...)."""
    tag, start, length = der(data, 0)
    if tag != 0x30:
        raise SystemExit('not an IM4P')
    items, at = [], start
    while at < start + length:
        _, body, size = der(data, at)
        items.append(data[body:body + size])
        at = body + size
    if len(items) < 4 or items[0] != b'IM4P':
        raise SystemExit('not an IM4P')
    return items[3]


def arm64_slice(fat: bytes) -> bytes:
    if fat[:4] != FAT_MAGIC:
        if fat[:4] == MH_MAGIC_64:
            return fat
        raise SystemExit(f'kernelcache is neither fat nor a 64-bit Mach-O: {fat[:4].hex()}')
    count = struct.unpack('>I', fat[4:8])[0]
    slices = [struct.unpack('>5I', fat[8 + 20 * i:28 + 20 * i]) for i in range(count)]
    arm64 = [s for s in slices if s[0] == CPU_TYPE_ARM64]
    if len(arm64) != 1:
        raise SystemExit(f'expected exactly one arm64 slice, found {len(arm64)}')
    _, _, offset, size, _ = arm64[0]
    slice_ = fat[offset:offset + size]
    if slice_[:4] != MH_MAGIC_64:
        raise SystemExit('the arm64 slice is not a 64-bit Mach-O')
    return slice_


def prelink_info(macho: bytes) -> list[dict]:
    """Kext entries of the __PRELINK_INFO.__info dictionary.

    The section is a plist <dict> fragment that shares values through ID and
    IDREF, which plistlib cannot resolve, so the references are resolved here.
    """
    magic, cputype, _sub, _ft, ncmds = struct.unpack_from('<5I', macho, 0)
    if magic != 0xfeedfacf:
        raise SystemExit(f'not a 64-bit Mach-O header: 0x{magic:x}')
    off, section = 32, None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<2I', macho, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', macho, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                name = macho[so:so + 16].rstrip(b'\0').decode()
                size = struct.unpack_from('<Q', macho, so + 40)[0]
                where = struct.unpack_from('<I', macho, so + 48)[0]
                if name == '__info':
                    section = macho[where:where + size]
                so += 80
        off += cmdsize
    if section is None:
        raise SystemExit('no __PRELINK_INFO.__info section')

    text = section[:section.rfind(b'</dict>') + 7].decode('utf-8', 'replace')
    ids = {}
    for m in re.finditer(r'<(\w+)([^>]*?)\sID="(\d+)"([^>]*)>(.*?)</\1>', text, re.S):
        ids[m.group(3)] = m.group(5)
    for m in re.finditer(r'<(\w+)([^>]*?)\sID="(\d+)"([^>]*?)/>', text):
        ids.setdefault(m.group(3), '')

    def value(raw: str) -> str:
        ref = re.match(r'^<\w+\s+IDREF="(\d+)"\s*/>$', raw)
        if ref:
            return ids.get(ref.group(1), '')
        own = re.match(r'^<\w+[^>]*>(.*?)</\w+>$', raw, re.S)
        return own.group(1) if own else raw

    array = re.search(r'<key>_PrelinkInfoDictionary</key><array>(.*)</array>', text, re.S)
    body = array.group(1)
    blocks, depth, start = [], 0, None
    for match in re.finditer(r'<dict>|</dict>', body):
        if match.group() == '<dict>':
            if depth == 0:
                start = match.end()
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                blocks.append(body[start:match.start()])

    entries = []
    for block in blocks:
        fields = {}
        for m in re.finditer(r'<key>(.*?)</key>(<[^>]+?(?:/>|>.*?</\w+>))', block, re.S):
            fields[m.group(1)] = value(m.group(2))
        entries.append(fields)
    return entries


def adt_node(buf: bytes, off: int):
    """One Apple device-tree node: u32 property_count, u32 child_count, then
    properties (name[32], u32 length, value, pad to 4) and children.

    The length field carries a flag in the high bit; the real length is the low
    31 bits.
    """
    pcount, ccount = struct.unpack_from('<2I', buf, off)
    off += 8
    props = {}
    for _ in range(pcount):
        name = buf[off:off + 32].split(b'\0')[0].decode('utf-8', 'replace')
        off += 32
        length, = struct.unpack_from('<I', buf, off)
        off += 4
        length &= 0x7FFFFFFF
        props[name] = buf[off:off + length]
        off += length
        off += (-length) % 4
    children = []
    for _ in range(ccount):
        off, child = adt_node(buf, off)
        children.append(child)
    return off, {'name': props.get('name', b'').rstrip(b'\0').decode('utf-8', 'replace'),
                 'props': props, 'children': children}


def show_property(key: str, value: bytes) -> str:
    if key == 'product-id':
        return f'"{value.hex()}"   # {len(value)} bytes'
    if len(value) == 4:
        return str(struct.unpack('<I', value)[0])
    text = value.rstrip(b'\0').decode('utf-8', 'replace')
    return f'"{text}"'


def device_tree_props(z: zipfile.ZipFile, manifest: dict, device_class: str) -> None:
    """The /product, /buttons and /defaults values of the real board tree."""
    member = f'Firmware/all_flash/DeviceTree.{device_class}.im4p'
    try:
        payload = im4p_payload(z.read(member))
    except KeyError:
        print(f'\n# no {member} in this image')
        return
    import liblzfse
    try:
        blob = liblzfse.decompress(payload)
    except Exception:
        blob = payload
    _, root = adt_node(blob, 0)
    nodes = {c['name']: c for c in root['children']}
    for name in ('product', 'buttons', 'defaults'):
        node = nodes.get(name)
        if not node:
            continue
        print(f'\n# /{name} of {member}')
        for key, value in node['props'].items():
            if key in ('name', 'AAPL,phandle') or key.startswith('function-button'):
                continue
            print(f'#   {key} = {show_property(key, value)}')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('ipsw', type=Path)
    parser.add_argument('--device-class', default='d22ap')
    args = parser.parse_args()

    z = zipfile.ZipFile(args.ipsw)
    manifest = plistlib.loads(z.read('BuildManifest.plist'))
    print(f'ProductVersion      : {manifest.get("ProductVersion")}')
    print(f'ProductBuildVersion : {manifest.get("ProductBuildVersion")}')
    print(f'SupportedProductTypes: {manifest.get("SupportedProductTypes")}')

    identity = [b for b in manifest['BuildIdentities']
                if b.get('Info', {}).get('DeviceClass') == args.device_class]
    if not identity:
        available = sorted({b.get('Info', {}).get('DeviceClass') for b in manifest['BuildIdentities']})
        raise SystemExit(f'no build identity for {args.device_class}; available: {available}')
    identity = identity[0]
    chip = int(identity['ApChipID'], 16) if isinstance(identity['ApChipID'], str) else identity['ApChipID']
    board = int(identity['ApBoardID'], 16) if isinstance(identity['ApBoardID'], str) else identity['ApBoardID']
    print(f'\n[ ipsw ]  device_class = {args.device_class}')
    print(f'chip_id = {chip}   # 0x{chip:x}')
    print(f'board_id = {board}   # 0x{board:x}')

    members = {name: m['Info']['Path'] for name, m in identity['Manifest'].items()
               if m.get('Info', {}).get('Path')}
    print('\n[ ipsw.sha256 ]  member file name = sha256')
    for component in ('OS', 'Cryptex1,SystemOS'):
        member = members.get(component)
        if not member:
            print(f'# {component}: no member in this build identity')
            continue
        digest = hashlib.sha256()
        with z.open(member) as handle:
            while chunk := handle.read(1 << 22):
                digest.update(chunk)
        print(f'"{Path(member).name}" = "{digest.hexdigest()}"   # {component}')

    print('\n[ kernel.prelink ]')
    payload = im4p_payload(z.read(members['KernelCache']))
    if payload[:4] != b'bvx2' and payload[:4] != b'bvxn':
        print(f'# note: kernelcache payload magic is {payload[:4]!r}, expected LZFSE')
    import liblzfse
    macho = arm64_slice(liblzfse.decompress(payload))
    print(f'kernelcache_sha256 = "{sha256_bytes(macho)}"   # decompressed arm64 slice')

    entries = prelink_info(macho)
    by_id = {}
    for entry in entries:
        by_id.setdefault(entry.get('CFBundleIdentifier'), entry)
    amfi = by_id.get('com.apple.driver.AppleMobileFileIntegrity')
    if amfi:
        # Not the value itself: early_security_init wants an unslid void(void)
        # address inside this module's __TEXT_EXEC, which is a reversing task
        # rather than a field lookup. _PrelinkExecutableLoadAddr is the __TEXT
        # kmod stub and fails emit_prelink's "outside original module code"
        # check. Print the range it has to fall in instead of a plausible wrong
        # answer.
        load = amfi.get('_PrelinkExecutableLoadAddr')
        print(f'# early_security_init: needs an unslid void(void) address inside '
              f'AppleMobileFileIntegrity __TEXT_EXEC')
        print(f'#   (its _PrelinkExecutableLoadAddr {load} is the __TEXT kmod stub, not a function; '
              f'do not use it here)')
    else:
        print('# com.apple.driver.AppleMobileFileIntegrity is not in this kernelcache')

    print(f'\n# {len(by_id)} kext bundle identifiers in __PRELINK_INFO')
    for name in sorted(k for k in by_id if k):
        entry = by_id[name]
        print(f'#   {entry.get("_PrelinkExecutableLoadAddr", "-"):>18}  {name}')

    device_tree_props(z, manifest, args.device_class)
    return 0


if __name__ == '__main__':
    sys.exit(main())
