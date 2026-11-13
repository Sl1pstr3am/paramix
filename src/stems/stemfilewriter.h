#pragma once

#include <QString>

#include "stems/stemfileformat.h"
#include "util/types.h"

namespace mixxx::stems {

/// Describes one native stem set.
struct StemSetInfo {
    /// Directory name under the output location, sanitized by the writer.
    QString baseName;
    /// Human-readable model name recorded in the sidecar.
    QString modelName;
    int sampleRate = 44100;
    SINT frames = 0;
    StemFileFormat format = StemFileFormat::Flac;
    int aacBitrateKbps = kDefaultAacBitrateKbps;
};

/// Writes four stereo stem files (drums, bass, other, vocals) and a
/// `stem-set.json` sidecar into `<outputDirectory>/<sanitized baseName>/`.
///
/// The set is written into a hidden temporary directory and renamed into
/// place, so a reader never observes a partial set. The call fails if the
/// destination already exists. The writer is intended for a background worker.
///
/// @param pInterleavedStems `frames * 8` samples in drums/bass/other/vocals
///        order, matching the engine and the cache
/// @param pError receives a user-readable reason on failure
bool writeStemSet(const QString& outputDirectory,
        const StemSetInfo& info,
        const CSAMPLE* pInterleavedStems,
        QString* pError);

/// Replaces path separators and characters that are invalid on Windows, trims
/// trailing dots/spaces, and falls back to "stems" when nothing is left.
QString sanitizeStemSetName(const QString& name);

} // namespace mixxx::stems
