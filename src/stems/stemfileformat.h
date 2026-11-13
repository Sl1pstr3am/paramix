#pragma once

#include <QString>

#include "preferences/usersettings.h"

namespace mixxx::stems {

/// Output format for a native stem set. This replaces the Traktor
/// `.stem.mp4` export: Paramix writes four stereo stem files instead of a
/// five-stream MP4 with an NI manifest atom.
enum class StemFileFormat {
    /// FLAC, lossless, 24-bit.
    Flac,
    /// Apple Lossless in a CAF container, lossless, 24-bit.
    Alac,
    /// AAC-LC in an ADTS stream, lossy.
    Aac,
};

inline constexpr int kDefaultAacBitrateKbps = 256;
inline constexpr int kMinAacBitrateKbps = 32;
inline constexpr int kMaxAacBitrateKbps = 512;

/// The quality/time switch exposed in Settings -> Stems.
struct StemExportSettings {
    StemFileFormat format = StemFileFormat::Flac;
    int aacBitrateKbps = kDefaultAacBitrateKbps;
};

/// Parses `flac`, `alac` or `aac` (case-insensitive); unknown values fall back
/// to FLAC rather than failing a background job.
StemFileFormat parseStemFileFormat(const QString& value);

QString stemFileFormatName(StemFileFormat format);
QString stemFileExtension(StemFileFormat format);
bool isLosslessStemFormat(StemFileFormat format);

/// Reads `[Stems],FileFormat` and `[Stems],AacBitrate`.
StemExportSettings readStemExportSettings(UserSettingsPointer pConfig);

/// Writes the same keys, clamping the AAC bitrate to the supported range.
void writeStemExportSettings(
        UserSettingsPointer pConfig, const StemExportSettings& settings);

} // namespace mixxx::stems
