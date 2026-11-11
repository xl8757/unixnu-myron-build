# UEFI firmware

The UEFI firmware port comes from Mu-Silicium. It supplies the services UniXNU and DarwinBoot use.

## Required services

| Service | Consumer |
| --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | UniXNU menu, DarwinBoot framebuffer handoff, XNU console |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | UniXNU menu input |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` | Configuration, payload and kernel files |
| `EFI_BLOCK_IO_PROTOCOL` | Userdata partition and block slices |
| `EFI_SERIAL_IO_PROTOCOL` | Logs on boards with a UART |
| `EFI_ACPI_TABLE_PROTOCOL` | Optional ACPI table injection |
| `EFI_FIRMWARE_VOLUME2_PROTOCOL` | UniXNUDxe boot option registration |

A board that lacks one of these services limits the stages it can run. Add the driver to the firmware volume or to the UniXNU payload before the dependent stage.

## Mu-Silicium layout

```
Mu-Silicium/
  BootShim/
    BootShim.S
  Resources/
    Configs/<device>.toml
    Scripts/mkbootimg.py
  Platforms/<Vendor>/<device>Pkg/
    <device>.dsc
    <device>.fdf
  Silicon/
  Common/
```

The UniXNU driver reads `Resources/Configs/<name>.toml` and `<device>.dsc`. It uses `Resources/Scripts/mkbootimg.py` for the boot image.

## Device configuration

`Resources/Configs/<device>.toml` supplies the addresses and image parameters:

```toml
[boot_shim]
requires_kernel_header = true

[uefi_fd]
base = 0x00000000
size = 0x000000

[boot_image_kernel]
kernel_compression = "gzip"
append_dtb = true

[boot_image]
header_version = 0
base = 0x00000000
kernel_offset = 0x8000
tags_offset = 0x1e00000
pagesize = 4096
```

| Key | Meaning |
| --- | --- |
| `boot_shim.requires_kernel_header` | The bootloader checks the ARM64 header at the start of the image |
| `uefi_fd.base` | Physical address where the firmware volume is loaded |
| `uefi_fd.size` | Exact firmware volume size |
| `boot_image_kernel.kernel_compression` | Compression of the shim and firmware volume |
| `boot_image_kernel.append_dtb` | Append the device tree to the kernel field |
| `boot_image.*` | Android boot image header parameters |

The firmware base and size are compiled into BootShim. They are also checked when the boot image is packed.

## DSC and FDF

The DSC lists the packages, libraries and components. The FDF places the components in the firmware volume.

UniXNU packages use `PACKAGES_PATH`:

```
{uefi}:{tree}/package:{tree}/drivers/DarwinBoot:{edk2}:...
```

`{uefi}` is the Mu-Silicium checkout, `{tree}` is the assembled build tree, and `{edk2}` is the Mu_Basecore checkout. The board file lists every package path.

UniXNU and DarwinBoot are included through their INFs:

```
UniXNUPkg/Application/UniXNU/UniXNU.inf
UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
DarwinBootPkg/Driver/DarwinBoot.inf
```

The firmware FDF decides whether they are embedded in the firmware volume or copied to the ESP.

## Preflight

`[firmware] preflight` lists scripts in the build tree that run before the EDK II build. A preflight script checks vendor inputs, firmware partitions or board files. `--skip-preflight` skips them.

## Vendor inputs

A Mu-Silicium port needs the vendor binaries for the board, for example:

- UEFI DXE drivers for storage, display, input or power.
- ACPI tables.
- Firmware partition images.
- Configuration data.

The board preflight script validates the files and their hashes.

## Submodules and patches

Mu-Silicium uses submodules for shared trees. The board file lists the ones it needs:

```toml
[firmware]
submodules = ["Common/Mu", "Silicon/Silicium", "Silicon/Qualcomm"]
patches = ["Resources/MuPatches/Gic-Spurious.patch"]
```

A patch in `patches` is applied to the shared edk2 tree for the build and reverted afterwards. The driver checks each patch with `git apply --check` before applying it.

## Build

With the device repository selected:

```sh
./unixnu build firmware --uefi /path/to/Mu-Silicium
```

The driver:

1. Runs the preflight scripts.
2. Initializes the listed submodules.
3. Builds BaseTools when `GenFw` is absent.
4. Applies the board patches.
5. Runs EDK II `build` with the DSC, `AARCH64`, `CLANGPDB` and the board defines.
6. Reverts the patches.
7. Checks the firmware volume size and path.

The output path comes from `[firmware] fd`:

```toml
fd = "Build/<device>Pkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd"
```

## Standalone build

The Mu-Silicium checkout can be built with its own script:

```sh
cd Mu-Silicium
python3 build_uefi.py -d <device>
```

The UniXNU driver performs the same build with the board defines and the shared edk2 patches.

## Boot image integration

`unixnu bootimg` compiles BootShim with the firmware base and size:

```sh
clang --target=aarch64-none-elf -c \
  -DFD_BASE=<base> -DFD_SIZE=<size> \
  -DREQUIRES_KERNEL_HEADER=<0 or 1> \
  BootShim/BootShim.S -o shim.o
```

It appends the firmware volume, compresses the pair with gzip, appends the device tree and packs the Android boot image.

## Verification

| Check | Method |
| --- | --- |
| Firmware volume size | The driver compares the output size with `[uefi_fd] size` |
| Shim header | The driver checks `ARM\x64` and the base and size fields |
| Boot image parts | The driver unpacks the image and compares every part |
| Menu | The serial log contains `UNIXNU:` lines |
| Payload | The menu lists the `xnu` entry and DarwinBoot logs the handoff |

## Adding a UEFI driver

A driver can live in the firmware volume or in the payload.

| Location | Build | Load |
| --- | --- | --- |
| Firmware volume | Add the INF to the DSC and FDF | Firmware dispatches it, or UniXNUDxe starts the application |
| Payload | Build a standalone `.efi` | `[[driver]]` in `unixnu.toml` or a kit |

See [UEFI drivers](/drivers/uefi-drivers) for the INF, entry point and injection details.

## Next steps

- [Device tree](/porting/device-tree)
- [Bring-up order](/porting/bring-up)
- [UEFI drivers](/drivers/uefi-drivers)
