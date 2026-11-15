#!/usr/bin/env python3
"""Measure the original IOBlockStorageDevice dispatch table out of a kernelcache.

Why this exists
---------------
`NativeUfs`, `NativeRamBlock` and `NativeVirtioBlock` subclass
`IOBlockStorageDevice`, which is not part of XNU: it ships inside the prelinked
`com.apple.iokit.IOStorageFamily` kext. Linking the class in as well registers it
twice and the boot panics with a duplicate OSMetaClass message, so the driver's
inherited slots are resolved to the *original* prelinked code instead, with
absolute symbols.

`build-storage-bindings.py` writes those bindings from
`scripts/prelink-storage-bindings.json`, which this script produces. The
addresses come from the prelink closure, which the kernelcache copies verbatim,
so they do not move between builds: measure once per kernelcache, commit the
result, and every later build binds identically.

Nothing here needs hardware. The live lldb snapshot that
`audit-storage-interface.py` cross-checks against is a qualification step, not a
source of addresses; everything below is read out of the image.

What it records
---------------
The vtable address and its 190 dispatch slots, the four symbols the driver
references that are not vtable entries (the class vtable, the metaclass, the
metaclass pointer, the base destructor and the protected constructor), and
which slots are the abstract destructor traps that must not be bound.

Usage
-----
    scripts/scan-prelink-storage.py --ipsw <restore.ipsw> [--output PATH]
    scripts/scan-prelink-storage.py --kernelcache <kernelcache>

The kernelcache is the IMG4 payload decoded and reduced to its arm64 slice, the
same pipeline `scripts/extract-ipsw-pins.py` uses.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import plistlib
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve()
DEVICE = HERE.parents[1]

CLASS = 'IOBlockStorageDevice'
MANGLED = str(len(CLASS)) + CLASS
VTABLE = '__ZTV' + MANGLED
SLOTS = 190
PURE_VIRTUAL = '___cxa_pure_virtual'
KEXT = 'com.apple.iokit.IOStorageFamily'
# brk #1, what an abstract class's complete and deleting destructor slots hold
# until a concrete subclass supplies them.
ABSTRACT_DESTRUCTOR_TRAP = 0xd4200020


def fail(message: str) -> None:
    raise SystemExit('scan-prelink-storage: ' + message)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f'cannot load {path}')
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def native_prelink(tree: Path):
    for candidate in (tree / 'kits/NativePrelink/native_prelink.py',
                      DEVICE / 'kits/NativePrelink/native_prelink.py'):
        if candidate.is_file():
            return load_module('native_prelink', candidate)
    fail('native_prelink.py not found; run "unixnu tree" first')


def kernelcache_bytes(args) -> tuple[bytes, str]:
    """The decompressed arm64 kernelcache, and what it came from."""
    if args.kernelcache:
        data = Path(args.kernelcache).read_bytes()
        return data, Path(args.kernelcache).name
    if not args.ipsw:
        fail('give --ipsw <restore.ipsw> or --kernelcache <file>')
    pins = load_module('pins', DEVICE / 'scripts/extract-ipsw-pins.py')
    with zipfile.ZipFile(args.ipsw) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        matches = [b for b in manifest['BuildIdentities']
                   if b.get('Info', {}).get('DeviceClass') == args.device_class]
        if not matches:
            available = sorted({b.get('Info', {}).get('DeviceClass')
                                for b in manifest['BuildIdentities']})
            fail(f'no build identity for {args.device_class}; available: {available}')
        members = {name: m['Info']['Path'] for name, m in matches[0]['Manifest'].items()
                   if m.get('Info', {}).get('Path')}
        if 'KernelCache' not in members:
            fail('this build identity has no KernelCache member')
        payload = pins.im4p_payload(archive.read(members['KernelCache']))
        compressed = payload[:4] in (b'bvx2', b'bvxn')
        if not compressed:
            fail(f'unexpected kernelcache payload magic {payload[:4]!r}; expected LZFSE')
        decompressed = lzfse_decode(payload)
        return pins.arm64_slice(decompressed), Path(members['KernelCache']).name


def lzfse_decode(payload: bytes) -> bytes:
    """Decode through pyliblzfse if it is installed, else through the lzfse tool."""
    try:
        import liblzfse
        return liblzfse.decompress(payload)
    except ImportError:
        pass
    import shutil
    tool = shutil.which('lzfse')
    if tool is None:
        fail('need pyliblzfse (pip install pyliblzfse) or an lzfse tool on PATH')
    import tempfile
    with tempfile.TemporaryDirectory(prefix='scan-prelink-storage-') as scratch:
        source, decoded = Path(scratch) / 'in.lzfse', Path(scratch) / 'out.bin'
        source.write_bytes(payload)
        subprocess.run([tool, '-decode', '-i', str(source), '-o', str(decoded)], check=True)
        return decoded.read_bytes()


def compile_environment(tree: Path) -> dict[str, str]:
    """The environment kernel.py gives the archive builders, so the shim resolves.

    The compile record names the CLT clang wrapper, which runs the cross clang
    that setup-toolchain.py builds and adds the Darwin target and the flag that
    keeps C++ static destructors in __mod_term_func. Without these the wrapper
    falls back to a default path and the reference class does not compile.
    Mirrors scripts/unixnu/kernel.py linux_env(); on a macOS host with Xcode the
    real toolchain is used and nothing here is needed.
    """
    import os
    import platform
    if platform.system() != 'Linux':
        return dict(os.environ)
    toolchain = Path(os.environ.get('UNIXNU_TOOLCHAIN', '/opt/unixnu-linux'))
    clt = toolchain / 'CommandLineTools/usr'
    xctoolchain = toolchain / 'XcodeDefault.xctoolchain/usr'
    fakeroot = tree / '.cache/fakeroot'
    env = dict(os.environ)
    env.update({
        'PATH': os.pathsep.join([str(clt / 'bin'), str(toolchain / 'bin'), os.environ.get('PATH', '')]),
        'LC_ALL': 'C',
        'CLT_PREFIX': str(clt),
        'XCTOOLCHAIN_PREFIX': str(xctoolchain),
        'XNU_TOOLCHAIN_BIN': f'{clt / "bin"} {xctoolchain / "bin"}',
        'UNIXNU_SDKROOT': str(fakeroot),
        'UNIXNU_SDK_VERSION': '17.0',
        'UNIXNU_PLATFORM_PATH': str(fakeroot / 'Platforms/iPhoneOS.platform'),
        'CROSS_CLANG': str(toolchain / 'llvm/bin/clang'),
        'DARWIN_TRIPLE': 'arm64-apple-ios17.0',
        'CROSS_EXTRA_FLAGS': '-mllvm -disable-atexit-based-global-dtor-lowering',
        'CCC_OVERRIDE_OPTIONS': '# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global',
    })
    return env


def compile_reference(tree: Path, out: Path) -> Path:
    """Compile IOBlockStorageDevice.cpp with the flags the kernel used for IOKit."""
    refer = tree / 'refer/IOStorageFamily'
    recipe_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not refer.is_dir():
        fail(f'missing {refer}; run "unixnu fetch IOStorageFamily"')
    if not recipe_path.is_file():
        fail(f'missing {recipe_path}; run "unixnu build kernel" once to record the compiles')
    source = refer / 'IOBlockStorageDevice.cpp'
    if not source.is_file():
        fail(f'missing {source}')
    record = json.loads(recipe_path.read_text().strip().rstrip(','))
    destination = out / (CLASS + '.reference.o')
    command, index = [], 0
    while index < len(record['arguments']):
        value = record['arguments'][index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(destination)]
            index += 1
        elif value == '-Xclang' and index + 1 < len(record['arguments']) \
                and record['arguments'][index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if '-o' not in command:
        command += ['-o', str(destination)]
    command.extend(['-Wno-error', '-I', str(tree / 'build/storage-abi/include'), '-I', str(refer)])
    (out / 'reference.compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], env=compile_environment(tree),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / 'reference.compile.log').write_text(result.stdout or '')
    if result.returncode:
        fail('the reference class did not compile:\n' + (result.stdout or '')[-3000:])
    return destination


def vtable_names(np, object_path: Path) -> list[str]:
    """The name each vtable slot dispatches to, in slot order."""
    obj = object_path.read_bytes()
    image = np.macho(obj)
    sycmd = next(at for command, at, size in image['commands'] if command == 2)
    symoff, count, stroff = struct.unpack_from('<4I', obj, sycmd + 8)[:3]
    symbols = []
    for index in range(count):
        name_at, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + index * 16)
        name = obj[stroff + name_at:obj.index(b'\0', stroff + name_at)].decode()
        symbols.append((name, value, kind))
    vtable = next((value for name, value, _ in symbols if name == VTABLE), None)
    if vtable is None:
        fail(f'{object_path} does not define {VTABLE}')
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if not section['va'] <= vtable < section['va'] + section['size']:
                continue
            reloff, relcount = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for i in range(relcount):
                address, word = struct.unpack_from('<II', obj, reloff + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xFFFFFF][0]
    names = [relocations.get(vtable + 16 + i * 8) for i in range(SLOTS)]
    if not all(names) or vtable + 16 + SLOTS * 8 in relocations:
        fail(f'the reference {CLASS} vtable is not {SLOTS} fully relocated slots')
    if not (names[0].endswith('D1Ev') and names[1].endswith('D0Ev')):
        fail('the reference vtable does not start with the complete/deleting destructors')
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ipsw', type=Path, help='iOS restore image to read the kernelcache from')
    parser.add_argument('--kernelcache', type=Path,
                        help='an already decompressed arm64 kernelcache, instead of --ipsw')
    parser.add_argument('--device-class', default='d22ap',
                        help='build identity to take the kernelcache from (default d22ap)')
    parser.add_argument('--tree', type=Path, default=None,
                        help='the assembled UniXNU build tree (default: found under the UniXNU checkout)')
    parser.add_argument('--output', type=Path, default=DEVICE / 'scripts/prelink-storage-bindings.json')
    parser.add_argument('--scratch', type=Path, default=None, help='where to put the reference object')
    args = parser.parse_args()

    tree = args.tree
    if tree is None:
        for candidate in sorted((DEVICE.parent / 'UniXNU' / 'out').glob('*/**/tree')):
            if (candidate / 'kits/NativePrelink/native_prelink.py').is_file():
                tree = candidate
                break
    if tree is None or not (tree / 'kits/NativePrelink/native_prelink.py').is_file():
        fail('cannot find the assembled build tree; pass --tree')

    np = native_prelink(tree)
    fat, origin = kernelcache_bytes(args)
    parent = np.macho(fat)
    if parent['kind'] != 2:
        fail('expected an MH_EXECUTE kernelcache')

    entries = {}
    for entry in load_module('pins', DEVICE / 'scripts/extract-ipsw-pins.py').prelink_info(fat):
        entries.setdefault(entry.get('CFBundleIdentifier'), entry)
    entry = entries.get(KEXT)
    if entry is None:
        fail(f'{KEXT} is not in this kernelcache')
    base = int(entry['_PrelinkExecutableLoadAddr'], 0)

    def read(address: int, length: int) -> bytes:
        offset = np.backed_offset(parent['segments'], address, length, len(fat))
        return fat[offset:offset + length]

    kext = np.macho(fat, np.backed_offset(parent['segments'], base, 64, len(fat)))
    if kext['kind'] != 11:
        fail(f'expected an embedded MH_KEXT_BUNDLE, got kind {kext["kind"]}')
    segments = {segment['name']: segment for segment in kext['segments'] if segment['size']}
    text, const = segments.get('__TEXT_EXEC'), segments.get('__DATA_CONST')
    if text is None or const is None:
        fail(f'{KEXT} has no __TEXT_EXEC/__DATA_CONST segment')

    scratch = args.scratch or (tree / 'build/prelink-storage')
    scratch.mkdir(parents=True, exist_ok=True)
    names = vtable_names(np, compile_reference(tree, scratch))

    pure_slots = [index for index, name in enumerate(names) if name == PURE_VIRTUAL]
    blob = read(const['va'], const['size'])

    # Slot names that the kernel itself exports pin each candidate table to this
    # class: a slot for an inherited method points at the kernel's own code, at
    # exactly the address the kernel exports for that name.
    export_at = {}
    for address, exported in np.exports(fat, parent).items():
        for name in exported:
            export_at.setdefault(name, address)
    checks = [index for index, name in enumerate(names) if name in export_at]
    if len(checks) < SLOTS // 4:
        fail(f'only {len(checks)} of the {SLOTS} slot names are kernel exports; '
             'this kernelcache cannot be scanned this way')

    # The dispatch table is an address point preceded by offset-to-top and
    # typeinfo, both zero because this build emits no RTTI for the class.
    found = []
    for at in range(0, len(blob) - (SLOTS + 2) * 8 + 1, 8):
        if blob[at:at + 16] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * SLOTS, blob, at + 16)
        if all(pointers[index] == export_at[names[index]] for index in checks):
            found.append((const['va'] + at, pointers))
    if len(found) != 1:
        fail(f'found {len(found)} dispatch tables for {CLASS} in {KEXT}; expected exactly 1')
    table, pointers = found[0]
    address_point = table + 16

    # The class is abstract: its complete and deleting destructor slots are
    # brk traps, and a concrete subclass supplies both.
    traps = [index for index in (0, 1)
             if struct.unpack('<I', read(pointers[index], 4))[0] == ABSTRACT_DESTRUCTOR_TRAP]
    if traps != [0, 1]:
        fail(f'slots 0 and 1 are not the abstract destructor traps (found {traps})')

    # The base destructor sits four bytes before the first trap and branches to
    # IOService's destructor.
    base_destructor = pointers[0] - 4

    # The protected constructor stores the address point into the new object and
    # forwards to IOService's constructor.
    constructors = []
    for address in range(text['va'], text['va'] + text['size'] - 32, 4):
        words = struct.unpack('<8I', read(address, 32))
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
                words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        immediate = ((words[3] >> 5 & 0x7ffff) << 2) | (words[3] >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
        if stored == address_point:
            constructors.append(address)
    if len(constructors) != 1:
        fail(f'found {len(constructors)} protected constructors storing the address point')
    protected_constructor = constructors[0]

    # The metaclass getter is the slot whose name says so. Index 7 is where it
    # lands in this class's declaration order, but a trivial getter elsewhere in
    # the table has the same three instructions, so the name decides.
    getters = [index for index, name in enumerate(names) if 'getMetaClass' in name]
    if len(getters) != 1:
        fail(f'the reference vtable has {len(getters)} metaclass getters, expected 1')
    function = pointers[getters[0]]
    adrp, add, ret = struct.unpack('<3I', read(function, 12))
    if not (adrp & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0):
        fail(f'slot {getters[0]} ({names[getters[0]]}) is not a metaclass getter')
    immediate = ((adrp >> 5 & 0x7ffff) << 2) | (adrp >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    metaclass = (function & ~4095) + immediate * 4096 + (add >> 10 & 4095)

    # Confirm the resolved address is where a static OSMetaClass instance would
    # live. Its fields are not checked against the image: gMetaClass is filled in
    # by OSMetaClass's constructor when the kext starts, so the file holds zeros
    # and the class size and class name are only observable in live memory. The
    # accessor the compiler emitted is the ground truth for the address.
    data = segments.get('__DATA')
    if data is None or not data['va'] <= metaclass < data['va'] + data['size']:
        where = f'{data["va"]:#x}..{data["va"] + data["size"]:#x}' if data else 'absent'
        fail(f'the metaclass getter resolves to {metaclass:#x}, outside __DATA ({where})')

    # The class-local OSMetaClass * global that the driver's metaclass reference
    # resolves to. Its initialiser is &gMetaClass, so unlike the instance it is
    # a relocation and is present in the image. Prefer a hit just before the
    # dispatch table, where the compiler lays these two out together.
    metaclass_pointer = None
    for offset in (8, 16, 24, 32):
        candidate = table - offset
        if const['va'] <= candidate < const['va'] + const['size'] and \
                struct.unpack('<Q', read(candidate, 8))[0] == metaclass:
            metaclass_pointer = candidate
            break
    if metaclass_pointer is None:
        for segment in kext['segments']:
            if not segment['size']:
                continue
            body = read(segment['va'], segment['size'])
            hit = next((segment['va'] + at for at in range(0, len(body) - 8, 8)
                        if struct.unpack_from('<Q', body, at)[0] == metaclass), None)
            if hit is not None:
                metaclass_pointer = hit
                break
    if metaclass_pointer is None:
        fail('cannot find the metaclass pointer')

    owned = [index for index, value in enumerate(pointers) if text['va'] <= value < text['va'] + text['size']]
    foreign = [index for index in range(SLOTS) if index not in owned]
    for index in foreign:
        value = pointers[index]
        if not any(s['va'] <= value < s['va'] + s['size'] for s in parent['segments']):
            fail(f'slot {index} points outside every mapped segment: {value:#x}')

    pin = {
        'kext': KEXT,
        'source': 'prelink closure of the original kernelcache',
        'measured_from': origin,
        'kernelcache_sha256': hashlib.sha256(fat).hexdigest(),
        'slot_count': SLOTS,
        'pure_virtual_slots': pure_slots,
        'abstract_destructor_slots': [0, 1],
        'kext_text': {'va': hex(text['va']), 'size': text['size'],
                      'sha256': hashlib.sha256(read(text['va'], text['size'])).hexdigest()},
        'kext_const': {'va': hex(const['va']), 'size': const['size'],
                       'sha256': hashlib.sha256(blob).hexdigest()},
        'vtable': hex(table),
        'vtable_address_point': hex(address_point),
        'metaclass': hex(metaclass),
        'metaclass_pointer': hex(metaclass_pointer),
        'protected_constructor': hex(protected_constructor),
        'base_destructor': hex(base_destructor),
        'kext_owned_slots': owned,
        'slots': [hex(value) for value in pointers],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(pin, indent=2) + '\n')
    print(f'measured {KEXT} dispatch table from {origin}')
    print(f'  vtable            {table:#x}  (address point {address_point:#x})')
    print(f'  metaclass         {metaclass:#x}  pointer {metaclass_pointer:#x}')
    print(f'  constructor       {protected_constructor:#x}  destructor {base_destructor:#x}')
    print(f'  slots             {SLOTS}  ({len(owned)} owned, {len(foreign)} shared)')
    print(f'  pure virtual      {len(pure_slots)} at {pure_slots}')
    print(f'  wrote             {args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
