#!/usr/bin/env python3
"""Generate a deterministic click-track beatgrid fixture (stdlib only).

This script writes two files under ``src/test/fixtures/``:

* ``beatgrid_128bpm.wav`` — a mono, 16-bit PCM, 44 100 Hz WAV file containing
  a short, deterministic click placed exactly on each beat of a fixed-tempo
  grid (128 BPM by default).
* ``beatgrid_128bpm.json`` — a "golden grid" describing the exact beat sample
  positions (in frames and seconds), the BPM, the sample rate, the channel
  count, the duration, and the click parameters.

Everything is computed from closed-form formulas. There is no randomness, no
network access, and no third-party dependency. Re-running the script produces
byte-identical output, which is what makes the fixture safe to commit and to
compare against in CI.

Run from anywhere:

    py tools/make_beatgrid_fixture.py
"""

import array
import json
import math
import os
import wave

# ---------------------------------------------------------------------------
# Fixture parameters (change these and the filenames together if you ever need
# a second fixture; keep them self-documenting).
# ---------------------------------------------------------------------------
SAMPLE_RATE = 44100
BPM = 128.0
DURATION_SECONDS = 8.0
NUM_CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2  # 16-bit PCM
CLICK_LENGTH_SAMPLES = 220  # ~5 ms
CLICK_FREQUENCY_HZ = 2000.0
CLICK_AMPLITUDE = 0.85

OUTPUT_BASENAME = "beatgrid_128bpm"


def repo_root():
    """Return the repository root (parent of tools/)."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fixtures_dir():
    path = os.path.join(repo_root(), "src", "test", "fixtures")
    os.makedirs(path, exist_ok=True)
    return path


def make_click(click_length, frequency_hz, amplitude, sample_rate):
    """Return a list of int16 samples forming one deterministic click.

    The click is a 2 kHz sine burst with a fast exponential decay so that it
    has a sharp attack (what an onset detector keys on) and stays well below
    full scale.
    """
    click = []
    for n in range(click_length):
        envelope = math.exp(-6.0 * n / click_length)
        value = amplitude * envelope * math.sin(
                2.0 * math.pi * frequency_hz * n / sample_rate)
        click.append(int(round(value * 32767.0)))
    return click


def generate():
    duration_frames = int(round(SAMPLE_RATE * DURATION_SECONDS))
    beat_period_seconds = 60.0 / BPM
    beat_period_frames = SAMPLE_RATE * beat_period_seconds

    # Ideal (fractional) beat positions. A beat is included while its ideal
    # position is strictly before the end of the file.
    beat_positions_frames = []
    beat_positions_seconds = []
    click_start_frames = []
    i = 0
    while i * beat_period_frames < duration_frames:
        ideal_frame = i * beat_period_frames
        beat_positions_frames.append(ideal_frame)
        beat_positions_seconds.append(i * beat_period_seconds)
        click_start_frames.append(int(round(ideal_frame)))
        i += 1

    click = make_click(
            CLICK_LENGTH_SAMPLES, CLICK_FREQUENCY_HZ, CLICK_AMPLITUDE, SAMPLE_RATE)

    # Build the full mono track: silence with a click on every beat.
    samples = array.array("h", [0]) * duration_frames
    for start in click_start_frames:
        for offset, value in enumerate(click):
            index = start + offset
            if 0 <= index < duration_frames:
                samples[index] = value

    # Write the WAV. array("h") is native-endian; every supported target is
    # little-endian, which is what the RIFF/WAVE container requires.
    wav_path = os.path.join(fixtures_dir(), OUTPUT_BASENAME + ".wav")
    with wave.open(wav_path, "wb") as wav_file:
        wav_file.setnchannels(NUM_CHANNELS)
        wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(samples.tobytes())

    golden = {
        "description": (
            "Deterministic click-track fixture for end-to-end beatgrid "
            "detection testing. A short 2 kHz click is placed on each beat of "
            "a constant-tempo grid."
        ),
        "sample_rate": SAMPLE_RATE,
        "channels": NUM_CHANNELS,
        "bit_depth": 8 * SAMPLE_WIDTH_BYTES,
        "bpm": BPM,
        "duration_seconds": DURATION_SECONDS,
        "duration_frames": duration_frames,
        "beat_period_seconds": beat_period_seconds,
        "beat_period_frames": beat_period_frames,
        "num_beats": len(beat_positions_frames),
        "click_length_samples": CLICK_LENGTH_SAMPLES,
        "click_frequency_hz": CLICK_FREQUENCY_HZ,
        "click_amplitude": CLICK_AMPLITUDE,
        "beat_positions_frames": beat_positions_frames,
        "beat_positions_seconds": beat_positions_seconds,
        "click_start_frames": click_start_frames,
    }

    json_path = os.path.join(fixtures_dir(), OUTPUT_BASENAME + ".json")
    with open(json_path, "w", encoding="utf-8") as json_file:
        json.dump(golden, json_file, indent=2)
        json_file.write("\n")

    print("wrote %s (%d frames, %d beats)" %
          (wav_path, duration_frames, len(beat_positions_frames)))
    print("wrote %s" % json_path)
    print("bpm=%.1f period=%.6fs period_frames=%.3f" %
          (BPM, beat_period_seconds, beat_period_frames))


if __name__ == "__main__":
    generate()
