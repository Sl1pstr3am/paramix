// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Radix-2 real FFT, no dependencies.
//
// Conventions match numpy.fft.rfft / numpy.fft.irfft: the forward transform is
// unnormalised and the inverse carries the 1/n. A length-n real transform is
// computed as a length-n/2 complex transform plus a split-radix style recombine,
// which is the usual factor-of-two saving.
//
// Instances hold nothing but immutable twiddle tables, so one can be shared by
// any number of threads; per-call scratch is supplied by the caller.

#pragma once

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace demucs::dsp {

inline constexpr double kPi = 3.14159265358979323846;

class RealFft {
public:
    // `size` must be a power of two and at least 4.
    explicit RealFft(int size) : n_(size), half_(size / 2) {
        cos_.resize(half_ / 2);
        sin_.resize(half_ / 2);
        for (int i = 0; i < half_ / 2; ++i) {
            const double angle = -2.0 * kPi * i / half_;
            cos_[i] = static_cast<float>(std::cos(angle));
            sin_[i] = static_cast<float>(std::sin(angle));
        }

        cos_n_.resize(half_);
        sin_n_.resize(half_);
        for (int k = 0; k < half_; ++k) {
            const double angle = -2.0 * kPi * k / n_;
            cos_n_[k] = static_cast<float>(std::cos(angle));
            sin_n_[k] = static_cast<float>(std::sin(angle));
        }

        int bits = 0;
        while ((1 << bits) < half_) ++bits;
        reverse_.resize(half_);
        for (int i = 0; i < half_; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reverse_[i] = r;
        }
    }

    int size() const { return n_; }
    int bins() const { return half_ + 1; }
    std::size_t scratch_floats() const { return static_cast<std::size_t>(half_) * 2; }

    // `in`: n real samples -> `re`/`im`: n/2 + 1 bins.
    void forward(const float* in, float* re_out, float* im_out, float* scratch) const {
        float* re = scratch;
        float* im = scratch + half_;
        for (int k = 0; k < half_; ++k) {
            re[k] = in[2 * k];
            im[k] = in[2 * k + 1];
        }
        transform(re, im, false);

        const int mask = half_ - 1;
        for (int k = 0; k < half_; ++k) {
            const int j = (half_ - k) & mask;
            const float ar = re[k], ai = im[k];
            const float br = re[j], bi = -im[j];                    // conj(Z[m - k])
            const float even_r = 0.5f * (ar + br), even_i = 0.5f * (ai + bi);
            const float odd_r = 0.5f * (ai - bi), odd_i = -0.5f * (ar - br);
            const float wr = cos_n_[k], wi = sin_n_[k];
            re_out[k] = even_r + (odd_r * wr - odd_i * wi);
            im_out[k] = even_i + (odd_r * wi + odd_i * wr);
        }
        re_out[half_] = re[0] - im[0];
        im_out[half_] = 0.0f;
    }

    // `re`/`im`: n/2 + 1 bins -> `out`: n real samples, including the 1/n scaling.
    void inverse(const float* re_in, const float* im_in, float* out, float* scratch) const {
        float* re = scratch;
        float* im = scratch + half_;
        for (int k = 0; k < half_; ++k) {
            // DC and Nyquist are taken as purely real, as numpy and torch do.
            // A predicted (non-physical) spectrum can carry a spurious
            // imaginary part there, and keeping it injects broadband noise.
            const float xr = re_in[k];
            const float xi = (k == 0) ? 0.0f : im_in[k];
            const float yr = re_in[half_ - k];
            const float yi = (k == 0) ? 0.0f : -im_in[half_ - k];
            const float even_r = 0.5f * (xr + yr), even_i = 0.5f * (xi + yi);
            const float diff_r = 0.5f * (xr - yr), diff_i = 0.5f * (xi - yi);
            const float wr = cos_n_[k], wi = -sin_n_[k];            // e^{+2 pi i k / n}
            const float odd_r = diff_r * wr - diff_i * wi;
            const float odd_i = diff_r * wi + diff_i * wr;
            re[k] = even_r - odd_i;
            im[k] = even_i + odd_r;
        }
        transform(re, im, true);

        const float scale = 1.0f / static_cast<float>(half_);
        for (int k = 0; k < half_; ++k) {
            out[2 * k] = re[k] * scale;
            out[2 * k + 1] = im[k] * scale;
        }
    }

private:
    // In-place iterative radix-2 decimation-in-time complex FFT, unscaled.
    void transform(float* re, float* im, bool inverse) const {
        for (int i = 0; i < half_; ++i) {
            const int j = reverse_[i];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= half_; len <<= 1) {
            const int span = len >> 1;
            const int step = half_ / len;
            for (int base = 0; base < half_; base += len) {
                for (int k = 0, t = 0; k < span; ++k, t += step) {
                    const float wr = cos_[t];
                    const float wi = inverse ? -sin_[t] : sin_[t];
                    const int a = base + k, b = a + span;
                    const float ur = re[a], ui = im[a];
                    const float vr = re[b] * wr - im[b] * wi;
                    const float vi = re[b] * wi + im[b] * wr;
                    re[a] = ur + vr;
                    im[a] = ui + vi;
                    re[b] = ur - vr;
                    im[b] = ui - vi;
                }
            }
        }
    }

    int n_;
    int half_;
    std::vector<float> cos_, sin_, cos_n_, sin_n_;
    std::vector<int> reverse_;
};

}  // namespace demucs::dsp
