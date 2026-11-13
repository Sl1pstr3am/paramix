#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtDebug>

#include <sndfile.h>

#include <cmath>
#include <cstddef>
#include <ostream>
#include <vector>

#include "audio/types.h"
#include "sources/soundsourceflac.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/samplebuffer.h"
#include "util/types.h"

namespace {

// ADR-002 stores four stereo stems in one 8-channel FLAC, interleaved as
// stem 0 L/R, stem 1 L/R, stem 2 L/R, stem 3 L/R. This test proves that the
// planned container survives a write/read round-trip on every CI target before
// any cache code is written.

constexpr int kStemChannelCount = 8;
static_assert(kStemChannelCount == mixxx::audio::ChannelCount::stem().value(),
        "four stereo stems must be eight channels");
constexpr SINT kFrameCount = 8192;
constexpr double kPi = 3.14159265358979323846;

struct FlacFormat {
    int sampleRate;
    int bitDepth;
};

std::ostream& operator<<(std::ostream& os, const FlacFormat& format) {
    return os << format.sampleRate << "Hz/" << format.bitDepth << "bit";
}

/// A distinct tone per channel, with amplitudes well below full scale so
/// nothing clips and a channel swap or drop cannot pass unnoticed.
std::vector<float> makeStemSignal(int sampleRate, SINT frameCount) {
    std::vector<float> samples(
            static_cast<std::size_t>(frameCount) * kStemChannelCount);
    for (SINT frame = 0; frame < frameCount; ++frame) {
        for (int channel = 0; channel < kStemChannelCount; ++channel) {
            const double amplitude = 0.10 + 0.05 * channel;
            const double frequency = 110.0 * (channel + 1);
            const double phase =
                    2.0 * kPi * frequency * static_cast<double>(frame) / sampleRate;
            samples[static_cast<std::size_t>(frame) * kStemChannelCount +
                    channel] = static_cast<float>(amplitude * std::sin(phase));
        }
    }
    return samples;
}

/// Rounding to PCM costs at most half a step; allow two for safety.
double quantizationTolerance(int bitDepth) {
    return 4.0 / std::pow(2.0, bitDepth - 1);
}

bool writeStemFlac(const QString& filePath,
        int sampleRate,
        int bitDepth,
        const std::vector<float>& samples,
        SINT frameCount) {
    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels = kStemChannelCount;
    info.format = SF_FORMAT_FLAC |
            (bitDepth == 24 ? SF_FORMAT_PCM_24 : SF_FORMAT_PCM_16);

    if (!sf_format_check(&info)) {
        qWarning() << "libsndfile rejects" << sampleRate << "Hz" << bitDepth
                   << "bit" << kStemChannelCount << "channel FLAC";
        return false;
    }

    SNDFILE* pFile = sf_open(
            filePath.toUtf8().constData(), SFM_WRITE, &info);
    if (pFile == nullptr) {
        qWarning() << "sf_open failed for" << filePath << sf_strerror(nullptr);
        return false;
    }

    const sf_count_t written =
            sf_writef_float(pFile, samples.data(), frameCount);
    if (written != frameCount) {
        qWarning() << "sf_writef_float wrote" << written << "of" << frameCount
                   << "frames:" << sf_strerror(pFile);
        sf_close(pFile);
        return false;
    }
    if (sf_close(pFile) != 0) {
        qWarning() << "sf_close failed for" << filePath;
        return false;
    }
    return true;
}

} // namespace

class StemFlacTest : public MixxxTest,
                     public testing::WithParamInterface<FlacFormat> {
  protected:
    static void expectSamplesEqual(const std::vector<float>& expected,
            const CSAMPLE* actual,
            int bitDepth) {
        const double tolerance = quantizationTolerance(bitDepth);
        for (SINT frame = 0; frame < kFrameCount; ++frame) {
            for (int channel = 0; channel < kStemChannelCount; ++channel) {
                const std::size_t index =
                        static_cast<std::size_t>(frame) * kStemChannelCount +
                        channel;
                ASSERT_NEAR(expected[index], actual[index], tolerance)
                        << "frame " << frame << " channel " << channel;
            }
        }
    }
};

TEST_P(StemFlacTest, RoundTripsThroughSoundSourceFlac) {
    const FlacFormat format = GetParam();

    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("stems.flac"));

    const std::vector<float> expected =
            makeStemSignal(format.sampleRate, kFrameCount);
    ASSERT_TRUE(writeStemFlac(
            filePath, format.sampleRate, format.bitDepth, expected, kFrameCount));

    // The file must be a FLAC container, not a WAV that happens to decode.
    QFile file(filePath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArrayLiteral("fLaC"), file.read(4));
    file.close();

    mixxx::SoundSourceFLAC source(QUrl::fromLocalFile(filePath));
    ASSERT_EQ(mixxx::AudioSource::OpenResult::Succeeded,
            source.open(mixxx::AudioSource::OpenMode::Strict,
                    mixxx::AudioSource::OpenParams()));

    EXPECT_EQ(mixxx::audio::ChannelCount::stem(),
            source.getSignalInfo().getChannelCount());
    EXPECT_EQ(format.sampleRate,
            static_cast<int>(source.getSignalInfo().getSampleRate().value()));
    EXPECT_EQ(kFrameCount, source.frameLength());

    mixxx::SampleBuffer buffer(
            source.getSignalInfo().frames2samples(kFrameCount));
    const auto readableSampleFrames = source.readSampleFrames(
            mixxx::WritableSampleFrames(
                    mixxx::IndexRange::forward(0, kFrameCount),
                    mixxx::SampleBuffer::WritableSlice(buffer)));
    ASSERT_EQ(kFrameCount, readableSampleFrames.frameIndexRange().length());
    ASSERT_EQ(kFrameCount * kStemChannelCount,
            readableSampleFrames.readableLength());

    expectSamplesEqual(expected,
            readableSampleFrames.readableData(),
            format.bitDepth);
}

INSTANTIATE_TEST_SUITE_P(
        StemFlacFormats,
        StemFlacTest,
        testing::Values(
                FlacFormat{44100, 16},
                FlacFormat{44100, 24},
                FlacFormat{48000, 16},
                FlacFormat{48000, 24},
                FlacFormat{96000, 16},
                FlacFormat{96000, 24}));

class StemFlacProxyTest : public MixxxTest,
                          public SoundSourceProviderRegistration {};

TEST_F(StemFlacProxyTest, OpensEightChannelFlacThroughProxy) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("stems.flac"));

    ASSERT_TRUE(writeStemFlac(filePath,
            44100,
            16,
            makeStemSignal(44100, kFrameCount),
            kFrameCount));

    auto pTrack = Track::newTemporary(filePath);
    SoundSourceProxy proxy(pTrack);
    auto pAudioSource = proxy.openAudioSource();
    ASSERT_TRUE(pAudioSource);

    EXPECT_EQ(mixxx::audio::ChannelCount::stem(),
            pAudioSource->getSignalInfo().getChannelCount());
    EXPECT_EQ(44100,
            static_cast<int>(pAudioSource->getSignalInfo().getSampleRate().value()));
    EXPECT_EQ(kFrameCount, pAudioSource->frameLength());
}
