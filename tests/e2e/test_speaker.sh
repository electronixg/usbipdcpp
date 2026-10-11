#!/usr/bin/env bash
# speaker end-to-end: usbip playback sound card appears after attach; aplay plays known 440Hz sine wave;
# mock_speaker --output receives stream and writes file; compare content (matching frequency/peak-and-trough count,
# not merely "file nonempty")
set -e
source "$(dirname "$0")/common.sh"

setup_workdir speaker
echo "== speaker: Start server (--output receives stream into file) + attach"
start_server mock_speaker --output "$WORK_DIR/speaker_rec.wav" || { report speaker; exit 1; }
attach_device 1-1

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip sound card appears" [ -n "$CARDS" ]

# Generate 440Hz sine-wave source file (48kHz 16bit mono, 1 second)
python3 - "$WORK_DIR/tone.wav" <<'EOF'
import array
import math
import sys
import wave

sr = 48000
data = array.array("h", (int(12000 * math.sin(2 * math.pi * 440 * i / sr)) for i in range(sr)))
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(sr)
w.writeframes(data.tobytes())
w.close()
EOF

# Playback → ISO OUT reception → mock_speaker writes WAV file
if command -v aplay > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    aplay -q -D "hw:$CARD_NO,0" "$WORK_DIR/tone.wav" || true
    sleep 1.5 # Wait for reception cleanup (WAV header backfill)
    # Content-level comparison: frequency (estimated from zero-crossing rate) and peak/trough count
    timeout 15 python3 "$E2E_DIR/speaker_verify.py" "$WORK_DIR/tone.wav" \
        "$WORK_DIR/speaker_rec.wav" > "$WORK_DIR/verify.out" 2>&1 || true
    assert "Received content matches source ($(head -2 "$WORK_DIR/verify.out" | tail -1))" \
        grep -q "MATCH" "$WORK_DIR/verify.out"
else
    echo "  [SKIP] No aplay; skip playback verification"
fi

stop_server
report speaker
