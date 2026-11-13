#include <gtest/gtest.h>
#include <sndfile.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "stems/stemfileformat.h"
#include "stems/stemfilewriter.h"
#include "stems/stemseparatorbackend.h"

namespace mixxx::stems {
namespace {

constexpr SINT kFrames = 4096;
constexpr int kSampleRate = 44100;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kStemNames[] = {"drums", "bass", "other", "vocals"};

std::vector<CSAMPLE> makeStems(SINT frames) {
    std::vector<CSAMPLE> stems(
            static_cast<std::size_t>(frames) * kStemChannelCount);
    for (SINT frame = 0; frame < frames; ++frame) {
        for (int stem = 0; stem < kStemCount; ++stem) {
            const double frequency = 220.0 * (stem + 1);
            const double time = static_cast<double>(frame) / kSampleRate;
            const auto index = static_cast<std::size_t>(frame) *
                            kStemChannelCount +
                    stem * 2;
            stems[index] = static_cast<CSAMPLE>(
                    0.2 * std::sin(2.0 * kPi * frequency * time));
            stems[index + 1] = static_cast<CSAMPLE>(
                    0.2 * std::sin(2.0 * kPi * frequency * time + 0.23));
        }
    }
    return stems;
}

// Reads interleaved samples, in frame-major left/right order.
std::vector<float> readSndFile(const QString& path, SINT frames) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.toUtf8().constData(), SFM_READ, &info);
    EXPECT_NE(nullptr, file) << path.toStdString();
    if (file == nullptr) {
        return {};
    }
    EXPECT_EQ(2, info.channels);
    EXPECT_EQ(kSampleRate, info.samplerate);
    std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
    EXPECT_EQ(static_cast<sf_count_t>(frames) * 2,
            sf_read_float(file, samples.data(), static_cast<sf_count_t>(samples.size())));
    EXPECT_EQ(0, sf_close(file));
    return samples;
}

StemSetInfo makeInfo(StemFileFormat format = StemFileFormat::Flac) {
    StemSetInfo info;
    info.baseName = QStringLiteral("track");
    info.modelName = QStringLiteral("test-model");
    info.sampleRate = kSampleRate;
    info.frames = kFrames;
    info.format = format;
    return info;
}

class StemFileFormatTest : public testing::Test {};

TEST_F(StemFileFormatTest, ParsesFormatNamesAndFallsBackToFlac) {
    EXPECT_EQ(StemFileFormat::Flac, parseStemFileFormat(QStringLiteral("flac")));
    EXPECT_EQ(StemFileFormat::Flac, parseStemFileFormat(QStringLiteral("FLAC")));
    EXPECT_EQ(StemFileFormat::Alac, parseStemFileFormat(QStringLiteral("alac")));
    EXPECT_EQ(StemFileFormat::Aac, parseStemFileFormat(QStringLiteral("Aac")));
    EXPECT_EQ(StemFileFormat::Flac, parseStemFileFormat(QStringLiteral("bogus")));
    EXPECT_EQ(StemFileFormat::Flac, parseStemFileFormat(QString()));
}

TEST_F(StemFileFormatTest, ReportsFormatNamesExtensionsAndLosslessness) {
    EXPECT_EQ(QStringLiteral("flac"), stemFileFormatName(StemFileFormat::Flac));
    EXPECT_EQ(QStringLiteral("alac"), stemFileFormatName(StemFileFormat::Alac));
    EXPECT_EQ(QStringLiteral("aac"), stemFileFormatName(StemFileFormat::Aac));
    EXPECT_EQ(QStringLiteral("flac"), stemFileExtension(StemFileFormat::Flac));
    EXPECT_EQ(QStringLiteral("caf"), stemFileExtension(StemFileFormat::Alac));
    EXPECT_EQ(QStringLiteral("aac"), stemFileExtension(StemFileFormat::Aac));
    EXPECT_TRUE(isLosslessStemFormat(StemFileFormat::Flac));
    EXPECT_TRUE(isLosslessStemFormat(StemFileFormat::Alac));
    EXPECT_FALSE(isLosslessStemFormat(StemFileFormat::Aac));
}

TEST_F(StemFileFormatTest, SanitizesStemSetNames) {
    const QString sanitized = sanitizeStemSetName(QStringLiteral("a/b:c*?"));
    EXPECT_FALSE(sanitized.contains(QLatin1Char('/')));
    EXPECT_FALSE(sanitized.contains(QLatin1Char('\\')));
    EXPECT_FALSE(sanitized.contains(QLatin1Char('*')));
    EXPECT_FALSE(sanitized.contains(QLatin1Char('?')));
    EXPECT_EQ(QStringLiteral("stems"), sanitizeStemSetName(QString()));
    EXPECT_EQ(QStringLiteral("stems"),
            sanitizeStemSetName(QStringLiteral(" \t\n ")));
    EXPECT_EQ(QStringLiteral("name"),
            sanitizeStemSetName(QStringLiteral("name.  ")));
}

class StemFileWriterTest : public testing::Test {};

TEST_F(StemFileWriterTest, WritesFlacSetAndRoundTrips) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    ASSERT_TRUE(writeStemSet(temp.path(), makeInfo(), stems.data(), &error))
            << error.toStdString();

    const QDir setDir(temp.path() + QStringLiteral("/track"));
    ASSERT_TRUE(setDir.exists());
    for (int stem = 0; stem < kStemCount; ++stem) {
        const QString filename = QString::fromLatin1(kStemNames[stem]) +
                QStringLiteral(".flac");
        const QString path = setDir.filePath(filename);
        ASSERT_TRUE(QFile::exists(path));
        EXPECT_GT(QFileInfo(path).size(), 0);
        const auto decoded = readSndFile(path, kFrames);
        ASSERT_EQ(static_cast<std::size_t>(kFrames) * 2, decoded.size());
        for (SINT frame = 0; frame < kFrames; ++frame) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto source = static_cast<std::size_t>(frame) *
                                kStemChannelCount +
                        stem * 2 + channel;
                const auto decodedIndex = static_cast<std::size_t>(frame) * 2 +
                        channel;
                EXPECT_NEAR(stems[source], decoded[decodedIndex], 1e-5f);
            }
        }
    }
    EXPECT_TRUE(QFile::exists(setDir.filePath(QStringLiteral("stem-set.json"))));
}

TEST_F(StemFileWriterTest, WritesAlacSetAndRoundTrips) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    ASSERT_TRUE(writeStemSet(temp.path(), makeInfo(StemFileFormat::Alac), stems.data(), &error)) << error.toStdString();
    const QDir setDir(temp.path() + QStringLiteral("/track"));
    for (int stem = 0; stem < kStemCount; ++stem) {
        const QString path = setDir.filePath(
                QString::fromLatin1(kStemNames[stem]) + QStringLiteral(".caf"));
        ASSERT_TRUE(QFile::exists(path));
        EXPECT_GT(QFileInfo(path).size(), 0);
        const auto decoded = readSndFile(path, kFrames);
        ASSERT_EQ(static_cast<std::size_t>(kFrames) * 2, decoded.size());
        for (SINT frame = 0; frame < kFrames; ++frame) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto source = static_cast<std::size_t>(frame) *
                                kStemChannelCount +
                        stem * 2 + channel;
                EXPECT_NEAR(stems[source],
                        decoded[static_cast<std::size_t>(frame) * 2 + channel],
                        1e-5f);
            }
        }
    }
    EXPECT_TRUE(QFile::exists(setDir.filePath(QStringLiteral("stem-set.json"))));
}

TEST_F(StemFileWriterTest, WritesAacSetOrSkips) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    auto info = makeInfo(StemFileFormat::Aac);
    info.aacBitrateKbps = 128;
    const auto stems = makeStems(kFrames);
    QString error;
    if (!writeStemSet(temp.path(), info, stems.data(), &error)) {
        const std::string message = error.toStdString();
        std::string lower = message;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("aac") != std::string::npos ||
                lower.find("fdk") != std::string::npos) {
            GTEST_SKIP() << message;
        }
        FAIL() << message;
    }

    const QDir setDir(temp.path() + QStringLiteral("/track"));
    for (int stem = 0; stem < kStemCount; ++stem) {
        const QString path = setDir.filePath(
                QString::fromLatin1(kStemNames[stem]) + QStringLiteral(".aac"));
        ASSERT_TRUE(QFile::exists(path));
        EXPECT_GT(QFileInfo(path).size(), 0);
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray header = file.read(2);
        ASSERT_EQ(2, header.size());
        const auto b0 = static_cast<unsigned char>(header[0]);
        const auto b1 = static_cast<unsigned char>(header[1]);
        EXPECT_EQ(0xFF, b0);
        EXPECT_EQ(0xF0, b1 & 0xF6);
    }
    EXPECT_TRUE(QFile::exists(setDir.filePath(QStringLiteral("stem-set.json"))));
}

TEST_F(StemFileWriterTest, SidecarDescribesTheSet) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    ASSERT_TRUE(writeStemSet(temp.path(), makeInfo(), stems.data(), &error))
            << error.toStdString();
    QFile file(temp.path() + QStringLiteral("/track/stem-set.json"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(file.readAll());
    ASSERT_TRUE(document.isObject());
    const auto root = document.object();
    EXPECT_EQ(1, root.value(QStringLiteral("version")).toInt());
    EXPECT_EQ(QStringLiteral("test-model"),
            root.value(QStringLiteral("model")).toString());
    EXPECT_EQ(QStringLiteral("flac"),
            root.value(QStringLiteral("format")).toString());
    EXPECT_TRUE(root.value(QStringLiteral("lossless")).toBool());
    EXPECT_EQ(kSampleRate, root.value(QStringLiteral("sample_rate")).toInt());
    EXPECT_EQ(kFrames, root.value(QStringLiteral("frames")).toInt());
    const auto sources = root.value(QStringLiteral("sources")).toArray();
    ASSERT_EQ(kStemCount, sources.size());
    for (int stem = 0; stem < kStemCount; ++stem) {
        const auto source = sources[stem].toObject();
        EXPECT_EQ(QString::fromLatin1(kStemNames[stem]),
                source.value(QStringLiteral("name")).toString());
        EXPECT_EQ(QString::fromLatin1(kStemNames[stem]) +
                        QStringLiteral(".flac"),
                source.value(QStringLiteral("file")).toString());
        EXPECT_FALSE(source.value(QStringLiteral("color")).toString().isEmpty());
    }
}

TEST_F(StemFileWriterTest, RefusesExistingSet) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    ASSERT_TRUE(writeStemSet(temp.path(), makeInfo(), stems.data(), &error));
    error.clear();
    EXPECT_FALSE(writeStemSet(temp.path(), makeInfo(), stems.data(), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFile::exists(temp.path() + QStringLiteral("/track/drums.flac")));
    EXPECT_TRUE(QFile::exists(temp.path() + QStringLiteral("/track/stem-set.json")));
}

TEST_F(StemFileWriterTest, RefusesInvalidArguments) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    auto info = makeInfo();
    EXPECT_FALSE(writeStemSet(temp.path(), info, nullptr, &error));
    EXPECT_FALSE(error.isEmpty());
    info = makeInfo();
    info.frames = 0;
    error.clear();
    EXPECT_FALSE(writeStemSet(temp.path(), info, stems.data(), &error));
    EXPECT_FALSE(error.isEmpty());
    error.clear();
    EXPECT_FALSE(writeStemSet(QString(), makeInfo(), stems.data(), &error));
    EXPECT_FALSE(error.isEmpty());
    info = makeInfo();
    info.sampleRate = 0;
    error.clear();
    EXPECT_FALSE(writeStemSet(temp.path(), info, stems.data(), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(StemFileWriterTest, LeavesNoTemporaryDirectory) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto stems = makeStems(kFrames);
    QString error;
    ASSERT_TRUE(writeStemSet(temp.path(), makeInfo(), stems.data(), &error))
            << error.toStdString();
    const QStringList directories = QDir(temp.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    ASSERT_EQ(1, directories.size());
    EXPECT_EQ(QStringLiteral("track"), directories.front());
}

} // namespace
} // namespace mixxx::stems
