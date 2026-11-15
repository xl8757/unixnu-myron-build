# Host verification

What has been run on a host, at which commit, with what result. UniXNU's
[completion levels](https://unixnu.com/porting/records#completion-levels) call
this the evidence a `host-verified` area needs: a command, the full source
commit, the result, and a log that can be reached. This file is that log.

Every entry below is a build, not a boot. Nothing here has been observed running
on the NX769J; `device-verified` needs a device command and a device log, and no
area claims one.

Host: Linux (WSL 2, Ubuntu 24.04), Python 3.12, the UniXNU Linux toolchain at
`~/.local/unixnu-linux` for `clang`, `lld-link` and `lzfse`, `dtc` and `mtools`
from the distribution.

## Revision

All four steps below ran at device-repository commit
[`dd97bc4b307f77df2f6189b7c92881133cc24ac3`](https://github.com/Asultop/UniXNU-NX769J/tree/dd97bc4b307f77df2f6189b7c92881133cc24ac3).
The inputs they read are pinned elsewhere and unchanged by this file:

| Input | Pin |
| --- | --- |
| XNU | `xnu-8796.101.5` at `aca3beaa3dfbd42498b42c5e5ce20a938e6554e5` |
| Restore image | `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, sha256 `c1091c40…7d52f2` |
| OS member | `096-04088-077.dmg`, sha256 `64354ba3…a3f290` |
| SoC source | `UniXNU-sm8550-common` at `23b2049fcd9e30356161def84231334fb55ecca9` |
| Mu-Silicium | `UniXNU/uefi-sheng` branch `sheng`, `nx769jPkg` staged from [`firmware/`](../firmware) |
| Vendor DXE drivers | 52 modules from `uefi.img`, sha256 `862c3c1a…900e804`, each pinned in [`proprietary-files.txt`](../proprietary-files.txt) |

## Resolution

```sh
./unixnu status --json -D ../UniXNU-NX769J
python3 scripts/porting-record.py check --device ../UniXNU-NX769J
```

| Result | Value |
| --- | --- |
| Patch series | resolves to the 64 entries of `patches/xnu/series`, in order |
| Components | every selected component resolves to a pinned revision |
| Record | `nx769j: 13 structured areas checked` |
| Repository | `scripts/check-repository.py` reports the structure consistent |

## Firmware

```sh
./unixnu build firmware -D ../UniXNU-NX769J --uefi ../uefi-nx769j
```

| Result | Value |
| --- | --- |
| Output | `Build/nx769jPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` |
| Size | 3145728 bytes = `0x300000`, exactly `[uefi_fd] size` |
| SHA-256 | `b06906387e20df18f2eef45267ccc11160af4b3ed8e30887a4b1c6d4df7a4fbc` |
| Preflight | `scripts/check-nx769j-firmware.py` reports `52/52 present`, tree consistent with `board.toml` |

The log exporter is in the volume: `AdvancedLogDxe.inf` builds as its own FFS,
`GenFfs` names it `8D4E7F50-3A2C-4C9A-9E1B-50C4C8472D41`, and
`FV/FVMAIN.inf` lists it, so it rides in the compressed `FvMain`. A byte scan of
the FD does not find its GUID or name for that reason; the FV manifest is what
shows it.

## userdata root image

```sh
./unixnu build userdata -D ../UniXNU-NX769J --ipsw <restore.ipsw>
```

| Result | Value |
| --- | --- |
| Output | `out/nx769j/ios-16.5-20F66/nx769j/userdata/userdata-root.img` |
| Size | 8357150720 bytes (7.78 GiB) in a partition of 494387671040 bytes |
| SHA-256 | `818e4e37bc9061ac8f5a2103234adb1c1b945fa1f2f4255ac148731b9ebf2631` |
| OS window | `096-04088-077.dmg`, 5628790272 bytes at `0x100000`, sha256 `484abb49…2021c8` |
| SystemOS window | `096-04666-078.dmg`, 2726297600 bytes at `0x14fa00000`, sha256 `2279bf44…afdd3` |

This image is what `scripts/extract-root-volume-uuid.py` reads, and it is
byte-identical across runs: the two windows are copies of pinned members and the
header is the same every time.

## Payload

```sh
./unixnu build payload -D ../UniXNU-NX769J --unique-chip-id 8abc5430 \
  --uefi ../uefi-nx769j --ipsw <restore.ipsw> --trust-cache <096-04088-077.dmg.trustcache> \
  --data-ramdisk <nx769j-data.dbr>
```

`--unique-chip-id`, `--trust-cache` and `--data-ramdisk` are all command-line
inputs and none of them is in the configuration, which is why the payload hash
changes even when the sources do not.

| Result | Value |
| --- | --- |
| Output | `out/nx769j/ios-16.5-20F66/nx769j/payload/payload.img` |
| Size | 218103808 bytes (208 MiB) |
| SHA-256 | `8f5d75ffda6cf8b25ddb2847a461c70c615b58fc10ad8cdae957f46851d9f497` |
| Data seed | `scratch-data.img` 970848 bytes, sha256 `e13f93e046ad2d60e8d3be706577be2d207df0c303b52682a2b54b6ad18da2bc` |
| Prelinked kernel | `kernel.macho` sha256 `2458882f89378ac7ead0cfd422d3201e088acdf2a1b4520ba199ff7cbe1819ef` |
| Trust cache | `df0f60fd05b7c312ad0fddb5c85d8adf97b487e8500518337bc4cbde3ca73ea7`, one of the two the profile pins |
| Menu | `EFI/UniXNU/unixnu.toml` lists `xnu` with `initrd = "Payloads/scratch-data.img"`, then `reset`, `shutdown` and `tools` |

## Boot image

```sh
./unixnu bootimg -D ../UniXNU-NX769J --uefi ../uefi-nx769j \
  --dtb ../uefi-nx769j/Resources/DTBs/nx769j.dtb \
  -o out/nx769j/ios-16.5-20F66/nx769j/boot/boot.img
```

| Result | Value |
| --- | --- |
| Output | `out/nx769j/ios-16.5-20F66/nx769j/boot/boot.img` |
| Size | 220344320 bytes |
| SHA-256 | `834403f3ba08c1e6da5a68eb738a97ac3a74478f60eec36e3a4bc67dd86a6a44` |
| Header | `ANDROID!`, `header_version` 0, `pagesize` 4096, kernel at `0x8000`, tags at `0x1e00000` |
| Packed FD | `fd_sha256` `b06906387e20df18f2eef45267ccc11160af4b3ed8e30887a4b1c6d4df7a4fbc`, the firmware volume above |
| Packed DTB | `dtb_sha256` `f336a7d70c72b8d4ac9c02c843b58bb49111903da154b012f8ff3c26dc7d38c7` |

`unixnu bootimg` round-trips the image and fails if any part does not match its
input, so the run is itself the check.

## What these results do and do not show

They show that the four build steps run to completion against the pinned inputs,
that the firmware volume is exactly the size the board declares, and that the
image packed into `boot.img` is the firmware that was built. They do not show
that anything starts, and they do not qualify the kernel:
`emit_prelink.py` still records `instruction_relocation_complete`,
`abi_qualified`, `loadable` and `runtime_qualified` as false.

Two of the outputs are not byte-reproducible and should not be expected to be:

| Output | Reproducible | Why |
| --- | --- | --- |
| `userdata-root.img` | yes | copies of pinned members in a fixed header |
| `SILICIUM_UEFI.fd` | **no** | two builds from identical sources gave `f5c3bc8f…` and `b0690638…` |
| `payload.img`, `boot.img` | **no** | the ADT carries `random-seed` of 64 random bytes, and it carries the FD |

The firmware is supposed to be reproducible — `firmware.py` pins
`SOURCE_DATE_EPOCH` to the Mu-Silicium commit time — so the two differing FDs
are an open question rather than an accepted property. The two runs were both
incremental against a reused `Build/` directory, which is the first thing to
rule out: build twice from a clean `Build/` and compare.

## The Data seed

Built on a hosted macOS runner, because every step needs `hdiutil`:
`.github/workflows/data-seed.yml` runs `scripts/build-data-seed.py` on
`macos-26` and uploads the seed as an artifact.

| Result | Value |
| --- | --- |
| Workflow run | [41026291337](https://github.com/Asultop/UniXNU-NX769J/actions/runs/41026291337) |
| Device revision | `ed45ee86452bc50735625fcf124fa2911d364ef9` |
| Artifact | `nx769j-data-seed`, 567485 bytes |
| Artifact sha256 | `589fb90de70a9d5b5e367464a66674c659c7bd32ee93ced445ead9f2f77136bd` |
| Data volume UUID | `DA9A01BC-22A9-47B0-8EA0-27B3C35E37E4` |
| Size | 256 MiB container, a RAM seed |

The volume UUID is new on every run: the seed is formatted from scratch each
time, so it is recorded here rather than pinned anywhere. Whether the kernel
needs it configured depends on the block driver that publishes the seed window;
take it from this table, not from a previous run.

Two things about the artifact are worth knowing. It is uploaded **unencrypted**,
because the `ARTIFACT_PASSPHRASE` secret is not set and the workflow refuses to
invent one; it holds files taken from the iOS restore image. And downloading an
artifact needs a token even for a public repository. `gh` does it:

```sh
gh run download 41026291337 -R Asultop/UniXNU-NX769J -n nx769j-data-seed -D out/seed
```

That produced `nx769j-data.dbr`, 970848 bytes, sha256
`e13f93e046ad2d60e8d3be706577be2d207df0c303b52682a2b54b6ad18da2bc`, matching the
report the run wrote. It holds 36 files and 589907 bytes of the iOS 16.5 OS
volume's initial `/private/var`, in a 256 MiB container. The seed's own
provenance is `os_launchd_sha256`
`4c61512d19b9a55a6ddad27e23b88fc5008fbf77d0c0b5e6bdd7c7d90cef3e06`, which is the
launchd of the iOS 16.5 OS image and not the iPadOS one UniXNU's fixture pins.

### Published

`gh repo edit Asultop/UniXNU-NX769J --add-topic unixnu` added the topic the
[device list](https://unixnu.com/devices/) searches on. The catalog workflow
that generates the device page runs twice a day on its own and cannot be
dispatched from here: `gh workflow run sync-devices.yml --repo UniXNU/UniXNU`
returns 403, which is a different repository's admin boundary rather than a
configuration problem.

### How the run got there

Nine runs failed before it produced anything, and job logs need repository
access, so each one was diagnosed from the run page's annotations instead. In
order: the cache key contained a comma, `tee` masked the script's exit code, the
seed was made at 32 MiB and then grown, `packed_ramdisk.py` was missing because
`drivers/DarwinBoot` is a UniXNU submodule that `actions/checkout` does not
initialise, the copy loop called `Path.symlink` where the method is
`symlink_to`, and finally the log redirect ran before `out/` existed so the
script never started. The `::error::` reporting and the per-step `ERR` trap that
named each of the last four are in the workflow and stay there.
