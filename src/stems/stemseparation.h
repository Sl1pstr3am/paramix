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

/// Fills pBuffer with exactly `frames` frames of interleaved stereo and returns
/// the number written. Returning fewer frames aborts the separation.
using SeparationInputCallback = std::function<SINT(CSAMPLE* pBuffer, SINT frames)>;

/// Receives interleaved eight channels in order, exactly once. Frames are
/// delivered in several calls, never more than one window at a time, and the
/// buffer is only valid for the duration of the call.
using SeparationOutputCallback =
        std::function<bool(const CSAMPLE* pBuffer, SINT frames)>;

/// Separates a whole track into four stereo stems by running a backend over
/// overlapping windows and cross-fading the results.
///
/// The implementation streams: it holds two windows plus an overlap
/// accumulator, so memory use is bounded by the backend's window size and does
/// not grow with the track length. Frames are written as soon as no later
/// window can contribute to them, which lets a caller encode them
/// incrementally.
///
/// Runs on the calling thread; call it from a background worker. The backend
/// and all callbacks are used only on that thread.
class StemSeparation final {
  public:
    /// @param pBackend backend to run; must not be null
    /// @param frameCount number of frames to separate; zero is allowed
    /// @param readInput supplies the frames in order, exactly frameCount of them
    /// @param writeOutput receives the separated frames in order
    /// @return false if the backend failed, a callback aborted, or the caller
    ///         cancelled; the output is then incomplete
    static bool separate(StemSeparatorBackend* pBackend,
            SINT frameCount,
            const SeparationInputCallback& readInput,
            const SeparationOutputCallback& writeOutput,
            const SeparationProgressCallback& progress = SeparationProgressCallback(),
            const SeparationCancelCallback& cancel = SeparationCancelCallback());

    /// Convenience overload for callers that already hold the whole track.
    ///
    /// @param pInput interleaved stereo at pBackend->sampleRate(),
    ///        frameCount * 2 samples
    /// @param pOutput interleaved eight channels, frameCount * 8 samples
    static bool separate(StemSeparatorBackend* pBackend,
            const CSAMPLE* pInput,
            SINT frameCount,
            CSAMPLE* pOutput,
            const SeparationProgressCallback& progress = SeparationProgressCallback(),
            const SeparationCancelCallback& cancel = SeparationCancelCallback());

    StemSeparation() = delete;
};

} // namespace mixxx::stems
