// Copyright (c) 2026 Hamidreza Sadeghi
// SPDX-License-Identifier: MIT
//
// Weighted overlap-add over a window that slides with the input. Chunks are
// added at absolute sample offsets; once a sample can no longer be touched by a
// future chunk it is normalised by the accumulated weight and handed back. Only
// `capacity` samples are ever held, so memory does not grow with track length.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace demucs {

class OverlapAdd {
public:
    OverlapAdd(int planes, std::int64_t capacity)
        : planes_(planes), capacity_(capacity),
          accumulator_(static_cast<std::size_t>(planes) * capacity, 0.0f),
          weight_(static_cast<std::size_t>(capacity), 0.0f) {}

    std::int64_t base() const { return base_; }

    // `block` holds `planes` rows of `count` samples, `stride` apart, to be
    // added at absolute sample `offset` under the first `count` values of
    // `weight`.
    void add(std::int64_t offset, const float* block, std::int64_t stride,
             std::int64_t count, const float* weight) {
        const std::int64_t start = offset - base_;
        if (start < 0 || start + count > capacity_)
            throw std::out_of_range("overlap-add window is too small for this chunk");

        for (int plane = 0; plane < planes_; ++plane) {
            const float* src = block + static_cast<std::size_t>(plane) * stride;
            float* dst = accumulator_.data() + static_cast<std::size_t>(plane) * capacity_ + start;
            for (std::int64_t i = 0; i < count; ++i) dst[i] += src[i] * weight[i];
        }
        float* sum = weight_.data() + start;
        for (std::int64_t i = 0; i < count; ++i) sum[i] += weight[i];
    }

    // Finalises everything before absolute sample `upto` into `out`, laid out
    // as `planes` rows of the returned length. Returns 0 if nothing is ready.
    std::int64_t flush(std::int64_t upto, std::vector<float>& out) {
        const std::int64_t count = upto - base_;
        if (count <= 0) return 0;

        out.resize(static_cast<std::size_t>(planes_) * count);
        for (int plane = 0; plane < planes_; ++plane) {
            const float* src = accumulator_.data() + static_cast<std::size_t>(plane) * capacity_;
            float* dst = out.data() + static_cast<std::size_t>(plane) * count;
            for (std::int64_t i = 0; i < count; ++i) dst[i] = src[i] / weight_[i];
        }

        const std::int64_t keep = capacity_ - count;
        for (int plane = 0; plane < planes_; ++plane) {
            float* row = accumulator_.data() + static_cast<std::size_t>(plane) * capacity_;
            std::memmove(row, row + count, sizeof(float) * static_cast<std::size_t>(keep));
            std::fill(row + keep, row + capacity_, 0.0f);
        }
        std::memmove(weight_.data(), weight_.data() + count,
                     sizeof(float) * static_cast<std::size_t>(keep));
        std::fill(weight_.begin() + static_cast<std::size_t>(keep), weight_.end(), 0.0f);

        base_ = upto;
        return count;
    }

private:
    int planes_;
    std::int64_t capacity_;
    std::int64_t base_ = 0;
    std::vector<float> accumulator_;
    std::vector<float> weight_;
};

// demucs' triangular cross-fade: up over the first half, down over the second.
inline std::vector<float> transition_weight(int segment, float power = 1.0f) {
    std::vector<float> weight(segment);
    for (int i = 0; i < segment; ++i)
        weight[i] = static_cast<float>(i < segment / 2 ? i + 1 : segment - i);
    const float peak = *std::max_element(weight.begin(), weight.end());
    for (auto& value : weight) {
        value /= peak;
        if (power != 1.0f) value = std::pow(value, power);
    }
    return weight;
}

}  // namespace demucs
