// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Band-limited resampling by windowed-sinc interpolation.
//
// htdemucs only ever runs at 44.1 kHz, so this exists to get other files there.
// The filter is a Blackman-windowed sinc, tabulated once and read with linear
// interpolation between table entries -- the usual trade: a table fine enough
// that the interpolation error sits well below the quantisation floor, without
// evaluating a transcendental per tap.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "demucs/audio_file.hpp"

namespace demucs::audio {

class SincResampler {
public:
    // `half_width` counts zero crossings on each side of the filter; 16 puts
    // the stopband well below 16-bit noise for music material.
    explicit SincResampler(double ratio, int half_width = 16, int steps = 512)
        : ratio_(ratio), half_(half_width), steps_(steps) {
        cutoff_ = std::min(1.0, ratio);
        support_ = half_ / cutoff_;
        table_.resize(static_cast<std::size_t>(2 * half_ * steps_) + 1);

        for (std::size_t i = 0; i < table_.size(); ++i) {
            const double x = static_cast<double>(i) / steps_ - half_;
            table_[i] = static_cast<float>(sinc(x) * blackman(x / half_));
        }
    }

    std::int64_t output_frames(std::int64_t input_frames) const {
        return static_cast<std::int64_t>(std::llround(input_frames * ratio_));
    }

    // `in` and `out` are single channels of `in_frames` / output_frames() samples.
    void process(const float* in, std::int64_t in_frames, float* out) const {
        const std::int64_t frames = output_frames(in_frames);
        for (std::int64_t i = 0; i < frames; ++i) {
            const double centre = static_cast<double>(i) / ratio_;
            const std::int64_t first =
                static_cast<std::int64_t>(std::ceil(centre - support_));
            const std::int64_t last =
                static_cast<std::int64_t>(std::floor(centre + support_));

            double sum = 0.0;
            for (std::int64_t j = first; j <= last; ++j) {
                const std::int64_t clamped = std::min<std::int64_t>(
                    std::max<std::int64_t>(j, 0), in_frames - 1);
                sum += in[clamped] * tap((centre - j) * cutoff_);
            }
            out[i] = static_cast<float>(sum * cutoff_);
        }
    }

private:
    static double sinc(double x) {
        if (std::abs(x) < 1e-9) return 1.0;
        const double t = kPiValue * x;
        return std::sin(t) / t;
    }

    static double blackman(double t) {          // t in [-1, 1]
        if (std::abs(t) >= 1.0) return 0.0;
        const double phase = kPiValue * (t + 1.0);
        return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
    }

    double tap(double x) const {
        const double position = (x + half_) * steps_;
        if (position <= 0.0 || position >= static_cast<double>(table_.size() - 1)) return 0.0;
        const std::size_t index = static_cast<std::size_t>(position);
        const double frac = position - index;
        return table_[index] * (1.0 - frac) + table_[index + 1] * frac;
    }

    static constexpr double kPiValue = 3.14159265358979323846;

    double ratio_, cutoff_, support_;
    int half_, steps_;
    std::vector<float> table_;
};

inline Buffer resample(const Buffer& in, int target_rate) {
    if (in.samplerate == target_rate || in.frames == 0) return in;

    const SincResampler resampler(static_cast<double>(target_rate) / in.samplerate);
    Buffer out;
    out.channels = in.channels;
    out.samplerate = target_rate;
    out.frames = resampler.output_frames(in.frames);
    out.samples.assign(static_cast<std::size_t>(out.channels) * out.frames, 0.0f);

    for (int c = 0; c < in.channels; ++c)
        resampler.process(in.channel(c), in.frames, out.channel(c));
    return out;
}

// Decode, resample and remix in one step.
inline Buffer load(const std::string& path, int samplerate, int channels) {
    return remix(resample(read_wav(path), samplerate), channels);
}

}  // namespace demucs::audio
