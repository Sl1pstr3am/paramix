#pragma once

#include <cstddef>

#include "util/types.h"

namespace mixxx::stems {

/// FFT size used by HTDemucs v4 and by the host-side transform below.
inline constexpr int kDemucsFftSize = 4096;
/// Distance between adjacent analysis frames.
inline constexpr int kDemucsHopLength = kDemucsFftSize / 4;
/// Sample rate the model and its transform are defined at.
inline constexpr int kDemucsSampleRate = 44100;
/// Number of frequency bins after the Nyquist bin is dropped.
inline constexpr int kDemucsBins = kDemucsFftSize / 2;
/// Number of real-valued channels in the complex-as-channels layout.
inline constexpr int kDemucsCacChannels = 4;

/// Number of STFT frames produced for `frames` input frames.
inline constexpr SINT demucsFrameCount(SINT frames) {
    return frames <= 0 ? 0 : (frames + kDemucsHopLength - 1) / kDemucsHopLength;
}

/// One inference window: 7.8 seconds at the model sample rate.
inline constexpr SINT kDemucsWindowFrames = 343980;
/// Frames the transform produces for one full inference window.
inline constexpr SINT kDemucsWindowFrameCount =
        demucsFrameCount(kDemucsWindowFrames);

/// Host-side implementation of HTDemucs v4's STFT and inverse STFT.
///
/// HTDemucs uses complex STFT/iSTFT tensors inside its `forward()`, which
/// cannot be exported to a standard inference graph without falling back to
/// the CPU for most of the model. The reference implementation therefore
/// places the transform at the "complex as channels" (CaC) boundary: the host
/// computes a spectrogram, the runtime executes only the convolutional and
/// transformer core, and the host turns the predicted spectrograms back into
/// audio. These functions are that boundary.
///
/// The parameters and framing follow `HTDemucs._spec` / `_ispec` in the
/// reference implementation (Copyright (c) Meta Platforms, Inc., MIT):
///
/// - a 4096-point FFT, a 1024-frame hop and a periodic Hann window;
/// - a normalized forward transform, i.e. the spectrum is divided by
///   sqrt(4096), matching `torch.stft(..., normalized=True)`;
/// - the Nyquist bin is dropped, leaving 2048 bins;
/// - the signal is reflected by 3/2 hops on each side and then by one more
///   half FFT on each side by the centered transform;
/// - two edge frames are dropped from each side, so the frame count is
///   exactly ceil(frames / 1024) and matches the model's time axis;
/// - four channels in the order left real, left imaginary, right real, right
///   imaginary, matching `HTDemucs._magnitude`.
///
/// `demucsSynthesize()` is the least-squares inverse of `demucsAnalyze()`:
/// analysis followed by synthesis reproduces the input. It deliberately
/// accumulates the window envelope only over the frames that carry a
/// spectrum. `torch.istft` also counts the zero-padded edge frames, which
/// fades the first and last 3/2 hops of the reference implementation; using
/// the present frames keeps the transform a true round trip and avoids a
/// fade at inference-window boundaries. As in the reference, the dropped
/// Nyquist bin means a signal with a derivative discontinuity where it is
/// reflected at a window boundary loses that bin's (tiny) contribution there.
///
/// Both functions allocate scratch storage and run several hundred FFTs for a
/// full window. Call them from a background worker, never from the real-time
/// audio thread. They do not share state, so separate calls may run on
/// different threads; a single call must not be split across threads.

/// Number of floats in the CaC spectrogram produced for `frames` input frames.
inline constexpr std::size_t demucsCacFloatCount(SINT frames) {
    return static_cast<std::size_t>(demucsFrameCount(frames)) *
            kDemucsBins * kDemucsCacChannels;
}

/// Offset of one value in a CaC buffer with `frameCount` time frames.
///
/// @param channel 0 = left real, 1 = left imaginary, 2 = right real,
///        3 = right imaginary
/// @param bin frequency bin in [0, kDemucsBins)
/// @param frame time frame in [0, frameCount)
inline constexpr std::size_t demucsCacIndex(
        int channel, int bin, SINT frame, SINT frameCount) {
    return (static_cast<std::size_t>(channel) * kDemucsBins + bin) *
            static_cast<std::size_t>(frameCount) +
            static_cast<std::size_t>(frame);
}

/// Computes the CaC spectrogram of interleaved stereo audio.
///
/// @param pStereo interleaved stereo, `frames * 2` samples at
///        kDemucsSampleRate
/// @param frames number of input frames; zero is allowed
/// @param pCac receives `demucsCacFloatCount(frames)` floats in the layout
///        described by demucsCacIndex(); must not be null when frames > 0
void demucsAnalyze(const CSAMPLE* pStereo, SINT frames, float* pCac);

/// Reconstructs interleaved stereo audio from a CaC spectrogram.
///
/// @param pCac `demucsCacFloatCount(frames)` floats in the layout described
///        by demucsCacIndex(); must not be null when frames > 0
/// @param frames number of output frames; usually the frame count that was
///        passed to demucsAnalyze(), but any positive length is accepted
/// @param pStereo receives `frames * 2` interleaved samples at
///        kDemucsSampleRate
void demucsSynthesize(const float* pCac, SINT frames, CSAMPLE* pStereo);

/// Reconstructs one stem as `residual + iSTFT(spectrum)`.
///
/// Hybrid HTDemucs models return both a spectrogram and a time-domain estimate;
/// this is the final combination for one stem (and for a bag, run it once after
/// DemucsBagAverage::finalize()).
///
/// @param pCac `demucsCacFloatCount(frames)` floats in the layout described
///        by demucsCacIndex(); must not be null when frames > 0
/// @param pResidual interleaved stereo time-domain estimate, or null for zero;
///        may alias pStereo
/// @param frames number of output frames
/// @param pStereo receives `frames * 2` interleaved samples at
///        kDemucsSampleRate
void demucsReconstruct(const float* pCac,
        const CSAMPLE* pResidual,
        SINT frames,
        CSAMPLE* pStereo);

} // namespace mixxx::stems
