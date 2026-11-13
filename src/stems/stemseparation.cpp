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
/// correct when the last window advances by less than a full hop.
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

/// Divides the finalized frames of the accumulator by their summed weight.
void normalizeFrames(CSAMPLE* pAccumulator,
        const std::vector<CSAMPLE>& weights,
        SINT firstLocalFrame,
        SINT frames) {
    for (SINT frame = 0; frame < frames; ++frame) {
        const SINT local = firstLocalFrame + frame;
        const CSAMPLE weightSum = weights[static_cast<std::size_t>(local)];
        const SINT base = local * kStemChannelCount;
        if (weightSum > kZero) {
            for (int channel = 0; channel < kStemChannelCount; ++channel) {
                pAccumulator[base + channel] /= weightSum;
            }
        } else {
            for (int channel = 0; channel < kStemChannelCount; ++channel) {
                pAccumulator[base + channel] = kZero;
            }
        }
    }
}

} // namespace

bool StemSeparation::separate(StemSeparatorBackend* pBackend,
        SINT frameCount,
        const SeparationInputCallback& readInput,
        const SeparationOutputCallback& writeOutput,
        const SeparationProgressCallback& progress,
        const SeparationCancelCallback& cancel) {
    if (pBackend == nullptr || !readInput || !writeOutput) {
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

    const SINT hopFrames = windowFrames - overlapFrames;
    const SINT lastStart = std::max<SINT>(0, frameCount - windowFrames);
    const SINT windowSamples = windowFrames * kStemChannelCount;

    SampleBuffer inputWindow(windowFrames * 2);
    SampleBuffer outputWindow(windowSamples);
    SampleBuffer accumulator(windowSamples);
    // SampleBuffer leaves its storage uninitialized. The first window has a
    // zero shift, so shiftAndZero() below is a no-op and would otherwise
    // accumulate the first window onto heap garbage.
    accumulator.clear();
    std::vector<CSAMPLE> weights(static_cast<std::size_t>(windowFrames), kZero);

    // Fill the first window. Everything past the end of the track is zero, so
    // the backend always sees a full window.
    const SINT firstFrames = std::min(windowFrames, frameCount);
    if (readInput(inputWindow.data(), firstFrames) != firstFrames) {
        return false;
    }
    if (firstFrames < windowFrames) {
        std::fill(inputWindow.data() + firstFrames * 2,
                inputWindow.data() + windowFrames * 2,
                kZero);
    }

    SINT accumulatorBase = 0;
    SINT writePosition = 0;
    SINT windowStart = 0;
    bool first = true;

    while (true) {
        if (cancel && cancel()) {
            return false;
        }

        const bool last = windowStart == lastStart;
        const SINT nextStart =
                last ? frameCount : std::min(windowStart + hopFrames, lastStart);

        // Move the accumulator so that its first frame is windowStart.
        const SINT shift = windowStart - accumulatorBase;
        shiftAndZero(accumulator.data(), windowSamples, shift * kStemChannelCount);
        shiftAndZero(weights.data(), windowFrames, shift);
        accumulatorBase = windowStart;

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

        // Every frame before the next window start can no longer change, so it
        // is normalized and handed to the caller now.
        const SINT finalEnd = last ? frameCount : nextStart;
        const SINT finalFrames = finalEnd - writePosition;
        if (finalFrames > 0) {
            const SINT firstLocalFrame = writePosition - accumulatorBase;
            normalizeFrames(pAccumulator, weights, firstLocalFrame, finalFrames);
            if (!writeOutput(pAccumulator + firstLocalFrame * kStemChannelCount,
                        finalFrames)) {
                return false;
            }
        }
        writePosition = finalEnd;

        if (progress) {
            progress(std::min(1.0,
                    static_cast<double>(finalEnd) /
                            static_cast<double>(frameCount)));
        }

        if (last) {
            break;
        }

        // Slide the input window forward by the same amount the window start
        // advances, then read exactly the frames that entered it.
        const SINT advance = nextStart - windowStart;
        DEBUG_ASSERT(advance > 0);
        DEBUG_ASSERT(advance <= hopFrames);
        const SINT retainedFrames = windowFrames - advance;
        std::memmove(inputWindow.data(),
                inputWindow.data() + advance * 2,
                sizeof(CSAMPLE) * static_cast<std::size_t>(retainedFrames) * 2);
        const SINT toRead = std::min(advance, frameCount - nextStart);
        if (toRead > 0 &&
                readInput(inputWindow.data() + retainedFrames * 2, toRead) !=
                        toRead) {
            return false;
        }
        if (toRead < advance) {
            std::fill(inputWindow.data() + (retainedFrames + toRead) * 2,
                    inputWindow.data() + windowFrames * 2,
                    kZero);
        }

        windowStart = nextStart;
        first = false;
    }

    return true;
}

bool StemSeparation::separate(StemSeparatorBackend* pBackend,
        const CSAMPLE* pInput,
        SINT frameCount,
        CSAMPLE* pOutput,
        const SeparationProgressCallback& progress,
        const SeparationCancelCallback& cancel) {
    if (pInput == nullptr || pOutput == nullptr) {
        return false;
    }

    SINT readPosition = 0;
    SINT writePosition = 0;
    return separate(
            pBackend,
            frameCount,
            [&readPosition, pInput](CSAMPLE* pBuffer, SINT frames) {
                std::memcpy(pBuffer,
                        pInput + readPosition * 2,
                        sizeof(CSAMPLE) * static_cast<std::size_t>(frames) * 2);
                readPosition += frames;
                return frames;
            },
            [&writePosition, pOutput](
                    const CSAMPLE* pBuffer, SINT frames) -> bool {
                std::memcpy(pOutput + writePosition * kStemChannelCount,
                        pBuffer,
                        sizeof(CSAMPLE) * static_cast<std::size_t>(frames) *
                                kStemChannelCount);
                writePosition += frames;
                return true;
            },
            progress,
            cancel);
}

} // namespace mixxx::stems
