#!/usr/bin/env python3
"""Remove the cached MobileGestalt answers from a packed Data seed.

  seed-data-remove-mobilegestalt.py BASE.dbr OUTPUT.dbr

MobileGestalt caches everything it answered per build and platform UUID, so after the ADT identity changes (ipad_identity
vs phone_identity in board.toml) a seed that carries the old cache can keep answering the previous device class and Home
button type. The cache lives on the Data volume:

  /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
  /private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist
  /private/var/mobile/Library/Preferences/com.apple.mobilegestalt.plist

The mounted Data seed maps /private/var at its root (mobile/, containers/, db/), as seed-data-skip-setup.py and
seed-data-launchd-policy.py rely on. BASE is expanded to a raw image, attached read-write, the cache files are removed and
the image is packed again; BASE is not modified and the volume UUID stays the same.
"""
import json
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk

CACHE_PATHS = (
    'containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist',
    'mobile/Library/Preferences/com.apple.MobileGestalt.plist',
    'mobile/Library/Preferences/com.apple.mobilegestalt.plist',
)


def main():
    positional = [arg for arg in sys.argv[1:] if not arg.startswith('-')]
    if len(positional) != 2:
        sys.exit(__doc__)
    base, output = Path(positional[0]).resolve(), Path(positional[1]).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    ROOT.joinpath('build').mkdir(exist_ok=True)
    removed, absent = [], []
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
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'on',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                sys.exit('The Data seed has no /mobile; wrong base image?')
            for rel in CACHE_PATHS:
                path = mount / rel
                if path.exists():
                    subprocess.run(['sudo', 'rm', '-f', str(path)], check=True)
                    removed.append(rel)
                    print('removed', rel)
                else:
                    absent.append(rel)
                    print('absent', rel)
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata,
                  removed=removed, absent=absent)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
