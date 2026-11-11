# kext 包

kext 包是标准 macOS 或 iOS 内核扩展目录。UniXNU 暂存包文件，并通过 `UNIXNU_LOADER_PROTOCOL` 交给 XNU 加载器。

## 包布局

```
MyDriver.kext/
  Contents/
    Info.plist
    MacOS/
      MyDriver
```

包名以 `.kext` 结尾。plist 默认路径为 `Contents/Info.plist`，可执行文件路径默认取 `CFBundleExecutable`，相对包目录。

无代码 kext 只有 plist，可以声明依赖或引用其他二进制的 personality。

## Info.plist

```xml
<dict>
  <key>CFBundleIdentifier</key><string>org.example.MyDriver</string>
  <key>CFBundleName</key><string>MyDriver</string>
  <key>CFBundleExecutable</key><string>MyDriver</string>
  <key>CFBundlePackageType</key><string>KEXT</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>OSBundleRequired</key><string>Root</string>
  <key>OSBundleLibraries</key>
  <dict>
    <key>com.apple.kpi.iokit</key><string>23.6.0</string>
  </dict>
  <key>IOKitPersonalities</key>
  <dict>
    <key>MyDevice</key>
    <dict>
      <key>IOClass</key><string>MyDriver</string>
      <key>IOProviderClass</key><string>IOPlatformDevice</string>
      <key>IONameMatch</key><string>my-device</string>
      <key>IOProbeScore</key><integer>1000</integer>
    </dict>
  </dict>
</dict>
```

## 注册到 UniXNU

菜单项级 kext：

```toml
[[entry]]
id = "xnu"
type = "xnu"
path = "Payloads/kernel.macho"

[[entry.kext]]
bundle = "Kexts/MyDriver.kext"
```

kit 级 kext 指定菜单项：

```toml
[[kext]]
bundle = "MyDriver.kext"
entry = "xnu"
plist = "Contents/Info.plist"
executable = "Contents/MacOS/MyDriver"
enabled = true
```

| 键 | 含义 |
| --- | --- |
| `bundle` | 相对配置或 kit 的包目录 |
| `plist` | 包内 plist 路径 |
| `executable` | 包内可执行文件路径，无代码 kext 留空 |
| `entry` | 菜单项 id，kit kext 必需 |
| `enabled` | `false` 跳过 |

菜单项 kext 和 kit kext 按配置顺序暂存，kit kext 在前，菜单项 kext 在后。重复包路径会被拒绝。

## UniXNU 校验

UniXNU 暂存前校验：

- 包名以 `.kext` 结尾。
- 所有路径相对且不含 `.` 或 `..` 段。
- plist 非空。
- 可执行文件存在时为 thin AArch64 `MH_KEXT_BUNDLE`：
  - Magic `0xfeedfacf`。
  - CPU type `0x0100000c`。
  - File type `11`。
  - Subtype `0` 或 `2`（arm64 或 arm64e）。
  - load commands 位于文件内。
- kit kext 指定菜单项。
- 菜单项 kext 不指定菜单项。

暂存的是未修改的 plist 和可执行文件。加载器校验签名、解析依赖并注入代码。

## 加载器协议

加载器收到：

```c
typedef struct {
  CONST CHAR16  *BundlePath;
  CONST CHAR16  *PlistPath;
  CONST CHAR16  *ExecutablePath;
  CONST VOID    *InfoPlist;
  UINTN         InfoPlistSize;
  CONST VOID    *Executable;
  UINTN         ExecutableSize;
} UNIXNU_LOADER_KEXT;
```

缓冲区属于 UniXNU，在 `Boot()` 期间有效。

接受 kext 的加载器实现协议 revision 2。发布版 DarwinBoot 接受内核、ADT sidecar、trust cache 和可选 ramdisk，拒绝携带暂存 kext 的请求。

## 构建 kext

用 Xcode 和 iOS kernel SDK 构建：

```sh
xcodebuild -target MyDriver -sdk iphoneos -configuration Release \
  ARCHS=arm64 ONLY_ACTIVE_ARCH=YES
```

目标使用 `MODULE_TYPE = KEXT` 并链接 kernel frameworks：

| 设置 | 值 |
| --- | --- |
| `MACH_O_TYPE` | `mh_kext_bundle` |
| `PRODUCT_TYPE` | `com.apple.product-type.kernel-extension` |
| `SDKROOT` | `iphoneos` |
| `ARCHS` | `arm64` 或 `arm64e` |
| `KERNEL_FRAMEWORKS` | `Kernel` |

可执行文件必须匹配 `xnu-10063.141.1` 的内核 ABI 和 iOS 17 deployment target。

## 签名与信任

AMFI 按 trust cache 和代码签名校验 kext。不受信任的 kext 加载失败并给出代码签名错误。

负载的 trust cache 包必须包含 kext 的 CD hash。负载步骤只接受 SHA-256 位于 `[payload] trust_cache_sha256` 的包。

## 顺序与依赖

UniXNU 按配置顺序暂存。加载器解析 `OSBundleLibraries`，按依赖顺序启动 kext。

无代码 kext 可以用于表达依赖，或把 personality 附到已有二进制上。

## 下一步

- [XNU 驱动](/zh/drivers/xnu-drivers)
- [完整性与启动策略](/zh/concepts/security)
- [启动配置](/zh/reference/boot-configuration)
