#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "audio/types.h"
#include "stems/stemseparation.h"
#include "util/types.h"

namespace mixxx::stems {
namespace {

// A deliberately tiny window keeps the tests fast while still exercising
// multiple hops, the fade region and the clamped last window.
constexpr SINT kWindowFrames = 1024;
constexpr SINT kOverlapFrames = 256;
constexpr CSAMPLE kTolerance = 1e-5f;

/// Copies the stereo input into every stem at a fixed gain.
///
/// Overlap-add of a constant-gain backend must reproduce the input exactly, so
/// this isolates the windowing arithmetic from any model behaviour: a gap, a
/// dropped window or a wrong crossfade shows up as a mismatch.
class CopyBackend : public StemSeparatorBackend {
  public:
    audio::SampleRate sampleRate() const override {
        return audio::SampleRate(44100);
    }
    SINT windowFrames() const override {
        return kWindowFrames;
    }
    SINT overlapFrames() const override {
        return kOverlapFrames;
    }

    bool processWindow(const CSAMPLE* pInput, CSAMPLE* pOutput) override {
        ++m_callCount;
        for (SINT frame = 0; frame < kWindowFrames; ++frame) {
            const CSAMPLE left = pInput[frame * 2];
            const CSAMPLE right = pInput[frame * 2 + 1];
            for (int stem = 0; stem < kStemCount; ++stem) {
                pOutput[frame * kStemChannelCount + stem * 2] = left;
                pOutput[frame * kStemChannelCount + stem * 2 + 1] = right;
            }
        }
        return true;
    }

    int callCount() const {
        return m_callCount;
    }

  private:
    int m_callCount = 0;
};

/// Gives every stem a different gain, so a swapped stem or an untouched stem
/// cannot pass the identity check that CopyBackend would allow.
class GainBackend : public StemSeparatorBackend {
  public:
    audio::SampleRate sampleRate() const override {
        return audio::SampleRate(44100);
    }
    SINT windowFrames() const override {
        return kWindowFrames;
    }
    SINT overlapFrames() const override {
        return kOverlapFrames;
    }

    bool processWindow(const CSAMPLE* pInput, CSAMPLE* pOutput) override {
        ++m_callCount;
        for (SINT frame = 0; frame < kWindowFrames; ++frame) {
            for (int stem = 0; stem < kStemCount; ++stem) {
                const CSAMPLE gain = gainForStem(stem);
                pOutput[frame * kStemChannelCount + stem * 2] =
                        pInput[frame * 2] * gain;
                pOutput[frame * kStemChannelCount + stem * 2 + 1] =
                        pInput[frame * 2 + 1] * gain;
            }
        }
        return true;
    }

    static CSAMPLE gainForStem(int stem) {
        return static_cast<CSAMPLE>(0.25 * (stem + 1));
    }

    int callCount() const {
        return m_callCount;
    }

  private:
    int m_callCount = 0;
};

class FailingBackend : public StemSeparatorBackend {
  public:
    audio::SampleRate sampleRate() const override {
        return audio::SampleRate(44100);
    }
    SINT windowFrames() const override {
        return kWindowFrames;
    }
    SINT overlapFrames() const override {
        return kOverlapFrames;
    }

    bool processWindow(const CSAMPLE* /*pInput*/, CSAMPLE* /*pOutput*/) override {
        return false;
    }
};

/// Deterministic, non-repeating-ish signal with a different value per channel
/// so a channel swap inside the pipeline cannot cancel out.
std::vector<CSAMPLE> makeInput(SINT frameCount) {
    std::vector<CSAMPLE> input(
            static_cast<std::size_t>(frameCount) * 2);
    for (SINT frame = 0; frame < frameCount; ++frame) {
        const auto index = static_cast<std::size_t>(frame) * 2;
        input[index] = static_cast<CSAMPLE>(
                0.5 * std::sin(0.011 * static_cast<double>(frame)));
        input[index + 1] = static_cast<CSAMPLE>(
                0.4 * std::cos(0.017 * static_cast<double>(frame)));
    }
    return input;
}

std::vector<CSAMPLE> makeOutput(SINT frameCount) {
    return std::vector<CSAMPLE>(
            static_cast<std::size_t>(frameCount) * kStemChannelCount, 0.0f);
}

/// Compares the whole output against `input * gainForStem(stem)` and reports
/// the first mismatch, so a failure points at the frame and stem that broke.
void expectReconstructed(const std::vector<CSAMPLE>& input,
        const std::vector<CSAMPLE>& output,
        SINT frameCount) {
    for (SINT frame = 0; frame < frameCount; ++frame) {
        for (int stem = 0; stem < kStemCount; ++stem) {
            const CSAMPLE gain = GainBackend::gainForStem(stem);
            for (int channel = 0; channel < 2; ++channel) {
                const auto inputIndex = static_cast<std::size_t>(frame) * 2 +
                        static_cast<std::size_t>(channel);
                const auto outputIndex =
                        static_cast<std::size_t>(frame) * kStemChannelCount +
                        static_cast<std::size_t>(stem) * 2 +
                        static_cast<std::size_t>(channel);
                const CSAMPLE expected = input[inputIndex] * gain;
                if (std::fabs(expected - output[outputIndex]) > kTolerance) {
                    ADD_FAILURE() << "frame " << frame << " stem " << stem
                                  << " channel " << channel << ": expected "
                                  << expected << ", got " << output[outputIndex];
                    return;
                }
            }
        }
    }
}

class StemSeparationTest : public testing::Test {};

TEST_F(StemSeparationTest, ReconstructsTrackShorterThanOneWindow) {
    GainBackend backend;
    const SINT frameCount = kWindowFrames - 1;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    ASSERT_TRUE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));

    EXPECT_EQ(1, backend.callCount());
    expectReconstructed(input, output, frameCount);
}

TEST_F(StemSeparationTest, ReconstructsSingleFrame) {
    GainBackend backend;
    const SINT frameCount = 1;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    ASSERT_TRUE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));

    expectReconstructed(input, output, frameCount);
}

TEST_F(StemSeparationTest, ReconstructsTrackExactlyOneWindowLong) {
    GainBackend backend;
    const SINT frameCount = kWindowFrames;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    ASSERT_TRUE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));

    EXPECT_EQ(1, backend.callCount());
    expectReconstructed(input, output, frameCount);
}

TEST_F(StemSeparationTest, ReconstructsTrackWithClampedLastWindow) {
    GainBackend backend;
    // Not a multiple of the hop, so the last window has to move backwards.
    const SINT frameCount = 3 * kWindowFrames + 1;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    ASSERT_TRUE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));

    EXPECT_GT(backend.callCount(), 1);
    expectReconstructed(input, output, frameCount);
}

TEST_F(StemSeparationTest, ReconstructsLongTrack) {
    GainBackend backend;
    const SINT frameCount = 10 * kWindowFrames + 37;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    ASSERT_TRUE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));

    EXPECT_EQ(14, backend.callCount());
    expectReconstructed(input, output, frameCount);
}

TEST_F(StemSeparationTest, ZeroFramesSucceedsWithoutInference) {
    CopyBackend backend;
    std::vector<CSAMPLE> input(2, 0.0f);
    std::vector<CSAMPLE> output(kStemChannelCount, 0.0f);

    double lastProgress = -1.0;
    ASSERT_TRUE(StemSeparation::separate(&backend,
            input.data(),
            0,
            output.data(),
            [&lastProgress](double progress) {
                lastProgress = progress;
            }));

    EXPECT_EQ(0, backend.callCount());
    EXPECT_DOUBLE_EQ(1.0, lastProgress);
}

TEST_F(StemSeparationTest, ReportsMonotonicProgressEndingAtOne) {
    CopyBackend backend;
    const SINT frameCount = 8 * kWindowFrames;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    std::vector<double> progressValues;
    ASSERT_TRUE(StemSeparation::separate(&backend,
            input.data(),
            frameCount,
            output.data(),
            [&progressValues](double progress) {
                progressValues.push_back(progress);
            }));

    ASSERT_FALSE(progressValues.empty());
    ASSERT_EQ(static_cast<std::size_t>(backend.callCount()),
            progressValues.size());
    EXPECT_DOUBLE_EQ(1.0, progressValues.back());
    for (std::size_t i = 0; i < progressValues.size(); ++i) {
        EXPECT_GE(progressValues[i], 0.0);
        EXPECT_LE(progressValues[i], 1.0);
        if (i > 0) {
            EXPECT_LE(progressValues[i - 1], progressValues[i])
                    << "progress went backwards at step " << i;
        }
    }
}

TEST_F(StemSeparationTest, PropagatesBackendFailure) {
    FailingBackend backend;
    const SINT frameCount = 2 * kWindowFrames;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    EXPECT_FALSE(StemSeparation::separate(
            &backend, input.data(), frameCount, output.data()));
}

TEST_F(StemSeparationTest, CancellationStopsBetweenWindows) {
    CopyBackend backend;
    const SINT frameCount = 20 * kWindowFrames;
    const auto input = makeInput(frameCount);
    auto output = makeOutput(frameCount);

    int polls = 0;
    const bool completed = StemSeparation::separate(&backend,
            input.data(),
            frameCount,
            output.data(),
            {},
            [&polls]() {
                return ++polls >= 3;
            });

    EXPECT_FALSE(completed);
    // The callback is polled before each window, so the third poll aborts
    // after exactly two windows have run.
    EXPECT_EQ(3, polls);
    EXPECT_EQ(2, backend.callCount());
}

TEST_F(StemSeparationTest, RejectsNullArguments) {
    CopyBackend backend;
    std::vector<CSAMPLE> input(2, 0.0f);
    std::vector<CSAMPLE> output(kStemChannelCount, 0.0f);

    EXPECT_FALSE(StemSeparation::separate(
            nullptr, input.data(), 1, output.data()));
    EXPECT_FALSE(StemSeparation::separate(
            &backend, nullptr, 1, output.data()));
    EXPECT_FALSE(StemSeparation::separate(
            &backend, input.data(), 1, nullptr));
}

TEST_F(StemSeparationTest, RejectsNegativeFrameCount) {
    CopyBackend backend;
    std::vector<CSAMPLE> input(2, 0.0f);
    std::vector<CSAMPLE> output(kStemChannelCount, 0.0f);

    EXPECT_FALSE(StemSeparation::separate(
            &backend, input.data(), -1, output.data()));
    EXPECT_EQ(0, backend.callCount());
}

} // namespace
} // namespace mixxx::stems
