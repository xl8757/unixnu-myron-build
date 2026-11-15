#!/bin/sh
# Collect the NX769J board inputs that no public source provides.
#
# The porting guide requires hardware addresses and partition geometry to come
# from the target board. Four values in this repository are still marked as
# pending for exactly that reason. Running this on the device (or against an
# image pulled from it) produces all four.
#
# Usage:
#   sh scripts/collect-device-inputs.sh            # on the device, with adb
#   sh scripts/collect-device-inputs.sh --offline  # print the commands only
#
# Output goes to device-inputs.txt, which is pasted into board.toml and
# porting.toml. Nothing here writes to the device.

set -eu

out=device-inputs.txt
offline=0
[ "${1:-}" = "--offline" ] && offline=1

run() {
    title=$1
    shift
    printf '\n=== %s ===\n# %s\n' "$title" "$*" >>"$out"
    if [ "$offline" -eq 1 ]; then
        printf '<run manually>\n' >>"$out"
    else
        "$@" >>"$out" 2>&1 || printf '<command failed>\n' >>"$out"
    fi
}

: >"$out"
printf '# NX769J device inputs, collected %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >>"$out"

# 1. userdata GPT extents. These become board.toml [userdata] first_lba/last_lba
#    together with lba_bytes. A wrong range overwrites an unrelated partition,
#    so read them from the live GPT and never guess.
run "GPT: userdata extents" adb shell "sgdisk --print /dev/block/sda"
run "GPT: block sizes" adb shell "cat /proc/partitions"
run "GPT: logical block size" adb shell "blockdev --getss /dev/block/sda"

# 2. DRAM base and size. These become the memory node in dts/nx769j.dts and
#    payload.adt.owned_memory_size.
run "DRAM: memory map" adb shell "cat /proc/iomem | head -40"
run "DRAM: total" adb shell "cat /proc/meminfo | head -3"

# 3. Reserved regions, including the contiguous-splash handoff. These become the
#    reserved-memory node in dts/nx769j.dts and a [[bootimg.dtb_node]] entry.
run "Reserved: kernel reservations" adb shell "cat /proc/iomem | grep -i -E 'reserved|splash|removed|adsp|gpu|ipa|wlan'"
run "Reserved: device tree" adb shell "ls -l /proc/device-tree/reserved-memory"

# 4. The APFS volume UUID of the OS window. This becomes
#    payload.adt.root_match_volume_uuid and, for the owner chain,
#    CONFIG_XNU_OWNER_DATA_VOLUME_UUID in configs/nx769j_defconfig.
run "Root: APFS volume UUID" adb shell "toybox blkid"
run "Root: mounted volumes" adb shell "cat /proc/mounts"

# 5. Firmware DTB consumed by `unixnu bootimg`, and the firmware revisions that
#    identify a verification result.
run "Firmware: vendor_boot" adb shell "ls -l /dev/block/by-name/vendor_boot"
run "Firmware: versions" adb shell "getprop | grep -E 'version|fingerprint|baseband'"

printf '\nWrote %s. Record each value in board.toml and in the matching porting.toml area.\n' "$out"
