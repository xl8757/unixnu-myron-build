# UniXNU-NX769J

UniXNU device repository for the **nubia RedMagic 9 series** — RedMagic 9 Pro,
9 Pro+, 9S Pro and 9S Pro+ — codename **NX769J**, on the Qualcomm **SM8650**
("pineapple", Snapdragon 8 Gen 3).

UniXNU boots iOS and macOS on non-Apple ARM64 hardware through UEFI. This
repository describes one board: its hardware values, component selection,
firmware and partition policy. Shared code lives in
[UniXNU](https://github.com/UniXNU/UniXNU) and in the component repositories.

| | |
| --- | --- |
| Board | nubia RedMagic 9 Pro / 9 Pro+ / 9S Pro / 9S Pro+ |
| Codename | `nx769j` (NX769J) |
| SoC | Qualcomm SM8650 "pineapple", Adreno 750 |
| Base board | Qualcomm Pineapple QRD (`qcom,pineapple-qrd`) |
| OS profile | `ios-16.5-20F66` — iOS 16.5 (build 20F66), Darwin 22.5.0, XNU 8796.101.5 |
| Identity | iPhone X (`iPhone10,3` / D22AP) forged by the ADT |
| Display | 1116x2480 at 480 dpi |
| Boot | UEFI, Android 14 ABL, boot header version 4, 4096-byte pages |
| Storage | UFS, A/B, userdata on f2fs |
| Touch | Goodix 9916R |
| Porting record | [`porting.toml`](porting.toml) |

## Status

The board description, component selection, device tree and kernel
configuration are complete and sourced. **Every image the port needs now
builds**: the XNU kernel, the UEFI firmware volume, the payload, the userdata
root image and the boot image. Hardware bring-up has not run, so there is no
device verification and nothing here has been observed booting.

| Milestone | State |
| --- | --- |
| Board, component selection, device tree, kernel config | complete |
| Kernel compiles and links on Linux | **done** |
| Prelink produces `kernel/kernel.macho` | **done** — experimental candidate, not runtime-qualified |
| `build package` produces the UniXNU UEFI drivers | **done** |
| UEFI firmware package | **built** — `SILICIUM_UEFI.fd`, exactly `0x300000` bytes; see [`docs/UEFI-FIRMWARE.md`](docs/UEFI-FIRMWARE.md) |
| Firmware image, payload, boot image | **built** — see [`docs/FLASHING.md`](docs/FLASHING.md) |
| Boot on hardware | not attempted |

`./unixnu build kernel` runs end to end against the iOS 16.5 restore image:
eleven archive builders, the link, and prelink. The kernel is an **experimental
candidate** — `emit_prelink.py` records `instruction_relocation_complete`,
`abi_qualified`, `loadable` and `runtime_qualified` as false in every plan it
writes, and those flags have not moved.

### The images

All four are reproducible from the inputs below. Sizes and hashes are from a
build on Linux with CLANGPDB.

| Image | Bytes | What it is |
| --- | --- | --- |
| `SILICIUM_UEFI.fd` | 3145728 | the UEFI firmware volume, in the `uefi-nx769j` checkout |
| `payload/payload.img` | 218103808 | the FAT payload: menu config, kernel, ADT, trust cache, Data seed |
| `userdata/userdata-root.img` | 8357150720 | the OS and SystemOS windows |
| `boot/boot.img` | 220344320 | BootShim + FD + payload + firmware DTB, for `fastboot boot` |

### What the build needs that no repository can hold

Four inputs come from the device's flash package or from the restore image, and
neither is redistributable. Each has an extraction script and a recorded hash.

| Input | Script | Recorded in |
| --- | --- | --- |
| 52 vendor UEFI DXE drivers | `scripts/extract-uefi-drivers.py --image uefi.img --layout ../uefi-nx769j` | `proprietary-files.txt` |
| Firmware DTB | `scripts/extract-firmware-dtb.py --vendor-boot vendor_boot.img --tree 2` | `proprietary-files.txt` |
| Unique chip ID | read from the device, then passed as `--unique-chip-id` | [`device-inputs.md`](device-inputs.md#chip-id) |
| OS trust cache | one of the two the profile pins, passed as `--trust-cache` | `os-profiles/ios-16.5-20F66/profile.toml` |

Two more are read out of what the build already produces: the root volume UUID
comes from `userdata-root.img` via
[`scripts/extract-root-volume-uuid.py`](device-inputs.md#root-volume), and the
root windows' verified-root policy bundle is still to be signed — see
`porting.toml`.

Every area states its own scope and next step in [`porting.toml`](porting.toml).

Board values that no public NX769J source provides were read from a physical
device over read-only adb and recorded in
[`device-inputs.md`](device-inputs.md), with the command behind each value.

| Value | Status | Recorded in |
| --- | --- | --- |
| `userdata` first/last LBA, block size | **measured** 3302184 / 124002298 / 4096 | `board.toml` `[userdata]` |
| Splash reservation | **measured** `0xe3940000` / `0x2b00000`, `cont_splash_region` | `dts/nx769j.dts`, `board.toml` `[[bootimg.dtb_node]]` |
| Panel geometry | **measured** 1116x2480 | `porting.toml` display area |
| DRAM base and SKU | **measured** base `0x80000000`, 16 GiB unit | `dts/nx769j.dts` `memory` |
| OS window APFS volume UUID | **read** `97A3EB1A-5218-4936-BC85-9574EC2C8CC9` | `board.toml` `payload.adt.root_match_volume_uuid` |

Re-run the commands in [`device-inputs.md`](device-inputs.md) before writing
anything to a device: the recorded GPT extents describe one unit at one point
in time, and a different SKU or a repartitioned device has different bounds.
[`scripts/collect-device-inputs.sh`](scripts/collect-device-inputs.sh) is the
same procedure in runnable form.

## Target: iOS 16.5

This port runs **iOS 16.5 (build 20F66)**, not iPadOS. The ADT forges an
**iPhone X** identity (`iPhone10,3` / `D22AP`).

XNU 8796 is the iOS 16 kernel. The profile pins
`xnu-8796.101.5` (`aca3beaa3dfbd42498b42c5e5ce20a938e6554e5`), the earliest
open-source drop of the 8796 train and the closest to Darwin 22.5.0. The
community iOS 16 ports — `dipa-studio/ios-sweet` and the `ios-topaz` family —
build the same train.

### Where the profile lives

`board.os_profile` names a directory under `UniXNU/profiles/os/`. UniXNU ships
only `ipados-17.7.11-21H461`, so this port keeps its own profile at
[`os-profiles/ios-16.5-20F66/`](os-profiles/ios-16.5-20F66/profile.toml). The
Build workflow installs it into the UniXNU checkout before resolving. It should
be contributed to UniXNU once it is complete.

`[ipsw]` and `[kernel.prelink]` are inherited from that profile rather than
repeated in `board.toml`, so the values below are edited in one file.

### What is filled and what is not

The prelink ABI is version bound. None of these values can be copied from the
iPadOS profile — the 16.5 kernelcache puts `AppleMobileFileIntegrity` at
`0xfffffff004e2b640` where 17.7.11 has `0xfffffff005ab4ae0`, and it does not
contain `com.apple.iokit.IOHIDEventDriver` at all.

`scripts/extract-ipsw-pins.py` reads every value below out of the restore image
and reproduces them.

| Field | Value | Status |
| --- | --- | --- |
| `ipsw.chip_id`, `ipsw.board_id` | `32789` (0x8015), `6` | **filled** from `BuildManifest` |
| `[ipsw.sha256]` OS | `64354ba3…` for `096-04088-077.dmg` | **filled** |
| `[ipsw.sha256]` SystemOS | `2279bf44…` for `096-04666-078.dmg` | **filled** |
| `kernel.prelink.kernelcache_sha256` | `e6fa7559…` | **filled** — the decompressed arm64 slice |
| `kernel.prelink.early_security_init` | `…=0xfffffff004e2b640` | **filled** from `__PRELINK_INFO` |
| `kernel.prelink.roots` | 30 bundle identifiers | **filled**, each verified present |
| `payload.trust_cache_sha256` | the two IPSW trust caches | **filled** |
| `adt.product.bytes.product-id` | `fd079b298144a7deb4d7c87fef3eb474d8f0e7eb` | **filled** from the real D22AP device tree |
| `ipsw.url` | member name | restore images are not redistributable; pass `--ipsw` or `UNIXNU_IPSW` |
| `payload.text_fixups` | — | **open** — from the dyld shared cache inside the OS image |

The `/product` values come from `Firmware/all_flash/DeviceTree.d22ap.im4p` in the
restore image, which is the real board device tree — the same source the
community iOS 16 ports use. Three of them correct what would otherwise be
copied from the iPadOS profile:

| Property | This profile | Would have been |
| --- | --- | --- |
| `artwork-device-subtype` | `2436` (iPhone X panel height) | `2732` (the iPad's) |
| `compatible-device-fallback` | `iPhone9,2` | `iPhone10,6` |
| `kern.maxvnodes` | `10000` | `66560` |

`payload.text_fixups` is the one field still open. It lives in the dyld shared
cache inside `096-04088-077.dmg`, not in the IPSW container, and it keeps a
placeholder so the payload step fails loudly rather than silently dropping the
fixups. The fixups only matter once
`patches/xnu/native-text-fixup.patch` is rebased onto `xnu-8796`.

### The patch rebase

**Done and verified.** All 64 patches apply strictly, in order, onto
`xnu-8796.101.5`. The series is in
[`patches/xnu/`](patches/xnu/series) and `board.toml` selects it on its own,
because the shared `arch:`, `soc:` and `os:` series still target
`xnu-10063.141.1`.

| Result | Count |
| --- | --- |
| carried over unchanged | 54 |
| hunk re-anchored | 2 |
| hunk dropped (include line absent in 8796) | 2 |
| patch dropped entirely | 2 |
| added for this train | 4 |

The four additions are the arm64 kpc backend xnu-8796 does not ship, both
IORPC definitions the interface generator expects, and the build fixes for
upstream clang.

The two dropped patches, `pgz-aligned-validation` and
`published-freezer-condition`, patch code that xnu-8796 does not have and that
no other patch in the series creates. `zzx-pmap-sync-64bit-pa` was split: its
`osfmk/arm64/sptm/` half is dropped because that directory does not exist in
8796, and its `osfmk/arm/pmap/` half is kept.

The two prelink patches apply to 8796 unmodified and are renamed
`xnu-8796.101.5-*`, since `XnuPrelink` ships them only under `xnu-10063` names.

One thing worth knowing before touching this series: a patch can depend on an
earlier one creating its target code, so testing one in isolation against a
clean tree reports "target absent" for patches that are in fact needed. The
series was regenerated by replaying it in order. Details, the reproduce
commands and the upstream targets are in
[`os-profiles/ios-16.5-20F66/patch-rebase.md`](os-profiles/ios-16.5-20F66/patch-rebase.md).

## Prerequisites

Host tools from [Requirements](https://unixnu.com/guide/requirements), plus a
checkout of [UniXNU](https://github.com/UniXNU/UniXNU) and of
[UniXNU-sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common) as the
SoC source. There is no `UniXNU-sm8650-common`; see
[`porting.toml`](porting.toml) for why the SM8550 repository is used and what it
does not cover.

## Build

From the UniXNU checkout:

```sh
./unixnu defconfig nx769j_defconfig -D ../UniXNU-NX769J
./unixnu fetch -D ../UniXNU-NX769J
./unixnu status --json -D ../UniXNU-NX769J
./unixnu tree -D ../UniXNU-NX769J
python3 scripts/porting-record.py check --device ../UniXNU-NX769J
```

Then follow [Building](https://unixnu.com/building) in order: kernel, package,
firmware, payload, userdata when required, then boot image. Output lands in
`out/nx769j/ipados-17.7.11-21H461/<config>/` and this repository's `package/`
appears there as `kits/nx769j`.

Copy `local.toml.example` to the ignored `local.toml` for machine-specific
source overrides.

## Installing alongside Android

Writing the OS images into the `[userdata]` windows destroys Android's `/data`,
which is its user data partition. Android keeps its system images but no longer
boots to a usable state. To keep the stock system bootable, install into a
separate `unixnu` partition instead, using the
[dual-system partition](https://unixnu.com/porting/dual-system-partition) tool.

That tool shrinks the F2FS `userdata` filesystem and carves `unixnu` from the
end of the partition. It preserves `userdata`'s start, type, GUID and
attributes and leaves `metadata`, `super`, `boot` and every other partition
untouched. The whole procedure runs from userspace: host Python over ADB, with
the recovery shell doing the writes.

NX769J satisfies the layout requirements that can be checked offline:

| Requirement | NX769J |
| --- | --- |
| Filesystem F2FS, 4096-byte blocks | Logical sector size is 4096 (measured). `recovery.fstab` mounts `/data` as f2fs; confirm the live filesystem and F2FS section geometry with `partition.py inspect` |
| Compatible TWRP with root ADB | Several NX769J TWRP device trees exist |
| Kernel supports `F2FS_IOC_RESIZE_FS` | Stock kernel 6.1; validate the recovery kernel on the device |
| `userdata` is the last occupied partition, free GPT entry present | **Satisfied** — `userdata` is `sda12`, ends on the last usable sector, and 12 of 32 GPT entries are used (measured) |
| Encryption is direct userdata or one mapping backed only by userdata | **Unverified** — NX769J has a separate f2fs `metadata` partition; a stacked mapping makes the tool refuse |

### Verify the stock system still boots

Do this before writing any image. Back up `userdata` and `metadata` offline
first; the tool provides no automatic retry or rollback, and restoring the GPT
does not undo the F2FS relocation.

```sh
# 1. Back up, then inspect. Read-only; writes nothing to the device.
python3 scripts/dual_sys/partition.py inspect

# 2. Preview the split. Writes only host files and recovery /tmp.
python3 scripts/dual_sys/partition.py plan \
  --size-gib 32 --helper out/dual_sys/f2fs-shrink --output out/dual_sys/preview

# 3. Apply, then restart recovery and verify the new layout.
python3 scripts/dual_sys/partition.py apply \
  --size-gib 32 --helper out/dual_sys/f2fs-shrink --output out/dual_sys/applied
python3 scripts/dual_sys/partition.py verify --plan out/dual_sys/applied/plan.json

# 4. Prove the stock system still boots.
adb reboot system
```

Step 4 is the verification that matters: if Android reaches its lock screen,
the system partitions were never touched. If it does not, stop and have the
device maintainer assess the captured recovery state before anything else.

`unixnu` stays unformatted after the split and may still hold old `userdata`
bytes. Before writing an image, confirm the flash command targets `unixnu` and
not Android's `userdata`.

## Component selection

`board.toml` lists the complete selection, because arrays replace the inherited
ARM64, SoC and OS arrays.

Selected: `ArmDTPlatform`, `ArmCpu`, `SoftwareAES`, `XnuPrelink`, `DarwinBoot`,
`ShengUfs`, `VirtioMmioBlock`, `VerifiedRoot`, `GopFramebuffer`.

Not selected, and why:

| Component | Reason |
| --- | --- |
| `Adreno740`, `AdrenoMetal`, `Sm8550Platform`, `AdrenoPM4`, `AdrenoFirmware`, `ArmSmmu4K` | Targets Adreno 740 on SM8550. This board has Adreno 750 (`qcom,adreno-43051401`, `qcom,adreno-gmu-750.1`) with no qualified driver |
| `ShengHidInput`, `ShengNovatekTouch`, `QcomGeniSpi` | Written for the Novatek NT36532E. This board has a Goodix 9916R |
| `Ath12kWifi` | Declares `os_abi` `nw_abi platform interface`, which does not match `ipados-17.7.11-21H461` |

## Documentation

| Topic | Documentation |
| --- | --- |
| Porting | [Porting](https://unixnu.com/porting/), [steps](https://unixnu.com/porting/steps), [device repository](https://unixnu.com/porting/device-repository) |
| Hardware | [Device tree](https://unixnu.com/porting/device-tree), [SoC support](https://unixnu.com/porting/soc-support) |
| Drivers | [Adaptation order](https://unixnu.com/porting/drivers), [component catalog](https://unixnu.com/reference/components) |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [boot chain](https://unixnu.com/concepts/boot-chain) |
| Verification | [Building](https://unixnu.com/building), [bring-up](https://unixnu.com/porting/bring-up), [log capture](https://unixnu.com/porting/log-capture) |
| Record | [Porting records](https://unixnu.com/porting/records), [`porting.toml`](porting.toml) |
| Package and flash | [`docs/FLASHING.md`](docs/FLASHING.md) — assemble the image and start it with `fastboot boot`, including which steps write to the device |
| UEFI firmware | [`docs/UEFI-FIRMWARE.md`](docs/UEFI-FIRMWARE.md) — the `uefi-nx769j` platform package design: the SM8650 SoC selection, every board value with its provenance, and the vendor inputs still to extract. New and changed files are staged under [`firmware/`](firmware/) |
| Sibling port | [UniXNU-nx729j](https://github.com/2713385115/UniXNU-nx729j) — RedMagic 8 Pro+ (NX729J, SM8550), same brand and one generation back. Its UEFI firmware boots on hardware and its `boot.img` reaches `fastboot boot OKAY`. It carries a `NativeGoodixTouch` driver, which is the driver family this board's input area needs, and its porting notes record build pitfalls that apply here. |
