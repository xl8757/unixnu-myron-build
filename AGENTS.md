# Agent instructions

This repository owns the NX769J board configuration: hardware description,
component selection, firmware and partition policy, and packaging. Shared SM8550
resources and operations belong to `UniXNU-sm8550-common`. The documentation
site, component catalog and common build tools belong to `UniXNU`.

Read the following before editing:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [repository format](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Hardware | [Device trees](https://unixnu.com/porting/device-tree), [matching](https://unixnu.com/drivers/device-tree-matching), the pinned SoC README and `sm8550.dtsi` |
| Components | Selected component README, catalog metadata, build interface and relevant source |
| Kernel | Selected OS profile, XNU source, [patch reference](https://unixnu.com/xnu/source-and-patches), ordered patch series |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [UEFI firmware](https://unixnu.com/porting/uefi-firmware) |
| Verification | [Building](https://unixnu.com/building), [bring-up](https://unixnu.com/porting/bring-up) |
| Migration | [Migration guide](https://unixnu.com/porting/migration) |

Search UniXNU's `components.toml` and read existing implementations before
adding a capability. Select components by interface, hardware scope, OS ABI and
board dependencies. Use canonical component names in sources, paths, build
commands and dependencies. Preserve source filenames, runtime classes, bundle
IDs, plugin filenames, symbols and protocol IDs.

Use `dts/nx769j.dts` for board hardware and include the selected SoC DTSI.
Inspect `local.toml` and CLI overrides before changing source pins.

Run from the UniXNU checkout:

```sh
./unixnu defconfig nx769j_defconfig --device ../UniXNU-NX769J
./unixnu status --device ../UniXNU-NX769J --json
./unixnu tree --device ../UniXNU-NX769J
./unixnu build kernel --device ../UniXNU-NX769J
```

Use the README for firmware and payload inputs. Run existing checks relevant to
the change. Report commands, outputs, missing inputs and hardware observations
separately. Record device boot, storage, display, input and GPU checkpoints.

Write concise English for this repository. Describe current behavior with direct
statements and ordinary punctuation.

## Board facts that are easy to get wrong

- The target is **iOS 16.5 (build 20F66, Darwin 22.5.0, XNU 8796)** with an
  iPhone X identity, not the iPadOS 17.7.11 profile UniXNU ships. The profile
  is `os-profiles/ios-16.5-20F66/` and has to land in `UniXNU/profiles/os/`
  before a build resolves. Its prelink ABI was read out of the 16.5 kernelcache
  by `scripts/extract-ipsw-pins.py`; never copy those values from another
  profile, they are version bound.
- The whole patch order targets `xnu-10063.141.1` and does not apply to
  `xnu-8796` yet. The OS series is deliberately empty for that reason; a
  version-mismatched patch path makes `unixnu status` abort with
  `patch missing`.
- The SoC source is `UniXNU-sm8550-common`. Its register map matches SM8650 for
  every resource this port drives, but its CPU core names and Adreno generation
  do not. Keep the per-node overrides in `dts/nx769j.dts` and
  `configs/nx769j_defconfig` in place until an `UniXNU-sm8650-common` exists.
- SM8650 CPU clusters are 2x Cortex-A520 (E) then 5x Cortex-A720 and 1x
  Cortex-X4 (P). SM8550 is 3x A510 then A715, A710 and X3. The MPIDR layout is
  unchanged at 0x100 spacing; only `compatible` differs.
- `soc:configs/sm8550.config` sets `CONFIG_XNU_NATIVE_ADRENO=y`. The device
  defconfig is applied last and turns it off. Do not drop that line.
- The boot image is header version 4. The Android 14 ABL rejects a version 0
  image.
- Nothing in this port is device verified. Do not raise an area's status without
  the evidence its level requires.

## Porting record

Read `porting.toml` before implementation. Identify the affected areas and their
existing components, then update the implementation and each affected area's
findings or evidence in the same change. Update configuration, procedure and
completion criteria when an interface changes.

Every area needs the `evidence` key even when it is empty. A changed code path
must be owned by an area's `paths`, and each owning area must gain a new
`findings` or `evidence` entry with that change.

```sh
python3 scripts/porting-record.py check --device ../UniXNU-NX769J
python3 scripts/porting-record.py check --device ../UniXNU-NX769J --base origin/main
```
