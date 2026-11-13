#pragma once

#include <functional>

#include "stems/stemseparatorbackend.h"
#include "util/types.h"

namespace mixxx::stems {

/// Reports separation progress as a fraction in [0, 1].
using SeparationProgressCallback = std::function<void(double progress)>;

/// Returns true to abort the separation. Polled between windows, so a cancel
/// request never waits for a long-running inference call to finish.
using SeparationCancelCallback = std::function<bool()>;

/// Separates a whole track into four stereo stems by running a backend over
/// overlapping windows and cross-fading the results.
///
/// The implementation streams: it holds two windows plus a small overlap
/// accumulator, not the whole track, so memory use does not grow with the
/// track length. Frames are written to pOutput as soon as no later window can
/// contribute to them.
///
/// Runs on the calling thread; call it from a background worker. The backend
/// and both buffers must stay alive for the duration of the call.
class StemSeparation final {
  public:
    /// @param pBackend backend to run; must not be null
    /// @param pInput interleaved stereo at pBackend->sampleRate(),
    ///        frameCount * 2 samples
    /// @param frameCount number of frames to separate
    /// @param pOutput interleaved eight channels, frameCount * 8 samples
    /// @return false if the backend failed or the caller cancelled; the
    ///         affected part of pOutput is then unspecified
    static bool separate(StemSeparatorBackend* pBackend,
            const CSAMPLE* pInput,
            SINT frameCount,
            CSAMPLE* pOutput,
            const SeparationProgressCallback& progress = SeparationProgressCallback(),
            const SeparationCancelCallback& cancel = SeparationCancelCallback());

    StemSeparation() = delete;
};

} // namespace mixxx::stems
