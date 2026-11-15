# NX769J measured hardware inputs

Read-only measurements from a physical NX769J, taken to fill the board values
that no public source provides. Every command below is read-only: it prints
device state and writes nothing to the phone.

## Device identity

The connected handset reports a spoofed Xiaomi identity through its userspace
properties while the boot image, kernel and device tree identify NX769J. The
values recorded here come from the kernel, the device tree and the raw UFS
partition table, which userspace property spoofing does not affect.

| Field | Value | Source |
| --- | --- | --- |
| PCB version | `NX769J_V1A` | `getprop persist.sys.pcb_version_ext` |
| Boot image device | `NX769J` | `getprop ro.product.bootimage.device` |
| Marketing name | `nubia-9pro` (RedMagic 9 Pro) | `getprop ro.cc.device.name` |
| Firmware | `GEN_CN_NX769SV2.0.0B08` (RedMagic 9S build) | `getprop ro.build.sw_internal_version` |
| Boot image fingerprint | `nubia/NX769J/NX769J:14/UKQ1.230917.001/20250331.162825:user/release-keys` | `getprop ro.bootimage.build.fingerprint` |
| Kernel | `6.1.118-Huge-Dragon-AsulTop-0.0.3` | `uname -a` |
| Baseband | `SS.DE.5.0.c3-00030-LANAI_GEN_PACK-1.46586.104` | `getprop gsm.version.baseband` |

SoC identity from the device tree confirms the platform:

```
model          = Qualcomm Technologies, Inc. Pineapple MTP
compatible     = qcom,pineapple-mtp, qcom,pineapple, qcom,pineapplep-mtp,
                 qcom,pineapplep, qcom,mtp
qcom,msm-id    = 0x22d00020000, 0x24100020000     # msm_id 0x22d = SM8650
qcom,board-id  = 0x800000000
```

## Storage and partition geometry {#userdata}

```
adb shell 'sgdisk --print /dev/block/sda'
```

```
Disk /dev/block/sda: 124002304 sectors, 473.0 GiB
Sector size (logical/physical): 4096/4096 bytes
Partition table holds up to 32 entries
First usable sector is 6, last usable sector is 124002298

Number  Start (sector)    End (sector)    Size       Code  Name
   7         8968         3154695       12.0 GiB    FFFF  super
  10      3154728         3171111       64.0 MiB    FFFF  metadata
  12      3302184       124002298      460.4 GiB    A03A  userdata
```

`userdata` is `/dev/block/sda12`. These are the values `board.toml` records in
`[userdata]`:

| Key | Value |
| --- | --- |
| `first_lba` | `3302184` |
| `last_lba` | `124002298` |
| `lba_bytes` | `4096` |

`userdata` ends exactly on the last usable sector, so it is the last occupied
partition and no `unixnu` partition exists yet. The GPT table can hold 32
entries and uses 12. Both are preconditions of the
[dual-system partition](https://unixnu.com/porting/dual-system-partition) tool,
so a `unixnu` partition can be carved from the end of `userdata` without
touching any other entry.

`super` is 12.0 GiB. The `BOARD_SUPER_PARTITION_SIZE := 9126805504` in the
public NX769J board configurations is marked `TODO: Fix hardcoded value` there
and is not the real size.

## Reserved memory {#splash}

```
adb shell 'od -An -v -tx1 /proc/device-tree/reserved-memory/splash_region/reg'
```

| Property | Value |
| --- | --- |
| node name | `splash_region` |
| `label` | `cont_splash_region` |
| `reg` | `0xe3940000` size `0x2b00000` |

The node name and label match the convention UniXNU-sheng uses for
`/reserved-memory/splash_region`, and the region size `0x2b00000` is the same
as sheng and shennong. Only the address is board specific.

Other regions read from the same node, for the areas that will consume them:

| Region | Address / size |
| --- | --- |
| `gunyah_hyp_region` | `0x80000000` / `0xe00000` |
| `gpu_microcode_region` | `0x9b09a000` / `0x2000` |
| `non_secure_display_region` | size `0xa400000`, `alloc-ranges 0x0 0xffffffff` (allocated at runtime) |

**This is not the complete list.** The UEFI memory map needs every reservation,
and the four above are the ones read at the time. Re-run the `ls` and read each
node's `reg` before building the firmware map; see
[`firmware/README.md`](firmware/README.md).

### Cross-check against the public firmware dump {#dump}

The splash reservation is confirmed by a second, independent source: the DTBO
partition of [`Klozz/nubia_nx769j_dump`](https://github.com/Klozz/nubia_nx769j_dump),
a public dump of this device's firmware.

```
curl -L -o dtbo.dtb "https://raw.githubusercontent.com/Klozz/nubia_nx769j_dump/<branch>/dtbo/18_dtbdump_Qualcomm_Technologies,_Inc._Pineapple_QRD.dtb"
dtc -I dtb -O dts dtbo.dtb -o dtbo.dts
```

| Node | In the dump | Measured on the device |
| --- | --- | --- |
| `splash_region` `label = "cont_splash_region"` | `reg = <0x00 0xe3940000 0x00 0x2b00000>` | `0xe3940000` / `0x2b00000` |
| `disp_rdump_region` `label = "disp_rdump_region"` | `reg = <0xe3940000 0x800000>` | not read |
| `demura_region_0`, `demura_region_1` | `reg = <0 0 0 0>` (runtime) | not read |

The two agree exactly on the splash, which is the one reservation the boot image
has to advertise. `disp_rdump_region` is an eighth-megabyte display ramdump
window at the same base address, inside the splash region, and was not in the
measured set.

The same overlay declares `model = "Qualcomm Technologies, Inc. Pineapple QRD"`
with `compatible = "qcom,pineapple-qrd", "qcom,pineapple", "qcom,pineapplep-qrd",
"qcom,pineapplep", "qcom,qrd"`, which is where the base-board name in
`porting.toml` comes from.

The dump is an **overlay**, applied on top of a base DTB it does not contain: the
repository has `dtbo.img` and its extracted trees but no `vendor_boot` and no
base `dtb`. `gunyah_hyp_region`, `gpu_microcode_region` and the vendor
reservations live in that base. The device's own flash package supplies that
base — see [Firmware device tree reservations](#fw-dtb) below.

### Firmware device tree reservations {#fw-dtb}

The complete reservation list now exists. It comes out of `vendor_boot.img` in
this device's flash package, whose `dtb` field is not one tree but a set of six
that the bootloader picks between by `qcom,msm-id`:

| # | Model | `qcom,msm-id` | Reservations |
| --- | --- | --- | --- |
| 0 | Qualcomm Technologies, Inc. Cliffs SoC | `0x266 0x10000` | 43 nodes |
| 1 | Qualcomm Technologies, Inc. Cliffs 7 SoC | `0x278 0x10000` | 43 nodes |
| 2 | Qualcomm Technologies, Inc. Pineapple v2 SoC | `0x22d 0x20000` | 36 nodes |
| 3 | Qualcomm Technologies, Inc. Pineapple SoC | `0x22d 0x10000` | 36 nodes |
| 4 | Qualcomm Technologies, Inc. PineappleP v2 SoC | `0x241 0x20000` | 36 nodes |
| 5 | Qualcomm Technologies, Inc. PineappleP SoC | `0x241 0x10000` | 36 nodes |

The device's own `qcom,msm-id` (recorded above under
[Device identity](#device-identity)) is `0x22d00020000, 0x24100020000` — both
SoC ids at **version `0x20000`** — which matches the two **v2** trees, 2 and 4,
and rules out the v1 pair. Its model is `Pineapple MTP`, so tree 2
(`qcom,pineapple`) is the one to hand to `unixnu bootimg --dtb`; tree 4 is the
alternative if the bootloader turns out to prefer `qcom,pineapplep`.

It does not matter for the memory map: all four Pineapple trees carry
byte-identical reservation sets. The table below is tree 3, which differs from
tree 2 only in SoC revision and not in a single reservation.

```
scripts/extract-firmware-dtb.py --vendor-boot /path/to/vendor_boot.img
```

walks the vendor boot header, splits the `dtb` region into its trees and prints
each one's model, IDs and reservations. Read-only.

| Node | `label` | Address | Size |
| --- | --- | --- | --- |
| `gunyah_hyp_region@80000000` | `—` | `0x80000000` | `0xe00000` |
| `cpusys_vm_region@80e00000` | `—` | `0x80e00000` | `0x400000` |
| `xbl_aop_merged_region@81a00000` | `—` | `0x81a00000` | `0x260000` |
| `aop_cmd_db_region@81c60000` | `—` | `0x81c60000` | `0x20000` |
| `aop_tme_uefi_merged_region@81c80000` | `—` | `0x81c80000` | `0x74000` |
| `chipinfo_region@81cf4000` | `—` | `0x81cf4000` | `0x1000` |
| `smem_region@81d00000` | `—` | `0x81d00000` | `0x200000` |
| `adsp_mhi_region@81f00000` | `—` | `0x81f00000` | `0x20000` |
| `pvmfw_region@0x824a0000` | `—` | `0x824a0000` | `0x100000` |
| `global_sync_region@82600000` | `—` | `0x82600000` | `0x100000` |
| `tz_stat_region@82700000` | `—` | `0x82700000` | `0x100000` |
| `qdss_region@82800000` | `—` | `0x82800000` | `0x2000000` |
| `qlink_logging_region@84800000` | `—` | `0x84800000` | `0x200000` |
| `mpss_dsm_region@86b00000` | `—` | `0x86b00000` | `0x4900000` |
| `mpss_dsm_region_2@8b400000` | `—` | `0x8b400000` | `0x800000` |
| `mpss_region@8bc00000` | `—` | `0x8bc00000` | `0xf400000` |
| `q6_mpss_dtb_region@9b000000` | `—` | `0x9b000000` | `0x80000` |
| `ipa_fw_region@9b080000` | `—` | `0x9b080000` | `0x10000` |
| `ipa_gsi_region@9b090000` | `—` | `0x9b090000` | `0xa000` |
| `qseecom_region` | `—` | allocated at runtime | — |
| `qseecom_ta_region` | `—` | allocated at runtime | — |
| `qmc_dma_region` | `—` | allocated at runtime | — |
| `non_secure_display_region` | `—` | allocated at runtime | — |
| `gpu_microcode_region@9b09a000` | `—` | `0x9b09a000` | `0x2000` |
| `spss_region_region@9b0a0000` | `—` | `0x9b0a0000` | `0x1e0000` |
| `spu_secure_shared_memory_region@9b280000` | `—` | `0x9b280000` | `0x60000` |
| `spu_secure_shared_memory_region@9b2e0000` | `—` | `0x9b2e0000` | `0x20000` |
| `camera_region@9b300000` | `—` | `0x9b300000` | `0x800000` |
| `video_region@9bb00000` | `—` | `0x9bb00000` | `0x800000` |
| `cvp_region@9c300000` | `—` | `0x9c300000` | `0x700000` |
| `cdsp_region@9ca00000` | `—` | `0x9ca00000` | `0x1400000` |
| `q6_cdsp_dtb_region@9de00000` | `—` | `0x9de00000` | `0x80000` |
| `q6_adsp_dtb_region@9de80000` | `—` | `0x9de80000` | `0x80000` |
| `adspslpi_region@9df00000` | `—` | `0x9df00000` | `0x4080000` |
| `tz_merged_region@d8000000` | `—` | `0xd8000000` | `0x800000` |
| `hwfence-shmem` | `—` | `0xe6440000` | `0x2dd000` |
| `vm_comm_mem_region` | `—` | allocated at runtime | — |
| `trust_ui_vm_region@f3800000` | `—` | `0xf3800000` | `0x4400000` |
| `oem_vm_region@f7c00000` | `—` | `0xf7c00000` | `0x4c00000` |
| `llcc_lpi_region@ff800000` | `—` | `0xff800000` | `0x600000` |
| `va_md_mem_region` | `—` | allocated at runtime | — |
| `sp_region` | `—` | allocated at runtime | — |
| `cdsp_eva_region` | `—` | allocated at runtime | — |
| `adsp_heap_region` | `—` | allocated at runtime | — |
| `linux,cma` | `—` | allocated at runtime | — |
| `secure_cdsp_region` | `—` | allocated at runtime | — |
| `debug_kinfo_region` | `—` | allocated at runtime | — |
| `ramoops@D7E00000` | `—` | `0xd7e00000` | `0x200000` |
| `mem_dump_region` | `—` | allocated at runtime | — |

49 nodes, 36 with a fixed address

### Unique chip ID {#chip-id}

`board.toml [payload.adt] unique_chip_id` is `required`, and the build records
that the value "is not stored in any repository". It is the SoC serial, read
over read-only adb from `192.168.5.195:41919`:

```
adb shell 'cat /sys/devices/soc0/serial_number'
```

| Property | Value |
| --- | --- |
| `/sys/devices/soc0/serial_number` | `2327598128` |
| as hex, for `--unique-chip-id` | **`8abc5430`** |

Cross-checks on the same read:

| Property | Value | |
| --- | --- | --- |
| `/sys/devices/soc0/soc_id` | `557` | `0x22d` — matches `qcom,msm-id`, so SM8650 |
| `/sys/devices/soc0/machine` | `PINEAPPLE` | |
| `/sys/devices/soc0/revision` | `2.0` | matches `qcom,msm-id` revision `0x20000` |
| `qcom,board-id` | `0x00000008_00000000` | board id 8, subtype 0 |
| `ro.boot.emmcid` | `17d5346c` | the UFS chip id, a different per-device value |

`soc_id` 557 is the earlier `qcom,msm-id` reading spelled out: `0x22d` is
SM8650/Pineapple, which also confirms which of the two `qcom,msm-id` entries is
this silicon.

## DRAM
`gunyah_hyp_region@80000000` and the lowest System RAM range both place the
DRAM base at **0x80000000**, which is the ARM64 Qualcomm base and the value
already in `dts/nx769j.dts`.

```
MemTotal: 15705424 kB        # about 15.0 GiB usable, 16 GiB SKU
```

`/proc/iomem` and the kernel `memory` node report System RAM split into
separate ranges with reservations removed, including ranges above 4 GiB at
`0x880000000`, `0x8b0000000`, `0x8c0000000` and `0xa00000000`. UniXNU's device
tree takes one contiguous handoff window, not this multi-range map.

## SoC register map

`/proc/iomem` confirms the ranges that `dts/nx769j.dts` takes from the SoC
DTSI:

| Resource | Measured | In `dts/nx769j.dts` |
| --- | --- | --- |
| UFS PHY | `01d80000-01d81fff` | `0x01d80000 / 0x2000` |
| UFS host | `01d84000-01d86fff` | `0x01d84000 / 0x3000` |
| UFS inline crypto | `01d88000-01d8ffff` | not exported |

CPU MPIDR affinities from `/proc/device-tree/cpus/cpu@*/reg` are
`0x0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700`, matching the board DTS.

The vendor kernel names the cores `qcom,kryo`; the board DTS uses the mainline
ARM core names (`arm,cortex-a520`, `arm,cortex-a720`, `arm,cortex-x4`) that
mainline `sm8650.dtsi` declares and that the UniXNU CPU driver matches on.

## Display and input

```
card0-DSI-1: connected  modes=1116x2480x60cmd
```

Panel geometry is **1116x2480**, matching the value already recorded from the
public NX769J board configurations.

Input devices registered by the kernel:

| Device | Meaning |
| --- | --- |
| `goodix_ts` | Goodix touch controller, `goodix_ts/input0` |
| `goodix_fp` | Goodix fingerprint sensor |
| `gpio-keys_nubia` | nubia side buttons |
| `nubia_tgk_aw_sar0_ch0`, `nubia_tgk_aw_sar1_ch0` | Awinic SAR sensing |
| `pmic_pwrkey`, `pmic_resin` | PMIC power and reset keys |

`goodix_ts` confirms the touch controller family behind
`goodix_firmware_9916r.bin` in `proprietary-files.txt`.

## Storage topology

Eight SCSI devices are published: LUNs `0` through `4`, plus the UFS well-known
LUNs `49456` (RPMB), `49476` and `49488` (boot).

### Root volume UUID {#root-volume}

`payload.adt.root_match_volume_uuid` is the UUID XNU matches a root against. The
ADT generator writes it to `/chosen/root-matching` as an IOKit dictionary over
`IOMedia`, the UFS block class publishes the userdata window, the partition
scheme publishes the volume, and XNU picks the IOMedia whose UUID matches.

It is the **volume** UUID, not the container's. The two are easy to confuse and
only one is mountable:

| UUID | What it is | Matches |
| --- | --- | --- |
| `97A3EB1A-5218-4936-BC85-9574EC2C8CC9` | the APFS root volume of the OS window | the volume's `IOMedia`, which is what mounts |
| `c155fa03-68f4-42b0-8117-1915b6aa8e4c` | the OS window container's `nx_uuid` | the synthesized whole-disk `IOMedia`, which is not a root |

`diskutil apfs list` on the attached OS image reports the first of these as the
volume's Volume UUID. The value set in `board.toml` is the first.

It is read out of the image the build flashes, so it does not need a device:

```sh
python3 scripts/extract-root-volume-uuid.py \
  out/nx769j/ios-16.5-20F66/nx769j/userdata/userdata-root.img
```

| Window | Container | Volume | Seen in |
| --- | --- | --- | --- |
| OS | `0x104400`, 5628755968 bytes | `97A3EB1A-5218-4936-BC85-9574EC2C8CC9` | 9 superblock copies |
| SystemOS | `0x14fa00000`, 2726297600 bytes | `556915A0-5005-4DE5-B3E4-6B489DBB4C1A` | 5 superblock copies |

The OS window is the root: it holds `096-04088-077.dmg`, the image with the dyld
shared cache and the prelinked kext set, and `board.toml [userdata].windows`
names it `OS`. `SystemOS` is the cryptex. A third container appears in the
report at `0x2a8b41b`; the script marks it embedded, and it is a disk image
file inside the OS volume rather than the window's own container.

APFS keeps several copies of a volume superblock through its checkpoint area, so
one volume appears more than once in a scan. Every copy carries the same UUID,
which is what the `Seen in` column shows.

## Still not measured

Nothing from the boot, storage, display, input or GPU areas. The values below
are open but none of them is on the device.

| Value | Why | How to get it |
| --- | --- | --- |
| Verified-root policy bundle | Policy data signed by the owner, not a device value; `scripts/build-verified-root.py` generates an empty table, so root verification is disabled rather than approximated | Produce the signed bundle over the root windows of the OS image that gets flashed |
| `payload.text_fixups` | A specific byte change in the dyld shared cache, chosen to bypass whatever turns out to be in the way; not derivable from the IPSW container | Derive one when a boot gets far enough to fail on one |

## Commands used

All read-only. `adb root` and any write, flash or reboot command was avoided.

```sh
adb devices -l
adb shell 'getprop ro.product.device; getprop ro.product.model; ...'
adb shell 'uname -a'
adb shell 'cat /proc/meminfo | head -4'
adb shell 'cat /proc/iomem'
adb shell 'ls /proc/device-tree/reserved-memory/'
adb shell 'od -An -v -tx1 /proc/device-tree/reserved-memory/splash_region/reg'
adb shell 'sgdisk --print /dev/block/sda'
adb shell 'blockdev --getss /dev/block/sda'
adb shell 'ls -l /dev/block/by-name/userdata'
adb shell 'for d in /sys/class/drm/*/; do ... done'
adb shell 'ls /sys/class/scsi_device/'
```
