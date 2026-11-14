#include "stems/stemfilewriter.h"

#include <algorithm>
#include <array>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#endif
#include <sndfile.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include "encoder/encodercallback.h"
#include "encoder/encoderfdkaac.h"
#include "encoder/encodersettings.h"
#include "recording/defs_recording.h"
#include "stems/stemseparatorbackend.h"
#include "util/assert.h"

namespace mixxx::stems {
namespace {

constexpr int kBlockFrames = 8192;
constexpr char kSidecarName[] = "stem-set.json";

constexpr std::array<const char*, kStemCount> kStemNames = {
        "drums", "bass", "other", "vocals"};
constexpr std::array<const char*, kStemCount> kStemColors = {
        "#E85D4A", "#7A5AF8", "#F2C94C", "#2BB673"};

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

SNDFILE* openSndFileForWrite(const QString& path, SF_INFO* pInfo) {
#ifdef _WIN32
    return sf_wchar_open(path.toStdWString().c_str(), SFM_WRITE, pInfo);
#else
    return sf_open(path.toUtf8().constData(), SFM_WRITE, pInfo);
#endif
}

bool writeBlockToSndFile(SNDFILE* pFile,
        const CSAMPLE* pStems,
        SINT frames,
        int stem,
        QString* pError) {
    std::vector<CSAMPLE> block(static_cast<std::size_t>(kBlockFrames) * 2);
    for (SINT offset = 0; offset < frames; offset += kBlockFrames) {
        const SINT count = std::min<SINT>(kBlockFrames, frames - offset);
        for (SINT frame = 0; frame < count; ++frame) {
            block[static_cast<std::size_t>(frame) * 2] =
                    pStems[(offset + frame) * kStemChannelCount + stem * 2];
            block[static_cast<std::size_t>(frame) * 2 + 1] =
                    pStems[(offset + frame) * kStemChannelCount + stem * 2 + 1];
        }
        const sf_count_t expected = count * 2;
        if (sf_write_float(pFile, block.data(), expected) != expected) {
            setError(pError,
                    QStringLiteral("Could not write stem audio: %1")
                            .arg(QString::fromUtf8(sf_strerror(pFile))));
            return false;
        }
    }
    return true;
}

bool writeSndFileStem(const QString& path,
        const CSAMPLE* pStems,
        SINT frames,
        int stem,
        int sampleRate,
        int format,
        QString* pError) {
    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels = 2;
    info.format = format;

    SNDFILE* pFile = openSndFileForWrite(path, &info);
    if (pFile == nullptr) {
        setError(pError,
                QStringLiteral("Could not create %1: %2")
                        .arg(path, QString::fromUtf8(sf_strerror(nullptr))));
        return false;
    }

    sf_command(pFile, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);
    sf_command(pFile, SFC_SET_CLIPPING, nullptr, SF_TRUE);

    const bool written = writeBlockToSndFile(pFile, pStems, frames, stem, pError);
    if (sf_close(pFile) != 0 && written) {
        setError(pError, QStringLiteral("Could not finalize %1").arg(path));
        return false;
    }
    return written;
}

class FileEncoderCallback final : public EncoderCallback {
  public:
    explicit FileEncoderCallback(const QString& path)
            : m_file(path) {
        const bool opened =
                m_file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        Q_UNUSED(opened);
    }

    bool isOpen() const {
        return m_file.isOpen();
    }

    bool hasError() const {
        return m_file.error() != QFileDevice::NoError;
    }

    void write(const unsigned char* pHeader,
            const unsigned char* pBody,
            int headerLength,
            int bodyLength) override {
        if (headerLength > 0) {
            m_file.write(reinterpret_cast<const char*>(pHeader), headerLength);
        }
        if (bodyLength > 0) {
            m_file.write(reinterpret_cast<const char*>(pBody), bodyLength);
        }
    }

    int tell() override {
        return static_cast<int>(m_file.pos());
    }

    void seek(int position) override {
        m_file.seek(position);
    }

    int filelen() override {
        return static_cast<int>(m_file.size());
    }

  private:
    QFile m_file;
};

class StemAacSettings final : public EncoderSettings {
  public:
    explicit StemAacSettings(int bitrateKbps)
            : m_bitrateKbps(bitrateKbps) {
    }

    QString getFormat() const override {
        return QStringLiteral(ENCODING_AAC);
    }

    ChannelMode getChannelMode() const override {
        return ChannelMode::STEREO;
    }

    int getQuality() const override {
        return m_bitrateKbps;
    }

  private:
    int m_bitrateKbps;
};

bool writeAacStem(const QString& path,
        const CSAMPLE* pStems,
        SINT frames,
        int stem,
        int sampleRate,
        int bitrateKbps,
        QString* pError) {
    FileEncoderCallback callback(path);
    if (!callback.isOpen()) {
        setError(pError, QStringLiteral("Could not create %1").arg(path));
        return false;
    }

    EncoderFdkAac encoder(&callback);
    const StemAacSettings settings(bitrateKbps);
    encoder.setEncoderSettings(settings);

    QString userError;
    if (encoder.initEncoder(mixxx::audio::SampleRate(sampleRate), &userError) != 0) {
        setError(pError,
                userError.isEmpty()
                        ? QStringLiteral("The AAC encoder is not available")
                        : userError);
        return false;
    }

    std::vector<CSAMPLE> block(static_cast<std::size_t>(kBlockFrames) * 2);
    for (SINT offset = 0; offset < frames; offset += kBlockFrames) {
        const SINT count = std::min<SINT>(kBlockFrames, frames - offset);
        for (SINT frame = 0; frame < count; ++frame) {
            block[static_cast<std::size_t>(frame) * 2] =
                    pStems[(offset + frame) * kStemChannelCount + stem * 2];
            block[static_cast<std::size_t>(frame) * 2 + 1] =
                    pStems[(offset + frame) * kStemChannelCount + stem * 2 + 1];
        }
        encoder.encodeBuffer(block.data(), static_cast<std::size_t>(count) * 2);
    }
    encoder.flush();

    if (callback.hasError()) {
        setError(pError, QStringLiteral("Could not write %1").arg(path));
        return false;
    }
    return true;
}

bool writeSidecar(const QString& path,
        const StemSetInfo& info,
        QString* pError) {
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("model")] = info.modelName;
    root[QStringLiteral("format")] = stemFileFormatName(info.format);
    root[QStringLiteral("lossless")] = isLosslessStemFormat(info.format);
    root[QStringLiteral("sample_rate")] = info.sampleRate;
    root[QStringLiteral("frames")] = static_cast<qint64>(info.frames);
    if (info.format == StemFileFormat::Aac) {
        root[QStringLiteral("aac_bitrate_kbps")] = std::clamp(
                info.aacBitrateKbps, kMinAacBitrateKbps, kMaxAacBitrateKbps);
    }

    QJsonArray sources;
    const QString extension = stemFileExtension(info.format);
    for (int stem = 0; stem < kStemCount; ++stem) {
        QJsonObject source;
        source[QStringLiteral("name")] = QString::fromLatin1(kStemNames[stem]);
        source[QStringLiteral("file")] =
                QStringLiteral("%1.%2")
                        .arg(QString::fromLatin1(kStemNames[stem]), extension);
        source[QStringLiteral("color")] = QString::fromLatin1(kStemColors[stem]);
        sources.append(source);
    }
    root[QStringLiteral("sources")] = sources;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(pError, QStringLiteral("Could not create %1").arg(path));
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        setError(pError, QStringLiteral("Could not write %1").arg(path));
        return false;
    }
    return true;
}

} // namespace

QString sanitizeStemSetName(const QString& name) {
    QString result = name.trimmed();
    static const QRegularExpression invalid(
            QStringLiteral("[\\\\/:*?\"<>|]"));
    result.replace(invalid, QStringLiteral("_"));
    while (result.endsWith(QLatin1Char('.')) ||
            result.endsWith(QLatin1Char(' '))) {
        result.chop(1);
    }
    if (result.isEmpty()) {
        return QStringLiteral("stems");
    }
    return result;
}

QString uniqueStemSetName(const QString& outputDirectory, const QString& baseName) {
    const QString sanitized = sanitizeStemSetName(baseName);
    QDir outputDir(outputDirectory);
    if (!outputDir.exists(sanitized)) {
        return sanitized;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(sanitized).arg(suffix);
        if (!outputDir.exists(candidate)) {
            return candidate;
        }
    }
    return sanitized;
}

bool writeStemSet(const QString& outputDirectory,
        const StemSetInfo& info,
        const CSAMPLE* pInterleavedStems,
        QString* pError) {
    if (pInterleavedStems == nullptr) {
        setError(pError, QStringLiteral("No stem audio was provided"));
        return false;
    }
    if (info.frames <= 0) {
        setError(pError, QStringLiteral("There are no frames to write"));
        return false;
    }
    if (info.sampleRate <= 0) {
        setError(pError, QStringLiteral("The sample rate is invalid"));
        return false;
    }
    if (outputDirectory.trimmed().isEmpty()) {
        setError(pError, QStringLiteral("No output folder was provided"));
        return false;
    }

    const QString name = sanitizeStemSetName(info.baseName);
    QDir outputDir(outputDirectory);
    if (!outputDir.exists() && !outputDir.mkpath(QStringLiteral("."))) {
        setError(pError,
                QStringLiteral("Could not create the output folder %1")
                        .arg(outputDirectory));
        return false;
    }
    if (outputDir.exists(name)) {
        setError(pError,
                QStringLiteral("A stem set named %1 already exists").arg(name));
        return false;
    }

    const QString tempName = QStringLiteral(".%1.tmp-%2")
                                     .arg(name,
                                             QUuid::createUuid().toString(
                                                     QUuid::WithoutBraces));
    if (!outputDir.mkdir(tempName)) {
        setError(pError,
                QStringLiteral("Could not create a temporary folder in %1")
                        .arg(outputDirectory));
        return false;
    }
    const QString tempPath = outputDir.filePath(tempName);

    const int sndFileFormat = info.format == StemFileFormat::Alac
            ? SF_FORMAT_CAF | SF_FORMAT_ALAC_24
            : SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
    const QString extension = stemFileExtension(info.format);
    const int bitrateKbps = std::clamp(
            info.aacBitrateKbps, kMinAacBitrateKbps, kMaxAacBitrateKbps);

    bool written = true;
    for (int stem = 0; stem < kStemCount && written; ++stem) {
        const QString path = QDir(tempPath).filePath(
                QStringLiteral("%1.%2")
                        .arg(QString::fromLatin1(kStemNames[stem]), extension));
        if (info.format == StemFileFormat::Aac) {
            written = writeAacStem(path,
                    pInterleavedStems,
                    info.frames,
                    stem,
                    info.sampleRate,
                    bitrateKbps,
                    pError);
        } else {
            written = writeSndFileStem(path,
                    pInterleavedStems,
                    info.frames,
                    stem,
                    info.sampleRate,
                    sndFileFormat,
                    pError);
        }
    }

    if (written) {
        written = writeSidecar(QDir(tempPath).filePath(
                                       QString::fromLatin1(kSidecarName)),
                info,
                pError);
    }

    if (written) {
        if (!outputDir.rename(tempName, name)) {
            setError(pError,
                    QStringLiteral("Could not move the stem set into place"));
            written = false;
        }
    }

    if (!written) {
        QDir(tempPath).removeRecursively();
    }
    return written;
}

} // namespace mixxx::stems
