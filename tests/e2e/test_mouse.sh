#!/usr/bin/env bash
# mouse end-to-end: mouse input device appears after attach; evtest captures movement events
# (mock_mouse periodically generates movement events; evtest requires sudo to read /dev/input)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir mouse
echo "== mouse: Start server + attach"
start_server mock_mouse || { report mouse; exit 1; }
attach_device 1-1

# Wait for mouse device (locate through by-path, avoiding eventX number mismatch)
MOUSE=""
for _ in $(seq 1 100); do
    MOUSE=$(ls /dev/input/by-path/*event-mouse 2>/dev/null | grep -i usb | head -1)
    [ -n "$MOUSE" ] && break
    sleep 0.1
done
assert "Mouse device appears ($MOUSE)" [ -n "$MOUSE" ]
[ -n "$MOUSE" ] || { stop_server; report mouse; exit 1; }

# evtest capture for 3 seconds: mock periodically generates REL movement events; should capture ≥1
EVENTS=$(timeout 3 sudo evtest "$MOUSE" 2>/dev/null | grep -c "EV_REL" || true)
assert "REL movement events captured ($EVENTS events)" [ "$EVENTS" -ge 1 ]

stop_server
report mouse
