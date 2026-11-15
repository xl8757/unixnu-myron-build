# Staged Mu-Silicium files for `uefi-nx769j`

These are the files that are new or changed for this board in a Mu-Silicium
checkout. They are staged here so the firmware design is reviewable and so
creating the `uefi-nx769j` checkout is a copy rather than an authoring task.

Nothing here is built in this repository; the tree is what gets lifted across.
`Mu_Basecore`, the edk2 checkout, is a separate submodule and has to be
initialised as well, because `firmware.py` builds against it rather than against
a tree of its own:

```sh
git clone https://github.com/UniXNU/uefi-sheng uefi-nx769j
cd uefi-nx769j && git checkout -b nx769j
cp -R ../UniXNU-NX769J/firmware/* .
git submodule update --init --depth 1 -- \
  Common/Mu Common/Mu_OEM_Sample Silicium-ACPI Mu_Basecore \
  Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
```

`firmware/` is laid out as it lands in the checkout, so the copy is a plain
`cp -R`.

## Staged

Everything the package adds is here, so `cp -R firmware/* .` is the whole step.

| Staged path | Origin |
| --- | --- |
| `Resources/Configs/nx769j.toml` | derived from `nx729j.toml`, `header_version` 4 |
| `Platforms/ZTE/nx769jPkg/nx769j.dsc` | derived from `nx729jPkg/nx729j.dsc` |
| `Platforms/ZTE/nx769jPkg/nx769j.fdf` | derived from `nx729jPkg/nx729j.fdf` |
| `Platforms/ZTE/nx769jPkg/{nx769j.dec,DeviceBuild.py}` | `nx729jPkg`'s, renamed |
| `Platforms/ZTE/nx769jPkg/Include/{APRIORI,DXE,RAW}.inc` | `nx729jPkg/Include/`'s, with the `Binaries/nx729j` paths renamed to `Binaries/nx769j` |
| `Platforms/ZTE/nx769jPkg/Library/MemoryMapLib/` | **this board's map**, rebuilt from the firmware device tree |
| `Platforms/ZTE/nx769jPkg/Library/ConfigurationMapLib/` | `nx729jPkg/Library/`'s, renamed |

## Not staged, and why

Both of these come out of the device's own firmware and are not redistributable,
so they are extracted onto the build machine rather than committed.

| File | How to produce it |
| --- | --- |
| `Resources/DTBs/nx769j.dtb` | `python3 scripts/extract-firmware-dtb.py --vendor-boot vendor_boot.img --tree 2 -o Resources/DTBs/nx769j.dtb`. Tree 2 is "Pineapple v2 SoC", which this board matches by `qcom,msm-id`; all four Pineapple trees carry the same reservation set, so the memory map does not depend on the choice. |
| `Binaries/nx769j/QcomPkg/Drivers/…` | `python3 scripts/extract-uefi-drivers.py --image uefi.img --layout <this checkout>`. All 52 modules the platform includes name come out of `uefi.img`, each written with its EDK II INF wrapper and depex. [`proprietary-files.txt`](../proprietary-files.txt) pins the hash of every one. |

`scripts/check-nx769j-firmware.py` reports exactly which of these are still
missing and whether they are fatal, and is what `./unixnu build firmware` runs
before EDK II. With both present it reports the tree as consistent with
`board.toml` and `52/52` vendor drivers.

## The memory map

`MemoryMapLib.c` is this board's map, rebuilt from the reservation list in
`vendor_boot.img`'s device tree rather than carried over from `nx729jPkg`. All
36 fixed-address reservations have a descriptor named after their node, and the
regions UEFI lays out for itself sit in the free band the board actually has.

The full reservation list comes from `vendor_boot.img`, whose `dtb` field holds
six device trees; four are Pineapple variants with byte-identical reservation
sets, and [`scripts/extract-firmware-dtb.py`](../scripts/extract-firmware-dtb.py)
prints all of them. The list is 49 nodes, 36 at fixed addresses, and is in
[`device-inputs.md`](../device-inputs.md#fw-dtb).

It is checked rather than reviewed:

```sh
python3 scripts/check-memory-map.py --dtb Resources/DTBs/nx769j.dtb
# check-memory-map: 64 descriptors, 37 fixed reservations, 37 covering one, 0 unexplained
```

37 rather than 36 because `board.toml [[bootimg.dtb_node]]` adds the contiguous
splash region to the boot image's device tree, and the script reads that table
too. The comparison refuses an uncovered reservation, two descriptors fighting
over one range, a descriptor covering nothing that is not a UEFI region, and any
DDR descriptor reaching outside DRAM; `scripts/test_check_memory_map.py` covers
each of those.

A map that reserves the wrong ranges lets the firmware write over what the
bootloader or TrustZone owns, which is why this is a gate and not a note. See
[`docs/UEFI-FIRMWARE.md`](../docs/UEFI-FIRMWARE.md) for the derivation and for
what is still open.

## The one line that is easy to get wrong

`nx729j.dsc` includes `KailuaPkg/KailuaPkg.dsc.inc`. That is SM8550. NX769J is
SM8650, whose Mu-Silicium SoC package is **`LanaiPkg`** — named after Qualcomm's
platform codename, not the part number. The staged DSC includes `LanaiPkg` and
sets `SOC_TYPE = 2` (`SM8650-AB`).

The device agrees: its baseband is `LANAI_GEN_PACK` and its trustzone is
`LANAIAAAAANAZT`, and `LanaiPkg.dsc.inc` declares the GIC bases
`0x17100000` / `0x17180000` that this board measures.

## Provenance

Every value in these files, and the values still open, are in
[`docs/UEFI-FIRMWARE.md`](../docs/UEFI-FIRMWARE.md). The working reference is
`Platforms/ZTE/nx729jPkg` in
[`UniXNU/uefi-sheng`](https://github.com/UniXNU/uefi-sheng), branch `sheng`.
