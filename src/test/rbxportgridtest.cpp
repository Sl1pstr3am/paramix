#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "analyzer/analyzerbeats.h"
#include "analyzer/plugins/analyzerrbxportbeats.h"
#include "analyzer/plugins/rbxport/grid.h"
#include "analyzer/plugins/rbxport/tempo.h"
#include "track/beatfactory.h"

namespace mixxx::rbxport {
namespace {

// The envelope rate of rbxport's own tests: 44.1 kHz / 256.
constexpr double kRate = 44100.0 / 256.0;
constexpr double kPi = 3.14159265358979323846;

TempoOptions envelopeOptions() {
    TempoOptions options;
    options.placement = Placement::Envelope;
    return options;
}

/// An onset envelope with a peak at each of `hits`, in seconds.
OnsetEnvelope envelope(const std::vector<double>& hits, double secs) {
    OnsetEnvelope result;
    result.values.assign(static_cast<size_t>(secs * kRate), 0.0f);
    for (const double t : hits) {
        const size_t i = static_cast<size_t>(std::round(t * kRate));
        if (i < result.values.size()) {
            result.values[i] = 1.0f;
        }
        if (i + 1 < result.values.size()) {
            result.values[i + 1] = 0.4f;
        }
    }
    result.rate = kRate;
    result.originSecs = 0.0;
    return result;
}

/// An onset envelope with a peak of its own height at each hit, in seconds.
OnsetEnvelope envelopeOf(const std::vector<std::pair<double, float>>& hits, double secs) {
    OnsetEnvelope result;
    result.values.assign(static_cast<size_t>(secs * kRate), 0.0f);
    for (const auto& [t, height] : hits) {
        const size_t i = static_cast<size_t>(std::round(t * kRate));
        if (i < result.values.size()) {
            result.values[i] = std::max(result.values[i], height);
        }
        if (i + 1 < result.values.size()) {
            result.values[i + 1] = std::max(result.values[i + 1], height * 0.4f);
        }
    }
    result.rate = kRate;
    result.originSecs = 0.0;
    return result;
}

/// Hits every `60 / bpm` seconds from `first` through `from..to`, at `height`.
std::vector<std::pair<double, float>> train(
        double bpm, double first, double from, double to, float height) {
    const double period = 60.0 / bpm;
    const double k = std::max(std::ceil((from - first) / period), 0.0);
    std::vector<std::pair<double, float>> hits;
    for (size_t i = 0; i < 100000; ++i) {
        const double t = first + (k + static_cast<double>(i)) * period;
        if (t >= to) {
            break;
        }
        hits.emplace_back(t, height);
    }
    return hits;
}

/// A track at 130 whose kick stops at 118 s, whose bass slows from an
/// eighth of 231 ms to one of 1.4 s over the next 24 s, and which comes
/// back at 130 at 147.03 s, `shift` seconds off the old line.
std::pair<OnsetEnvelope, OnsetEnvelope> slowdown(double shift) {
    const double period = 60.0 / 130.0;
    const double first = 0.05;
    std::vector<double> kicks;
    for (size_t k = 0; k < 1000; ++k) {
        const double t = first + static_cast<double>(k) * period;
        if (t >= 118.0) {
            break;
        }
        kicks.push_back(t);
    }
    std::vector<double> all = kicks;
    double t = 118.205;
    double eighth = period / 2.0;
    while (t < 142.5) {
        all.push_back(t);
        t += eighth;
        eighth *= 1.038;
    }
    const double k = std::ceil((147.03 - first - shift) / period);
    const double resume = first + shift + k * period;
    std::vector<double> back;
    for (size_t i = 0; i < 1000; ++i) {
        const double hit = resume + static_cast<double>(i) * period;
        if (hit >= 300.0) {
            break;
        }
        back.push_back(hit);
    }
    kicks.insert(kicks.end(), back.begin(), back.end());
    all.insert(all.end(), back.begin(), back.end());
    return {envelope(all, 300.0), envelope(kicks, 300.0)};
}

TEST(RbxportTempoTest, ASlowdownThatComesBackOnAnotherPhaseIsWalkedAndCut) {
    const auto [onsets, kicks] = slowdown(0.21);
    const TempoResult result = detectTempo(
            onsets, &kicks, nullptr, envelopeOptions());
    ASSERT_FALSE(result.segments.empty());
    const Segment& first = result.segments.front();
    const Segment& last = result.segments.back();
    EXPECT_NEAR(first.bpm(), 130.0, 0.1);
    EXPECT_NEAR(first.startSecs(), 0.05, 0.02);
    EXPECT_NEAR(last.bpm(), 130.0, 0.1);
    const double expectedCut = 0.05 + 0.21 +
            std::ceil((147.03 - 0.26) / (60.0 / 130.0)) * (60.0 / 130.0);
    EXPECT_NEAR(last.fromSecs, expectedCut, 0.03);
    // The ramp: one-beat segments, slowing.
    std::vector<const Segment*> ramp;
    for (const Segment& segment : result.segments) {
        if (segment.beats() == 1) {
            ramp.push_back(&segment);
        }
    }
    EXPECT_GE(ramp.size(), 8u);
    ASSERT_FALSE(ramp.empty());
    EXPECT_GT(ramp.front()->bpm(), 100.0);
    EXPECT_LT(ramp.back()->bpm(), 40.0);
    // The old line stops before the ramp, and nothing overlaps.
    for (size_t i = 0; i + 1 < result.segments.size(); ++i) {
        EXPECT_GE(result.segments[i + 1].fromSecs,
                result.segments[i].toSecs - 1e-9);
    }
}

/// A track at 150 whose kick plays throughout at `kickHeight` in the kick
/// band, except where `kickFollows` moves the kick onto the pattern, with a
/// pattern at 168.75 (nine eighths of it) much louder than the kick in the
/// full band from 120 s to 180 s.
std::pair<OnsetEnvelope, OnsetEnvelope> nineEighths(float kickHeight, bool kickFollows) {
    constexpr double start = 120.0;
    constexpr double end = 180.0;
    auto all = train(150.0, 0.05, 0.0, start, 1.0f);
    const auto quiet = train(150.0, 0.05, start, end, 0.25f);
    all.insert(all.end(), quiet.begin(), quiet.end());
    const auto pattern = train(168.75, start, start, end, 1.0f);
    all.insert(all.end(), pattern.begin(), pattern.end());
    const auto after = train(150.0, 0.05, end, 300.0, 1.0f);
    all.insert(all.end(), after.begin(), after.end());
    auto kicks = train(150.0, 0.05, 0.0, start, 1.0f);
    if (kickFollows) {
        const auto follows = train(168.75, start, start, end, 1.0f);
        kicks.insert(kicks.end(), follows.begin(), follows.end());
    } else {
        const auto stays = train(150.0, 0.05, start, end, kickHeight);
        kicks.insert(kicks.end(), stays.begin(), stays.end());
    }
    const auto kicksAfter = train(150.0, 0.05, end, 300.0, 1.0f);
    kicks.insert(kicks.end(), kicksAfter.begin(), kicksAfter.end());
    return {envelopeOf(all, 300.0), envelopeOf(kicks, 300.0)};
}

TEST(RbxportTempoTest, APatternTheKickDoesNotFollowIsNotATempoChange) {
    for (const float kick : {1.0f, 0.2f, 0.0f}) {
        const auto [onsets, kicks] = nineEighths(kick, false);
        const TempoResult result = detectTempo(
                onsets, &kicks, nullptr, envelopeOptions());
        EXPECT_EQ(result.segments.size(), 1u) << "kick at " << kick;
        EXPECT_NEAR(result.bpm, 150.0, 0.05) << "kick at " << kick;
    }
}

TEST(RbxportTempoTest, AKicklessIntroAtAnotherPeriodTakesTheTracksTempo) {
    auto all = train(96.75, 0.1, 0.0, 40.0, 1.0f);
    const auto rest = train(172.0, 0.1, 40.0, 240.0, 1.0f);
    all.insert(all.end(), rest.begin(), rest.end());
    const auto kicks = train(172.0, 0.1, 40.0, 240.0, 1.0f);
    const OnsetEnvelope onsets = envelopeOf(all, 240.0);
    const OnsetEnvelope kickEnvelope = envelopeOf(kicks, 240.0);
    const TempoResult result = detectTempo(
            onsets, &kickEnvelope, nullptr, envelopeOptions());
    ASSERT_EQ(result.segments.size(), 1u);
    EXPECT_NEAR(result.bpm, 172.0, 0.05);
    EXPECT_LT(result.segments.front().fromSecs, 0.01);
}

TEST(RbxportTempoTest, ATempoTheKickFollowsIsStillATempoChange) {
    const auto [onsets, kicks] = nineEighths(0.0f, true);
    const TempoResult result = detectTempo(
            onsets, &kicks, nullptr, envelopeOptions());
    ASSERT_GE(result.segments.size(), 3u);
    bool found = false;
    for (const Segment& segment : result.segments) {
        if (std::abs(segment.bpm() - 168.75) < 0.1) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_NEAR(result.segments.front().bpm(), 150.0, 0.05);
    EXPECT_NEAR(result.segments.back().bpm(), 150.0, 0.05);
}

TEST(RbxportTempoTest, ASlowdownThatComesBackOnItsOwnGridHoldsTheLine) {
    const auto [onsets, kicks] = slowdown(0.0);
    const TempoResult result = detectTempo(
            onsets, &kicks, nullptr, envelopeOptions());
    ASSERT_EQ(result.segments.size(), 1u);
    EXPECT_NEAR(result.bpm, 130.0, 0.1);
}

/// A synthetic 4/4 kick with a click, at `bpm`, for `seconds`.
std::vector<float> clickTrack(double bpm, double seconds, uint32_t sampleRate) {
    std::vector<float> samples(
            static_cast<size_t>(seconds * sampleRate), 0.0f);
    const double period = 60.0 / bpm;
    for (double t = 0.1; t < seconds - 0.2; t += period) {
        const size_t start = static_cast<size_t>(t * sampleRate);
        const size_t body = static_cast<size_t>(0.30 * sampleRate);
        for (size_t i = 0; i < body && start + i < samples.size(); ++i) {
            const double time = static_cast<double>(i) / sampleRate;
            const double envelope = std::exp(-time / 0.06);
            samples[start + i] += static_cast<float>(
                    0.6 * envelope * std::sin(2.0 * kPi * 55.0 * time));
        }
        const size_t click = static_cast<size_t>(0.02 * sampleRate);
        for (size_t i = 0; i < click && start + i < samples.size(); ++i) {
            const double time = static_cast<double>(i) / sampleRate;
            const double envelope = std::exp(-time / 0.002);
            samples[start + i] += static_cast<float>(
                    0.5 * envelope * std::sin(2.0 * kPi * 3000.0 * time));
        }
    }
    return samples;
}

TEST(RbxportTempoTest, AnEarlyGapThatReturnsOnAnotherPhaseIsSplit) {
    const double period = 60.0 / 130.0;
    const double first = 0.05;
    std::vector<double> hits;
    for (size_t k = 0; k < 1000; ++k) {
        const double t = first + static_cast<double>(k) * period;
        if (t >= 20.0) {
            break;
        }
        hits.push_back(t);
    }
    const double k = std::ceil((24.1 - first) / period);
    const double resume = first + k * period + 0.2;
    for (size_t i = 0; i < 1000; ++i) {
        const double t = resume + static_cast<double>(i) * period;
        if (t >= 60.0) {
            break;
        }
        hits.push_back(t);
    }
    const OnsetEnvelope onsets = envelope(hits, 60.0);
    const TempoResult result = detectTempo(
            onsets, &onsets, nullptr, envelopeOptions());
    ASSERT_GE(result.segments.size(), 2u);
    EXPECT_NEAR(result.segments.front().bpm(), 130.0, 0.1);
    EXPECT_NEAR(result.segments.back().bpm(), 130.0, 0.1);
    for (size_t i = 0; i + 1 < result.segments.size(); ++i) {
        EXPECT_GE(result.segments[i + 1].fromSecs,
                result.segments[i].toSecs - 1e-9);
    }
}

TEST(RbxportGridTest, DetectsBpmAndDownbeatOnAClickTrack) {
    constexpr uint32_t kSampleRate = 44100;
    const std::vector<float> samples = clickTrack(128.0, 20.0, kSampleRate);
    const TempoResult result = analyseGrid(samples, kSampleRate);
    ASSERT_FALSE(result.beats.empty());
    EXPECT_NEAR(result.bpm, 128.0, 0.2);
    ASSERT_FALSE(result.segments.empty());
    // Every beat carries a downbeat number in 1..4, and the list starts at
    // the first beat of the file (the downbeat phase is carried through).
    for (const Beat& beat : result.beats) {
        EXPECT_GE(beat.beatNumber, 1);
        EXPECT_LE(beat.beatNumber, 4);
    }
    // Beat spacing matches the tempo.
    const double expectedPeriodMs = 60000.0 / 128.0;
    for (size_t i = 1; i < result.beats.size(); ++i) {
        EXPECT_NEAR(static_cast<double>(result.beats[i].timeMs) -
                        static_cast<double>(result.beats[i - 1].timeMs),
                expectedPeriodMs,
                3.0);
    }
}

TEST(RbxportPluginTest, IsOfferedInBeatDetectionPreferences) {
    const QList<mixxx::AnalyzerPluginInfo> plugins =
            AnalyzerBeats::availablePlugins();
    ASSERT_GE(plugins.size(), 3);
    bool found = false;
    for (const auto& info : plugins) {
        if (info.id() == AnalyzerRbxportBeats::pluginInfo().id()) {
            found = true;
            EXPECT_EQ(info.name(), AnalyzerRbxportBeats::pluginInfo().name());
            EXPECT_TRUE(info.isConstantTempoSupported());
        }
    }
    EXPECT_TRUE(found);
}

TEST(RbxportPluginTest, StreamsStereoAudioAndReturnsBeats) {
    constexpr uint32_t kSampleRate = 44100;
    const std::vector<float> mono = clickTrack(128.0, 20.0, kSampleRate);
    std::vector<float> stereo(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
    AnalyzerRbxportBeats plugin;
    ASSERT_TRUE(plugin.initialize(mixxx::audio::SampleRate(kSampleRate)));
    constexpr size_t kChunkFrames = 4096;
    for (size_t start = 0; start < mono.size(); start += kChunkFrames) {
        const size_t frames = std::min(kChunkFrames, mono.size() - start);
        ASSERT_TRUE(plugin.processSamples(
                stereo.data() + start * 2, static_cast<SINT>(frames * 2)));
    }
    ASSERT_TRUE(plugin.finalize());
    const QVector<mixxx::audio::FramePos> beats = plugin.getBeats();
    ASSERT_FALSE(beats.isEmpty());
    EXPECT_NEAR(plugin.getBpm().value(), 128.0, 0.2);
    // The first returned beat is a downbeat at the first kick (0.1 s).
    EXPECT_NEAR(beats.first().value(), 0.1 * kSampleRate, 0.02 * kSampleRate);

    // The existing beat factory consumes the result for both grid types.
    const auto end = mixxx::audio::FramePos(20.0 * kSampleRate);
    const auto pGrid = BeatFactory::makePreferredBeats(
            beats, {}, true, mixxx::audio::SampleRate(kSampleRate));
    ASSERT_TRUE(pGrid);
    EXPECT_NEAR(pGrid->getBpmInRange(mixxx::audio::kStartFramePos, end).value(),
            128.0,
            0.5);
    const auto pMap = BeatFactory::makePreferredBeats(
            beats, {}, false, mixxx::audio::SampleRate(kSampleRate));
    ASSERT_TRUE(pMap);
    EXPECT_NEAR(pMap->getBpmInRange(mixxx::audio::kStartFramePos, end).value(),
            128.0,
            0.5);
}

/// A bare 2 kHz click on each beat, optionally with the first click at the
/// very start of the file.
std::vector<float> clickOnlyTrack(double bpm, double seconds, double firstBeat) {
    constexpr uint32_t kSampleRate = 44100;
    std::vector<float> samples(
            static_cast<size_t>(seconds * kSampleRate), 0.0f);
    const double period = 60.0 / bpm;
    for (double t = firstBeat; t < seconds - 0.01; t += period) {
        const size_t start = static_cast<size_t>(t * kSampleRate);
        for (size_t i = 0; i < 220 && start + i < samples.size(); ++i) {
            samples[start + i] += static_cast<float>(
                    0.85 * std::sin(2.0 * kPi * 2000.0 * static_cast<double>(i) / kSampleRate));
        }
    }
    return samples;
}

TEST(RbxportGridTest, AnchorsBeatOneAtTheStartOfTheFile) {
    constexpr uint32_t kSampleRate = 44100;
    const std::vector<float> samples = clickOnlyTrack(128.0, 8.0, 0.0);
    const TempoResult result = analyseGrid(samples, kSampleRate);
    ASSERT_FALSE(result.beats.empty());
    EXPECT_NEAR(result.bpm, 128.0, 0.2);
    // The opening click is at frame 0: the grid must not lose that beat and
    // start almost a beat late.
    EXPECT_LE(result.beats.front().timeMs, 20u);
}

} // namespace
} // namespace mixxx::rbxport
