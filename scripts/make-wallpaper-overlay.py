#!/usr/bin/env python3
"""Write the phone-side wallpaper lookup files for the iPadOS system image.

  make-wallpaper-overlay.py SYSTEM_ROOT OUTPUT_DIR

The image ships only the idiom-suffixed iPad files under /Library/Wallpaper: DefaultWallpapers~ipad.plist (the table that
maps a device to its default wallpaper), Wallpapers~ipad.plist and Collections/Collections~ipad.plist (the collection
index). With phone_identity WallpaperKit asks for the ~iphone names, finds none, and leaves the lock/home background black.

OUTPUT_DIR gets the same files under the phone names: collections/Collections~iphone.plist and
defaults/{DefaultWallpapers,Wallpapers}~iphone.plist. make-gpu-addon.py --wallpaper-collections/--wallpaper-defaults
carries them and the kernel mounts each group over the matching directory in /Library/Wallpaper (union, so the stock iPad
files stay visible)."""
import sys
from pathlib import Path

SYS = 'Library/Wallpaper'
PAIRS = {
    'collections/Collections~iphone.plist': SYS + '/Collections/Collections~ipad.plist',
    'defaults/DefaultWallpapers~iphone.plist': SYS + '/DefaultWallpapers~ipad.plist',
    'defaults/Wallpapers~iphone.plist': SYS + '/Wallpapers~ipad.plist',
}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    if out.exists():
        sys.exit('Refuse to overwrite ' + str(out))
    for dest, src in PAIRS.items():
        source = root / src
        if not source.is_file():
            sys.exit('Missing ' + str(source))
        target = out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        print(f'{source.stat().st_size:>8} {src} -> {dest}')


if __name__ == '__main__':
    main()
