# Overview

UniXNU starts iOS or macOS on an ARM64 device from a UEFI environment. The stack has six layers. Each layer has a repository, a build output and a handoff contract.

## Layer model

```
Android bootloader (ABL)
  |
  |  fastboot boot boot.img
  v
BootShim + UEFI firmware volume + device tree
  |
  |  UEFI BDS starts UniXNU
  v
UniXNU (UEFI boot manager)
  |
  |  UNIXNU_LOADER_PROTOCOL
  v
DarwinBoot (XNU loader)
  |
  |  boot_args, ADT, trust cache, ramdisk
  v
Patched XNU kernel
  |
  |  IOKit, BSD, launchd
  v
Userland from the IPSW (OS and SystemOS)
```

## Components

### Boot image

The boot image is an Android boot image with header version 0. Its kernel field holds gzip data with two parts: the compiled BootShim and the UEFI firmware volume. The device tree follows the gzip stream. The ramdisk field holds the UniXNU payload.

The stock Android bootloader loads the image at the address declared by the Mu-Silicium device configuration, verifies the ARM64 image header, and jumps to the shim. BootShim relocates the firmware volume and enters UEFI.

### UEFI firmware

The firmware comes from a Mu-Silicium checkout for the device. It provides the UEFI services the boot manager needs:

- `EFI_GRAPHICS_OUTPUT_PROTOCOL` for the menu and the kernel framebuffer.
- `EFI_SIMPLE_TEXT_IN_PROTOCOL` for menu input.
- `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and `EFI_BLOCK_IO_PROTOCOL` for the payload volume and disk partitions.
- `EFI_SERIAL_IO_PROTOCOL` for logs when the board exposes a UART.

The `unixnu build firmware` step applies the board patches to the shared edk2 tree, builds the board DSC and returns `SILICIUM_UEFI.fd`.

### Boot manager

The UniXNU boot manager is a UEFI application built from `package/UniXNUPkg`. It reads `\EFI\UniXNU\unixnu.toml` from the volume it was started from, then from every other volume. The configuration declares menu entries, drivers, device tree patches, ACPI tables, block slices and kits.

The boot manager performs five jobs before the menu:

1. Publishes every `[[block_slice]]` as a block device.
2. Loads and starts every configured UEFI driver.
3. Installs every configured ACPI table.
4. Merges every kit whose `[match]` section is satisfied.
5. Prepares the device tree for the selected entry.

An entry of type `xnu` is handed to a loader that implements `UNIXNU_LOADER_PROTOCOL`.

### DarwinBoot

DarwinBoot is a UEFI driver in the firmware volume. It installs the loader protocol at `EL2` or `EL1`. On an `xnu` entry it performs these steps:

1. Reads the kernel Mach-O from the payload volume.
2. Reads the `.adt` sidecar next to the kernel.
3. Reads the optional `.trustcache` sidecar.
4. Validates the Mach-O segments, the ADT and the trust cache structure.
5. Allocates a physical window for the kernel and its data.
6. Copies the kernel segments, the ADT, the trust cache and the ramdisk.
7. Builds a Darwin `boot_args` structure.
8. Exits boot services, disables the MMU and caches, and jumps to the kernel entry point in EL1.

The loader requires a single ARM64 Mach-O with a `__TEXT` segment, a `LC_MAIN` entry point and a base address aligned to the firmware page size.

### XNU kernel

The kernel is built from Apple's `xnu-10063.141.1` source with the patches in `patches/xnu`. The BCM2837 machine configuration supplies the 4 KiB page layout for non-Apple ARM64 boards.

The native kernel is prelinked against the original kernelcache from the target IPSW. The prelink keeps the original Apple kexts and binds the native kernel to their symbols. The result is a single `kernel.macho` that contains the native kernel segments and the selected original components.

### Userland

The userdata image holds the unchanged `OS` and `Cryptex1,SystemOS` disk images from the IPSW. A 4 KiB manifest at the start of the partition describes both windows. The native UFS or block driver publishes the partition, and XNU mounts the root volume by UUID.

## Repositories

| Repository | Purpose |
| --- | --- |
| `UniXNU` | Build driver, UEFI boot manager, DarwinBoot, generic drivers, kits, patches and documentation |
| `UniXNU-<device>` | Board description, kernel defconfig, device scripts, device drivers and package |
| `uefi-<device>` | Mu-Silicium UEFI port for the device |

A device repository has the same directory layout as `UniXNU`. Its `drivers/`, `kits/`, `patches/`, `scripts/` and `package/` override the generic modules with the same name.

## Build outputs

Everything is written under `out/`:

```
out/
  state.json                 remembered device repository
  feeds/                     fetched git sources
  ipsw/<build>/              extracted IPSW members
  <board>/
    .config                  selected defconfig
    tree/                    assembled build tree
    logs/                    one log per build step
    kernel/kernel.macho      prelinked kernel
    package/Build/Out/       UniXNU and test EFI binaries
    payload/payload.img      FAT payload volume
    payload/payload.json     payload report
    boot/boot.img            Android boot image
    boot/boot.json           boot image report
    userdata/userdata-root.img
    userdata/userdata-root.json
```

The reports record input hashes and the commands that produce the next step. The boot image is deterministic for a given firmware volume, device tree and ramdisk. The payload contains a random seed in the ADT, so two payload builds differ.

## Next steps

- [Requirements](/guide/requirements) lists the host tools and external inputs.
- [Build](/building) walks through the driver commands.
- [Porting](/porting/) describes how to add a device.
- [Devices](/devices/) lists device repositories. The sheng page records one device; the general guide uses placeholders.
