#!/usr/bin/env python3
"""Give a copy of a packed Data seed the directory /private/var/ngpu (the Data volume's /ngpu, mode 0755), which the GPU addon needs.

  prepare-ngpu-data.py [--publish] BASE.dbr OUTPUT.dbr

The kernel's addon install (patch zzy-native-gpu-addon-mount) writes the addon's files below /private/var/ngpu and creates missing
directories below it, but not /private/var/ngpu itself: on a seed without it every file fails with ENOENT and none of the addon
mounts finds its source (boot-myron-24: "ADD 1.2 1.2 1.2 files 8 w0 =0 x8 e2"). sheng's seed has the directory from
add-gpu-plugin-data.py, which also copies the whole plugin into the seed; here only the directory is added and the plugin keeps
coming from the addon, so the payload barely grows and the seed does not change with each plugin build.

--publish also writes the render-server preferences add-gpu-plugin-data.py --publish writes (no deep or compressed framebuffers, no
compressed IOSurfaces, for mobile and root): backboardd reads them before any plugin code runs. The publish marker itself comes from
the addon (make-gpu-addon.py --publish). BASE is not modified and the volume UUID is unchanged. macOS only (hdiutil); nothing is
written to Git or to the OS inputs."""
import argparse
import json
import os
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--publish', action='store_true', help='also write the render-server preferences of the published mode')
parser.add_argument('base', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
base, output = args.base.resolve(), args.output.resolve()
if output.exists():
    raise SystemExit('Refuse to overwrite ' + str(output))
logical, extents = packed_ramdisk.regions(base.read_bytes())
(ROOT / 'build').mkdir(exist_ok=True)
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
        ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
         '-mountpoint', str(mount), '-plist', str(raw)]))
    device = attached['system-entities'][0]['dev-entry']
    try:
        if not (mount / 'mobile').is_dir():
            raise SystemExit('The Data seed has no /mobile; wrong base image?')
        ngpu = mount / 'ngpu'
        ngpu.mkdir(exist_ok=True)
        os.chmod(ngpu, 0o755)
        if args.publish:
            # As add-gpu-plugin-data.py --publish: the process's own domain and the global one, for its user (mobile) and for root.
            for home, name in [(h, n) for h in ('mobile', 'root') for n in ('backboardd.plist', '.GlobalPreferences.plist')]:
                directory = mount / home / 'Library/Preferences'
                directory.mkdir(parents=True, exist_ok=True)
                file = directory / name
                values = plistlib.loads(file.read_bytes()) if file.exists() else {}
                values.update({'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False,
                               'CAEnableIOSurfaceCompression': False})
                file.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
                os.chmod(file, 0o644)
        print('seed /ngpu:', oct(ngpu.stat().st_mode & 0o7777), 'entries', sorted(p.name for p in ngpu.iterdir()))
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)
    with raw.open('rb') as f:
        superblock = f.read(36)
    if superblock[32:36] != b'NXSB':
        raise SystemExit('The edited image is no APFS container any more')
    metadata = packed_ramdisk.pack(raw, output)
report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata, ngpu_directory=True, publish_preferences=args.publish)
output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
