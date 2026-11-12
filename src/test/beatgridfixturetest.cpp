#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtDebug>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/types.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/samplebuffer.h"

namespace {

// A detected beat counts as correct when it is within this tolerance of a
// golden beat. This mirrors the +/-70 ms tolerance from the plan
// (docs/beatgrid-audit.md §8.2).
constexpr double kBeatToleranceSec = 0.070;
// A uniform shift larger than the tolerance is guaranteed to miss every beat.
constexpr double kPerturbShiftSec = 0.100;

const QString kClickTrackPath = QStringLiteral("fixtures/beatgrid_128bpm.wav");
const QString kGoldenGridPath = QStringLiteral("fixtures/beatgrid_128bpm.json");

// F-measure between two beat lists expressed in seconds. Matches are assigned
// greedily in time order so a single candidate cannot match two golden beats.
// This is the reference implementation; tools/check_fixtures.py contains the
// same algorithm and the two must stay in sync.
double beatFMeasure(
        std::vector<double> candidates,
        std::vector<double> golden,
        double toleranceSec = kBeatToleranceSec) {
    std::sort(candidates.begin(), candidates.end());
    std::sort(golden.begin(), golden.end());

    std::size_t i = 0;
    std::size_t j = 0;
    std::size_t matched = 0;
    while (i < candidates.size() && j < golden.size()) {
        const double delta = candidates[i] - golden[j];
        if (std::abs(delta) <= toleranceSec) {
            ++matched;
            ++i;
            ++j;
        } else if (delta < 0.0) {
            ++i;
        } else {
            ++j;
        }
    }

    const double precision = candidates.empty()
            ? 0.0
            : static_cast<double>(matched) / candidates.size();
    const double recall = golden.empty()
            ? 0.0
            : static_cast<double>(matched) / golden.size();
    if (precision + recall == 0.0) {
        return 0.0;
    }
    return 2.0 * precision * recall / (precision + recall);
}

class BeatGridFixtureTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        const QString jsonPath = getTestDir().filePath(kGoldenGridPath);
        QFile jsonFile(jsonPath);
        ASSERT_TRUE(jsonFile.open(QIODevice::ReadOnly | QIODevice::Text))
                << "missing golden grid " << jsonPath.toStdString();

        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        ASSERT_FALSE(doc.isNull());
        ASSERT_TRUE(doc.isObject());

        const QJsonObject root = doc.object();
        m_bpm = root.value(QLatin1String("bpm")).toDouble();
        m_sampleRate = root.value(QLatin1String("sample_rate")).toInt();
        m_channels = root.value(QLatin1String("channels")).toInt();
        m_durationFrames =
                static_cast<SINT>(root.value(QLatin1String("duration_frames")).toDouble());

        const QJsonArray positions =
                root.value(QLatin1String("beat_positions_seconds")).toArray();
        m_goldenBeatsSec.clear();
        m_goldenBeatsSec.reserve(positions.size());
        for (const auto& value : positions) {
            m_goldenBeatsSec.push_back(value.toDouble());
        }
        ASSERT_FALSE(m_goldenBeatsSec.empty());
    }

    mixxx::AudioSourcePointer openClickTrack() const {
        const QString wavPath = getTestDir().filePath(kClickTrackPath);
        auto pTrack = Track::newTemporary(wavPath);
        mixxx::AudioSource::OpenParams openParams;
        openParams.setChannelCount(mixxx::audio::ChannelCount::mono());
        return SoundSourceProxy(pTrack).openAudioSource(openParams);
    }

    double m_bpm = 0.0;
    int m_sampleRate = 0;
    int m_channels = 0;
    SINT m_durationFrames = 0;
    std::vector<double> m_goldenBeatsSec;
};

TEST_F(BeatGridFixtureTest, DecodesAsExpected) {
    ASSERT_NEAR(m_bpm, 128.0, 1e-9);

    const auto pSource = openClickTrack();
    ASSERT_TRUE(pSource);

    EXPECT_EQ(static_cast<int>(m_sampleRate),
            static_cast<int>(pSource->getSignalInfo().getSampleRate().value()));
    EXPECT_EQ(m_channels,
            static_cast<int>(pSource->getSignalInfo().getChannelCount().value()));
    EXPECT_EQ(m_durationFrames, pSource->frameLength());
}

TEST_F(BeatGridFixtureTest, FMeasureOfGoldenGridIsPerfect) {
    // A candidate grid identical to the golden grid must score a perfect 1.0.
    EXPECT_DOUBLE_EQ(1.0, beatFMeasure(m_goldenBeatsSec, m_goldenBeatsSec));
}

TEST_F(BeatGridFixtureTest, FMeasureOfShiftedGridFails) {
    // Shifting every beat by 100 ms (beyond the 70 ms tolerance) must collapse
    // the F-measure, proving the harness actually detects a wrong grid.
    std::vector<double> shifted;
    shifted.reserve(m_goldenBeatsSec.size());
    for (const double beat : m_goldenBeatsSec) {
        shifted.push_back(beat + kPerturbShiftSec);
    }
    EXPECT_LT(beatFMeasure(shifted, m_goldenBeatsSec), 0.5);
}

TEST_F(BeatGridFixtureTest, FMeasureHelperSemantics) {
    // Hand-verified examples that lock the helper's behavior independently of
    // the fixture.
    EXPECT_DOUBLE_EQ(1.0, beatFMeasure({0.0, 0.5, 1.0}, {0.0, 0.5, 1.0}));
    // One golden beat missed: precision 1.0, recall 2/3 -> F = 4/5.
    EXPECT_NEAR(0.8, beatFMeasure({0.0, 1.0}, {0.0, 0.5, 1.0}), 1e-12);
    // Nothing within tolerance.
    EXPECT_DOUBLE_EQ(0.0, beatFMeasure({0.0, 0.5, 1.0}, {10.0, 10.5, 11.0}));
    // Empty golden list is well defined.
    EXPECT_DOUBLE_EQ(0.0, beatFMeasure({0.0, 0.5, 1.0}, {}));
}

// NOTE: the real Queen Mary detector (AnalyzerQueenMaryBeats) is intentionally
// not wired in here. Running it end-to-end requires the analyzer pipeline and
// a full build; this harness verifies the fixture decodes and the F-measure
// helper that any candidate grid (detected or otherwise) is scored against.
// The integration point is: decode the click track, feed it to the analyzer,
// collect the returned beat positions in seconds, and call
// `beatFMeasure(candidateBeats, m_goldenBeatsSec)`.

} // namespace
