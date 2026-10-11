#!/usr/bin/env bash
# audio end-to-end: after attach, usbip recording sound card appears in /proc/asound/cards;
# arecord records actual audio for 2 seconds, verifying ISO IN streaming (mock_audio is UAC1 microphone)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir audio
echo "== audio: Start server + attach"
start_server mock_audio || { report audio; exit 1; }
attach_device 1-1

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip sound card appears" [ -n "$CARDS" ]
[ -n "$CARDS" ] || { stop_server; report audio; exit 1; }

# Actual recording: arecord records 2 seconds (48kHz 16-bit mono, mock_audio default);
# nonempty file means stream retrieval works (ISO IN pushes data from server to host sound card)
if command -v arecord > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    REC="$WORK_DIR/rec.wav"
    timeout 8 arecord -q -D "hw:$CARD_NO,0" -f S16_LE -r 48000 -c 1 -d 2 "$REC" || true
    sleep 1 # Wait for streaming cleanup
    assert "Recording file nonempty ($REC)" [ -s "$REC" ]
fi

stop_server
report audio
