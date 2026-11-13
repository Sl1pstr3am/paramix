#include "stems/demucsstft.h"

#include <dsp/transforms/FFT.h>

#include <cmath>
#include <vector>

#include "util/assert.h"

namespace mixxx::stems {
namespace {

constexpr double kPi = 3.14159265358979323846;
/// sqrt(kDemucsFftSize), i.e. the normalizing factor of the forward transform.
constexpr double kFftNormalization = 64.0;
static_assert(kDemucsFftSize == 4096);
/// Reflective padding applied by HTDemucs._spec before the centered transform.
constexpr SINT kEdgePad = kDemucsHopLength * 3 / 2;

/// Periodic Hann window, matching torch.hann_window(n_fft).
std::vector<double> makeWindow() {
    std::vector<double> window(kDemucsFftSize);
    for (int n = 0; n < kDemucsFftSize; ++n) {
        window[n] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / kDemucsFftSize);
    }
    return window;
}

/// Reflective index mapping, matching torch's pad(mode="reflect").
///
/// torch rejects pads longer than the input; this mapping keeps reflecting
/// instead, so short signals are handled without a special case.
SINT reflectIndex(SINT index, SINT length) {
    if (length <= 1) {
        return 0;
    }
    const SINT period = 2 * (length - 1);
    SINT reflected = index % period;
    if (reflected < 0) {
        reflected += period;
    }
    if (reflected >= length) {
        reflected = period - reflected;
    }
    return reflected;
}

/// Number of samples in the reflect-padded signal HTDemucs analyzes.
SINT paddedLength(SINT frameCount) {
    return frameCount * kDemucsHopLength + 2 * kEdgePad;
}

} // namespace

void demucsAnalyze(const CSAMPLE* pStereo, SINT frames, float* pCac) {
    if (frames <= 0) {
        return;
    }
    DEBUG_ASSERT(pStereo != nullptr);
    DEBUG_ASSERT(pCac != nullptr);

    const SINT frameCount = demucsFrameCount(frames);
    const SINT length = paddedLength(frameCount);
    const std::vector<double> window = makeWindow();

    std::vector<double> padded(static_cast<std::size_t>(length));
    // FFTReal::forward writes the full conjugate spectrum, so both buffers
    // must hold kDemucsFftSize samples even though only kDemucsBins are used.
    std::vector<double> windowed(kDemucsFftSize);
    std::vector<double> real(kDemucsFftSize);
    std::vector<double> imaginary(kDemucsFftSize);
    FFTReal fft(kDemucsFftSize);

    for (int channel = 0; channel < 2; ++channel) {
        for (SINT i = 0; i < length; ++i) {
            padded[i] = pStereo[reflectIndex(i - kEdgePad, frames) * 2 + channel];
        }

        // The centered transform reflects another half FFT on each side. Its
        // first two frames and last two frames are dropped, which cancels the
        // offset exactly: kept frame f covers padded samples
        // [f * hop, f * hop + FFT), with no reflection.
        for (SINT frame = 2; frame < frameCount + 2; ++frame) {
            const SINT start = (frame - 2) * kDemucsHopLength;
            for (int n = 0; n < kDemucsFftSize; ++n) {
                windowed[n] = padded[start + n] * window[n];
            }
            fft.forward(windowed.data(), real.data(), imaginary.data());

            const SINT outputFrame = frame - 2;
            for (int bin = 0; bin < kDemucsBins; ++bin) {
                pCac[demucsCacIndex(channel * 2, bin, outputFrame, frameCount)] =
                        static_cast<float>(real[bin] / kFftNormalization);
                pCac[demucsCacIndex(
                        channel * 2 + 1, bin, outputFrame, frameCount)] =
                        static_cast<float>(imaginary[bin] / kFftNormalization);
            }
        }
    }
}

namespace {

/// Shared body of demucsSynthesize() and demucsReconstruct(). `pResidual` may
/// alias `pStereo`; the residual is read before the matching output is
/// written, so an in-place combination is safe.
void synthesize(const float* pCac,
        const CSAMPLE* pResidual,
        SINT frames,
        CSAMPLE* pStereo) {
    if (frames <= 0) {
        return;
    }
    DEBUG_ASSERT(pCac != nullptr);
    DEBUG_ASSERT(pStereo != nullptr);

    const SINT frameCount = demucsFrameCount(frames);
    const SINT length = paddedLength(frameCount);
    const std::vector<double> window = makeWindow();

    std::vector<double> accumulator(static_cast<std::size_t>(length) * 2, 0.0);
    std::vector<double> envelope(static_cast<std::size_t>(length), 0.0);
    std::vector<double> real(kDemucsBins + 1);
    std::vector<double> imaginary(kDemucsBins + 1);
    std::vector<double> time(kDemucsFftSize);
    FFTReal fft(kDemucsFftSize);

    for (SINT frame = 2; frame < frameCount + 2; ++frame) {
        // Dropping the two centered frames on each side cancels the centered
        // transform's half-FFT offset, so kept frame f starts at padded sample
        // f * hop.
        const SINT start = (frame - 2) * kDemucsHopLength;
        for (int n = 0; n < kDemucsFftSize; ++n) {
            envelope[start + n] += window[n] * window[n];
        }

        for (int channel = 0; channel < 2; ++channel) {
            for (int bin = 0; bin < kDemucsBins; ++bin) {
                real[bin] = pCac[demucsCacIndex(
                        channel * 2, bin, frame - 2, frameCount)];
                imaginary[bin] = pCac[demucsCacIndex(
                        channel * 2 + 1, bin, frame - 2, frameCount)];
            }
            // The Nyquist bin was dropped during analysis.
            real[kDemucsBins] = 0.0;
            imaginary[kDemucsBins] = 0.0;
            fft.inverse(real.data(), imaginary.data(), time.data());

            for (int n = 0; n < kDemucsFftSize; ++n) {
                accumulator[static_cast<std::size_t>(channel) * length +
                        start + n] += time[n] * kFftNormalization * window[n];
            }
        }
    }

    for (SINT i = 0; i < frames; ++i) {
        const SINT index = kEdgePad + i;
        const double windowSum = envelope[index];
        const double scale = windowSum > 0.0 ? 1.0 / windowSum : 0.0;
        const CSAMPLE residualLeft =
                pResidual != nullptr ? pResidual[i * 2] : CSAMPLE_ZERO;
        const CSAMPLE residualRight =
                pResidual != nullptr ? pResidual[i * 2 + 1] : CSAMPLE_ZERO;
        pStereo[i * 2] = static_cast<CSAMPLE>(
                accumulator[index] * scale + residualLeft);
        pStereo[i * 2 + 1] = static_cast<CSAMPLE>(
                accumulator[static_cast<std::size_t>(length) + index] * scale +
                residualRight);
    }
}

} // namespace

void demucsSynthesize(const float* pCac, SINT frames, CSAMPLE* pStereo) {
    synthesize(pCac, nullptr, frames, pStereo);
}

void demucsReconstruct(const float* pCac,
        const CSAMPLE* pResidual,
        SINT frames,
        CSAMPLE* pStereo) {
    synthesize(pCac, pResidual, frames, pStereo);
}

} // namespace mixxx::stems
