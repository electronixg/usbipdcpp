#!/usr/bin/env bash
# keyboard end-to-end: keyboard input device appears after attach; evtest captures key events
# (mock_keyboard presses/releases A every second; evtest requires sudo to read /dev/input)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir keyboard
echo "== keyboard: Start server + attach"
start_server mock_keyboard || { report keyboard; exit 1; }
attach_device 1-1

# Wait for keyboard device: WSL devtmpfs eventX and inputX numbers differ (easy to select wrong node);
# locate actual device through /dev/input/by-path symlink
KBD=""
for _ in $(seq 1 100); do
    KBD=$(ls /dev/input/by-path/*event-kbd 2>/dev/null | grep -i usb | head -1)
    [ -n "$KBD" ] && break
    sleep 0.1
done
assert "Keyboard device appears ($KBD)" [ -n "$KBD" ]
[ -n "$KBD" ] || { stop_server; report keyboard; exit 1; }

# evtest capture for 3 seconds: mock presses+releases A (KEY_A) once each per second; should capture ≥2 events
EVENTS=$(timeout 3 sudo evtest "$KBD" 2>/dev/null | grep -c "KEY_A" || true)
assert "KEY_A events captured ($EVENTS events)" [ "$EVENTS" -ge 2 ]

stop_server
report keyboard
