#!/usr/bin/env python3
"""Inspect a packed Data seed read-only for the widget and wallpaper state.

  seed-data-inspect.py BASE.dbr

Mounts the packed APFS Data seed (DBRDISK1, as seed-data-launchd-policy.py) read-only on this Mac and prints:
  - the SpringBoard preferences (com.apple.springboard) and their keys, incl. the wallpaper keys;
  - the listing of the SpringBoard, PosterBoard and Widgets directories and the chronod containers;
  - the MobileGestalt cache (removed by seed-data-remove-mobilegestalt.py when the identity changed).
Nothing is modified and the seed is not repacked.
"""
import os
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# packed_ramdisk lives in the source tree (UniXNU/drivers), which is a sibling of this device repository.
for candidate in (ROOT / 'drivers/DarwinBoot/Test', ROOT.parent / 'UniXNU/drivers/DarwinBoot/Test'):
    if candidate.is_dir():
        sys.path.insert(0, str(candidate))
import packed_ramdisk

TREES = (
    'mobile/Library/SpringBoard',
    'mobile/Library/PosterBoard',
    'mobile/Library/Widgets',
    'mobile/Library/Preferences',
    'containers/Shared/SystemGroup',
    'containers/Data/System',
)
PREF = 'mobile/Library/Preferences/com.apple.springboard.plist'


def listing(root, limit=40):
    if not root.exists():
        return ['  (absent)']
    out = []
    count = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            path = Path(dirpath) / name
            rel = path.relative_to(root)
            size = path.stat().st_size if not path.is_symlink() else -1
            out.append('  %-60s %s' % (rel, size))
            count += 1
            if count >= limit:
                out.append('  ...')
                return out
    if not out:
        return ['  (empty)']
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    base = Path(sys.argv[1]).resolve()
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    with tempfile.TemporaryDirectory(prefix='data-seed-', dir=str(ROOT / 'build')) as td:
        raw = Path(td) / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset)
                f.write(data)
        mount = Path(td) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-readonly', '-owners', 'off',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            print('seed:', base.name, 'root:', 'ok' if (mount / 'mobile').is_dir() else 'MISSING /mobile')
            pref = mount / PREF
            print('\n==', PREF)
            if pref.is_file():
                try:
                    data = plistlib.loads(pref.read_bytes())
                    print('  keys:', ', '.join(sorted(data)))
                    for key in sorted(data):
                        if 'wallpaper' in key.lower() or 'poster' in key.lower() or 'widget' in key.lower():
                            print('  %s = %r' % (key, data[key]))
                except Exception as error:   # noqa: BLE001 - report, do not fail the inspection
                    print('  unreadable:', error)
            else:
                print('  (absent)')
            for tree in TREES:
                print('\n==', tree)
                for line in listing(mount / tree):
                    print(line)
            print('\n== MobileGestalt cache')
            cache = mount / ('containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/'
                             'Library/Caches/com.apple.MobileGestalt.plist')
            print('  present' if cache.exists() else '  (absent)')
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)


if __name__ == '__main__':
    main()
