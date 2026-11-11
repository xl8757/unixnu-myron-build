# 设备树匹配

驱动通过 IOKit personality 绑定到 provider。personality 是匹配键字典。

## personality 键

| 键 | 含义 |
| --- | --- |
| `IOClass` | 驱动类 |
| `IOProviderClass` | Provider 类 |
| `IONameMatch` | Provider 名称或 `compatible` 值 |
| `IOPropertyMatch` | Provider 属性精确匹配字典 |
| `IOProbeScore` | 分数越高越先 probe |
| `IOMatchCategory` | 匹配类别，允许第二个驱动匹配同一 provider |
| `IOResourceMatch` | probe 前必须发布的资源 |
| `IOProviderMergeProperties` | 加到 provider 的属性 |

没有 `IONameMatch` 或 `IOPropertyMatch` 的 personality 匹配该类的所有 provider，`IOProbeScore` 决定候选顺序。

## Provider 类

| Provider | 发布方 | 用途 |
| --- | --- | --- |
| `IOPlatformExpertDevice` | 平台专家 | 平台身份、product、buttons |
| `IOPlatformDevice` | `ArmDTPlatform` 为每个 `arm-io` 子节点发布 | SoC 设备 |
| `IOResources` | IOKit | AES、RAM 块等全局资源 |
| `IOMedia` | 分区和文件系统栈 | 根卷匹配 |
| `IOBlockStorageDevice` | 存储驱动 | 块设备客户端 |

`ArmDTPlatform` 把 nub 附加到平台专家上。nub 保留设备树父节点，`reg` 和 `ranges` 通过 `arm-io` 解析。

## 匹配示例

### 按 compatible 匹配平台设备

```c
"   ,{"
"     'IOClass'         = NativeUfs;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'qcom,sm8550-ufshc';"
"     'IOProbeScore'    = 1000:32;"
"   }"
```

`IONameMatch` 与 provider 名称和 `compatible` 字符串列表比较。

### 全局资源

```c
"   ,{"
"     'IOClass'         = NativeAES;"
"     'IOProviderClass' = IOResources;"
"     'IOResourceMatch' = IOBSD;"
"     'IOProbeScore'    = 1000:32;"
"   }"
```

`IOResourceMatch = IOBSD` 等到 BSD 子系统发布资源。

### 按 UUID 匹配根卷

```xml
<key>IOProviderClass</key><string>IOMedia</string>
<key>IOPropertyMatch</key>
<dict>
  <key>UUID</key><string>00000000-0000-0000-0000-000000000000</string>
</dict>
```

该字典存放在 `/chosen/root-matching`。

## 设备树节点

### 设备节点

```c
ufshc {
  name = "ufshc";
  compatible = "qcom,sm8550-ufshc";
  reg = <0x03ff0000 0x3000>;
  interrupts = <0 0x100 4>;
  status = "okay";
}
```

平台专家发布带这些属性的 nub，驱动从 provider 读取 `reg` 和 `interrupts`。

### 总线节点

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

子节点 `reg` 是 `ranges` 窗口内的偏移，平台专家解析物理地址。

## 读取属性

```cpp
OSData *data = OSDynamicCast(OSData, provider->getProperty("reg"));
if (data == nullptr) {
  return false;
}

const uint32_t *cells = (const uint32_t *)data->getBytesNoCopy();
uint32_t count = data->getLength() / sizeof(uint32_t);
```

cell 数量来自父节点。`#address-cells = <2>` 和 `#size-cells = <2>` 的节点每个范围使用四个 cell。

字符串属性：

```cpp
OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
const char *value = (const char *)compatible->getBytesNoCopy();
```

字符串列表是连续的 NUL 结尾字符串。

## overlay 与补丁

kit 可以应用设备树 overlay：

```toml
[[overlay]]
file = "board.dtbo"
```

配置可以修改属性：

```toml
[[dt_patch]]
op = "set"
node = "/arm-io/intc"
prop = "status"
value = "disabled"
```

补丁应用于 EFI 和 Linux 菜单项。Darwin 路径读取 `.adt` sidecar。

## probe 顺序

1. 平台专家发布 provider。
2. IOKit 收集 `IOProviderClass` 匹配的 personality。
3. `IONameMatch` 或 `IOPropertyMatch` 不匹配的项被跳过。
4. `IOResourceMatch` 对应的 provider 必须已发布。
5. 剩余候选按 `IOProbeScore` 排序。
6. 每个候选运行 `probe()`。
7. 启动成功的最高分候选获得 provider。

## 下一步

- [XNU 驱动](/zh/drivers/xnu-drivers)
- [设备树](/zh/porting/device-tree)
- [kext 包](/zh/drivers/kexts)
