# Driver layers

UniXNU has three driver layers. Each layer runs in a different environment and has a different build and load path.

| Layer | Runs in | Built as | Loaded by |
| --- | --- | --- | --- |
| UEFI driver | UEFI boot services | EDK II `.efi` | Firmware dispatch, UniXNU `[[driver]]`, or a kit |
| Built-in XNU driver | XNU kernel | Static archive linked into the kernel | The kernel at boot |
| Kext bundle | XNU kernel, staged by UEFI | `.kext` bundle | An XNU loader that implements the protocol |

## Choosing a layer

| Need | Layer |
| --- | --- |
| Display, input or storage available before XNU | UEFI driver |
| Platform expert, CPU, interrupt controller, storage, display or GPU inside XNU | Built-in XNU driver |
| A bundle that an external loader validates and injects at runtime | Kext bundle |

A UEFI driver can provide the framebuffer and input for the UniXNU menu. It can also publish a protocol that another UEFI driver consumes.

A built-in XNU driver has full kernel API access. It is compiled against the exact XNU build ABI and force-loaded into the kernel.

A kext bundle keeps the standard bundle layout. UniXNU stages the plist and the executable and passes them to the loader. The loader owns validation, dependency resolution and injection.

## Matching

UEFI drivers bind through the UEFI driver model or run as applications.

XNU drivers bind through `IOKitPersonalities`. A personality names the provider class, the driver class, an optional name match and a probe score.

| Key | Meaning |
| --- | --- |
| `IOClass` | Driver class |
| `IOProviderClass` | Provider class |
| `IONameMatch` | Provider name or `compatible` value |
| `IOProbeScore` | Higher scores probe first |
| `IOMatchCategory` | Allows a second driver to match the same provider |
| `IOResourceMatch` | Waits for a named resource, for example `IOBSD` |

Built-in personalities live in `iokit/KernelConfigTables.cpp`. Kext personalities live in the bundle `Info.plist`.

## Device tree

The ADT describes the hardware. The platform expert publishes a nub for each `arm-io` child. A driver reads its resources from the provider:

```cpp
OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
```

The `compatible` property selects a driver. `IONameMatch` compares against the provider name.

## Driver directories

| Directory | Contents |
| --- | --- |
| `drivers/<Name>` | Built-in XNU driver source and reference `Info.plist` |
| `kits/<Name>` | Kernel library linked into a driver or the kernel |
| `package/UniXNUPkg/Driver` | UEFI drivers |
| `package/UniXNUPkg/Test` | UEFI test drivers and payloads |

Drivers and kits are independent repositories included as submodules. A device repository can override any of these directories with its own version.

## Next steps

- [UEFI drivers](/drivers/uefi-drivers)
- [XNU drivers](/drivers/xnu-drivers)
- [Adreno 740 driver](/drivers/native-adreno)
- [Kext bundles](/drivers/kexts)
- [Device tree matching](/drivers/device-tree-matching)
