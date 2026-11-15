#!/usr/bin/env python3
"""Bake launchd's disabled-jobs policy into a copy of a packed Data seed.

  seed-data-launchd-policy.py BASE.dbr OUTPUT.dbr LABEL[,LABEL...]

sheng writes this policy from the kernel before launchd starts (kits/sheng/BootWatchdogPolicy.c, called by the boot
progress patch; sheng-disable-watchdogd=1, sheng-disable-jobs=...). myron has no boot progress layer, and its Data
volume is the RAM seed of the payload (no sheng-data-persist), so every boot starts from the seed: the same files are
put into the seed instead.
  /private/var/db/com.apple.xpc.launchd/disabled.plist    <label>: true for every LABEL, root:wheel 0644
  /private/var/db/com.apple.xpc.launchd/disabled.migrated empty marker for launchd's modern override loader
The directory is root:wheel 0755. com.apple.watchdogd must be disabled on myron: it traps (SIGTRAP) without Apple's
hardware watchdog, and launchd panics on its exit (hardware 2026-10-22).

BASE is expanded to a raw image, attached read-write with ownership on, edited, detached and packed again; the volume
UUID is unchanged and BASE is not modified. The ownership change uses sudo.
"""
import json
import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk

LABEL = re.compile(r'[A-Za-z0-9.\-]+')


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    labels = [label for label in sys.argv[3].split(',') if label]
    if not labels or not all(LABEL.fullmatch(label) for label in labels):
        sys.exit('labels: [A-Za-z0-9.-], separated by commas')
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    ROOT.joinpath('build').mkdir(exist_ok=True)
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
            directory = mount / 'db/com.apple.xpc.launchd'
            directory.mkdir(parents=True, exist_ok=True)
            policy, marker = directory / 'disabled.plist', directory / 'disabled.migrated'
            policy.write_bytes(plistlib.dumps({label: True for label in labels}, fmt=plistlib.FMT_XML))
            marker.write_bytes(b'')
            # Modes first, then the owner: once root owns them, only sudo may change them
            subprocess.run(['sudo', 'chmod', '0755', str(directory)], check=True)
            subprocess.run(['sudo', 'chmod', '0644', str(policy), str(marker)], check=True)
            subprocess.run(['sudo', 'chown', '0:0', str(directory), str(policy), str(marker)], check=True)
            for path, mode in ((directory, 0o755), (policy, 0o644), (marker, 0o644)):
                st = path.lstat()
                if st.st_uid or st.st_gid or (st.st_mode & 0o7777) != mode:
                    sys.exit(f'{path.relative_to(mount)}: {st.st_uid}:{st.st_gid} {oct(st.st_mode)}, expected root:wheel {oct(mode)}')
            print('disabled.plist:', ', '.join(labels))
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata, disabled=labels,
                  files=['db/com.apple.xpc.launchd/disabled.plist', 'db/com.apple.xpc.launchd/disabled.migrated'])
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
