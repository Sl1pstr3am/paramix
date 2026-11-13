#include "stems/stemseparation.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "util/assert.h"
#include "util/samplebuffer.h"

namespace mixxx::stems {

namespace {

constexpr CSAMPLE kZero = 0.0f;

/// Linear crossfade weight for local frame `frame` of one window.
///
/// Adjacent windows overlap by `overlapFrames`, so the fade-out of one window
/// and the fade-in of the next sum to exactly one across the overlap. The
/// accumulator divides by the summed weight anyway, which keeps the result
/// correct if a clamped last window overlaps by more than the nominal amount.
CSAMPLE windowWeight(SINT frame,
        SINT windowFrames,
        SINT overlapFrames,
        bool fadeIn,
        bool fadeOut) {
    CSAMPLE weight = 1.0f;
    if (fadeIn && frame < overlapFrames) {
        weight *= static_cast<CSAMPLE>(frame) / overlapFrames;
    }
    if (fadeOut && frame >= windowFrames - overlapFrames) {
        weight *= static_cast<CSAMPLE>(windowFrames - frame) / overlapFrames;
    }
    return weight;
}

void shiftAndZero(CSAMPLE* pBuffer, SINT length, SINT shift) {
    DEBUG_ASSERT(shift >= 0);
    DEBUG_ASSERT(shift <= length);
    if (shift == 0) {
        return;
    }
    if (shift < length) {
        std::memmove(pBuffer,
                pBuffer + shift,
                sizeof(CSAMPLE) * static_cast<std::size_t>(length - shift));
    }
    std::fill(pBuffer + (length - shift), pBuffer + length, kZero);
}

} // namespace

bool StemSeparation::separate(StemSeparatorBackend* pBackend,
        const CSAMPLE* pInput,
        SINT frameCount,
        CSAMPLE* pOutput,
        const SeparationProgressCallback& progress,
        const SeparationCancelCallback& cancel) {
    if (pBackend == nullptr || pInput == nullptr || pOutput == nullptr) {
        return false;
    }
    if (frameCount < 0) {
        return false;
    }

    if (frameCount == 0) {
        if (progress) {
            progress(1.0);
        }
        return true;
    }

    const SINT windowFrames = pBackend->windowFrames();
    const SINT overlapFrames = pBackend->overlapFrames();
    if (windowFrames <= 0 || overlapFrames < 0 ||
            overlapFrames >= windowFrames) {
        DEBUG_ASSERT(!"invalid separator window geometry");
        return false;
    }

    const SINT windowSamples = windowFrames * kStemChannelCount;
    SampleBuffer inputWindow(windowFrames * 2);
    SampleBuffer outputWindow(windowSamples);
    SampleBuffer accumulator(windowSamples);
    std::vector<CSAMPLE> weights(static_cast<std::size_t>(windowFrames), kZero);

    const SINT hopFrames = windowFrames - overlapFrames;
    const SINT lastStart = std::max<SINT>(0, frameCount - windowFrames);

    SINT accumulatorBase = 0;
    SINT writePosition = 0;
    SINT start = 0;
    bool first = true;

    while (true) {
        if (cancel && cancel()) {
            return false;
        }

        const SINT windowStart = std::min(start, lastStart);
        const bool last = windowStart == lastStart;
        const SINT nextStart =
                last ? frameCount : std::min(windowStart + hopFrames, lastStart);

        // Move the accumulator so that its first sample is windowStart.
        const SINT shift = windowStart - accumulatorBase;
        shiftAndZero(accumulator.data(), windowSamples, shift * kStemChannelCount);
        shiftAndZero(weights.data(), windowFrames, shift);
        accumulatorBase = windowStart;

        // Copy the window, zero-padding past the end of the track so that the
        // model sees a full window.
        const SINT availableFrames =
                std::min(windowFrames, frameCount - windowStart);
        std::memcpy(inputWindow.data(),
                pInput + windowStart * 2,
                sizeof(CSAMPLE) * static_cast<std::size_t>(availableFrames) * 2);
        if (availableFrames < windowFrames) {
            std::fill(inputWindow.data() + availableFrames * 2,
                    inputWindow.data() + windowFrames * 2,
                    kZero);
        }

        if (!pBackend->processWindow(inputWindow.data(), outputWindow.data())) {
            return false;
        }

        const bool fadeIn = !first;
        const bool fadeOut = !last;
        CSAMPLE* pAccumulator = accumulator.data();
        for (SINT frame = 0; frame < windowFrames; ++frame) {
            const CSAMPLE weight = windowWeight(
                    frame, windowFrames, overlapFrames, fadeIn, fadeOut);
            if (weight == kZero) {
                continue;
            }
            weights[static_cast<std::size_t>(frame)] += weight;
            const SINT base = frame * kStemChannelCount;
            for (int channel = 0; channel < kStemChannelCount; ++channel) {
                pAccumulator[base + channel] +=
                        outputWindow[base + channel] * weight;
            }
        }

        // Every frame before the next window start can no longer change.
        const SINT finalEnd = last ? frameCount : nextStart;
        for (SINT frame = writePosition; frame < finalEnd; ++frame) {
            const SINT local = frame - accumulatorBase;
            DEBUG_ASSERT(local >= 0);
            DEBUG_ASSERT(local < windowFrames);
            const CSAMPLE weightSum = weights[static_cast<std::size_t>(local)];
            const SINT outputBase = frame * kStemChannelCount;
            const SINT accumulatorSample = local * kStemChannelCount;
            if (weightSum > kZero) {
                for (int channel = 0; channel < kStemChannelCount; ++channel) {
                    pOutput[outputBase + channel] =
                            pAccumulator[accumulatorSample + channel] / weightSum;
                }
            } else {
                for (int channel = 0; channel < kStemChannelCount; ++channel) {
                    pOutput[outputBase + channel] = kZero;
                }
            }
        }
        writePosition = finalEnd;

        if (progress) {
            progress(std::min(1.0,
                    static_cast<double>(finalEnd) / static_cast<double>(frameCount)));
        }

        if (last) {
            break;
        }
        start = nextStart;
        first = false;
    }

    return true;
}

} // namespace mixxx::stems
