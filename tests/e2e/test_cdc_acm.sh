#!/usr/bin/env bash
# cdc_acm end-to-end: serial port appears after attach; serial write/read echo matches exactly.
# Reader uses dd to read fixed byte count once (count=1 exits after first portion, avoiding usb-serial
# duplicate data from continuous reading); disable tty conversions (echo/icrnl/onlcr) before write/read to prevent interference
set -e
source "$(dirname "$0")/common.sh"

setup_workdir cdc_acm
echo "== cdc_acm: Start server + attach"
start_server mock_cdc_acm || { report cdc_acm; exit 1; }
attach_device 1-1

TTY=$(wait_dev "/dev/ttyACM" 10) || true
assert "Serial port appears ($TTY)" [ -n "$TTY" ]
[ -n "$TTY" ] || { stop_server; report cdc_acm; exit 1; }

sleep 1 # Wait for driver ready (ACM initialization takes some time)

# Write/read echo: python3 helper uses one fd for termios (disable echo/canonical mode, etc.) + write + readback.
# Avoid separate sh tee/dd processes: tests showed no echo when writer exited and reader opened later;
# serial default canonical mode also returns reads by line, so without newline reader waits indefinitely for line terminator
MSG="E2E_$(date +%s)"
GOT=$(timeout 12 sudo python3 "$E2E_DIR/cdc_acm_test.py" "$TTY" "$MSG" 2>/dev/null)
assert "Echo matches exactly ([$GOT])" [ "$GOT" = "$MSG" ]

stop_server
report cdc_acm
