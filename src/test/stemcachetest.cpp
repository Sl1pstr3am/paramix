#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <QVariant>
#include <cmath>
#include <vector>

#include "preferences/configobject.h"
#include "sources/audiosource.h"
#include "sources/soundsourceproxy.h"
#include "stems/stemcache.h"
#include "stems/stemseparatorbackend.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace mixxx::stems {
namespace {

constexpr int kFrames = 2048;
constexpr int kSampleRate = 44100;

std::vector<CSAMPLE> makeStems() {
    std::vector<CSAMPLE> stems(static_cast<std::size_t>(kFrames) * kStemChannelCount);
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int channel = 0; channel < kStemChannelCount; ++channel) {
            stems[static_cast<std::size_t>(frame) * kStemChannelCount + channel] =
                    static_cast<CSAMPLE>(0.1 * std::sin(0.01 * frame + channel));
        }
    }
    return stems;
}

} // namespace

class StemCacheTest : public MixxxTest {};

TEST_F(StemCacheTest, WritesAndOpensAnEightChannelCache) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    config()->setValue(ConfigKey("[Stems]", "CacheDirectory"), tempDir.path());
    StemCache::initialize(config());

    const TrackId trackId(QVariant(42));
    const auto stems = makeStems();
    QString error;
    ASSERT_TRUE(StemCache::writeCache(
            trackId, stems.data(), kFrames, kSampleRate, &error))
            << error.toStdString();
    EXPECT_TRUE(StemCache::hasCache(trackId));

    const TrackPointer pTrack = Track::newDummy(QStringLiteral("dummy.flac"), trackId);
    SoundSourceProxy proxy(pTrack);
    const mixxx::AudioSourcePointer pSource = proxy.openAudioSource();
    ASSERT_NE(nullptr, pSource);
    EXPECT_EQ(mixxx::audio::ChannelCount::stem(),
            pSource->getSignalInfo().getChannelCount());
    EXPECT_EQ(mixxx::audio::SampleRate(kSampleRate),
            pSource->getSignalInfo().getSampleRate());
    EXPECT_EQ(kFrames, pSource->frameLength());
    EXPECT_TRUE(pTrack->hasStem());
    EXPECT_EQ(4, static_cast<int>(pTrack->getStemInfo().size()));

    // Release the open FLAC handle before deleting the cache on Windows.
    pSource->close();

    // A stereo request (samplers/preview) must read the original file, not the
    // 8-channel cache. The dummy file does not exist, so this fails cleanly.
    SoundSourceProxy stereoProxy(pTrack);
    mixxx::AudioSource::OpenParams stereoParams;
    stereoParams.setChannelCount(mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(nullptr, stereoProxy.openAudioSource(stereoParams));

    ASSERT_TRUE(StemCache::removeCache(trackId));
    EXPECT_FALSE(StemCache::hasCache(trackId));
}

} // namespace mixxx::stems
