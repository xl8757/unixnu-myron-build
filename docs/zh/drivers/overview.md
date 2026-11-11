# 驱动分层

UniXNU 有三层驱动，分别运行在不同环境，构建和加载方式不同。

| 层 | 运行环境 | 构建产物 | 加载方 |
| --- | --- | --- | --- |
| UEFI 驱动 | UEFI boot services | EDK II `.efi` | 固件 dispatch、UniXNU `[[driver]]` 或 kit |
| 内置 XNU 驱动 | XNU 内核 | 链接进内核的静态库 | 内核启动时 |
| kext 包 | XNU 内核，由 UEFI 暂存 | `.kext` 包 | 实现协议的 XNU 加载器 |

## 选择层次

| 需求 | 层次 |
| --- | --- |
| XNU 启动前可用的显示、输入或存储 | UEFI 驱动 |
| XNU 内的平台专家、CPU、中断控制器、存储、显示或 GPU | 内置 XNU 驱动 |
| 由外部加载器在运行时校验并注入的包 | kext 包 |

UEFI 驱动可以为 UniXNU 菜单提供 framebuffer 和输入，也可以发布供其他 UEFI 驱动使用的协议。

内置 XNU 驱动可以使用完整内核 API，按实际 XNU 构建 ABI 编译，并强制链接进内核。

kext 包保持标准 bundle 布局。UniXNU 暂存 plist 和可执行文件并交给加载器，校验、依赖解析和注入由加载器负责。

## 匹配

UEFI 驱动通过 UEFI driver model 绑定，或作为应用运行。

XNU 驱动通过 `IOKitPersonalities` 绑定。personality 指定 provider 类、驱动类、可选名称匹配和 probe 分数。

| 键 | 含义 |
| --- | --- |
| `IOClass` | 驱动类 |
| `IOProviderClass` | Provider 类 |
| `IONameMatch` | Provider 名称或 `compatible` 值 |
| `IOProbeScore` | 分数越高越先 probe |
| `IOMatchCategory` | 允许第二个驱动匹配同一 provider |
| `IOResourceMatch` | 等待命名资源，例如 `IOBSD` |

内置 personality 位于 `iokit/KernelConfigTables.cpp`，kext personality 位于 bundle 的 `Info.plist`。

## 设备树

ADT 描述硬件。平台专家为每个 `arm-io` 子节点发布 nub。驱动从 provider 读取资源：

```cpp
OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
```

`compatible` 属性用于选择驱动，`IONameMatch` 与 provider 名称比较。

## 驱动目录

| 目录 | 内容 |
| --- | --- |
| `drivers/<Name>` | 内置 XNU 驱动源码和参考 `Info.plist` |
| `kits/<Name>` | 链接进驱动或内核的内核库 |
| `package/UniXNUPkg/Driver` | UEFI 驱动 |
| `package/UniXNUPkg/Test` | UEFI 测试驱动和负载 |

驱动和 kit 是独立仓库，以子模块形式引入。设备仓库可以覆盖其中任何目录。

## 下一步

- [UEFI 驱动](/zh/drivers/uefi-drivers)
- [XNU 驱动](/zh/drivers/xnu-drivers)
- [Adreno 740 驱动](/zh/drivers/native-adreno)
- [kext 包](/zh/drivers/kexts)
- [设备树匹配](/zh/drivers/device-tree-matching)
