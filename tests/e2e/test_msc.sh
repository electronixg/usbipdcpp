#!/usr/bin/env bash
# msc end-to-end: new block device appears after attach; verify reads/writes in separate steps.
#
# Safety rules (mandatory; wrong device name destroys physical disks in WSL):
#  - Block-device reads/writes must be separate steps: first identify new device separately; only after confirmation, write separately
#  - Size/identity check mandatory before writing: abort immediately if device size differs from mock image; never write
#  - Never combine device lookup and writing in one command
set -e
source "$(dirname "$0")/common.sh"

setup_workdir msc
EXPECTED_SIZE=$((4096 * 512)) # mock_msc default disk.img: 4096 blocks × 512 bytes = 2MiB

# Prepare disk image (virtual disk file; writing it touches no real disk)
dd if=/dev/zero of="$WORK_DIR/disk.img" bs=512 count=4096 status=none

echo "== msc: Start server (image $WORK_DIR/disk.img)"
start_server mock_msc -i "$WORK_DIR/disk.img" || { report msc; exit 1; }

echo "== msc: Record pre-attach block-device snapshot and dmesg line count; attach"
BEFORE=$(snapshot_block_devices)
DMESG_LINES=$(sudo dmesg | wc -l)
attach_device 1-1

# Step 1 (query only, no changes): lsblk comparison + kernel dmesg enumeration cross-check new device
NEW_DEV=""
for _ in $(seq 1 100); do
    NEW_DEV=$(find_new_block_device "$BEFORE")
    DMESG_DEV=$(dmesg_find_new_device "$DMESG_LINES")
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] && break
    sleep 0.1
done
assert "New block device appears (lsblk=$NEW_DEV, dmesg=$DMESG_DEV, both agree)" \
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ]
[ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] || { stop_server; report msc; exit 1; }

# Step 2 (query only, no changes): identity verification—size must equal mock image; mismatch means suspicious device
assert "Device $NEW_DEV size == ${EXPECTED_SIZE} (identity verification)" \
    check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE"
check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE" || {
    echo "ABORT: Device identity verification failed; no writes performed"
    stop_server
    report msc
    exit 1
}

# Step 3 (write): device confirmed and size matches; write separately (starting at sector 100, leaving partition table untouched).
# Prepare data in file (fill 512 bytes); compare files for both writes/reads (cmp)—echo piped to dd
# with fewer than 512 bytes does not fill block; readback includes NUL/whitespace, making string comparisons problematic
TEST_FILE="$WORK_DIR/test_data.bin"
printf "e2e msc test %s\n" "$(date +%s)" > "$TEST_FILE"
truncate -s 512 "$TEST_FILE"
echo "== msc: Write test data to /dev/$NEW_DEV (sector 100)"
sudo dd if="$TEST_FILE" of="/dev/$NEW_DEV" bs=512 seek=100 count=1 conv=fsync status=none
sudo sync

# Step 4 (readback verification, read-only)
GOT_FILE="$WORK_DIR/read_back.bin"
sudo dd if="/dev/$NEW_DEV" bs=512 skip=100 count=1 of="$GOT_FILE" status=none
assert "Write/readback match" cmp -s "$TEST_FILE" "$GOT_FILE"

stop_server
report msc
