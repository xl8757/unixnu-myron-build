# Package the image and flash it

How to turn this repository's build outputs into a bootable `boot.img` and start
it on the NX769J, following [UniXNU's boot procedure](https://unixnu.com/guide/boot).

Read this before touching the device. The first boot is started from RAM and
writes nothing; the step that does write is called out explicitly below.

## What has to exist first

| Input | State | Where it comes from |
| --- | --- | --- |
| XNU kernel | **built** | `./unixnu build kernel` produces `mach.development.bcm2837` |
| OS profile pins | **filled** | `os-profiles/ios-16.5-20F66/profile.toml`, extracted from the iOS 16.5 restore image |
| Board hardware values | **filled** | `device-inputs.md`, measured read-only on the device |
| `uefi-nx769j` firmware package | **built** | `./unixnu build firmware`, `SILICIUM_UEFI.fd` at exactly `0x300000` |
| Vendor UEFI DXE drivers (52) | **extracted** | `scripts/extract-uefi-drivers.py --layout`, hashes in `proprietary-files.txt` |
| Firmware DTB | **extracted** | `scripts/extract-firmware-dtb.py --vendor-boot … --tree 2` |
| `payload.adt.root_match_volume_uuid` | **filled** | `scripts/extract-root-volume-uuid.py` reads it out of the OS window |
| `payload.text_fixups` | **empty** | an honest empty list; add one only when a boot fails on one |
| `payload.adt.unique_chip_id` | **required on the command line** | the SoC serial, recorded in [`device-inputs.md`](../device-inputs.md#chip-id) |

Three inputs are passed at build time rather than stored in the configuration:
`--unique-chip-id` is per-device, `--data-ramdisk` is the Data seed a macOS
runner builds and a new one is formatted on every run, and the restore image is
not redistributable so `--ipsw` and `--trust-cache` take a local path.

## 1. Build the parts

From the UniXNU checkout, in order, checking each output before continuing:

```sh
./unixnu defconfig nx769j_defconfig -D ../UniXNU-NX769J
./unixnu fetch -D ../UniXNU-NX769J
./unixnu build kernel   -D ../UniXNU-NX769J
./unixnu build package  -D ../UniXNU-NX769J
./unixnu build firmware -D ../UniXNU-NX769J --uefi ../uefi-nx769j
./unixnu build userdata -D ../UniXNU-NX769J --ipsw <restore.ipsw>
./unixnu build payload  -D ../UniXNU-NX769J --uefi ../uefi-nx769j --ipsw <restore.ipsw> \
  --unique-chip-id 8abc5430 --trust-cache <096-04088-077.dmg.trustcache> \
  --data-ramdisk <nx769j-data.dbr>
./unixnu bootimg -D ../UniXNU-NX769J --uefi ../uefi-nx769j \
  --dtb ../uefi-nx769j/Resources/DTBs/nx769j.dtb \
  -o out/nx769j/ios-16.5-20F66/nx769j/boot/boot.img
```

The Data seed comes from `.github/workflows/data-seed.yml` on a macOS runner;
fetch it with `gh run download <run> -n nx769j-data-seed` and pass it as
`--data-ramdisk`. Without it there is no writable Data volume.

`build userdata` comes before `build payload` here because
`scripts/extract-root-volume-uuid.py` reads the root volume UUID out of the
userdata image it writes. If `board.toml` already carries the value the order
does not matter.

On Linux the kernel step uses the UniXNU open source Mach-O toolchain rather
than Xcode; `scripts/linux/setup-toolchain.py` installs it. Put its `bin`
directory on `PATH` for the image steps too: `build userdata` decodes LZFSE
chunks with the `lzfse` tool the same installer provides. `CCACHE` is free in
the XNU makefiles and `scripts/compile-records.py` uses it to record every
compile, which the component archive builders need.

Outputs land under `out/nx769j/ios-16.5-20F66/nx769j/`. This repository's
`package/` appears in the assembled tree as `kits/nx769j`.

| Output | Size | What it is |
| --- | --- | --- |
| `firmware` step's `SILICIUM_UEFI.fd` | 3145728 | the UEFI firmware volume, in the Mu-Silicium checkout |
| `userdata/userdata-root.img` | 8357150720 | the OS and SystemOS windows, ready for `fastboot flash userdata` |
| `payload/payload.img` | 218103808 | the FAT payload: menu config, kernel, ADT, trust cache, Data seed |
| `boot/boot.img` | 220344320 | the whole thing, what `fastboot boot` takes |

## 2. Pack the boot image

```sh
./unixnu bootimg -D ../UniXNU-NX769J --uefi ../uefi-nx769j \
  --dtb ../uefi-nx769j/Resources/DTBs/nx769j.dtb \
  -o out/nx769j/ios-16.5-20F66/nx769j/boot/boot.img
```

`unixnu bootimg` round-trips the image and fails if any part does not match its
input, so a successful run is itself the check. It writes `boot.json` beside the
image, recording the SHA-256 of the FD it packed and of the DTB it appended —
those two are what tie the image back to the firmware and to the device.

The image contains:

| Part | Contents |
| --- | --- |
| Kernel | gzip of BootShim followed by the UEFI firmware volume |
| Ramdisk | the UniXNU payload FAT volume: menu config, kernel, ADT, trust cache and Data seed |
| Appended data | the device tree the stock bootloader expects |

The stock Android bootloader loads the image, verifies the ARM64 header in
BootShim and jumps to the shim, which starts the firmware volume.

`board.toml` sets `header_version = 0`, `pagesize = 4096`, `base = 0x0`. The
value is 0 rather than the 4 that `BOARD_BOOTIMG_HEADER_VERSION` records,
because `unixnu bootimg`'s own verifier unpacks the header at the v0/v1/v2
offsets and a v4 header fails the round-trip check. If the NX769J ABL turns out
to reject a v0 image the answer is to extend that verifier, not to change the
value here.

## 3. Start it from RAM

```sh
fastboot devices
fastboot boot out/nx769j/ios-16.5-20F66/nx769j/boot/boot.img
```

`fastboot boot` transfers the image and starts it once. **It writes no
partition**, so it is repeatable, needs no backup, and a reset returns the
phone to its normal boot. This is the right first test.

It requires an **unlocked bootloader**. Unlocking is the one step that can wipe
`userdata`, and it is a one-time cost; if the device is already unlocked nothing
else is written.

Watch the boot in order:

| Stage | What to expect |
| --- | --- |
| UEFI | firmware starts, GOP framebuffer appears |
| UniXNU | version and configuration summary, then the menu |
| Payload | the FAT volume is read and the entry is selected |
| DarwinBoot | `DARWINBOOT:` lines, `boot_args` built, exit boot services |
| Kernel | `serial=3` kernel output |

Logs come from the serial console (`UNIXNU:` for the boot manager, `DARWINBOOT:`
for the loader, plain kernel output after handoff) and from
`out/nx769j/ios-16.5-20F66/nx769j/logs/*.log` on the host.

If it stalls before the menu, capture the last output, reset into the bootloader
and confirm `fastboot devices` still responds:

```sh
fastboot reboot bootloader
```

Then work through the failure isolation table in
[bring-up](https://unixnu.com/porting/bring-up).

## 4. Root storage, when you get that far

Stages 1 to 5 above need no root volume. Stage 6, mounting the OS window, does.
Two ways to provide it, and only one of them leaves Android usable:

**Keep Android: carve a `unixnu` partition.** Use the
[dual-system partition](https://unixnu.com/porting/dual-system-partition) tool.
It shrinks the F2FS `userdata` filesystem and takes the new partition from the
end, preserving `userdata`'s start, type, GUID and attributes and leaving
`metadata`, `super` and `boot` untouched.

```sh
python3 scripts/dual_sys/partition.py inspect
python3 scripts/dual_sys/partition.py plan  --size-gib 32 --helper out/dual_sys/f2fs-shrink --output out/dual_sys/preview
python3 scripts/dual_sys/partition.py apply --size-gib 32 --helper out/dual_sys/f2fs-shrink --output out/dual_sys/applied
python3 scripts/dual_sys/partition.py verify --plan out/dual_sys/applied/plan.json
adb reboot system          # the check: Android must still reach its lock screen
```

**Replace `userdata`.** This is the default UniXNU layout and it erases Android's
data partition:

```sh
fastboot flash userdata out/nx769j/ios-16.5-20F66/nx769j/userdata/userdata-root.img
fastboot reboot bootloader
```

Do not run that until `board.toml` `[userdata]` extents have been re-read from
the live GPT on the device in hand. They are recorded in `device-inputs.md` for
one unit and a different SKU or a repartitioned device has different bounds. A
wrong range overwrites an unrelated partition.

Before writing an image, confirm the flash command targets `unixnu` and not
Android's `userdata`.

## What this does not cover

Booting `boot.img` starts the kernel. Running the iOS userland additionally needs
the OS and SystemOS windows to exist with a root volume UUID that the kernel
matches. See `porting.toml` for what is still open on each step.
