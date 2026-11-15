<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="assets/logo-light.svg">
    <img alt="UniXNU" src="assets/logo-light.svg" width="200">
  </picture>
</p>

# UniXNU-myron

Device repository for the Redmi K90 Pro Max (myron, SM8850). It holds the board file, kernel defconfig and device scripts.

- [Documentation](https://unixnu.com)
- [Porting guide](https://unixnu.com/porting/)
- [Build](https://unixnu.com/building)

Build with the `unixnu` driver of [UniXNU](https://github.com/UniXNU/UniXNU).

## State

The Mu-Silicium port (`Platforms/Xiaomi/myronPkg`, derived from the OnePlus 15 package) starts through `fastboot boot` and reaches its boot manager with the bootloader framebuffer and the RAM partition table. The board file and the ADT generator carry the values read from the stock firmware and the stock kernel log.

On top of that the tree boots iPadOS 17.7.11 (iPad7,1 userland) to SpringBoard: the UI presents on the firmware framebuffer, touch works, the Adreno 840 runs Metal through NativeMetal/Turnip, and the `unixnu` UFS partition carries a persistent Data store. Everything is RAM boots (`fastboot boot`); nothing is written outside the `unixnu` partition.

The prime cores are `0x10000` and `0x10100` in MPIDR Affinity2. Secondary CPU startup needs the `native-smp-wide-affinity` XNU patch of UniXNU and `sheng-smp=1`; the default arguments start the boot CPU only.

## Known-good boot arguments

Beyond the arguments the board generates, these are needed on this device:

| Argument | Why |
| --- | --- |
| `sheng-smp=1` + `armcpu-spi-ipi=1` | start the secondary CPUs (their software-SPI IPIs) |
| `myron-clear-sigp=1` + `myron-ipi-clearpend=1` | **always as a pair**: clear `SIGPdisabled` so `cpu_idle()` can idle, and clear the software SPI's pending bit after each interrupt. Alone, the first leaves the SPI asserted forever; without both the prime core takes ~3.5M IPIs/s and the phone runs hot |
| `myron-dp-none=1` | report file-protection class D on the unprotected Data volume; without it PosterBoard rejects its own store ("Wallpaper Limit Reached") |
| `myron-gpu=11 myron-gpu-metal=1 sheng-gpu-addon=1 sheng-fb-gpu=1` | Adreno 840 bring-up, the Metal plugin and the GPU scan-out hand-off |
| `myron-cpufreq=2 myron-gpu-dcvs=1` | optional: SCMI CPU governor + GPU DCVS |
| `sheng-vsync=120` | optional: publish 120 Hz (a work-loop timer, not the panel's vblank) |

Never use `armcpu-self-ipi=1`: it unblocks IPI delivery without the pending-bit clear and the core storms.

## Board values

| Item | Value | Source |
| --- | --- | --- |
| SoC | SM8850 Canoe, msm-id 0x294, revision 2 | `/sys/devices/soc0`, stock base DTB |
| CPUs | `0,0x100,0x200,0x300,0x400,0x500,0x10000,0x10100`, PSCI `smc` | stock base DTB, kernel log |
| GICv3 | distributor 0x17000000, redistributors 0x17080000 + n * 0x40000 | stock base DTB, kernel log |
| Timer | 19.2 MHz | stock base DTB |
| DRAM | 0x80000000 + 2 GiB, 0x880000000 + 14 GiB (16 GB unit) | kernel log |
| Splash region | 0xfc800000, 0x2b00000 | stock dtbo overlay |
| UniXNU disk | GPT entry `unixnu` (`[userdata] partition`): LUN 0, 4096-byte LBAs, 0x5aaeb00..0x7aaeaff (128 GiB); HyperOS `userdata` is 0x39f200..0x5aaeaff | GPT on the phone, `fastboot getvar` |
| Boot image | header v0, gzip(BootShim + FD) with the Canoe v2 base DTB appended; the ABL applies the dtbo overlay | tested with `fastboot boot` |
| Ramdisk | with header v4 the ABL loads the generic ramdisk from `init_boot`, not the boot image's, so the payload needs header v0 | `fastboot boot`, initrd size |

`scripts/data/myron-observed.json` records the reserved memory, the RAM ranges the kernel received and the ranges the firmware hides. A UEFI memory map must keep all of them out of conventional memory.

## Scripts

| Script | Purpose |
| --- | --- |
| `scripts/make-myron-adt.py` | Apple-format device tree for the first boot |
| `scripts/extract-myron-dtb.py` | Stock Canoe base DTB from `vendor_boot.img`, for `unixnu bootimg --dtb` |
| `scripts/build-arm-driver.py` | Compiles a generic driver against the XNU compile records (from UniXNU-sheng) |

## Device support

| Feature | State | Notes |
| --- | --- | --- |
| UEFI | ⚠️ | Mu-Silicium boots to its boot manager; no storage or input in UEFI, the kernel gets the RAM partition table |
| Touch | ✅ | FT3683 on QUP SE19 SPI (`MyronTouch.cpp`, `myron-touch=`) feeding the HID source |
| Display | ⚠️ | The firmware GOP framebuffer (1220x2656); the panel is not re-initialised (no DSC init), refresh is a work-loop timer |
| UFS | ⚠️ | XNU side: NativeUfs on the `unixnu` partition (persistent Data store); UEFI side: none |
| Wi-Fi | ❌ | |
| Bluetooth | ❌ | |
| Camera | ❌ | |
| GPU | ⚠️ | Adreno 840 / A8XX run mode, Metal via NativeMetal + Turnip; the UI renders, some wallpapers and previews are still blank and the scan-out hand-off stalls intermittently |
| USB | ❌ | |
| Audio | ❌ | |
| Sensors | ❌ | |
| Battery driver | ❌ | |
| Suspend and resume | ❌ | CPU idle (WFI/PSCI) works; system sleep does not |

✅ available, ⚠️ partial, ❌ not available.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
