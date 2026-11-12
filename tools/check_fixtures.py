#!/usr/bin/env python3
"""Self-check for the committed beatgrid and stem fixtures (stdlib only).

This script proves, end to end and with no network access, that:

1. The golden grid JSON is internally consistent: the stored beat positions
   match the documented BPM and beat period, the click positions lie inside
   the file, and the WAV header matches the JSON metadata.
2. The F-measure (beat-detection accuracy metric, tolerance +/-70 ms) of the
   golden grid against itself is exactly 1.0.
3. A perturbed grid (uniformly shifted by 100 ms, which is beyond the 70 ms
   tolerance) drops the F-measure below the pass threshold.
4. The stem fixture files exist and have the expected channel counts.

The F-measure implementation here is the reference for the C++ helper in
``src/test/beatgridfixturetest.cpp``; the two must stay in sync.

Run from the repository root:

    py tools/check_fixtures.py
"""

import json
import math
import os
import wave

# Tolerance for a detected beat to count as a match against a golden beat.
BEAT_TOLERANCE_SECONDS = 0.07  # +/-70 ms (see docs/beatgrid-audit.md §8.2)
PERTURB_SHIFT_SECONDS = 0.100  # 100 ms > tolerance, so every beat is a miss
F_MEASURE_FAIL_THRESHOLD = 0.5

EXPECTED_BPM = 128.0
EXPECTED_SAMPLE_RATE = 44100
EXPECTED_CHANNELS = 1
EXPECTED_DURATION_SECONDS = 8.0


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fixtures_dir():
    return os.path.join(repo_root(), "src", "test", "fixtures")


def f_measure(candidate, golden, tolerance=BEAT_TOLERANCE_SECONDS):
    """One-to-one greedy F-measure between two lists of beat times (seconds).

    A candidate beat is "correct" if it is within ``tolerance`` seconds of a
    golden beat. Matches are assigned greedily in time order so a single
    candidate cannot match two golden beats. Returns 0.0 when either list is
    empty or no beats match.
    """
    cand = sorted(float(x) for x in candidate)
    gold = sorted(float(x) for x in golden)
    i = j = 0
    matched = 0
    while i < len(cand) and j < len(gold):
        delta = cand[i] - gold[j]
        if abs(delta) <= tolerance:
            matched += 1
            i += 1
            j += 1
        elif delta < 0:
            i += 1
        else:
            j += 1
    precision = matched / len(cand) if cand else 0.0
    recall = matched / len(gold) if gold else 0.0
    if precision + recall == 0.0:
        return 0.0
    return 2.0 * precision * recall / (precision + recall)


def check_beatgrid():
    json_path = os.path.join(fixtures_dir(), "beatgrid_128bpm.json")
    wav_path = os.path.join(fixtures_dir(), "beatgrid_128bpm.wav")

    with open(json_path, encoding="utf-8") as handle:
        golden = json.load(handle)

    failures = []

    # 1. JSON metadata matches the expected constants.
    if golden["bpm"] != EXPECTED_BPM:
        failures.append("bpm %r != %r" % (golden["bpm"], EXPECTED_BPM))
    if golden["sample_rate"] != EXPECTED_SAMPLE_RATE:
        failures.append("sample_rate mismatch")
    if golden["channels"] != EXPECTED_CHANNELS:
        failures.append("channels mismatch")
    if abs(golden["duration_seconds"] - EXPECTED_DURATION_SECONDS) > 1e-12:
        failures.append("duration mismatch")

    # 2. Stored positions agree with the documented beat period.
    period = golden["beat_period_seconds"]
    positions = golden["beat_positions_seconds"]
    frames = golden["beat_positions_frames"]
    clicks = golden["click_start_frames"]
    if len(positions) != golden["num_beats"]:
        failures.append("num_beats != len(beat_positions_seconds)")
    if len(frames) != len(positions) or len(clicks) != len(positions):
        failures.append("position list length mismatch")
    for i, pos in enumerate(positions):
        if abs(pos - i * period) > 1e-9 * max(1.0, abs(pos)):
            failures.append("beat %d position %.12f != %.12f" % (i, pos, i * period))
    for i, (frame, click) in enumerate(zip(frames, clicks)):
        if abs(frame - i * golden["beat_period_frames"]) > 1e-6:
            failures.append("beat %d frame mismatch" % i)
        if not (0 <= click < golden["duration_frames"]):
            failures.append("click %d out of range" % i)

    # 3. WAV header agrees with the JSON metadata, and a click is present at
    #    each golden click position (non-silence at the click onset).
    with wave.open(wav_path, "rb") as wav_file:
        if wav_file.getnchannels() != golden["channels"]:
            failures.append("wav channel count mismatch")
        if wav_file.getframerate() != golden["sample_rate"]:
            failures.append("wav sample rate mismatch")
        if wav_file.getsampwidth() != golden["bit_depth"] // 8:
            failures.append("wav sample width mismatch")
        if wav_file.getnframes() != golden["duration_frames"]:
            failures.append("wav frame count mismatch")
        raw = wav_file.readframes(wav_file.getnframes())
        import array
        mono = array.array("h")
        mono.frombytes(raw)
        # The first sample of a sine burst is sin(0) == 0, so check the peak
        # magnitude across the whole click window instead of the onset sample.
        click_len = golden["click_length_samples"]
        click_peak_threshold = 10000
        for click in clicks:
            window = mono[click:click + click_len]
            peak = max((abs(v) for v in window), default=0)
            if peak < click_peak_threshold:
                failures.append(
                        "expected click near frame %d but peak %.0f < %d" %
                        (click, peak, click_peak_threshold))

    # 4. F-measure: correct grid must be perfect; perturbed grid must fail.
    f_correct = f_measure(positions, positions)
    shifted = [p + PERTURB_SHIFT_SECONDS for p in positions]
    f_perturbed = f_measure(shifted, positions)

    print("Beatgrid fixture: %s" % json_path)
    print("  bpm               = %.1f" % golden["bpm"])
    print("  sample_rate       = %d Hz" % golden["sample_rate"])
    print("  channels          = %d" % golden["channels"])
    print("  duration          = %.1f s (%d frames)" %
          (golden["duration_seconds"], golden["duration_frames"]))
    print("  beat period       = %.6f s (%.3f frames)" %
          (golden["beat_period_seconds"], golden["beat_period_frames"]))
    print("  num beats         = %d" % golden["num_beats"])
    print("  beat 0..3 (s)     = %s" %
          ", ".join("%.4f" % p for p in positions[:4]))
    print("  F-measure(correct grid)   = %.6f" % f_correct)
    print("  F-measure(shifted +100ms) = %.6f" % f_perturbed)

    if abs(f_correct - 1.0) > 1e-12:
        failures.append("correct grid F-measure %.12f != 1.0" % f_correct)
    if f_perturbed >= F_MEASURE_FAIL_THRESHOLD:
        failures.append(
                "perturbed grid F-measure %.6f >= %.2f" %
                (f_perturbed, F_MEASURE_FAIL_THRESHOLD))

    return failures


def check_stems():
    expected = {
        "stem_drums.wav": 2,
        "stem_bass.wav": 2,
        "stem_vocals.wav": 2,
        "stem_other.wav": 2,
        "stem_mix.wav": 2,
        "stem_8ch.wav": 8,
    }
    failures = []
    print("\nStem fixtures:")
    for name, channels in expected.items():
        path = os.path.join(fixtures_dir(), name)
        if not os.path.exists(path):
            failures.append("missing %s" % name)
            print("  %-16s MISSING" % name)
            continue
        with wave.open(path, "rb") as wav_file:
            actual = wav_file.getnchannels()
        status = "ok" if actual == channels else "BAD"
        if actual != channels:
            failures.append("%s: %d channels != %d" % (name, actual, channels))
        print("  %-16s %d channels (%s)" % (name, actual, status))
    return failures


def main():
    failures = check_beatgrid()
    failures += check_stems()
    if failures:
        print("\nFAIL")
        for failure in failures:
            print("  - " + failure)
        raise SystemExit(1)
    print("\nPASS: fixtures are deterministic, consistent, and the golden-grid "
          "F-measure behaves as expected.")


if __name__ == "__main__":
    main()
