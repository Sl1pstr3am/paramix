// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// STFT / ISTFT reproducing HTDemucs._spec and HTDemucs._ispec, padding quirks
// included:
//
//   _spec  reflect-pad by 3*hop/2 -> torch.stft(nfft, hop, hann periodic,
//          normalized=True, center=True, pad_mode='reflect')
//          -> drop the Nyquist bin -> drop two frames at each end
//   _ispec put that bin and those frames back -> torch.istft -> trim
//
// Spectrograms use the "channels as complex" layout the model was trained with:
// plane 2c holds the real part of channel c, plane 2c+1 the imaginary part.
//
// One instance serves a fixed (nfft, hop, channels, length) geometry and any
// batch size; all the geometry-dependent tables are built in the constructor.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "demucs/fft.hpp"
#include "demucs/thread_pool.hpp"

namespace demucs::dsp {

inline std::vector<float> hann_periodic(int n) {
    std::vector<float> window(n);
    for (int i = 0; i < n; ++i)
        window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / n)));
    return window;
}

// numpy/torch 'reflect' padding: out[-1] == in[1], out[len] == in[len - 2].
inline void reflect_pad(const float* in, int length, int left, int right, float* out) {
    for (int i = 0; i < left; ++i) out[i] = in[left - i];
    std::memcpy(out + left, in, sizeof(float) * static_cast<std::size_t>(length));
    for (int i = 0; i < right; ++i) out[left + length + i] = in[length - 2 - i];
}

class Spectrogram {
public:
    Spectrogram(int nfft, int hop, int channels, int length, util::ThreadPool* pool)
        : nfft_(nfft), hop_(hop), channels_(channels), length_(length),
          pool_(pool), fft_(nfft) {
        if (nfft % hop != 0) throw std::invalid_argument("nfft must be a multiple of hop");

        window_ = hann_periodic(nfft_);
        forward_scale_ = 1.0f / std::sqrt(static_cast<float>(nfft_));
        inverse_scale_ = std::sqrt(static_cast<float>(nfft_));

        freq_bins_ = nfft_ / 2;                            // Nyquist bin dropped
        frames_ = (length_ + hop_ - 1) / hop_;             // ceil, demucs' `le`
        pad_ = hop_ / 2 * 3;
        right_pad_ = pad_ + frames_ * hop_ - length_;
        padded_ = length_ + pad_ + right_pad_;
        centred_ = padded_ + nfft_;
        total_frames_ = 1 + padded_ / hop_;                // == frames_ + 4
        total_samples_ = nfft_ + hop_ * (total_frames_ - 1);
        trim_ = nfft_ / 2 + pad_;

        envelope_.assign(total_samples_, 0.0f);
        for (int t = 0; t < total_frames_; ++t)
            for (int j = 0; j < nfft_; ++j)
                envelope_[t * hop_ + j] += window_[j] * window_[j];

        scratch_floats_ = fft_.scratch_floats() + static_cast<std::size_t>(nfft_) +
                          2 * static_cast<std::size_t>(freq_bins_ + 1);
    }

    int freq_bins() const { return freq_bins_; }
    int frames() const { return frames_; }
    int length() const { return length_; }

    // mix:  [batch][channels][length]
    // out:  [batch][2 * channels][freq_bins][frames]
    void forward(const float* mix, int batch, float* out) {
        const int planes = batch * channels_;
        prepare_padding(mix, planes);

        const std::size_t plane_size = static_cast<std::size_t>(freq_bins_) * frames_;
        dispatch(planes * frames_, [&](int index, float* scratch) {
            const int plane = index / frames_;
            const int frame = index % frames_;
            const int item = plane / channels_;
            const int channel = plane % channels_;

            float* windowed = scratch;
            float* re = windowed + nfft_;
            float* im = re + (freq_bins_ + 1);
            float* fft_scratch = im + (freq_bins_ + 1);

            const float* src = padded_buffer_.data() +
                               static_cast<std::size_t>(plane) * centred_ +
                               static_cast<std::size_t>(frame + 2) * hop_;
            for (int j = 0; j < nfft_; ++j) windowed[j] = src[j] * window_[j];
            fft_.forward(windowed, re, im, fft_scratch);

            float* dst_re = out + (static_cast<std::size_t>(item) * 2 * channels_ +
                                   2 * channel) * plane_size + frame;
            float* dst_im = dst_re + plane_size;
            for (int f = 0; f < freq_bins_; ++f) {
                dst_re[static_cast<std::size_t>(f) * frames_] = re[f] * forward_scale_;
                dst_im[static_cast<std::size_t>(f) * frames_] = im[f] * forward_scale_;
            }
        });
    }

    // spec: [items][2 * channels][freq_bins][frames]
    // out:  [items][channels][length]
    void inverse(const float* spec, int items, float* out) {
        const int planes = items * channels_;
        const std::size_t plane_size = static_cast<std::size_t>(freq_bins_) * frames_;
        const std::size_t item_stride = 2 * static_cast<std::size_t>(channels_) * plane_size;

        accumulator_.assign(static_cast<std::size_t>(planes) * total_samples_, 0.0f);
        dispatch(planes, [&](int plane, float* scratch) {
            const int item = plane / channels_;
            const int channel = plane % channels_;

            float* re = scratch;
            float* im = re + (freq_bins_ + 1);
            float* frame_out = im + (freq_bins_ + 1);
            float* fft_scratch = frame_out + nfft_;

            const float* src_re = spec + static_cast<std::size_t>(item) * item_stride +
                                  static_cast<std::size_t>(2 * channel) * plane_size;
            const float* src_im = src_re + plane_size;
            float* acc = accumulator_.data() + static_cast<std::size_t>(plane) * total_samples_;

            re[freq_bins_] = 0.0f;                    // the dropped Nyquist bin stays zero
            im[freq_bins_] = 0.0f;
            for (int t = 0; t < frames_; ++t) {
                for (int f = 0; f < freq_bins_; ++f) {
                    re[f] = src_re[static_cast<std::size_t>(f) * frames_ + t];
                    im[f] = src_im[static_cast<std::size_t>(f) * frames_ + t];
                }
                fft_.inverse(re, im, frame_out, fft_scratch);
                float* dst = acc + static_cast<std::size_t>(t + 2) * hop_;
                for (int j = 0; j < nfft_; ++j)
                    dst[j] += frame_out[j] * inverse_scale_ * window_[j];
            }

            float* dst = out + static_cast<std::size_t>(plane) * length_;
            for (int i = 0; i < length_; ++i)
                dst[i] = acc[trim_ + i] / envelope_[trim_ + i];
        });
    }

private:
    void prepare_padding(const float* mix, int planes) {
        padded_buffer_.assign(static_cast<std::size_t>(planes) * centred_, 0.0f);
        std::vector<float> inner(padded_);
        for (int plane = 0; plane < planes; ++plane) {
            reflect_pad(mix + static_cast<std::size_t>(plane) * length_, length_,
                        pad_, right_pad_, inner.data());
            reflect_pad(inner.data(), padded_, nfft_ / 2, nfft_ / 2,
                        padded_buffer_.data() + static_cast<std::size_t>(plane) * centred_);
        }
    }

    template <class Body>
    void dispatch(int count, Body&& body) {
        if (pool_ == nullptr || pool_->size() <= 1) {
            std::vector<float> scratch(scratch_floats_);
            for (int i = 0; i < count; ++i) body(i, scratch.data());
            return;
        }
        const std::size_t needed = scratch_floats_;
        pool_->parallel_for(count, [&](int i) {
            static thread_local std::vector<float> scratch;
            if (scratch.size() < needed) scratch.assign(needed, 0.0f);
            body(i, scratch.data());
        });
    }

    int nfft_, hop_, channels_, length_;
    util::ThreadPool* pool_;
    RealFft fft_;

    int freq_bins_ = 0, frames_ = 0, pad_ = 0, right_pad_ = 0;
    int padded_ = 0, centred_ = 0, total_frames_ = 0, total_samples_ = 0, trim_ = 0;
    float forward_scale_ = 1.0f, inverse_scale_ = 1.0f;
    std::size_t scratch_floats_ = 0;

    std::vector<float> window_, envelope_, padded_buffer_, accumulator_;
};

}  // namespace demucs::dsp
