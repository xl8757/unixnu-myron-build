#!/usr/bin/env python3
"""Grow the APFS container of a packed Data seed for a RAM-only Data volume (no persistent store).

  seed-data-ram-size.py --mib N BASE.dbr OUTPUT.dbr        (N up to 2048)

myron runs its Data volume from RAM (no sheng-data-persist). NativeRamBlock keeps a disk of up to 2 GiB as a sparse
page table plus the pages that hold data, so memory follows what is written; a larger disk is served by the persistent
store on UFS alone (NativeRamBlock.cpp). UniXNU's seed-data-grow.py makes those larger disks (3 to 1024 GiB). This makes
the small ones, the same way: the container is grown on this Mac (a sparse raw image attached without mounting,
`diskutil apfs resizeContainer`) and packed again, keeping only the nonzero pages. The volume UUID is unchanged.

The 32 MiB seed of prepare-owner-data-fixture.py fills up during the first boot (hardware 2026-10-22: residentKiB=32696 of
32768); lsd then fails to save its database and crashes, and SpringBoard waits for lsd.
"""
import argparse
import json
import plistlib
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk

MIB = 1024**2
RAM_LIMIT_MIB = 2048                     # NativeRamBlock: above 0x80000000 bytes only the persistent store serves the disk


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f'{" ".join(args)} failed ({result.returncode}):\n{result.stdout}{result.stderr}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mib', type=int, required=True)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        sys.exit('Refuse to overwrite ' + str(args.output))
    if not 64 <= args.mib <= RAM_LIMIT_MIB:
        sys.exit(f'{args.mib} MiB is outside 64..{RAM_LIMIT_MIB}')
    target = args.mib * MIB
    logical, extents = packed_ramdisk.regions(args.base.read_bytes())
    if target <= logical:
        sys.exit(f'The seed already holds {logical // MIB} MiB; growing needs a larger size')
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='data-ram-size-', dir=str(ROOT / 'build')) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:
            handle.truncate(target)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        attached = run('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse', str(raw))
        store = re.match(r'(/dev/disk\d+)', attached).group(1).split('/')[-1]
        try:
            # The seed is a bare APFS container (no partition map): macOS assembles it a moment after the attach.
            container, stores = None, []
            for _ in range(30):
                listing = plistlib.loads(subprocess.check_output(['diskutil', 'apfs', 'list', '-plist']))
                stores = [(s['DeviceIdentifier'], entry['ContainerReference'])
                          for entry in listing['Containers'] for s in entry.get('PhysicalStores', [])]
                container = next((c for d, c in stores if d == store or re.fullmatch(store + r's\d+', d)), None)
                if container:
                    break
                time.sleep(1)
            if not container:
                # resizeContainer also takes the physical store itself
                print(f'No APFS container listed for {store} (physical stores: {stores}); resizing through the store',
                      file=sys.stderr)
                container = store
            run('diskutil', 'apfs', 'resizeContainer', container, '0')
        finally:
            subprocess.run(['hdiutil', 'detach', store, '-force'], capture_output=True)
        header = raw.open('rb').read(4096)
        if header[32:36] != b'NXSB':
            sys.exit('The grown image does not start with an APFS container')
        block_size = int.from_bytes(header[36:40], 'little')
        blocks = int.from_bytes(header[40:48], 'little')
        if block_size != 4096 or blocks * block_size > target or blocks * block_size < target - 64 * MIB:
            sys.exit(f'The container is {blocks * block_size} bytes, not the {target} asked for')
        metadata = packed_ramdisk.pack(raw, args.output)
    report = dict(base=str(args.base), mib=args.mib, container_bytes=blocks * block_size, **metadata)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
