#pragma once

#include <cstddef>

#include "stems/stemseparatorbackend.h"
#include "util/types.h"

namespace mixxx::stems {

/// Returns the shared gain that keeps four separated stems inside [-1, 1],
/// or 1.0 when nothing would clip.
///
/// The peak is taken over every stem sample and over the sum of the four
/// stems, so applying the returned gain also keeps the reconstructed mix
/// inside range. A single scalar keeps the stems phase-coherent and preserves
/// the property that they sum back to the original mix.
///
/// @param pStems interleaved eight-channel audio in StemKind order; samples
///        must be finite
/// @param frames number of frames; zero returns 1.0
float demucsLimiterGain(const CSAMPLE* pStems, SINT frames);

/// Multiplies `sampleCount` samples by `gain` in place.
void demucsApplyGain(CSAMPLE* pSamples, std::size_t sampleCount, float gain);

} // namespace mixxx::stems
