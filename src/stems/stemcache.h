#pragma once

#include <QList>
#include <QString>

#include "preferences/usersettings.h"
#include "track/steminfo.h"
#include "track/trackid.h"
#include "util/types.h"

namespace mixxx::stems {

/// The internal 8-channel playback cache from ADR-002. It is separate from the
/// user-facing four-file export: one FLAC holds the four stereo stems
/// interleaved in drums/bass/other/vocals order, at the source rate and frame
/// count, so the existing engine and waveform paths can consume it.
class StemCache final {
  public:
    /// Reads `[Stems],CacheDirectory`; must be called once at startup before
    /// any source is opened. The default is `<settings>/StemCache`.
    static void initialize(const UserSettingsPointer& pConfig);

    static QString rootDirectory();
    static QString cacheFilePath(TrackId trackId);
    static bool hasCache(TrackId trackId);

    /// Writes/replaces the cache for a track. Uses a temporary file and a
    /// rename so a reader never sees a partial cache.
    static bool writeCache(TrackId trackId,
            const CSAMPLE* pInterleavedStems,
            SINT frames,
            int sampleRate,
            QString* pError);

    /// Removes the cache directory for a track, if it is recognisably ours.
    static bool removeCache(TrackId trackId);

    /// Labels and colors for the four cached stems, matching the export.
    static QList<StemInfo> stemInfo();
};

} // namespace mixxx::stems
