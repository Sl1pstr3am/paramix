#include <gtest/gtest.h>

#include <QStringList>
#include <QtDebug>
#include <array>
#include <cmath>

#include "audio/types.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/samplebuffer.h"

namespace {

const QString kFixtureDir = QStringLiteral("fixtures");
constexpr int kNumStems = 4;

// Stem order in the 8-channel file, matching tools/make_stem_fixture.py:
// drums, bass, vocals, other. Each stem is one stereo pair (L, R).
const QStringList kStemNames = {
        QStringLiteral("drums"),
        QStringLiteral("bass"),
        QStringLiteral("vocals"),
        QStringLiteral("other"),
};

class StemFixtureTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    QString fixturePath(const QString& fileName) const {
        return getTestDir().filePath(
                kFixtureDir + QLatin1String("/") + fileName);
    }

    QString stemPath(const QString& name) const {
        return fixturePath(QStringLiteral("stem_%1.wav").arg(name));
    }

    mixxx::AudioSourcePointer openAudio(
            const QString& path,
            mixxx::audio::ChannelCount channelCount) const {
        auto pTrack = Track::newTemporary(path);
        mixxx::AudioSource::OpenParams openParams;
        openParams.setChannelCount(channelCount);
        return SoundSourceProxy(pTrack).openAudioSource(openParams);
    }
};

TEST_F(StemFixtureTest, EightChannelFixtureDecodesAsStem) {
    const auto pSource = openAudio(
            fixturePath(QStringLiteral("stem_8ch.wav")),
            mixxx::audio::ChannelCount::stem());
    ASSERT_TRUE(pSource);

    EXPECT_EQ(static_cast<int>(mixxx::audio::ChannelCount::stem().value()),
            static_cast<int>(pSource->getSignalInfo().getChannelCount().value()));
    EXPECT_EQ(static_cast<int>(mixxx::audio::SampleRate(44100).value()),
            static_cast<int>(pSource->getSignalInfo().getSampleRate().value()));

    const SINT numFrames = pSource->frameLength();
    const SINT numSamples = pSource->getSignalInfo().frames2samples(numFrames);
    ASSERT_EQ(8 * numFrames, numSamples);

    mixxx::SampleBuffer buffer(numSamples);
    const auto readable = pSource->readSampleFrames(mixxx::WritableSampleFrames(
            mixxx::IndexRange::forward(0, numFrames),
            mixxx::SampleBuffer::WritableSlice(buffer)));
    ASSERT_EQ(numFrames, readable.frameLength());

    // RMS per stem pair. The interleaved channel order is 1L,1R,2L,2R,3L,3R,4L,4R,
    // so stem k occupies channels 2k and 2k+1.
    std::array<double, kNumStems> rms{};
    for (SINT frame = 0; frame < numFrames; ++frame) {
        for (int stem = 0; stem < kNumStems; ++stem) {
            const double left = buffer[frame * 8 + 2 * stem];
            const double right = buffer[frame * 8 + 2 * stem + 1];
            rms[stem] += left * left + right * right;
        }
    }
    for (int stem = 0; stem < kNumStems; ++stem) {
        rms[stem] = std::sqrt(rms[stem] / (2.0 * numFrames));
    }

    // Every stem is non-silent.
    for (int stem = 0; stem < kNumStems; ++stem) {
        EXPECT_GT(rms[stem], 0.01)
                << "stem " << kStemNames[stem].toStdString()
                << " is silent (rms=" << rms[stem] << ")";
    }

    // The stems are mutually distinct: the generator assigns strictly
    // decreasing amplitudes (drums > bass > vocals > other), so the per-stem
    // RMS must be strictly decreasing in that same order.
    for (int stem = 0; stem < kNumStems - 1; ++stem) {
        EXPECT_GT(rms[stem], rms[stem + 1])
                << "stem " << kStemNames[stem].toStdString()
                << " (rms=" << rms[stem] << ") should be louder than "
                << kStemNames[stem + 1].toStdString()
                << " (rms=" << rms[stem + 1] << ")";
    }
}

TEST_F(StemFixtureTest, IndividualStemsAndMixDecodeAsStereo) {
    for (const auto& name : kStemNames) {
        const auto pSource = openAudio(
                stemPath(name), mixxx::audio::ChannelCount::stereo());
        ASSERT_TRUE(pSource) << "failed to decode stem " << name.toStdString();
        EXPECT_EQ(static_cast<int>(mixxx::audio::ChannelCount::stereo().value()),
                static_cast<int>(pSource->getSignalInfo().getChannelCount().value()))
                << "stem " << name.toStdString();
        EXPECT_EQ(static_cast<int>(mixxx::audio::SampleRate(44100).value()),
                static_cast<int>(pSource->getSignalInfo().getSampleRate().value()))
                << "stem " << name.toStdString();
    }

    const auto pMix = openAudio(
            fixturePath(QStringLiteral("stem_mix.wav")),
            mixxx::audio::ChannelCount::stereo());
    ASSERT_TRUE(pMix);
    EXPECT_EQ(static_cast<int>(mixxx::audio::ChannelCount::stereo().value()),
            static_cast<int>(pMix->getSignalInfo().getChannelCount().value()));
}

} // namespace
