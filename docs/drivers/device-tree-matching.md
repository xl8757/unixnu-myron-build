# Device tree matching

A driver binds to a provider through an IOKit personality. The personality is a dictionary of match keys.

## Personality keys

| Key | Meaning |
| --- | --- |
| `IOClass` | Driver class |
| `IOProviderClass` | Provider class |
| `IONameMatch` | Provider name or `compatible` value |
| `IOPropertyMatch` | Dictionary of exact provider property values |
| `IOProbeScore` | Higher scores probe first |
| `IOMatchCategory` | Match category, which allows a second driver on the same provider |
| `IOResourceMatch` | Resource that must be published before probing |
| `IOProviderMergeProperties` | Properties added to the provider |

A personality without `IONameMatch` or `IOPropertyMatch` matches every provider of the named class. `IOProbeScore` decides which candidate wins.

## Provider classes

| Provider | Published by | Use |
| --- | --- | --- |
| `IOPlatformExpertDevice` | Platform expert | Platform identity, product and buttons |
| `IOPlatformDevice` | `ArmDTPlatform` for every `arm-io` child | SoC devices |
| `IOResources` | IOKit | Global resources such as AES or RAM block |
| `IOMedia` | Partition and file system stack | Root volume matching |
| `IOBlockStorageDevice` | Storage driver | Block device clients |

`ArmDTPlatform` attaches a nub to the platform expert for each `arm-io` child. The nub keeps the device tree parent, so `reg` and `ranges` resolve through `arm-io`.

## Matching examples

### Platform device by compatible

```c
"   ,{"
"     'IOClass'         = NativeUfs;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'qcom,sm8550-ufshc';"
"     'IOProbeScore'    = 1000:32;"
"   }"
```

`IONameMatch` compares against the provider name and the `compatible` string list.

### Global resource

```c
"   ,{"
"     'IOClass'         = NativeAES;"
"     'IOProviderClass' = IOResources;"
"     'IOResourceMatch' = IOBSD;"
"     'IOProbeScore'    = 1000:32;"
"   }"
```

`IOResourceMatch = IOBSD` waits until the BSD subsystem publishes its resource.

### Root volume by UUID

```xml
<key>IOProviderClass</key><string>IOMedia</string>
<key>IOPropertyMatch</key>
<dict>
  <key>UUID</key><string>00000000-0000-0000-0000-000000000000</string>
</dict>
```

This dictionary is stored in `/chosen/root-matching`.

## Device tree nodes

### Device node

```c
ufshc {
  name = "ufshc";
  compatible = "qcom,sm8550-ufshc";
  reg = <0x03ff0000 0x3000>;
  interrupts = <0 0x100 4>;
  status = "okay";
}
```

The platform expert publishes a nub with these properties. The driver reads `reg` and `interrupts` from the provider.

### Bus node

```c
arm-io {
  name = "arm-io";
  device_type = "arm-io";
  #address-cells = <2>;
  #size-cells = <2>;
  ranges = <0 0x17000000 0x08000000>;
  intc {
    name = "intc";
    compatible = "arm,gic-v3";
    reg = <0x0 0x10000 0x0 0x10000>;
  };
}
```

A child `reg` is an offset inside the `ranges` window. The platform expert resolves the physical address.

## Reading properties

```cpp
OSData *data = OSDynamicCast(OSData, provider->getProperty("reg"));
if (data == nullptr) {
  return false;
}

const uint32_t *cells = (const uint32_t *)data->getBytesNoCopy();
uint32_t count = data->getLength() / sizeof(uint32_t);
```

The number of cells comes from the parent node. A node under `#address-cells = <2>` and `#size-cells = <2>` uses four cells per range.

A string property:

```cpp
OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
const char *value = (const char *)compatible->getBytesNoCopy();
```

The string list is a sequence of NUL-terminated strings.

## Overlays and patches

A kit can apply a device tree overlay:

```toml
[[overlay]]
file = "board.dtbo"
```

A configuration can patch a property:

```toml
[[dt_patch]]
op = "set"
node = "/arm-io/intc"
prop = "status"
value = "disabled"
```

Patches apply to EFI and Linux entries. The Darwin path reads the `.adt` sidecar.

## Probe order

1. The platform expert publishes the provider.
2. IOKit collects personalities whose `IOProviderClass` matches.
3. Personalities whose `IONameMatch` or `IOPropertyMatch` fails are skipped.
4. `IOResourceMatch` providers must be published.
5. The remaining candidates are ordered by `IOProbeScore`.
6. Each candidate runs `probe()`.
7. The highest scoring candidate that starts owns the provider.

## Next steps

- [XNU drivers](/drivers/xnu-drivers)
- [Device tree](/porting/device-tree)
- [Kext bundles](/drivers/kexts)
