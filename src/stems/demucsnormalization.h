#pragma once

#include <cstddef>

namespace mixxx::stems {

/// Epsilon added to the standard deviation before dividing, matching
/// HTDemucs.forward()'s `(x - mean) / (1e-5 + std)`.
inline constexpr double kDemucsNormalizationEpsilon = 1e-5;

/// Mean and standard deviation of one model input or output tensor.
///
/// `standardDeviation` uses the unbiased (N-1) estimator, matching the default
/// of `torch.std()`. With fewer than two values it is zero rather than NaN, so
/// a degenerate window stays finite.
struct DemucsStatistics {
    double mean = 0.0;
    double standardDeviation = 0.0;
};

/// Measures `count` values. `pData` may be null only when `count` is zero.
DemucsStatistics demucsStatistics(const float* pData, std::size_t count);

/// Computes `(input - mean) / (1e-5 + std)`.
///
/// Input and output may alias. `count` must match the tensor the statistics
/// were measured from.
void demucsNormalize(const float* pInput,
        float* pOutput,
        std::size_t count,
        const DemucsStatistics& statistics);

/// Computes `input * std + mean`, the reference output formula.
///
/// The forward path divides by `1e-5 + std`, so this is the inverse up to that
/// small epsilon guard; matching the reference matters more than an exact
/// algebraic round trip.
///
/// Input and output may alias.
void demucsDenormalize(const float* pInput,
        float* pOutput,
        std::size_t count,
        const DemucsStatistics& statistics);

} // namespace mixxx::stems
