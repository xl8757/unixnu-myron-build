#!/usr/bin/env python3
"""Generate the minimal myron hardware Apple-format DT for the first boot.

The tree follows make-sheng-adt.py of UniXNU-sheng (same required nodes and
properties) with myron SM8850 values taken from the stock Canoe v2 base DTB of
vendor_boot and the stock kernel boot log:

  - GICv3 distributor 0x17000000/0x10000, redistributor 0x17080000/0x200000
    (arm-io child offsets under an APSS range at 0x17000000), stride 0x40000
  - CPU timebase 19200000 Hz (Qualcomm SM8xxx architectural counter)
  - CPUs 0x0..0x500 in Affinity1 and the prime cores 0x10000, 0x10100 in
    Affinity2; the kernel needs the native-smp-wide-affinity XNU patch for them
  - DRAM base 0x80000000; size comes from the board file.
  - UFS host 0x1d84000, apps SMMU 0x15000000, UFS stream 0x60 (as SM8550).

This is first-boot bring-up data: the kernel is expected to reach the video
console and report the first missing driver. No Apple device identity is
claimed beyond --ipad-identity and no property is invented beyond these sources.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DRAM_BASE = 0x80000000
ARMIO_BASE = 0x17000000          # APSS peripheral block containing the GIC
GIC_DIST = 0x17000000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17080000
GIC_REDIST_SIZE = 0x200000
GIC_REDIST_STRIDE = 0x40000
TIMEBASE = 19200000
# Phone identities for --phone-identity: model -> (hw.target, product name, artwork-device-subtype = the long screen side in
# pixels). 14 Pro / 15 Pro are 393x852 @3x (2556 px tall); myron's panel is 1200x2608, i.e. 400x869 @3x, so their metrics
# are the closest any iPhone gets to it.
PHONE_MODELS = {
    'iPhone10,3': ('D22', 'iPhone X', 2436),
    'iPhone15,2': ('D73', 'iPhone 14 Pro', 2556),
    'iPhone16,1': ('D83', 'iPhone 15 Pro', 2556),
}


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/myron/boot/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='DRAM size reported in /chosen, e.g. 0x80000000')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='Explicit DarwinBoot contiguous window request, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia (the standard source mechanism)')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID; software identity seeds, not Apple silicon IDs')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device without a Home button (MobileGestalt reads these from the tree); '
                             'values follow the unchanged iPad7,1 tree, the button set is myron\'s')
    parser.add_argument('--phone-identity', action='store_true',
                        help='the same identity fields describe an iPhone-class device without a Home button (idiom phone), '
                             'so iPadOS userland lays out the phone interface on myron')
    parser.add_argument('--phone-model', choices=sorted(PHONE_MODELS), default='iPhone10,3',
                        help='model written with --phone-identity (default iPhone10,3 / iPhone X; '
                             'iPhone15,2 / 14 Pro and iPhone16,1 / 15 Pro match myron\'s 400x869 @3x panel more closely)')
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values (the /cpus reg of each core); the first is the running boot CPU. '
                             'SM8850 numbers its cores 0x000..0x500 in Affinity1 and the prime cores 0x10000, 0x10100 in Affinity2. '
                             'More than one adds /psci')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by the secondary-CPU start (the board DT declares smc)')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes to the native sparse block driver, without a dense memdev alias')
    parser.add_argument('--persistent-data', action='store_true',
                        help='the Data volume is not ephemeral (no ephemeral-storage marker): the first-boot migration installs staged system apps')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range for a scratch Data seed')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--gpu-addon', action='store_true', help='Zeroed GpuAddon sidecar handoff placeholder (the loader fills in kernel.macho.gpuaddon)')
    args = parser.parse_args()
    if args.ipad_identity and args.phone_identity:
        parser.error('choose one identity: --ipad-identity or --phone-identity')
    identity = 'ipad' if args.ipad_identity else 'phone' if args.phone_identity else None
    phone_target, phone_name, phone_subtype = PHONE_MODELS[args.phone_model]
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 8
            or any(reg < 0 or reg > 0xffffff or reg & 0xff for reg in cpu_regs)):
        parser.error('--cpu-regs: up to eight distinct cores numbered in Affinity1 and Affinity2, boot CPU 0 first')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   # The stock board DT declares every core "arm,armv8".
                                   ('device_type', b'cpu\0'), ('compatible', b'arm,armv8\0'),
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        # hw.product/hw.machine/hw.target come from the root 'model'/'target-type' (IODTPlatformExpert);
        # libMobileGestalt prefix-matches hw.product against iPhone/iPod/iPad, so the iPad identity needs both here.
        ('model', b'iPad7,1\0' if identity == 'ipad' else args.phone_model.encode() + b'\0' if identity == 'phone' else b'myron-first-boot\0'),
        *([('target-type', b'J120\0')] if identity == 'ipad' else [('target-type', phone_target.encode() + b'\0')] if identity == 'phone' else []),
        # Root compatible strings of the stock board DT (myron dtbo overlay over the Canoe base).
        ('compatible', b'qcom,canoe-mtp\0qcom,canoe\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(64)),
                        # Without these XNU never publishes IOPlatformUUID and gethostuuid() blocks forever
                        # for callers (configd) that pass a zero timeout.
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        # This boots the normal target OS. Volatile Data is
                        # described separately by ephemeral-storage below.
                        ('osenvironment', b'main\0'),
                        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1), as read by MobileGestalt: mobileactivationd's
                        # DeviceType then takes its native "Hactivation" branch (Activated without an activation record; no SEP or
                        # baseband). Values established on hardware by github.com/dipa-studio/ios-sweet (tools/mkadt.py).
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if identity else []),
                        # This property describes the actual volatile Data RAM disk.
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) and not args.persistent_data else []),
                        *( [('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                           if args.owned_memory_size else [] ),
                        ('dram-base', struct.pack('<Q', DRAM_BASE)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        # Image4 hash algorithm. The target iPad's DT specifies sha2-384 in /chosen; this
                        # selects the algorithm only and asserts nothing about Apple silicon or fuses.
                        ('crypto-hash-method', b'sha2-384\0'),
                        *( [('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                               '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                               args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                           if args.root_match_volume_uuid else [] ),
                        # Strict policy for the original AMFI: platform code only. This keeps AMFI
                        # enforcing; it does not relax it.
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *empty_nvram_proxy()],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
              # Read by the original AppleImage4 at start-up; present and empty here.
              node('manifest-properties')]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        # Suspend/resume is not implemented for this board. Use the original
        # root-domain policy input rather than allowing the CPU to park forever.
        node('defaults', [('sleep-disabled', struct.pack('<I', 1))]),
        node('options', []),
        # The original AppleImage4 panics without a product node. The model is the stock board DT's own string.
        *( [node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                             # No 'menu' entry: myron has no Home button. 2 = the non-mechanical answer BSPlatform passes through.
                             ('button-names', b'hold\0voldown\0volup\0'),
                             ('home-button-type', struct.pack('<I', 2))])] if identity else [] ),
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if identity == 'ipad' else
                                         phone_name.encode() + b'\0' if identity == 'phone' else b'Redmi K90 Pro Max\0'),
                         ('model', b'Qualcomm Technologies, Inc. Myron based on SM8850\0'),
                         *( [('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0' if identity == 'ipad' else
                                                     phone_name.encode() + b'\0'),
                             ('artwork-device-idiom', b'pad\0' if identity == 'ipad' else b'phone\0'),
                             # iPad Pro 12.9 (2732 px tall, @2x) vs the chosen iPhone's long screen side (@3x).
                             ('artwork-device-subtype', struct.pack('<I', 2732 if identity == 'ipad' else phone_subtype)),
                             ('artwork-scale-factor', struct.pack('<I', 2 if identity == 'ipad' else 3)),
                             ('artwork-display-gamut', b'P3\0'),
                             ('compatible-device-fallback', b'iPad6,7\0' if identity == 'ipad' else args.phone_model.encode() + b'\0'),
                             ('thin-bezel', struct.pack('<I', 1))] if identity else [] )]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x08000000))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              # UFS (ufshc@1d84000) lies below the arm-io window, so a child reg offset would be negative. As on
              # sheng, the node exists only for class matching: NativeUfs matches "qcom,sm8550-ufshc" and takes its
              # windows from its own constants. The Canoe v2 base DTB has the same host (0x1d84000), apps SMMU
              # (0x15000000) and UFS stream (0x60) as SM8550.
              node('ufshc', [('compatible', b'qcom,sm8850-ufshc\0qcom,sm8550-ufshc\0')])]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'myron ADT: {len(tree)} bytes, dram {args.dram_size:#x}, timebase {TIMEBASE}, cpus {len(cpus)}')


if __name__ == '__main__':
    main()
