#include "stems/demucslimiter.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

namespace mixxx::stems {

float demucsLimiterGain(const CSAMPLE* pStems, SINT frames) {
    if (frames <= 0) {
        return 1.0f;
    }
    DEBUG_ASSERT(pStems != nullptr);

    float peak = 0.0f;
    for (SINT frame = 0; frame < frames; ++frame) {
        const CSAMPLE* pFrame = pStems + frame * kStemChannelCount;
        CSAMPLE left = 0.0f;
        CSAMPLE right = 0.0f;
        for (int stem = 0; stem < kStemCount; ++stem) {
            const CSAMPLE stemLeft = pFrame[stem * 2];
            const CSAMPLE stemRight = pFrame[stem * 2 + 1];
            peak = std::max(peak, std::fabs(stemLeft));
            peak = std::max(peak, std::fabs(stemRight));
            left += stemLeft;
            right += stemRight;
        }
        peak = std::max(peak, std::fabs(left));
        peak = std::max(peak, std::fabs(right));
    }
    return peak > 1.0f ? 1.0f / peak : 1.0f;
}

void demucsApplyGain(CSAMPLE* pSamples, std::size_t sampleCount, float gain) {
    if (sampleCount == 0) {
        return;
    }
    DEBUG_ASSERT(pSamples != nullptr);
    DEBUG_ASSERT(std::isfinite(gain));
    DEBUG_ASSERT(gain >= 0.0f);

    for (std::size_t i = 0; i < sampleCount; ++i) {
        pSamples[i] *= gain;
    }
}

} // namespace mixxx::stems
