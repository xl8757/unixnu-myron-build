# 总览

UniXNU 在 ARM64 设备上从 UEFI 环境启动 iOS 或 macOS。整个栈分为六层，每一层都有对应的仓库、构建产物和交接约定。

## 分层模型

```
Android bootloader (ABL)
  |
  |  fastboot boot boot.img
  v
BootShim + UEFI 固件卷 + 设备树
  |
  |  UEFI BDS 启动 UniXNU
  v
UniXNU（UEFI 启动管理器）
  |
  |  UNIXNU_LOADER_PROTOCOL
  v
DarwinBoot（XNU 加载器）
  |
  |  boot_args、ADT、trust cache、ramdisk
  v
打过补丁的 XNU 内核
  |
  |  IOKit、BSD、launchd
  v
来自 IPSW 的用户态（OS 与 SystemOS）
```

## 组件

### 启动镜像

启动镜像是 header version 0 的 Android boot image。kernel 字段存放 gzip 数据，由两部分组成：编译后的 BootShim 和 UEFI 固件卷。gzip 流之后是设备树。ramdisk 字段存放 UniXNU 负载。

设备原有的 Android bootloader 按 Mu-Silicium 设备配置中的地址加载镜像，校验 ARM64 image header，然后跳转到 shim。BootShim 重定位固件卷并进入 UEFI。

### UEFI 固件

固件来自设备的 Mu-Silicium 仓库，提供启动管理器所需的服务：

- `EFI_GRAPHICS_OUTPUT_PROTOCOL`：菜单和内核 framebuffer。
- `EFI_SIMPLE_TEXT_IN_PROTOCOL`：菜单输入。
- `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` 与 `EFI_BLOCK_IO_PROTOCOL`：负载卷和磁盘分区。
- `EFI_SERIAL_IO_PROTOCOL`：板级 UART 日志。

`unixnu build firmware` 会把板级补丁应用到共享 edk2 树，构建板级 DSC，并返回 `SILICIUM_UEFI.fd`。

### 启动管理器

UniXNU 启动管理器是由 `package/UniXNUPkg` 构建的 UEFI 应用。它先从启动所在卷读取 `\EFI\UniXNU\unixnu.toml`，再扫描其他卷。配置声明菜单项、驱动、设备树补丁、ACPI 表、块设备切片和 kit。

菜单出现前，启动管理器完成五件事：

1. 把每个 `[[block_slice]]` 发布为块设备。
2. 加载并启动配置中的 UEFI 驱动。
3. 安装配置中的 ACPI 表。
4. 合并 `[match]` 条件满足的 kit。
5. 为选中项准备设备树。

类型为 `xnu` 的菜单项会交给实现 `UNIXNU_LOADER_PROTOCOL` 的加载器。

### DarwinBoot

DarwinBoot 是固件卷中的 UEFI 驱动，在 EL2 或 EL1 安装加载器协议。处理 `xnu` 菜单项时执行以下步骤：

1. 从负载卷读取内核 Mach-O。
2. 读取内核旁边的 `.adt` sidecar。
3. 读取可选的 `.trustcache` sidecar。
4. 校验 Mach-O 段、ADT 和 trust cache 结构。
5. 为内核及其数据分配物理窗口。
6. 复制内核段、ADT、trust cache 和 ramdisk。
7. 构造 Darwin `boot_args`。
8. 退出 boot services，关闭 MMU 和缓存，在 EL1 跳转到内核入口。

加载器要求单个 ARM64 Mach-O，包含 `__TEXT` 段、`LC_MAIN` 入口，基地址按固件页大小对齐。

### XNU 内核

内核由 Apple `xnu-10063.141.1` 源码加上 `patches/xnu` 中的补丁构建。BCM2837 机型配置提供非 Apple ARM64 板所需的 4 KiB 页布局。

构建出的原生内核会与目标 IPSW 的原始 kernelcache 预链接。预链接保留原始 Apple kext，并把原生内核绑定到它们的符号。结果是单个 `kernel.macho`，包含原生内核段和选中的原始组件。

### 用户态

userdata 镜像保存 IPSW 中未修改的 `OS` 和 `Cryptex1,SystemOS` 磁盘镜像。分区开头有一个 4 KiB manifest，描述两个窗口。原生 UFS 或块驱动发布分区，XNU 按 UUID 挂载根卷。

## 仓库

| 仓库 | 用途 |
| --- | --- |
| `UniXNU` | 构建驱动、UEFI 启动管理器、DarwinBoot、通用驱动、kit、补丁和文档 |
| `UniXNU-<device>` | 板级描述、内核 defconfig、设备脚本、设备驱动和 package |
| `uefi-<device>` | 设备的 Mu-Silicium UEFI 移植 |

设备仓库的目录布局与 `UniXNU` 相同。它的 `drivers/`、`kits/`、`patches/`、`scripts/` 和 `package/` 会覆盖同名通用模块。

## 构建产物

所有输出位于 `out/`：

```
out/
  state.json                 记住的设备仓库
  feeds/                     拉取的 git 源码
  ipsw/<build>/              解压的 IPSW 成员
  <board>/
    .config                  选中的 defconfig
    tree/                    组装后的构建树
    logs/                    每一步的日志
    kernel/kernel.macho      预链接内核
    package/Build/Out/       UniXNU 和测试 EFI 程序
    payload/payload.img      FAT 负载卷
    payload/payload.json     负载报告
    boot/boot.img            Android 启动镜像
    boot/boot.json           启动镜像报告
    userdata/userdata-root.img
    userdata/userdata-root.json
```

报告记录输入哈希和生成下一步的命令。给定固件卷、设备树和 ramdisk 时，启动镜像是确定性的。ADT 包含随机种子，因此两次负载构建的结果不同。

## 下一步

- [构建环境](/zh/guide/requirements)列出主机工具和外部输入。
- [构建](/zh/building)介绍驱动命令。
- [移植](/zh/porting/)介绍如何新增设备。
- [设备](/zh/devices/)列出设备仓库。sheng 页面记录一个设备，通用指南使用占位符。
