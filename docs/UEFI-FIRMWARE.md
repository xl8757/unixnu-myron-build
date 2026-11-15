# UEFI firmware design for NX769J

This is the design for `uefi-nx769j`, the Mu-Silicium platform package that
`./unixnu build firmware --uefi ../uefi-nx769j` builds. It states what the
package must contain, where every value comes from, and which values are still
open. The machine-readable part lives in [`board.toml`](../board.toml)
`[firmware]`; the files to place in the Mu-Silicium checkout are staged under
[`firmware/`](../firmware/).

Read it alongside [UEFI firmware](https://unixnu.com/porting/uefi-firmware),
which specifies the integration contract this design has to satisfy.

## What already exists

The starting point is not a blank port. Two things are already written and
verified on hardware, one generation back:

| Artifact | Where | Why it matters here |
| --- | --- | --- |
| `Platforms/ZTE/nx729jPkg/` | [`UniXNU/uefi-sheng`](https://github.com/UniXNU/uefi-sheng), branch `sheng` | A complete platform package for a nubia RedMagic — same brand, same vendor, one SoC generation back. Its `boot.img` reaches `fastboot boot OKAY` on hardware. |
| `Silicon/Qualcomm/LanaiPkg/` | same | Mu-Silicium's SM8650 SoC support. **This board is Lanai**, not Kailua. |
| `Resources/DTBs/nx729j.dtb` | same | The pattern for the firmware DTB `unixnu bootimg --dtb` consumes. |

`nx729jPkg` is `Platforms/Qualcomm/qrd8550Pkg` — the Pineapple QRD base board
this board is derived from — with board identity, framebuffer geometry and a
unique platform GUID substituted. `nx769jPkg` is the same derivation again.

### NX769J is a Lanai part

The SoC package is the one decision that is easy to get wrong, because Mu-Silicium
names SoC packages by Qualcomm platform codename rather than by `SM8xxx` part
number. The mapping is in each package's `*Pkg.dsc.inc`:

| Package | SMBIOS processor | Part numbers |
| --- | --- | --- |
| `WaipioPkg` | Snapdragon 8 Gen 1 | SM8450 |
| `KailuaPkg` | Snapdragon 8 Gen 2 | SM8550-AB / -AC |
| **`LanaiPkg`** | **Snapdragon 8 Gen 3** | **SM8650-AA / -Q-AA / -AB / -Q-AB / -AC** |
| `PalawanPkg` | Snapdragon 8s Gen 3 | SM8635 |
| `KaanapaliPkg` | Snapdragon 8 Elite Gen 5 | SM8850 |

Two independent sources agree that NX769J is Lanai:

1. The device's own firmware revisions, recorded in [`device-inputs.md`](../device-inputs.md):
   baseband `5.0.c3-00030-LANAI_GEN_PACK-1.46586.20` and trustzone
   `TZ.XF.5.0-07512.2-LANAIAAAAANAZT-1.46586.1`.
2. `LanaiPkg.dsc.inc` declares `SM8650-AA` and friends, and its GIC PCDs are
   `PcdGicDistributorBase|0x17100000` and `PcdGicRedistributorsBase|0x17180000`
   — the distributor and redistributor bases measured on this board.

So the DSC includes `LanaiPkg/LanaiPkg.dsc.inc`, not `KailuaPkg/`. Note that
`nx729jPkg` — being an SM8550 board — includes `KailuaPkg`; that single line is
the main SoC-level difference between the two packages.

## The package

```
uefi-nx769j/                        a Mu-Silicium checkout, branch nx769j
  Resources/
    Configs/nx769j.toml             NEW      firmware and boot image parameters
    DTBs/nx769j.dtb  nx769j.dts     NEW      firmware DTB, from the device
  Platforms/ZTE/nx769jPkg/          NEW
    nx769j.dsc
    nx769j.fdf
    nx769j.dec
    DeviceBuild.py
    Include/APRIORI.inc  DXE.inc  RAW.inc
    Library/MemoryMapLib/           NEW      the board memory map
    Library/ConfigurationMapLib/    NEW      the SoC configuration map
  Binaries/nx769j/QcomPkg/Drivers/  NEW      vendor DXE drivers, from the device
```

Everything not marked NEW comes from Mu-Silicium, either directly or by copying
`nx729jPkg` and renaming. `firmware/` in this repository holds the files that
are new or changed, ready to lift across.

## Values and where they come from

`Resources/Configs/nx769j.toml`:

| Key | Value | Source |
| --- | --- | --- |
| `boot_shim.requires_kernel_header` | `true` | Inherited from `nx729j.toml`. The ABL checks the ARM64 header at the start of the image. |
| `uefi_fd.base` | `0xA7100000` | Inherited. `sheng`, `qrd8550` and `nx729j` all use it; the UEFI stack PCD in the DSC sits at `0xA760D000`, just above a `0x300000` FD. |
| `uefi_fd.size` | `0x300000` | Inherited. Must equal the built FD size or `firmware.py` refuses it. |
| `boot_image_kernel.kernel_compression` | `"gzip"` | Inherited. BootShim and the FD are gzipped into the kernel field. |
| `boot_image_kernel.append_dtb` | `true` | Inherited. The firmware DTB is appended to the kernel field. |
| `boot_image.header_version` | `4` | **From `nx729j.toml`**, and this is the one place the sibling differs from `sheng`/`qrd8550`. NX769J is an Android 14 ABL board with `BOARD_BOOTIMG_HEADER_VERSION := 4`, exactly like nx729j. |

The remaining `boot_image` keys (`base`, `kernel_offset`, `tags_offset`,
`pagesize`) are not in `nx729j.toml`; `board.toml [bootimg.image]` supplies them.
See the header-version note there for why `unixnu bootimg` still writes a v0
header and what to do if the ABL rejects it.

`Platforms/ZTE/nx769jPkg/nx769j.dsc`, by class of value:

| Setting | Value | Source |
| --- | --- | --- |
| `PLATFORM_NAME`, `OUTPUT_DIRECTORY`, `FLASH_DEFINITION` | `nx769j` / `Build/nx769jPkg` / `nx729jPkg/nx769j.fdf` | Derived from `nx729jPkg`; `fd` in `board.toml` must match `OUTPUT_DIRECTORY`. |
| `PLATFORM_GUID` | new GUID | **Must be unique** — not nx729j's `DF09B1B0-…`. |
| `SOC_TYPE` | `2` | `LanaiPkg.dsc.inc`: `2` = `SM8650-AB`, Snapdragon 8 Gen 3 @ 3.30 GHz, 8 cores. See the open item below. |
| SoC include | `LanaiPkg/LanaiPkg.dsc.inc` | See above. |
| `PcdSystemMemoryBase` | `0x80000000` | **Measured** on this board — lowest System RAM range and `gunyah_hyp_region@80000000`. |
| `PcdCPUCoresStackBase` | `0xA760D000` | Inherited from nx729j/qrd8550. Above the FD at `0xA7100000`+`0x300000`. |
| `PcdCPUCorePrimaryStackSize` | `0x40000` | Inherited. |
| `PcdFrameBufferWidth/Height/ColorDepth` | `1116` / `2480` / `32` | **Measured** on this board from `card0-DSI-1`, and identical to what `nx729jPkg` uses. |
| `PcdSmbiosSystemManufacturer` | `"ZTE"` | nubia is ZTE's brand; matches `nx729jPkg`. |
| `PcdSmbiosSystemModel` / `…BoardModel` | `"RedMagic 9 Pro"` | Board name. |
| `PcdSmbiosSystemRetailModel` | `"nx769j"` | Codename. |
| `PcdSmbiosSystemRetailSku` | `"RedMagic_9_Pro_nx769j"` | Follows the `nx729jPkg` convention. |
| `PcdPlatformType` | `"LA"` | Inherited. |
| `MemoryMapLib`, `ConfigurationMapLib` | `nx769jPkg/Library/…` | Board specific; see below. |
| `AcpiTables.inf` | `nx769j/AcpiTables.inf` | Board specific. |

`Platforms/ZTE/nx769jPkg/nx769j.fdf` is `nx729j.fdf` with `nx729j` replaced by
`nx769j` throughout. It places `Include/APRIORI.inc`, `Include/DXE.inc` and
`Include/RAW.inc` in `FV.FvMain`, and pulls `QcomPkg/Extra.fdf.inc` and
`SiliciumPkg/Common.fdf.inc`. The commented-out `DeviceTreeBlob` block in the
FDF is left commented: this port appends the firmware DTB to the boot image
rather than embedding it in the FD.

## Required UniXNU integration

[Required integration](https://unixnu.com/porting/uefi-firmware#required-integration)
names six modules that must appear in the DSC `[Components]` **and** in the FDF
firmware volume:

| Module | Purpose |
| --- | --- |
| `UniXNUPkg/Application/UniXNU/UniXNU.inf` | Boot menu and configuration reader |
| `UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf` | Firmware boot option registration |
| `DarwinBootPkg/Driver/DarwinBoot.inf` | XNU loader |
| `UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf` | Publish the FDT initrd as a block device |
| `MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf` | Disk access for the filesystem driver |
| `FatPkg/EnhancedFatDxe/Fat.inf` | Expose the payload's FAT filesystem |

`nx729jPkg/Include/DXE.inc` already carries the last two, plus
`PartitionDxe` and `EnglishDxe` (the FAT driver's Unicode collation service).
The first four are **not** in the nx729j package and have to be added here. They
resolve through `PACKAGES_PATH`, not through the Mu-Silicium tree:

```
{uefi}:{uefi}/Platforms/ZTE:{tree}/package:{tree}/drivers/DarwinBoot:{edk2}:…
```

`{tree}/package` is this repository's `package/`, which supplies UniXNUPkg;
`{tree}/drivers/DarwinBoot` supplies DarwinBootPkg. That is what `board.toml
[firmware].packages` lists.

Two library mappings are required for the shared RAM-disk driver:

```ini
HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
```

`UniXNURamDiskDxe` must be **embedded in the firmware volume**, not loaded from
the payload: the payload is the thing it has to discover.

## Payload discovery path

The Android bootloader places the FAT payload in RAM and writes
`/chosen/linux,initrd-start` and `linux,initrd-end` into the firmware FDT, each
a 32- or 64-bit big-endian address. The range must hold complete 512-byte
sectors and stay reserved while UEFI runs.

Mu-Silicium SEC preserves the incoming FDT address as a `UINT64` in the
entry-argument HOB `DAEE82E6-8090-4E72-BFB9-78C13CD4E359`. `UniXNURamDiskDxe`
reads that handoff and publishes Block I/O; DiskIoDxe and FAT then expose
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, which lets UniXNU read
`\EFI\UniXNU\unixnu.toml` and list this board's entries from
[`package/unixnu.toml`](../package/unixnu.toml).

If the menu appears without the `xnu` entry, work through the five checks in
[payload discovery](https://unixnu.com/porting/uefi-firmware#payload-discovery),
starting with `boot/boot.json` reporting a nonzero `ramdisk_bytes`.

## Required services

| Service | Consumer |
| --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | UniXNU menu, DarwinBoot framebuffer handoff, XNU console |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | UniXNU menu input |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` | Configuration, payload and kernel files |
| `EFI_BLOCK_IO_PROTOCOL` | Userdata partition and block slices |
| `EFI_SERIAL_IO_PROTOCOL` | Logs on boards with a UART |
| `EFI_FIRMWARE_VOLUME2_PROTOCOL` | UniXNUDxe boot option registration |

The vendor DXE drivers in `Binaries/nx769j/` supply storage (`UFSDxe`) and the
platform services. Display comes from the GPU drivers; this board has Adreno 750
with no qualified driver, so **GOP is the display path and the firmware is what
publishes it** — see the display area of [`porting.toml`](../porting.toml).

## Vendor inputs

A Mu-Silicium port needs the vendor UEFI DXE drivers for the board. `nx729jPkg`
lists them from `Binaries/nx769j/QcomPkg/Drivers/…`, and the `Binaries`
submodule points at [`Project-Silicium/Device-Binaries`](https://github.com/Project-Silicium/Device-Binaries).

The set `nx729jPkg/Include/DXE.inc` uses, and therefore the set this board needs
an equivalent of:

| Driver | Role |
| --- | --- |
| `EnvDxe` | Environment and board configuration |
| `ShmBridgeDxe`, `TzDxe` (`ScmDxeCompat`, `TzDxeLA`) | TrustZone and shared-memory bridge |
| `ChipInfoDxe`, `PlatformInfoDxe`, `IPCCDxe` | SoC identity, platform info, inter-processor communication |
| `DALSYSDxe`, `HALIOMMUDxe`, `HWIODxe`, `I2CDxe`, `TLMMDxe` | Hardware access layer, IOMMU, bus access |
| `PmicDxe` (`PmicDxeLa`) | PMIC |
| `UFSDxe` | Internal storage — the block path XNU's root depends on |
| `CmdDbDxe`, `PwrUtilsDxe`, `RpmhDxe`, `NpaDxe`, `ULogDxe`, `VcsDxe` | RPMh power and logging |

`APRIORI.inc`, `DXE.inc` and `RAW.inc` between them name **52** modules, and all
52 come out of the device's own `uefi.img`. That image is an outer firmware
volume holding only SEC and a platform configuration, with the DXE drivers in
gzip-compressed nested volumes inside it. Twelve carry a UI section name that
differs from the INF name the DSC uses — `I2CDxe`'s module is called plain `I2C`,
`TLMMDxe`'s is `DALTLMM` — so
[`scripts/extract-uefi-drivers.py`](../scripts/extract-uefi-drivers.py) maps
them by name rather than guessing by prefix. The expected set is read out of the
three include files rather than kept in step with them, because `APRIORI.inc`
names one module that `DXE.inc` has commented out.

```sh
python3 scripts/extract-uefi-drivers.py --image uefi.img            # list
python3 scripts/extract-uefi-drivers.py --image uefi.img --layout ../uefi-nx769j
# referenced by Include: 52 recovered, 0 not in the image, 0 without a module body
```

`--layout` writes `Binaries/nx769j/QcomPkg/Drivers/<family>/<name>.inf`, the
`.efi` and its `.depex`, and the three raw files under `Binaries/nx769j/RawFiles`.
Every module is a prebuilt binary wrapped in an EDK II INF; the wrapper's GUID is
derived from the INF name, and it does not have to be the one Device-Binaries
picked.

**They are not redistributable**, so `uefi-nx769j` cannot ship them and every
build machine runs the extraction. [`proprietary-files.txt`](../proprietary-files.txt)
records the hash of each extracted module, so a truncated or wrong extraction is
caught before EDK II runs.

`preflight = ["scripts/check-nx769j-firmware.py"]` exists to make a missing or
wrong vendor binary fail before EDK II runs rather than mid-build. With the
extraction done it reports `52/52 present`; `--require-vendors` makes a missing
one fatal.

## Memory map

`Library/MemoryMapLib/MemoryMapLib.c` is the board's UEFI memory map and is the
most hardware-specific file in the package. It is rebuilt from this board's own
firmware device tree, not carried over from nx729j.

Each of the 36 fixed-address reservations has a descriptor named after its node,
and the 13 regions UEFI lays out for itself -- `FD Reserved`, `UEFI FD`, `UEFI
FD Reserved`, `Info Blk`, `MMU PageTables`, `Log Buffer`, `UEFI Stack`, `SEC
Heap`, `Sched Heap`, `FV Region`, `UEFI RESV`, `Kernel`, `DXE Heap` -- stay
where nx729jPkg puts them. Those fit this board's free band exactly apart from
two gaps the map does not claim: 80 MiB at `0xa1f80000` and 1 MiB at
`0xd7cc0000`. Neither is reserved by the device tree either, so leaving them out
of the map costs capacity rather than safety.

Where the two boards differ:

| Region | nx729j's map | NX769J | |
| --- | --- | --- | --- |
| DRAM base | `0x80000000` | `0x80000000` | agrees — `PcdSystemMemoryBase` |
| Firmware volume | `0xA7100000` / `0x300000` | same | agrees — `uefi_fd.base` / `.size` |
| UEFI stack | `0xA760D000` / `0x40000` | same | agrees — `PcdCPUCoresStackBase` |
| `HYP` | `0x80000000` / `0x00A00000` | `0x80000000` / `0x00E00000` | **differs** — `gunyah_hyp_region` |
| Contiguous splash | `0xB8000000` / `0x2B00000` | `0xE3940000` / `0x2B00000` | **differs** — `splash_region`, `cont_splash_region` |
| `DXE Heap` | `0xBAB00000` | `0xB8000000` / `0x1FCC0000` | **differs** — nx729j's splash address is ordinary RAM here |
| `gpu_microcode_region` | absent | `0x9B09A000` / `0x2000` | now reserved |
| `ramoops` | absent | `0xD7E00000` / `0x200000` | now reserved, and no longer under `TZApps` |

The reservation list is complete. It comes from `vendor_boot.img` in the device's
flash package, whose `dtb` field holds six device trees; four are Pineapple
variants with byte-identical reservation sets. `scripts/extract-firmware-dtb.py`
prints them and the full table is in
[`device-inputs.md`](../device-inputs.md#fw-dtb).

The mapping is checked rather than reviewed:

```sh
python3 scripts/check-memory-map.py --dtb Resources/DTBs/nx769j.dtb -v
# check-memory-map: 64 descriptors, 37 fixed reservations, 37 covering one, 0 unexplained
```

37 rather than 36 because `board.toml [[bootimg.dtb_node]]` adds the contiguous
splash region to the boot image's device tree, and the script reads that table
too. `scripts/test_check_memory_map.py` covers the four ways the map can be
wrong: an uncovered reservation, two entries fighting over one range, an entry
covering nothing, and an entry reaching outside DRAM.

`Library/ConfigurationMapLib/ConfigurationMapLib.c` is the SoC configuration map
and is shared with every Lanai board; it follows `nx729jPkg`'s.

The splash reservation is the one that bites: it is inside the DRAM window this
port hands to XNU (`[payload] dram_size = 0x80000000`), so the firmware map and
the ADT must both exclude it, or the framebuffer is overwritten under the boot
menu.

## Build

```sh
git clone https://github.com/UniXNU/uefi-sheng uefi-nx769j
cd uefi-nx769j && git checkout -b nx769j
# lift firmware/ across, then:
git submodule update --init --depth 1 -- \
  Common/Mu Common/Mu_OEM_Sample Silicium-ACPI \
  Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl

cd ../UniXNU
./unixnu defconfig nx769j_defconfig -D ../UniXNU-NX769J
./unixnu build firmware --uefi ../uefi-nx769j
```

`firmware.py` then runs the preflight, builds BaseTools if `GenFw` is absent,
applies `Resources/MuPatches/Gic-Spurious.patch` to the edk2 tree for the build
and reverts it afterwards, runs EDK II `build -p Platforms/ZTE/nx769jPkg/nx769j.dsc
-a AARCH64 -b <target> -t CLANGPDB`, and checks the output is exactly `0x300000`
bytes at `Build/nx769jPkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd`.

`SOURCE_DATE_EPOCH` is pinned to the Mu-Silicium commit time, so the same
sources give the same firmware.

The command has been run to completion on this package:

```text
== firmware: uefi-nx769j
== firmware: initializing submodules of the Mu-Silicium checkout
+ python3 …/check-nx769j-firmware.py
+ git -C …/mu_basecore-8f10084c apply …/Resources/MuPatches/Gic-Spurious.patch
+ build -p Platforms/ZTE/nx769jPkg/nx769j.dsc -a AARCH64 -b DEBUG -t CLANGPDB
         -n 16 -D FD_BASE=0xa7100000 -D FD_SIZE=0x300000 -D FD_BLOCKS=768 …
== firmware: …/Build/nx769jPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

| Output | Value |
| --- | --- |
| FD | `Build/nx769jPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` |
| Size | 3145728 bytes = `0x300000`, exactly `[uefi_fd] size` |
| SHA-256 | `93c8eece14da9189536ee7880b5bf268a2b2987b68aedbebc64526bb7ce3ab4d` |

That is the firmware volume inside `boot.img`, and `boot.json` records the same
`fd_sha256`, so what was packed is what was built.

## Verification

| Check | Method |
| --- | --- |
| Firmware volume size | `firmware.py` compares the output with `[uefi_fd] size` |
| Shim header | `unixnu bootimg` checks `ARM\x64` and the base and size fields |
| Boot image parts | `unixnu bootimg` unpacks the image and compares every part |
| Menu | The serial log contains `UNIXNU:` lines |
| Payload | The menu lists the `xnu` entry and DarwinBoot logs the handoff |
| Framebuffer | GOP geometry is `1116x2480x32`, matching the measured panel |

## Open items

Everything below is unresolved and stated as such rather than filled with a
plausible value. What has been closed — the memory map, the firmware DTB and the
vendor DXE drivers — is described above with the command that closed it.

| Item | Status | How to close it |
| --- | --- | --- |
| `SOC_TYPE` | Set to `2` (SM8650-AB, 3.30 GHz, 8 cores). The RedMagic 9 Pro is the 3.30 GHz bin and this board reports 8 CPUs (2×A520 + 5×A720 + 1×X4), so it is not a 6-core Q variant. | Read the prime core's max frequency from the device; `SOC_TYPE` `2` vs `4` distinguishes 3.30 from 3.40 GHz. |
| `PcdCPUCoresStackBase` | Inherited from `nx729jPkg`, and `MemoryMapLib` puts `UEFI Stack` at the same address. | Check it against the Lanai memory map and the splash reservation before the first boot. |
| Silicium-ACPI tables | The SoC tables are Kailua's, which is SM8550: `APIC`, `GTDT` and `PPTT` describe Kailua's cores where this board has two A520, five A720 and one X4. Silicium-ACPI has no Lanai set. XNU reads the Apple device tree rather than ACPI, so this does not affect the XNU boot. | Build a Lanai table set, or drop the tables from the DSC if XNU is the only target. |
| `early_security_init` | Unset. `extract-ipsw-pins.py` used to emit AMFI's kmod stub address, which is not what the hook calls. | Reversing task; see the packaging area of [`porting.toml`](../porting.toml). |
| Boot on hardware | Not attempted. The firmware volume is built and its map is checked, but nothing here has been observed running. | Follow [bring-up](https://unixnu.com/porting/bring-up) and [`docs/FLASHING.md`](FLASHING.md). |

## Related

- [`docs/FLASHING.md`](FLASHING.md) — assembling and starting the boot image
- [`porting.toml`](../porting.toml) — the firmware area's scope, evidence and completion criteria
- [`README.md`](../README.md) — board overview and the non-destructive install path
