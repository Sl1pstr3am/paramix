#!/usr/bin/env python3
"""Generate a deterministic synthetic 4-band stem mix fixture (stdlib only).

This script writes six files under ``src/test/fixtures/``:

* ``stem_mix.wav``      — the stereo mix (sum of the four stems).
* ``stem_drums.wav``    — drums stem (stereo).
* ``stem_bass.wav``     — bass stem (stereo).
* ``stem_vocals.wav``   — vocals stem (stereo).
* ``stem_other.wav``    — "other" stem (stereo).
* ``stem_8ch.wav``      — all four stereo stems interleaved into one 8-channel
                          WAV, in the order drums,bass,vocals,other
                          (channels 1L,1R,2L,2R,3L,3R,4L,4R).

Each stem is a single pure sine in a distinct frequency band with a distinct
amplitude, which makes the stems (a) non-silent, (b) mutually distinct by RMS,
and (c) trivially separable by a simple band-pass filter. Left and right
channels differ (the right channel is attenuated and phase-shifted) so each
stem is a true stereo signal rather than dual mono.

Everything is computed from closed-form formulas. There is no randomness, no
network access, and no third-party dependency. Re-running the script produces
byte-identical output.

Run from anywhere:

    py tools/make_stem_fixture.py
"""

import array
import math
import os
import wave

SAMPLE_RATE = 44100
DURATION_SECONDS = 1.0
SAMPLE_WIDTH_BYTES = 2  # 16-bit PCM

# (name, frequency_hz, amplitude). Amplitudes are strictly decreasing so the
# per-stem RMS values are strictly ordered and mutually distinct. The sum of
# the amplitudes stays below 1.0, so the mix never clips.
STEMS = [
    ("drums", 200.0, 0.30),
    ("bass", 60.0, 0.24),
    ("vocals", 1000.0, 0.20),
    ("other", 5000.0, 0.16),
]

RIGHT_ATTENUATION = 0.8
RIGHT_PHASE = math.pi / 4.0


def repo_root():
    """Return the repository root (parent of tools/)."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fixtures_dir():
    path = os.path.join(repo_root(), "src", "test", "fixtures")
    os.makedirs(path, exist_ok=True)
    return path


def int16(samples):
    """Convert a list of float samples in [-1.0, 1.0] to an array of int16."""
    return array.array("h", (int(round(v * 32767.0)) for v in samples))


def write_wav(path, num_channels, sample_rate, interleaved):
    with wave.open(path, "wb") as wav_file:
        wav_file.setnchannels(num_channels)
        wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(interleaved.tobytes())


def generate_stem(frequency_hz, amplitude, num_frames):
    """Return (left, right) as lists of float samples for one stem."""
    left = []
    right = []
    omega = 2.0 * math.pi * frequency_hz / SAMPLE_RATE
    for n in range(num_frames):
        phase = omega * n
        left.append(amplitude * math.sin(phase))
        right.append(amplitude * RIGHT_ATTENUATION *
                     math.sin(phase + RIGHT_PHASE))
    return left, right


def interleave_stereo(left, right):
    """Interleave two same-length channels into one flat sample list."""
    out = array.array("h", [0]) * (2 * len(left))
    out[0::2] = int16(left)
    out[1::2] = int16(right)
    return out


def generate():
    num_frames = int(round(SAMPLE_RATE * DURATION_SECONDS))
    fixtures = fixtures_dir()

    stems = [(name, freq, amp, generate_stem(freq, amp, num_frames))
             for name, freq, amp in STEMS]

    # Individual stereo stem files.
    for name, _freq, _amp, (left, right) in stems:
        write_wav(
                os.path.join(fixtures, "stem_%s.wav" % name),
                2,
                SAMPLE_RATE,
                interleave_stereo(left, right))

    # Stereo mix = plain sum of the four stems (amplitudes are chosen so this
    # never exceeds full scale).
    mix_left = [sum(channels[0][n] for channels in [s[3] for s in stems])
                for n in range(num_frames)]
    mix_right = [sum(channels[1][n] for channels in [s[3] for s in stems])
                 for n in range(num_frames)]
    write_wav(
            os.path.join(fixtures, "stem_mix.wav"),
            2,
            SAMPLE_RATE,
            interleave_stereo(mix_left, mix_right))

    # 8-channel file: drums,bass,vocals,other (1L,1R,2L,2R,3L,3R,4L,4R).
    eight = array.array("h", [0]) * (8 * num_frames)
    for stem_index, (name, _freq, _amp, (left, right)) in enumerate(stems):
        base = 2 * stem_index
        eight[base::8] = int16(left)
        eight[base + 1::8] = int16(right)
    write_wav(
            os.path.join(fixtures, "stem_8ch.wav"),
            8,
            SAMPLE_RATE,
            eight)

    print("wrote %d stem files, mix, and 8-channel file under %s" %
          (len(stems), fixtures))
    for name, freq, amp, _ in stems:
        print("  stem_%-7s freq=%7.1f Hz  amplitude=%.2f" % (name, freq, amp))


if __name__ == "__main__":
    generate()
