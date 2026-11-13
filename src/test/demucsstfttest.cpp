#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "stems/demucsstft.h"

namespace mixxx::stems {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kCanary = 12345.0f;

class DemucsStftTest : public testing::Test {};

std::vector<CSAMPLE> makeSignal(SINT frames, bool left, bool right) {
    std::vector<CSAMPLE> signal(static_cast<std::size_t>(frames) * 2, 0.0f);
    constexpr double frequencies[] = {110.0, 440.0, 1760.0, 7040.0};
    constexpr double amplitudes[] = {0.31, 0.23, 0.17, 0.11};
    // HTDemucs drops the Nyquist bin, so a derivative discontinuity at the
    // reflection boundary (a signal that starts or ends at a nonzero value
    // with a nonzero slope) loses a tiny broadband component there. Fading
    // both ends to zero with zero slope keeps the reflected signal smooth and
    // isolates the transform itself; the interior is unaffected either way.
    const SINT fade = std::min<SINT>(frames / 8, 4096);
    for (SINT frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / kDemucsSampleRate;
        double l = 0.0;
        double r = 0.0;
        for (int tone = 0; tone < 4; ++tone) {
            const double phase = 2.0 * kPi * frequencies[tone] * time;
            l += amplitudes[tone] * std::sin(phase + 0.13 * tone);
            r += amplitudes[tone] * 0.7 * std::sin(phase + 0.41 + 0.19 * tone);
        }
        double envelope = 1.0;
        if (fade >= 2) {
            if (frame < fade) {
                envelope *= 0.5 - 0.5 * std::cos(kPi * frame / fade);
            }
            if (frame >= frames - fade) {
                envelope *= 0.5 - 0.5 * std::cos(kPi * (frames - 1 - frame) / fade);
            }
        }
        const auto index = static_cast<std::size_t>(frame) * 2;
        signal[index] = left ? static_cast<CSAMPLE>(l * envelope) : 0.0f;
        signal[index + 1] = right ? static_cast<CSAMPLE>(r * envelope) : 0.0f;
    }
    return signal;
}

double signalToNoiseRatio(const std::vector<CSAMPLE>& reference,
        const std::vector<CSAMPLE>& actual) {
    double signalPower = 0.0;
    double errorPower = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double ref = reference[i];
        const double error = ref - static_cast<double>(actual[i]);
        signalPower += ref * ref;
        errorPower += error * error;
    }
    return errorPower == 0.0
            ? INFINITY
            : 10.0 * std::log10(signalPower / errorPower);
}

void expectRoundTrip(SINT frames) {
    const auto input = makeSignal(frames, true, true);
    const auto cacSize = demucsCacFloatCount(frames);
    std::vector<float> cac(cacSize);
    demucsAnalyze(input.data(), frames, cac.data());

    std::vector<CSAMPLE> guarded(static_cast<std::size_t>(frames) * 2 + 8,
            static_cast<CSAMPLE>(kCanary));
    demucsSynthesize(cac.data(), frames, guarded.data());
    for (std::size_t i = static_cast<std::size_t>(frames) * 2;
            i < guarded.size();
            ++i) {
        EXPECT_EQ(static_cast<CSAMPLE>(kCanary), guarded[i]);
    }
    std::vector<CSAMPLE> output(guarded.begin(),
            guarded.begin() + static_cast<std::size_t>(frames) * 2);
    EXPECT_GE(signalToNoiseRatio(input, output), 120.0);
}

TEST_F(DemucsStftTest, FrameCountsAndCacSizes) {
    EXPECT_EQ(0, demucsFrameCount(0));
    EXPECT_EQ(1, demucsFrameCount(1));
    EXPECT_EQ(1, demucsFrameCount(1024));
    EXPECT_EQ(2, demucsFrameCount(1025));
    EXPECT_EQ(336, kDemucsWindowFrameCount);
    EXPECT_EQ(kDemucsWindowFrameCount,
            demucsFrameCount(kDemucsWindowFrames));
    EXPECT_EQ(4u * 2048u * demucsFrameCount(1025),
            demucsCacFloatCount(1025));
}

TEST_F(DemucsStftTest, CaCLayoutIsChannelMajorWithTimeFastest) {
    constexpr SINT frameCount = 3;
    EXPECT_EQ(0u, demucsCacIndex(0, 0, 0, frameCount));
    EXPECT_EQ(2u, demucsCacIndex(0, 0, 2, frameCount));
    EXPECT_EQ(3u, demucsCacIndex(0, 1, 0, frameCount));
    EXPECT_EQ(2048u * 3u, demucsCacIndex(1, 0, 0, frameCount));
    EXPECT_EQ(4u * 2048u * 3u - 1,
            demucsCacIndex(3, 2047, 2, frameCount));
    EXPECT_EQ(4u * 2048u, demucsCacFloatCount(1024));
}

TEST_F(DemucsStftTest, FullWindowRoundTripIncludesEdges) {
    expectRoundTrip(kDemucsWindowFrames);
}

TEST_F(DemucsStftTest, ArbitraryLengthsRoundTripWithoutOverrun) {
    expectRoundTrip(1);
    expectRoundTrip(1024);
    expectRoundTrip(1025);
    expectRoundTrip(12345);
}

TEST_F(DemucsStftTest, AnalysisKeepsChannelsSeparate) {
    constexpr SINT frames = 4097;
    for (const bool leftOnly : {true, false}) {
        const auto input = makeSignal(frames, leftOnly, !leftOnly);
        std::vector<float> cac(demucsCacFloatCount(frames));
        demucsAnalyze(input.data(), frames, cac.data());
        const int firstSilentChannel = leftOnly ? 2 : 0;
        for (int channel = firstSilentChannel;
                channel < firstSilentChannel + 2;
                ++channel) {
            for (SINT bin = 0; bin < kDemucsBins; ++bin) {
                for (SINT frame = 0; frame < demucsFrameCount(frames); ++frame) {
                    EXPECT_NEAR(0.0f,
                            cac[demucsCacIndex(channel, bin, frame, demucsFrameCount(frames))],
                            1e-6f);
                }
            }
        }
        if (leftOnly) {
            std::vector<CSAMPLE> output(static_cast<std::size_t>(frames) * 2);
            demucsSynthesize(cac.data(), frames, output.data());
            for (SINT frame = 0; frame < frames; ++frame) {
                EXPECT_LT(std::fabs(output[static_cast<std::size_t>(frame) * 2 + 1]),
                        1e-6f);
            }
        }
    }
}

TEST_F(DemucsStftTest, ExactBinTonePeaksInItsBinOnEitherChannel) {
    constexpr SINT frames = 8192;
    constexpr int bin = 137;
    const double frequency = static_cast<double>(bin) * kDemucsSampleRate /
            kDemucsFftSize;
    for (const bool leftOnly : {true, false}) {
        std::vector<CSAMPLE> input(static_cast<std::size_t>(frames) * 2, 0.0f);
        for (SINT frame = 0; frame < frames; ++frame) {
            const CSAMPLE sample = static_cast<CSAMPLE>(std::sin(
                    2.0 * kPi * frequency * frame / kDemucsSampleRate));
            input[static_cast<std::size_t>(frame) * 2 + (leftOnly ? 0 : 1)] = sample;
        }
        const SINT timeFrames = demucsFrameCount(frames);
        std::vector<float> cac(demucsCacFloatCount(frames));
        demucsAnalyze(input.data(), frames, cac.data());
        const int realChannel = leftOnly ? 0 : 2;
        const int imaginaryChannel = realChannel + 1;
        std::vector<double> magnitudes(kDemucsBins, 0.0);
        for (SINT currentBin = 0; currentBin < kDemucsBins; ++currentBin) {
            for (SINT frame = 0; frame < timeFrames; ++frame) {
                const double real = cac[demucsCacIndex(realChannel,
                        currentBin,
                        frame,
                        timeFrames)];
                const double imaginary = cac[demucsCacIndex(imaginaryChannel,
                        currentBin,
                        frame,
                        timeFrames)];
                magnitudes[currentBin] += std::hypot(real, imaginary);
            }
        }
        const auto peak = static_cast<int>(std::distance(magnitudes.begin(),
                std::max_element(magnitudes.begin(), magnitudes.end())));
        EXPECT_LE(std::abs(peak - bin), 1);
    }
}

TEST_F(DemucsStftTest, DcInputPinsScaleAndChannelOrder) {
    // A left-only DC signal has a closed-form bin 0: sum(hann) / sqrt(4096) =
    // 2048 / 64 = 32. This fails if the normalization is wrong in both
    // directions, or if the real and imaginary channels are swapped.
    constexpr SINT frames = 4096;
    std::vector<CSAMPLE> input(static_cast<std::size_t>(frames) * 2, 0.0f);
    for (SINT frame = 0; frame < frames; ++frame) {
        input[static_cast<std::size_t>(frame) * 2] = 1.0f;
    }
    const SINT timeFrames = demucsFrameCount(frames);
    ASSERT_EQ(4, timeFrames);
    std::vector<float> cac(demucsCacFloatCount(frames));
    demucsAnalyze(input.data(), frames, cac.data());

    for (SINT frame = 0; frame < timeFrames; ++frame) {
        EXPECT_NEAR(32.0f, cac[demucsCacIndex(0, 0, frame, timeFrames)], 1e-4f);
        EXPECT_NEAR(0.0f, cac[demucsCacIndex(1, 0, frame, timeFrames)], 1e-5f);
        EXPECT_NEAR(0.0f, cac[demucsCacIndex(2, 0, frame, timeFrames)], 1e-5f);
        EXPECT_NEAR(0.0f, cac[demucsCacIndex(3, 0, frame, timeFrames)], 1e-5f);
    }
}

TEST_F(DemucsStftTest, InteriorFrameMatchesDirectDft) {
    // Locks the sign convention, normalization and frame alignment against a
    // direct evaluation of the defining sum for one interior frame.
    constexpr SINT frames = kDemucsWindowFrames;
    constexpr SINT frame = 100;
    constexpr int bin = 137;
    const auto input = makeSignal(frames, true, true);
    const SINT timeFrames = demucsFrameCount(frames);
    ASSERT_LT(frame, timeFrames);
    std::vector<float> cac(demucsCacFloatCount(frames));
    demucsAnalyze(input.data(), frames, cac.data());

    const SINT originalStart =
            frame * kDemucsHopLength - kDemucsHopLength * 3 / 2;
    for (int channel = 0; channel < 2; ++channel) {
        double expectedReal = 0.0;
        double expectedImaginary = 0.0;
        for (int n = 0; n < kDemucsFftSize; ++n) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * n / kDemucsFftSize);
            const double sample =
                    input[static_cast<std::size_t>(originalStart + n) * 2 + channel];
            const double angle = 2.0 * kPi * bin * n / kDemucsFftSize;
            expectedReal += sample * window * std::cos(angle);
            expectedImaginary -= sample * window * std::sin(angle);
        }
        EXPECT_NEAR(expectedReal / 64.0,
                cac[demucsCacIndex(channel * 2, bin, frame, timeFrames)],
                1e-4);
        EXPECT_NEAR(expectedImaginary / 64.0,
                cac[demucsCacIndex(channel * 2 + 1, bin, frame, timeFrames)],
                1e-4);
    }
}

TEST_F(DemucsStftTest, ZeroSpectrumSynthesizesSilence) {
    constexpr SINT frames = 1025;
    std::vector<float> cac(demucsCacFloatCount(frames), 0.0f);
    std::vector<CSAMPLE> output(static_cast<std::size_t>(frames) * 2, 1.0f);
    demucsSynthesize(cac.data(), frames, output.data());
    for (CSAMPLE sample : output) {
        EXPECT_FLOAT_EQ(0.0f, sample);
    }
}

TEST_F(DemucsStftTest, ZeroFramesAreNoOps) {
    EXPECT_EQ(0, demucsFrameCount(-1));
    std::vector<CSAMPLE> audio(8, static_cast<CSAMPLE>(kCanary));
    std::vector<float> cac(8, kCanary);
    demucsAnalyze(audio.data(), 0, cac.data());
    demucsSynthesize(cac.data(), 0, audio.data());
    demucsAnalyze(audio.data(), -1, cac.data());
    demucsSynthesize(cac.data(), -1, audio.data());
    for (CSAMPLE sample : audio) {
        EXPECT_EQ(static_cast<CSAMPLE>(kCanary), sample);
    }
    for (float value : cac) {
        EXPECT_EQ(kCanary, value);
    }
}

} // namespace
} // namespace mixxx::stems
