#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "stems/demucsbag.h"
#include "stems/demucslimiter.h"
#include "stems/demucsnormalization.h"
#include "stems/demucsstft.h"

namespace mixxx::stems {
namespace {

constexpr float kCanary = 12345.0f;

class DemucsNormalizationTest : public testing::Test {};
class DemucsBagAverageTest : public testing::Test {};
class DemucsLimiterTest : public testing::Test {};
class DemucsReconstructTest : public testing::Test {};

TEST_F(DemucsNormalizationTest, StatisticsUseUnbiasedStandardDeviation) {
    const float values[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const auto stats = demucsStatistics(values, 4);
    EXPECT_NEAR(2.5, stats.mean, 1e-12);
    EXPECT_NEAR(std::sqrt(5.0 / 3.0), stats.standardDeviation, 1e-12);
}

TEST_F(DemucsNormalizationTest, EmptyAndSingleValueStatistics) {
    const auto empty = demucsStatistics(nullptr, 0);
    EXPECT_EQ(0.0, empty.mean);
    EXPECT_EQ(0.0, empty.standardDeviation);
    const float value[] = {7.0f};
    const auto single = demucsStatistics(value, 1);
    EXPECT_EQ(7.0, single.mean);
    EXPECT_EQ(0.0, single.standardDeviation);
}

TEST_F(DemucsNormalizationTest, NormalizeAndDenormalizeRestoreValues) {
    std::vector<float> input(100);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(0.5 + 3.0 * std::sin(i * 0.37));
    }
    const auto stats = demucsStatistics(input.data(), input.size());
    std::vector<float> normalized(input.size());
    std::vector<float> restored(input.size());
    demucsNormalize(input.data(), normalized.data(), input.size(), stats);
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR((input[i] - stats.mean) /
                        (kDemucsNormalizationEpsilon + stats.standardDeviation),
                normalized[i],
                1e-6);
    }
    demucsDenormalize(normalized.data(), restored.data(), input.size(), stats);
    const double scale = stats.standardDeviation /
            (kDemucsNormalizationEpsilon + stats.standardDeviation);
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR((input[i] - stats.mean) * scale + stats.mean,
                restored[i],
                1e-6);
    }
}

TEST_F(DemucsNormalizationTest, ConstantValuesNormalizeToZero) {
    const std::vector<float> input(8, 4.25f);
    const auto stats = demucsStatistics(input.data(), input.size());
    std::vector<float> normalized(input.size());
    std::vector<float> restored(input.size());
    demucsNormalize(input.data(), normalized.data(), input.size(), stats);
    demucsDenormalize(normalized.data(), restored.data(), input.size(), stats);
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_FLOAT_EQ(0.0f, normalized[i]);
        EXPECT_FLOAT_EQ(input[i], restored[i]);
    }
}

TEST_F(DemucsNormalizationTest, InPlaceRoundTrip) {
    std::vector<float> values(16);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<float>(i) - 3.5f;
    }
    const auto original = values;
    const auto stats = demucsStatistics(values.data(), values.size());
    demucsNormalize(values.data(), values.data(), values.size(), stats);
    demucsDenormalize(values.data(), values.data(), values.size(), stats);
    const double scale = stats.standardDeviation /
            (kDemucsNormalizationEpsilon + stats.standardDeviation);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_NEAR((original[i] - stats.mean) * scale + stats.mean,
                values[i],
                1e-6);
    }
}

TEST_F(DemucsNormalizationTest, EpsilonIsExpectedValue) {
    EXPECT_DOUBLE_EQ(1e-5, kDemucsNormalizationEpsilon);
}

TEST_F(DemucsNormalizationTest, DenormalizeUsesStdWithoutEpsilon) {
    const float input[] = {1.0f};
    float output = 0.0f;
    demucsDenormalize(input, &output, 1, DemucsStatistics{2.0, 0.0});
    EXPECT_FLOAT_EQ(2.0f, output);
}

TEST_F(DemucsNormalizationTest, ZeroCountDoesNotWrite) {
    demucsNormalize(nullptr, nullptr, 0, {});
    demucsDenormalize(nullptr, nullptr, 0, {});
    float canary = 1.0f;
    demucsApplyGain(&canary, 0, 0.5f);
    EXPECT_FLOAT_EQ(1.0f, canary);
}

TEST_F(DemucsBagAverageTest, EqualWeightsAverageCacAndWav) {
    DemucsBagAverage bag(2, 3);
    const float cacA[] = {1, 2}, wavA[] = {10, 20, 30};
    const float cacB[] = {3, 4}, wavB[] = {30, 40, 50};
    bag.add(cacA, wavA);
    bag.add(cacB, wavB);
    float cac[2], wav[3];
    bag.finalize(cac, wav);
    EXPECT_FLOAT_EQ(2, cac[0]);
    EXPECT_FLOAT_EQ(3, cac[1]);
    EXPECT_FLOAT_EQ(20, wav[0]);
    EXPECT_FLOAT_EQ(30, wav[1]);
    EXPECT_FLOAT_EQ(40, wav[2]);
    EXPECT_DOUBLE_EQ(2.0, bag.totalWeight());
    EXPECT_FALSE(bag.isEmpty());
}

TEST_F(DemucsBagAverageTest, WeightedAverage) {
    DemucsBagAverage bag(2, 3);
    const float cacA[] = {1, 2}, wavA[] = {10, 20, 30};
    const float cacB[] = {3, 4}, wavB[] = {30, 40, 50};
    bag.add(cacA, wavA, 1);
    bag.add(cacB, wavB, 3);
    float cac[2], wav[3];
    bag.finalize(cac, wav);
    EXPECT_FLOAT_EQ(2.5f, cac[0]);
    EXPECT_FLOAT_EQ(3.5f, cac[1]);
    EXPECT_FLOAT_EQ(25, wav[0]);
    EXPECT_FLOAT_EQ(35, wav[1]);
    EXPECT_FLOAT_EQ(45, wav[2]);
    EXPECT_DOUBLE_EQ(4.0, bag.totalWeight());
}

TEST_F(DemucsBagAverageTest, EmptyBagFinalizesToZeros) {
    DemucsBagAverage bag(2, 3);
    float cac[2] = {1, 1}, wav[3] = {1, 1, 1};
    bag.finalize(cac, wav);
    for (float value : cac) {
        EXPECT_FLOAT_EQ(0, value);
    }
    for (float value : wav) {
        EXPECT_FLOAT_EQ(0, value);
    }
    EXPECT_TRUE(bag.isEmpty());
    EXPECT_DOUBLE_EQ(0.0, bag.totalWeight());
}

TEST_F(DemucsBagAverageTest, SpectrogramOnlyAverage) {
    DemucsBagAverage bag(2, 0);
    const float cacA[] = {1, 2}, cacB[] = {5, 6};
    bag.add(cacA, nullptr, 1);
    bag.add(cacB, nullptr, 3);
    float cac[2];
    bag.finalize(cac, nullptr);
    EXPECT_FLOAT_EQ(4, cac[0]);
    EXPECT_FLOAT_EQ(5, cac[1]);
    EXPECT_EQ(0u, bag.wavFloats());
}

TEST_F(DemucsBagAverageTest, ReportsConstructorSizes) {
    DemucsBagAverage bag(2, 3);
    EXPECT_EQ(2u, bag.cacFloats());
    EXPECT_EQ(3u, bag.wavFloats());
}

TEST_F(DemucsBagAverageTest, ResetAllowsReuse) {
    DemucsBagAverage bag(2, 1);
    const float cac[] = {2, 4};
    const float wav[] = {6};
    bag.add(cac, wav);
    bag.reset();
    EXPECT_TRUE(bag.isEmpty());
    EXPECT_DOUBLE_EQ(0.0, bag.totalWeight());

    float cacOut[2] = {1, 1};
    float wavOut[1] = {1};
    bag.finalize(cacOut, wavOut);
    for (float value : cacOut) {
        EXPECT_FLOAT_EQ(0, value);
    }
    EXPECT_FLOAT_EQ(0, wavOut[0]);

    const float cac2[] = {8, 10};
    bag.add(cac2, wav, 2);
    bag.finalize(cacOut, wavOut);
    EXPECT_FLOAT_EQ(8, cacOut[0]);
    EXPECT_FLOAT_EQ(10, cacOut[1]);
    EXPECT_FLOAT_EQ(6, wavOut[0]);
}

TEST_F(DemucsBagAverageTest, AveragingBeforeReconstructionMatchesPerModelAverage) {
    constexpr SINT frames = 1025;
    const std::size_t cacCount = demucsCacFloatCount(frames);
    const std::size_t wavCount = static_cast<std::size_t>(frames) * 2;
    std::vector<float> cacA(cacCount);
    std::vector<float> cacB(cacCount);
    std::vector<CSAMPLE> wavA(wavCount);
    std::vector<CSAMPLE> wavB(wavCount);
    for (std::size_t i = 0; i < cacCount; ++i) {
        cacA[i] = 1e-3f * static_cast<float>((i * 37) % 101) - 0.05f;
        cacB[i] = 1e-3f * static_cast<float>((i * 53) % 97) - 0.04f;
    }
    for (std::size_t i = 0; i < wavCount; ++i) {
        wavA[i] = 0.001f * static_cast<float>(i % 17) - 0.008f;
        wavB[i] = -0.002f * static_cast<float>(i % 13) + 0.006f;
    }

    std::vector<CSAMPLE> outA(wavCount);
    std::vector<CSAMPLE> outB(wavCount);
    demucsReconstruct(cacA.data(), wavA.data(), frames, outA.data());
    demucsReconstruct(cacB.data(), wavB.data(), frames, outB.data());
    std::vector<CSAMPLE> expected(wavCount);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = 0.5f * (outA[i] + outB[i]);
    }

    DemucsBagAverage bag(cacCount, wavCount);
    bag.add(cacA.data(), wavA.data(), 1);
    bag.add(cacB.data(), wavB.data(), 1);
    std::vector<float> cacAverage(cacCount);
    std::vector<CSAMPLE> wavAverage(wavCount);
    bag.finalize(cacAverage.data(), wavAverage.data());
    std::vector<CSAMPLE> combined(wavCount);
    demucsReconstruct(
            cacAverage.data(), wavAverage.data(), frames, combined.data());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(expected[i], combined[i], 1e-5);
    }
}

TEST_F(DemucsLimiterTest, SubFullScaleStemsNeedNoGain) {
    std::vector<CSAMPLE> stems(8 * 8);
    const CSAMPLE frameSamples[] = {
            0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f};
    for (std::size_t frame = 0; frame < 8; ++frame) {
        for (std::size_t channel = 0; channel < 8; ++channel) {
            stems[frame * 8 + channel] = frameSamples[channel];
        }
    }
    EXPECT_FLOAT_EQ(1.0f, demucsLimiterGain(stems.data(), 8));
}

TEST_F(DemucsLimiterTest, ClipsPeakAndAppliesGain) {
    std::vector<CSAMPLE> stems(8 * 2, 0.0f);
    stems[3] = 2.0f;
    const float gain = demucsLimiterGain(stems.data(), 2);
    EXPECT_FLOAT_EQ(0.5f, gain);
    demucsApplyGain(stems.data(), stems.size(), gain);
    for (CSAMPLE sample : stems)
        EXPECT_LE(std::fabs(sample), 1.0f);
    EXPECT_FLOAT_EQ(1.0f, stems[3]);
}

TEST_F(DemucsLimiterTest, ConsidersSumAcrossStems) {
    CSAMPLE stems[8] = {0.4f, 0.0f, 0.4f, 0.0f, 0.4f, 0.0f, 0.4f, 0.0f};
    EXPECT_NEAR(1.0f / 1.6f, demucsLimiterGain(stems, 1), 1e-6);
}

TEST_F(DemucsLimiterTest, ClipsSingleNegativePeak) {
    CSAMPLE stems[8] = {};
    stems[6] = -2.0f;
    EXPECT_FLOAT_EQ(0.5f, demucsLimiterGain(stems, 1));
}

TEST_F(DemucsLimiterTest, ConsidersNegativeSumOnTheRight) {
    CSAMPLE stems[8] = {0.0f, -0.4f, 0.0f, -0.4f, 0.0f, -0.4f, 0.0f, -0.4f};
    EXPECT_NEAR(1.0f / 1.6f, demucsLimiterGain(stems, 1), 1e-6);
}

TEST_F(DemucsLimiterTest, SilenceAndZeroFramesNeedNoGain) {
    CSAMPLE stems[8] = {};
    EXPECT_FLOAT_EQ(1.0f, demucsLimiterGain(stems, 1));
    EXPECT_FLOAT_EQ(1.0f, demucsLimiterGain(stems, 0));
}

TEST_F(DemucsLimiterTest, ApplyGainMultipliesEverySample) {
    CSAMPLE samples[] = {-2.0f, -0.5f, 0.0f, 1.5f};
    demucsApplyGain(samples, 4, 0.25f);
    EXPECT_FLOAT_EQ(-0.5f, samples[0]);
    EXPECT_FLOAT_EQ(-0.125f, samples[1]);
    EXPECT_FLOAT_EQ(0.0f, samples[2]);
    EXPECT_FLOAT_EQ(0.375f, samples[3]);
}

TEST_F(DemucsReconstructTest, ZeroSpectrumAddsResidualExactly) {
    constexpr SINT frames = 1025;
    std::vector<float> cac(demucsCacFloatCount(frames), 0.0f);
    std::vector<CSAMPLE> residual(static_cast<std::size_t>(frames) * 2);
    for (std::size_t i = 0; i < residual.size(); ++i) {
        residual[i] = 0.001f * static_cast<float>(i % 17) - 0.008f;
    }
    std::vector<CSAMPLE> output(residual.size());
    demucsReconstruct(cac.data(), residual.data(), frames, output.data());
    for (std::size_t i = 0; i < output.size(); ++i) {
        EXPECT_FLOAT_EQ(residual[i], output[i]);
    }
}

TEST_F(DemucsReconstructTest, NullResidualMatchesSynthesis) {
    constexpr SINT frames = 1025;
    std::vector<float> cac(demucsCacFloatCount(frames));
    for (std::size_t i = 0; i < cac.size(); ++i) {
        cac[i] = 1e-3f * static_cast<float>((i * 37) % 101) - 0.05f;
    }
    std::vector<CSAMPLE> expected(static_cast<std::size_t>(frames) * 2);
    std::vector<CSAMPLE> actual(expected.size());
    demucsSynthesize(cac.data(), frames, expected.data());
    demucsReconstruct(cac.data(), nullptr, frames, actual.data());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_FLOAT_EQ(expected[i], actual[i]);
    }
}

TEST_F(DemucsReconstructTest, ResidualAddsToSynthesis) {
    constexpr SINT frames = 1025;
    std::vector<float> cac(demucsCacFloatCount(frames));
    std::vector<CSAMPLE> residual(static_cast<std::size_t>(frames) * 2);
    for (std::size_t i = 0; i < cac.size(); ++i) {
        cac[i] = 1e-3f * static_cast<float>((i * 37) % 101) - 0.05f;
    }
    for (std::size_t i = 0; i < residual.size(); ++i) {
        residual[i] = 0.001f * static_cast<float>(i % 17) - 0.008f;
    }
    std::vector<CSAMPLE> expected(residual.size()), actual(residual.size());
    demucsSynthesize(cac.data(), frames, expected.data());
    demucsReconstruct(cac.data(), residual.data(), frames, actual.data());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_NEAR(expected[i] + residual[i], actual[i], 1e-6);
    }
}

TEST_F(DemucsReconstructTest, ResidualMayAliasOutput) {
    constexpr SINT frames = 1025;
    std::vector<float> cac(demucsCacFloatCount(frames));
    for (std::size_t i = 0; i < cac.size(); ++i) {
        cac[i] = 1e-3f * static_cast<float>((i * 37) % 101) - 0.05f;
    }
    std::vector<CSAMPLE> residual(static_cast<std::size_t>(frames) * 2);
    for (std::size_t i = 0; i < residual.size(); ++i) {
        residual[i] = 0.001f * static_cast<float>(i % 17) - 0.008f;
    }
    std::vector<CSAMPLE> expected(residual.size());
    demucsReconstruct(cac.data(), residual.data(), frames, expected.data());

    std::vector<CSAMPLE> inPlace = residual;
    demucsReconstruct(cac.data(), inPlace.data(), frames, inPlace.data());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_FLOAT_EQ(expected[i], inPlace[i]);
    }
}

TEST_F(DemucsReconstructTest, ZeroFramesDoNotWrite) {
    CSAMPLE canary[] = {kCanary, kCanary, kCanary, kCanary};
    CSAMPLE residual[] = {1.0f, 2.0f, 3.0f, 4.0f};
    demucsReconstruct(nullptr, nullptr, 0, canary);
    demucsReconstruct(nullptr, residual, 0, canary);
    demucsReconstruct(nullptr, residual, -1, canary);
    for (CSAMPLE sample : canary) {
        EXPECT_FLOAT_EQ(kCanary, sample);
    }
}

} // namespace
} // namespace mixxx::stems
