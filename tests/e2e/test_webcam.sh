#!/usr/bin/env bash
# webcam end-to-end: single composite device provides camera + microphone simultaneously (UVC VS retrieval + UAC AS streaming).
# After attach, both /dev/video0 and usbip sound card appear; dual verification via ffmpeg frame retrieval + arecord recording
# (mock_webcam default 320x240 color bars + 440Hz sine wave)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir webcam
echo "== webcam: Start server + attach"
start_server mock_webcam || { report webcam; exit 1; }
attach_device 1-1

VIDEO=$(wait_dev "/dev/video" 15) || true
assert "video device appears ($VIDEO)" [ -n "$VIDEO" ]

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip sound card appears" [ -n "$CARDS" ]
[ -n "$VIDEO" ] && [ -n "$CARDS" ] || { stop_server; report webcam; exit 1; }

# Actual stream retrieval: ffmpeg retrieves 5 frames (v4l2 negotiates mock default format automatically); nonempty output means success
if command -v ffmpeg > /dev/null 2>&1; then
    OUT="$WORK_DIR/frame.jpg"
    # -update 1: image2 writing multiple frames to same file requires overwrite mode; otherwise reports "Cannot write more
    # than one file with the same name"
    timeout 15 ffmpeg -loglevel error -f v4l2 -i "$VIDEO" -frames:v 5 -update 1 -y "$OUT" || true
    assert "ffmpeg retrieved frame ($OUT)" [ -s "$OUT" ]
fi

# Actual recording: arecord records 2 seconds (48kHz 16-bit mono, mock_webcam default),
# nonempty file means streaming works (ISO IN pushes data from server to host sound card)
if command -v arecord > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    REC="$WORK_DIR/rec.wav"
    timeout 8 arecord -q -D "hw:$CARD_NO,0" -f S16_LE -r 48000 -c 1 -d 2 "$REC" || true
    sleep 1 # Wait for streaming cleanup
    assert "Recording file nonempty ($REC)" [ -s "$REC" ]
fi

stop_server
report webcam
