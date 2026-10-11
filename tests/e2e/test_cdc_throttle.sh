#!/usr/bin/env bash
# cdc_throttle end-to-end: serial port appears after attach, periodic count reports received
# (mock_cdc_throttle sends accumulated '1' count to host as "number\n" every window/2 milliseconds)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir cdc_throttle
echo "== cdc_throttle: Start server + attach"
# -l 64 -w 5: receive at most 64 bytes per 5-second window; excess gets NAK until window ends (backpressure demonstration)
start_server mock_cdc_throttle -l 64 -w 5 || { report cdc_throttle; exit 1; }
attach_device 1-1

TTY=$(wait_dev "/dev/ttyACM" 10) || true
assert "Serial port appears ($TTY)" [ -n "$TTY" ]
[ -n "$TTY" ] || { stop_server; report cdc_throttle; exit 1; }

# Read for 3 seconds: mock periodically reports count (number\n); reception means pass
DATA=$(timeout 3 sudo cat "$TTY" 2>/dev/null || true)
assert "Count report received" grep -qE '^[0-9]+$' <<<"$DATA"

stop_server
report cdc_throttle
