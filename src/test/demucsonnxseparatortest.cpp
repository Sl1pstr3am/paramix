#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "stems/demucsonnxseparator.h"
#include "stems/stemseparatorfactory.h"
#include "util/types.h"

namespace mixxx::stems {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr SINT kFrameCount = 2048;
constexpr int kOutputChannels = 8;

DemucsOnnxConfig modelConfig() {
    DemucsOnnxConfig config;
    const char* modelPath = std::getenv("PARAMIX_TEST_DEMUCS_ONNX");
    if (modelPath != nullptr) {
        config.modelPath = modelPath;
    }
    config.device = "cpu";
    config.batch = 1;
    config.overlap = 0.25f;
    return config;
}

std::vector<float> makeInput() {
    std::vector<float> input(static_cast<std::size_t>(kFrameCount) * 2);
    for (SINT frame = 0; frame < kFrameCount; ++frame) {
        const double time = static_cast<double>(frame) / 44100.0;
        input[static_cast<std::size_t>(frame) * 2] =
                static_cast<float>(0.4 * std::sin(2.0 * kPi * 440.0 * time));
        input[static_cast<std::size_t>(frame) * 2 + 1] =
                static_cast<float>(0.4 * std::sin(2.0 * kPi * 660.0 * time));
    }
    return input;
}

#ifdef __STEM_SEPARATION__
std::unique_ptr<DemucsOnnxSeparator> createTestSeparator() {
    std::string error;
    auto separator = createDemucsOnnxSeparator(modelConfig(), &error);
    EXPECT_NE(nullptr, separator) << error;
    return separator;
}
#endif

class DemucsOnnxSeparatorTest : public testing::Test {};

TEST_F(DemucsOnnxSeparatorTest, ReportsUnavailableBuild) {
    if (!isDemucsOnnxAvailable()) {
        std::string error;
        EXPECT_EQ(nullptr, createDemucsOnnxSeparator({}, &error));
        EXPECT_FALSE(error.empty());
    }
}

#ifdef __STEM_SEPARATION__
TEST_F(DemucsOnnxSeparatorTest, RejectsEmptyModelPath) {
    DemucsOnnxConfig config;
    std::string error;
    EXPECT_EQ(nullptr, createDemucsOnnxSeparator(config, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(DemucsOnnxSeparatorTest, RejectsUnknownDevice) {
    DemucsOnnxConfig config;
    config.modelPath = "test-model.onnx";
    config.device = "bogus";
    std::string error;
    EXPECT_EQ(nullptr, createDemucsOnnxSeparator(config, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(DemucsOnnxSeparatorTest, SeparatesTinyModelAndReportsProgress) {
    const char* modelPath = std::getenv("PARAMIX_TEST_DEMUCS_ONNX");
    if (modelPath == nullptr || *modelPath == '\0') {
        GTEST_SKIP() << "set PARAMIX_TEST_DEMUCS_ONNX";
    }
    auto separator = createTestSeparator();
    ASSERT_NE(nullptr, separator);
    EXPECT_EQ(44100, separator->sampleRate());
    EXPECT_EQ(2, separator->channels());
    EXPECT_EQ(4u, separator->sources().size());
    EXPECT_EQ("cpu", separator->device());

    const auto input = makeInput();
    std::vector<CSAMPLE> output;
    std::vector<std::pair<SINT, SINT>> blocks;
    std::vector<double> progress;
    const bool completed = separator->separate(input.data(), kFrameCount, [&output, &blocks](SINT offset, const CSAMPLE* buffer, SINT frames) {
                blocks.emplace_back(offset, frames);
                output.insert(output.end(), buffer,
                        buffer + static_cast<std::size_t>(frames) *
                                kOutputChannels);
                return true; }, [&progress](double value) { progress.push_back(value); });
    ASSERT_TRUE(completed) << separator->lastError();
    ASSERT_FALSE(blocks.empty());
    SINT totalFrames = 0;
    for (const auto& block : blocks) {
        EXPECT_EQ(totalFrames, block.first);
        totalFrames += block.second;
    }
    EXPECT_EQ(kFrameCount, totalFrames);
    ASSERT_EQ(static_cast<std::size_t>(kFrameCount) * kOutputChannels,
            output.size());
    double squareSum = 0.0;
    for (const CSAMPLE sample : output) {
        ASSERT_TRUE(std::isfinite(sample));
        squareSum += static_cast<double>(sample) * sample;
    }
    EXPECT_GT(std::sqrt(squareSum / output.size()), 0.0);

    ASSERT_FALSE(progress.empty());
    for (std::size_t i = 0; i < progress.size(); ++i) {
        EXPECT_GE(progress[i], 0.0);
        EXPECT_LE(progress[i], 1.0);
        if (i > 0) {
            EXPECT_LE(progress[i - 1], progress[i]);
        }
    }
    EXPECT_GE(progress.back(), 0.9);
}

TEST_F(DemucsOnnxSeparatorTest, CancelsAfterFirstOutputBlock) {
    const char* modelPath = std::getenv("PARAMIX_TEST_DEMUCS_ONNX");
    if (modelPath == nullptr || *modelPath == '\0') {
        GTEST_SKIP() << "set PARAMIX_TEST_DEMUCS_ONNX";
    }
    auto separator = createTestSeparator();
    ASSERT_NE(nullptr, separator);
    const auto input = makeInput();
    int blocksWritten = 0;
    const bool completed = separator->separate(input.data(), kFrameCount, [&blocksWritten](SINT, const CSAMPLE*, SINT) {
                ++blocksWritten;
                return true; }, {}, [&blocksWritten]() { return blocksWritten > 0; });
    EXPECT_FALSE(completed);
    EXPECT_GT(blocksWritten, 0);
    EXPECT_FALSE(separator->lastError().empty());
}

TEST_F(DemucsOnnxSeparatorTest, ZeroFramesWritesNothing) {
    const char* modelPath = std::getenv("PARAMIX_TEST_DEMUCS_ONNX");
    if (modelPath == nullptr || *modelPath == '\0') {
        GTEST_SKIP() << "set PARAMIX_TEST_DEMUCS_ONNX";
    }
    auto separator = createTestSeparator();
    ASSERT_NE(nullptr, separator);
    int writes = 0;
    EXPECT_TRUE(separator->separate(nullptr, 0, [&writes](SINT, const CSAMPLE*, SINT) {
        ++writes;
        return true;
    }));
    EXPECT_EQ(0, writes);
}

TEST_F(DemucsOnnxSeparatorTest, RejectsNullInputWithFrames) {
    const char* modelPath = std::getenv("PARAMIX_TEST_DEMUCS_ONNX");
    if (modelPath == nullptr || *modelPath == '\0') {
        GTEST_SKIP() << "set PARAMIX_TEST_DEMUCS_ONNX";
    }
    auto separator = createTestSeparator();
    ASSERT_NE(nullptr, separator);
    EXPECT_FALSE(separator->separate(nullptr, 1, [](SINT, const CSAMPLE*, SINT) { return true; }));
    EXPECT_FALSE(separator->lastError().empty());
}
#endif

} // namespace
} // namespace mixxx::stems
