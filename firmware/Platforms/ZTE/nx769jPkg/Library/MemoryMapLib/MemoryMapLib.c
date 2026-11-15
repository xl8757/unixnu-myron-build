//
//  NX769J UEFI memory map.
//
//  Rebuilt from this board's firmware device tree rather than carried over from
//  nx729j. The reservation list is scripts/extract-firmware-dtb.py's output for
//  vendor_boot.img, recorded in device-inputs.md under "Firmware device tree
//  reservations"; 36 of the 49 nodes carry a fixed address and each of those
//  has a descriptor below, named after its node.
//
//  Two kinds of region, then. The reserved ones come from the device tree and
//  describe what the bootloader, TrustZone, the modem and the coprocessors own.
//  The rest -- FD Reserved, UEFI Stack, SEC Heap, Sched Heap, FV Region, UEFI
//  RESV, Kernel and DXE Heap -- are UEFI's own layout and the device tree knows
//  nothing about them. They are kept where nx729jPkg puts them, which fits this
//  board's free band 0xa1f80000..0xd7e00000 exactly.
//
//  Differences from the map this file used to hold, which was nx729j's with the
//  identifiers renamed:
//
//    HYP               0x80000000 / 0x00A00000 was nx729j's
//                      0x80000000 / 0x00E00000 is gunyah_hyp_region here
//    Display Reserved  0xB8000000 was nx729j's splash
//                      0xE3940000 / 0x02B00000 is cont_splash_region here,
//                      measured and confirmed in the public dump's DTBO
//    DXE Heap          now starts at 0xB8000000 rather than 0xBAB00000, because
//                      the region nx729j gave to its splash is ordinary RAM on
//                      this board and the device tree reserves nothing there
//
//  The splash must stay reserved: the boot image advertises it as
//  cont_splash_region and the bootloader has already lit the panel with it.
//
#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR: reservations read from the firmware device tree
  {"HYP",                0x80000000, 0x00E00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"CPU VM",             0x80E00000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL AOP",            0x81A00000, 0x00260000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP CmdDB",          0x81C60000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP TME UEFI",       0x81C80000, 0x00074000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ChipInfo",           0x81CF4000, 0x00001000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM",               0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ADSP MHI",           0x81F00000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"PVMFW",              0x824A0000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Global Sync",        0x82600000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TZ Stat",            0x82700000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"QDSS",               0x82800000, 0x02000000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"QLink Log",          0x84800000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Modem DSM",          0x86B00000, 0x04900000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Modem DSM 2",        0x8B400000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Modem",              0x8BC00000, 0x0F400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Modem DTB",          0x9B000000, 0x00080000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"IPA FW",             0x9B080000, 0x00010000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"IPA GSI",            0x9B090000, 0x0000A000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"GPU Microcode",      0x9B09A000, 0x00002000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SPSS",               0x9B0A0000, 0x001E0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SPU Shared",         0x9B280000, 0x00060000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SPU Shared 2",       0x9B2E0000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Camera",             0x9B300000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Video",              0x9BB00000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CVP",                0x9C300000, 0x00700000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CDSP",               0x9CA00000, 0x01400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CDSP DTB",           0x9DE00000, 0x00080000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ADSP DTB",           0x9DE80000, 0x00080000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ADSP SLPI",          0x9DF00000, 0x04080000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Ramoops",            0xD7E00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"TZ Merged",          0xD8000000, 0x00800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display Reserved",   0xE3940000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"HW Fence",           0xE6440000, 0x002DD000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Trust UI VM",        0xF3800000, 0x04400000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"OEM VM",             0xF7C00000, 0x04C00000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"LLCC LPI",           0xFF800000, 0x00600000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  // DDR: UEFI's own layout, which the device tree does not describe
  {"FD Reserved",        0xA7000000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",            0xA7100000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD Reserved",   0xA7400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Info Blk",           0xA7601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU PageTables",     0xA7602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer",         0xA7605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI Stack",         0xA760D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",           0xA764D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched Heap",         0xA76D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV Region",          0xA7AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI RESV",          0xA7ED9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",             0xA8000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"DXE Heap",           0xB8000000, 0x1FCC0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Other memory regions
  {"AOP_SS_MSG_RAM",     0x0C300000, 0x00100000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM Base",          0x14680000, 0x00040000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register regions
  {"IPC_ROUTER_TOP",     0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY CONTROL",   0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",                0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",      0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0 CRYPTO",     0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CORE_TOP_CSR",       0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",          0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",                0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",               0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",               0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",               0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",            0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
  )
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
