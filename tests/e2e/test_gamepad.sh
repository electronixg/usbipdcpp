#!/usr/bin/env bash
# gamepad end-to-end: gamepad input device appears after attach; evtest captures events
# (mock_gamepad periodically changes stick/button states; evtest requires sudo to read /dev/input)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir gamepad
echo "== gamepad: Start server + attach"
start_server mock_gamepad || { report gamepad; exit 1; }
attach_device 1-1

# Wait for gamepad device (locate through by-path)
PAD=""
for _ in $(seq 1 100); do
    PAD=$(ls /dev/input/by-path/*event-joystick 2>/dev/null | grep -i usb | head -1)
    [ -n "$PAD" ] && break
    sleep 0.1
done
assert "Gamepad device appears ($PAD)" [ -n "$PAD" ]
[ -n "$PAD" ] || { stop_server; report gamepad; exit 1; }

# evtest capture for 3 seconds: mock periodically generates events; should capture ≥1
EVENTS=$(timeout 3 sudo evtest "$PAD" 2>/dev/null | grep -c "EV_ABS\|EV_KEY" || true)
assert "Gamepad events captured ($EVENTS events)" [ "$EVENTS" -ge 1 ]

stop_server
report gamepad
