##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##
#
#  nubia RedMagic 9 series (NX769J), Qualcomm SM8650 "Lanai".
#
#  Derived from Platforms/ZTE/nx729jPkg/nx729j.dsc in UniXNU/uefi-sheng, which
#  is a nubia RedMagic on the previous SoC generation and boots on hardware.
#  The differences from that file are:
#
#    * every nx729j identifier becomes nx769j, and PLATFORM_GUID is new
#    * SOC_TYPE 2 and LanaiPkg instead of SOC_TYPE 0 and KailuaPkg — this board
#      is SM8650, not SM8550
#    * SMBIOS describes a RedMagic 9 Pro
#    * the UniXNU and DarwinBoot modules are listed, together with the two
#      library mappings their shared RAM-disk driver needs
#
#  docs/UEFI-FIRMWARE.md records where each value comes from.
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = nx769j
  PLATFORM_GUID                  = F57D1EE0-603C-4EE3-AF74-1C995CE51AF3
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/nx769jPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = nx769jPkg/nx769j.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8650-AA          Snapdragon 8 Gen 3 @ 3.00 GHz
  # 1 = SM8650-Q-AA        the 6-core variant
  # 2 = SM8650-AB          Snapdragon 8 Gen 3 @ 3.30 GHz   <- NX769J
  # 3 = SM8650-Q-AB        the 6-core variant
  # 4 = SM8650-AC          Snapdragon 8 Gen 3 @ 3.40 GHz
  #
  # The RedMagic 9 Pro is the 3.30 GHz bin and reports 8 CPUs
  # (2x Cortex-A520 + 5x Cortex-A720 + 1x Cortex-X4), so it is not a Q variant.
  # Confirm the prime core's maximum frequency on the device before treating
  # 2 versus 4 as settled.
  #
  SOC_TYPE                       = 2

!include LanaiPkg/LanaiPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  # Measured: the lowest System RAM range and gunyah_hyp_region@80000000.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  # Above the firmware volume at 0xA7100000 + 0x300000. Reconcile against the
  # Lanai memory map and the contiguous-splash reservation before first boot.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"ZTE"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"RedMagic 9 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"nx769j"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"RedMagic_9_Pro_nx769j"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"RedMagic 9 Pro"

  #
  # Simple Frame Buffer
  #
  # Measured on the device from the DRM connector mode list:
  # card0-DSI-1 reports 1116x2480x60cmd.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1116
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2480
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # The AdvancedLogDxe exporter names its log pages after this, so it is the
  # board's ASCII name rather than UniXNUPkg's default.
  #
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"nx769j"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|nx769jPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|nx769jPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNURamDiskDxe publishes the FDT initrd as a block device and needs both
  # of these mapped before it can compile.
  #
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

  #
  # UniXNU.inf's own library, out of the device package tree rather than a
  # Mu-Silicium one.
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

  #
  # AdvancedLogDxe pages the Advanced Logger buffer out as QR codes and needs
  # both of these. They live in Common/Mu, which board.toml [firmware].packages
  # already puts on PACKAGES_PATH.
  #
  AdvancedLoggerAccessLib|AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf

[Components]
  #
  # ACPI Tables
  #
  nx769j/AcpiTables.inf

  #
  # UniXNU boot chain. These resolve through PACKAGES_PATH, not through this
  # tree: {tree}/package supplies UniXNUPkg and {tree}/drivers/DarwinBoot
  # supplies DarwinBootPkg. See board.toml [firmware].packages.
  #
  # UniXNURamDiskDxe has to be embedded in the firmware volume rather than
  # loaded from the payload: the payload is what it has to discover.
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

  #
  # The firmware log exporter from
  # https://unixnu.com/porting/log-capture#firmware-setup. It reads the
  # Advanced Logger buffer and pages it out as QR codes under Tools, which is
  # how the first boot's firmware log is captured before the kernel handoff.
  # Its Depex is gAdvancedLoggerProtocolGuid, so it loads only when the
  # platform's Advanced Logger is configured; the menu entry appears then.
  #
  UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf
