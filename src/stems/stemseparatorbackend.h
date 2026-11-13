#pragma once

#include "audio/types.h"
#include "util/types.h"

namespace mixxx::stems {

/// Canonical order of the four separated stems. It matches the interleaved
/// channel layout of the cache, the engine and the waveform data: drums L/R
/// occupy channels 0 and 1, bass 2 and 3, other 4 and 5, vocals 6 and 7.
enum class StemKind : int {
    Drums = 0,
    Bass = 1,
    Other = 2,
    Vocals = 3,
};

constexpr int kStemCount = 4;
/// Four stereo stems, as consumed by EngineDeck::processStem().
constexpr int kStemChannelCount = kStemCount * 2;

/// One model invocation: a window of stereo audio in, four stereo stems out.
///
/// Implementations wrap a concrete runtime (ONNX Runtime today, demucs.cpp as
/// the documented fallback) and are only ever called from a background
/// separation worker, never from the audio thread. A backend may keep a
/// session and scratch buffers between calls, so callers must not share one
/// across threads without synchronizing.
class StemSeparatorBackend {
  public:
    virtual ~StemSeparatorBackend() = default;

    /// Sample rate the backend expects for its input and produces on output.
    virtual audio::SampleRate sampleRate() const = 0;

    /// Number of frames in one inference window.
    virtual SINT windowFrames() const = 0;

    /// Number of frames shared by adjacent windows. Must be smaller than
    /// windowFrames().
    virtual SINT overlapFrames() const = 0;

    /// Separates one window.
    ///
    /// @param pInput interleaved stereo, windowFrames() * 2 samples
    /// @param pOutput interleaved eight channels, windowFrames() * 8 samples,
    ///        drums L/R, bass L/R, other L/R, vocals L/R
    /// @return false if inference failed; pOutput is then unspecified
    virtual bool processWindow(const CSAMPLE* pInput, CSAMPLE* pOutput) = 0;
};

} // namespace mixxx::stems
