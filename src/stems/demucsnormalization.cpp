#include "stems/demucsnormalization.h"

#include <cmath>

#include "util/assert.h"

namespace mixxx::stems {

DemucsStatistics demucsStatistics(const float* pData, std::size_t count) {
    if (count == 0) {
        return {};
    }
    DEBUG_ASSERT(pData != nullptr);

    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sum += pData[i];
    }
    const double mean = sum / static_cast<double>(count);
    if (count < 2) {
        return {mean, 0.0};
    }

    double squaredDeviations = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double deviation = static_cast<double>(pData[i]) - mean;
        squaredDeviations += deviation * deviation;
    }
    return {mean,
            std::sqrt(squaredDeviations / static_cast<double>(count - 1))};
}

void demucsNormalize(const float* pInput,
        float* pOutput,
        std::size_t count,
        const DemucsStatistics& statistics) {
    if (count == 0) {
        return;
    }
    DEBUG_ASSERT(pInput != nullptr);
    DEBUG_ASSERT(pOutput != nullptr);

    const double divisor =
            kDemucsNormalizationEpsilon + statistics.standardDeviation;
    for (std::size_t i = 0; i < count; ++i) {
        pOutput[i] = static_cast<float>(
                (static_cast<double>(pInput[i]) - statistics.mean) / divisor);
    }
}

void demucsDenormalize(const float* pInput,
        float* pOutput,
        std::size_t count,
        const DemucsStatistics& statistics) {
    if (count == 0) {
        return;
    }
    DEBUG_ASSERT(pInput != nullptr);
    DEBUG_ASSERT(pOutput != nullptr);

    for (std::size_t i = 0; i < count; ++i) {
        pOutput[i] = static_cast<float>(
                static_cast<double>(pInput[i]) *
                        statistics.standardDeviation +
                statistics.mean);
    }
}

} // namespace mixxx::stems
