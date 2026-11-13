#include "stems/stemfileformat.h"

#include <algorithm>

#include "preferences/configobject.h"

namespace mixxx::stems {
namespace {

constexpr char kGroup[] = "[Stems]";
constexpr char kFileFormatKey[] = "FileFormat";
constexpr char kAacBitrateKey[] = "AacBitrate";

int clampedAacBitrate(int bitrateKbps) {
    return std::clamp(bitrateKbps, kMinAacBitrateKbps, kMaxAacBitrateKbps);
}

} // namespace

StemFileFormat parseStemFileFormat(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("alac")) {
        return StemFileFormat::Alac;
    }
    if (normalized == QStringLiteral("aac")) {
        return StemFileFormat::Aac;
    }
    return StemFileFormat::Flac;
}

QString stemFileFormatName(StemFileFormat format) {
    switch (format) {
    case StemFileFormat::Alac:
        return QStringLiteral("alac");
    case StemFileFormat::Aac:
        return QStringLiteral("aac");
    case StemFileFormat::Flac:
        break;
    }
    return QStringLiteral("flac");
}

QString stemFileExtension(StemFileFormat format) {
    switch (format) {
    case StemFileFormat::Alac:
        // libsndfile writes ALAC in a CAF container.
        return QStringLiteral("caf");
    case StemFileFormat::Aac:
        // The FDK encoder writes an ADTS stream.
        return QStringLiteral("aac");
    case StemFileFormat::Flac:
        break;
    }
    return QStringLiteral("flac");
}

bool isLosslessStemFormat(StemFileFormat format) {
    return format != StemFileFormat::Aac;
}

StemExportSettings readStemExportSettings(UserSettingsPointer pConfig) {
    StemExportSettings settings;
    if (!pConfig) {
        return settings;
    }
    settings.format = parseStemFileFormat(pConfig->getValue(
            ConfigKey(kGroup, kFileFormatKey), QStringLiteral("flac")));
    settings.aacBitrateKbps = clampedAacBitrate(pConfig->getValue(
            ConfigKey(kGroup, kAacBitrateKey), kDefaultAacBitrateKbps));
    return settings;
}

void writeStemExportSettings(
        UserSettingsPointer pConfig, const StemExportSettings& settings) {
    if (!pConfig) {
        return;
    }
    pConfig->setValue(
            ConfigKey(kGroup, kFileFormatKey), stemFileFormatName(settings.format));
    pConfig->setValue(ConfigKey(kGroup, kAacBitrateKey),
            clampedAacBitrate(settings.aacBitrateKbps));
}

} // namespace mixxx::stems
