#!/usr/bin/env python3
# Verify WAV written from mock_speaker reception approximately matches playback source (content level, not merely nonempty):
#  - Frequency: estimate with zero-crossing rate (440Hz sine wave)
#  - Waveform shape: total local peaks + troughs
# Playback path has ALSA buffering; allow small errors: frequency difference < 2%, extrema-count difference < 5%
import array
import sys
import wave


def analyze(path):
    w = wave.open(path)
    ch = w.getnchannels()
    sr = w.getframerate()
    n = w.getnframes()
    data = array.array("h")
    data.frombytes(w.readframes(n))
    if ch > 1:
        data = data[::ch]
    if not data:
        return None
    # Frequency from zero-crossing rate: crossings per second / 2 = periods
    crossings = sum(1 for i in range(1, len(data)) if (data[i - 1] < 0) != (data[i] < 0))
    freq = crossings / 2.0 / (n / sr)
    # Local extrema (peaks + troughs)
    peaks = valleys = 0
    for i in range(2, len(data)):
        if data[i - 1] > data[i - 2] and data[i - 1] >= data[i]:
            peaks += 1
        if data[i - 1] < data[i - 2] and data[i - 1] <= data[i]:
            valleys += 1
    return {"freq": freq, "extrema": peaks + valleys}


src = analyze(sys.argv[1])
rec = analyze(sys.argv[2])
if src is None or rec is None:
    print("EMPTY")
    sys.exit(2)

ok = True
if abs(src["freq"] - rec["freq"]) / src["freq"] > 0.02:
    print(f"FREQ_MISMATCH src={src['freq']:.1f}Hz rec={rec['freq']:.1f}Hz")
    ok = False
if abs(src["extrema"] - rec["extrema"]) / src["extrema"] > 0.05:
    print(f"EXTREMA_MISMATCH src={src['extrema']} rec={rec['extrema']}")
    ok = False
print(f"freq src={src['freq']:.1f}Hz rec={rec['freq']:.1f}Hz, extrema src={src['extrema']} rec={rec['extrema']}")
print("MATCH" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
