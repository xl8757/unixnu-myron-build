# UEFI 固件

UEFI 固件移植来自 Mu-Silicium，提供 UniXNU 和 DarwinBoot 使用的服务。

## 必需服务

| 服务 | 使用方 |
| --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | UniXNU 菜单、DarwinBoot framebuffer 交接、XNU 控制台 |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | UniXNU 菜单输入 |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` | 配置、负载和内核文件 |
| `EFI_BLOCK_IO_PROTOCOL` | userdata 分区和块设备切片 |
| `EFI_SERIAL_IO_PROTOCOL` | 有 UART 的板级日志 |
| `EFI_ACPI_TABLE_PROTOCOL` | 可选 ACPI 表注入 |
| `EFI_FIRMWARE_VOLUME2_PROTOCOL` | UniXNUDxe 启动选项注册 |

缺少某项服务会限制对应阶段。把驱动加入固件卷或 UniXNU 负载后再运行依赖阶段。

## Mu-Silicium 布局

```
Mu-Silicium/
  BootShim/
    BootShim.S
  Resources/
    Configs/<device>.toml
    Scripts/mkbootimg.py
  Platforms/<Vendor>/<device>Pkg/
    <device>.dsc
    <device>.fdf
  Silicon/
  Common/
```

UniXNU 驱动读取 `Resources/Configs/<name>.toml` 和 `<device>.dsc`，启动镜像使用 `Resources/Scripts/mkbootimg.py`。

## 设备配置

`Resources/Configs/<device>.toml` 提供地址和镜像参数：

```toml
[boot_shim]
requires_kernel_header = true

[uefi_fd]
base = 0x00000000
size = 0x000000

[boot_image_kernel]
kernel_compression = "gzip"
append_dtb = true

[boot_image]
header_version = 0
base = 0x00000000
kernel_offset = 0x8000
tags_offset = 0x1e00000
pagesize = 4096
```

| 键 | 含义 |
| --- | --- |
| `boot_shim.requires_kernel_header` | bootloader 检查镜像开头的 ARM64 header |
| `uefi_fd.base` | 固件卷加载物理地址 |
| `uefi_fd.size` | 固件卷精确大小 |
| `boot_image_kernel.kernel_compression` | shim 和固件卷的压缩方式 |
| `boot_image_kernel.append_dtb` | 把设备树追加到 kernel 字段 |
| `boot_image.*` | Android 启动镜像 header 参数 |

固件基址和大小编译进 BootShim，并在打包启动镜像时检查。

## DSC 与 FDF

DSC 列出 package、库和组件，FDF 把组件放入固件卷。

UniXNU 使用 `PACKAGES_PATH`：

```
{uefi}:{tree}/package:{tree}/drivers/DarwinBoot:{edk2}:...
```

`{uefi}` 是 Mu-Silicium 仓库，`{tree}` 是组装后的构建树，`{edk2}` 是 Mu_Basecore。板级文件列出全部 package 路径。

UniXNU 和 DarwinBoot 通过 INF 加入：

```
UniXNUPkg/Application/UniXNU/UniXNU.inf
UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
DarwinBootPkg/Driver/DarwinBoot.inf
```

FDF 决定它们嵌入固件卷还是复制到 ESP。

## 预检

`[firmware] preflight` 列出 EDK II 构建前运行的构建树脚本，用于检查厂商输入、固件分区或板级文件。`--skip-preflight` 跳过。

## 厂商输入

Mu-Silicium 移植需要板级厂商二进制，例如：

- 存储、显示、输入或电源的 UEFI DXE 驱动。
- ACPI 表。
- 固件分区镜像。
- 配置数据。

板级预检脚本校验文件和哈希。

## 子模块与补丁

Mu-Silicium 使用子模块：

```toml
[firmware]
submodules = ["Common/Mu", "Silicon/Silicium", "Silicon/Qualcomm"]
patches = ["Resources/MuPatches/Gic-Spurious.patch"]
```

`patches` 中的补丁在构建期间应用到共享 edk2 树，之后还原。驱动在应用前运行 `git apply --check`。

## 构建

```sh
./unixnu build firmware --uefi /path/to/Mu-Silicium
```

驱动：

1. 运行预检脚本。
2. 初始化列出的子模块。
3. 缺少 `GenFw` 时构建 BaseTools。
4. 应用板级补丁。
5. 用 DSC、`AARCH64`、`CLANGPDB` 和板级 defines 运行 EDK II `build`。
6. 还原补丁。
7. 检查固件卷大小和路径。

输出路径来自 `[firmware] fd`：

```toml
fd = "Build/<device>Pkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd"
```

## 独立构建

Mu-Silicium 也可以用自己的脚本构建：

```sh
cd Mu-Silicium
python3 build_uefi.py -d <device>
```

UniXNU 驱动使用板级 defines 和共享 edk2 补丁执行同样的构建。

## 启动镜像集成

`unixnu bootimg` 用固件基址和大小编译 BootShim：

```sh
clang --target=aarch64-none-elf -c \
  -DFD_BASE=<base> -DFD_SIZE=<size> \
  -DREQUIRES_KERNEL_HEADER=<0 or 1> \
  BootShim/BootShim.S -o shim.o
```

它追加固件卷，用 gzip 压缩，追加设备树，再打包 Android 启动镜像。

## 验证

| 检查 | 方法 |
| --- | --- |
| 固件卷大小 | 驱动与 `[uefi_fd] size` 比较 |
| shim header | 检查 `ARM\x64` 及基址和大小字段 |
| 启动镜像部分 | 解包并逐部分比较 |
| 菜单 | 串口出现 `UNIXNU:` 行 |
| 负载 | 菜单列出 `xnu` 项，DarwinBoot 打印交接信息 |

## 添加 UEFI 驱动

驱动可以放在固件卷或负载中。

| 位置 | 构建 | 加载 |
| --- | --- | --- |
| 固件卷 | 把 INF 加入 DSC 和 FDF | 固件 dispatch，或 UniXNUDxe 启动应用 |
| 负载 | 构建独立 `.efi` | `unixnu.toml` 的 `[[driver]]` 或 kit |

INF、入口和注入细节见 [UEFI 驱动](/zh/drivers/uefi-drivers)。

## 下一步

- [设备树](/zh/porting/device-tree)
- [bring-up 顺序](/zh/porting/bring-up)
- [UEFI 驱动](/zh/drivers/uefi-drivers)
