#!/usr/bin/env bash
# uvc end-to-end: /dev/video0 appears after attach; ffmpeg retrieves actual frames to verify ISO streaming
# (mock_uvc default 320x240 color bars; nonempty retrieved image means streaming works)
set -e
source "$(dirname "$0")/common.sh"

setup_workdir uvc
echo "== uvc: Start server + attach"
start_server mock_uvc || { report uvc; exit 1; }
attach_device 1-1

VIDEO=$(wait_dev "/dev/video" 15) || true
assert "video device appears ($VIDEO)" [ -n "$VIDEO" ]
[ -n "$VIDEO" ] || { stop_server; report uvc; exit 1; }

# Actual stream retrieval: ffmpeg retrieves 5 frames (v4l2 negotiates mock default format automatically); nonempty output means success
if command -v ffmpeg > /dev/null 2>&1; then
    OUT="$WORK_DIR/frame.jpg"
    # -update 1: image2 writing multiple frames to same file requires overwrite mode; otherwise reports "Cannot write more
    # than one file with the same name"
    timeout 15 ffmpeg -loglevel error -f v4l2 -i "$VIDEO" -frames:v 5 -update 1 -y "$OUT" || true
    assert "ffmpeg retrieved frame ($OUT)" [ -s "$OUT" ]
fi

stop_server
report uvc
