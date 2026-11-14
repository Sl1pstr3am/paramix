#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "track/trackid.h"
#include "util/types.h"

/// One exported native stem set, as recorded in the `stem_set` table.
/// The row is an index only: `stem-set.json` next to the audio remains
/// authoritative and the table can be rebuilt from it.
struct StemSetRecord {
    int id = -1;
    TrackId trackId;
    /// Sanitized directory name below `directory`.
    QString setName;
    /// Absolute parent directory of the set.
    QString directory;
    /// `flac`, `alac` or `aac`.
    QString format;
    /// Human-readable model name recorded in the sidecar.
    QString model;
    int sampleRate = 0;
    SINT frameCount = 0;
    /// Zero unless the format is AAC.
    int aacBitrateKbps = 0;
    QDateTime createdAt;
};

/// Database index for exported stem sets. Intended for a background worker,
/// so it holds its own thread-local connection instead of using TrackCollection.
class StemSetDao final {
  public:
    explicit StemSetDao(QSqlDatabase database = QSqlDatabase());

    /// Inserts the record and fills `pId` on success. Returns false when the
    /// same track, directory and set name already exists.
    bool insert(const StemSetRecord& record, int* pId = nullptr);

    /// Records for one track, oldest first.
    QList<StemSetRecord> loadForTrack(TrackId trackId) const;

    /// Removes every record for the given tracks. Used when tracks are purged.
    static bool deleteForTracks(
            const QSqlDatabase& database, const QList<TrackId>& trackIds);

    const QSqlDatabase& database() const {
        return m_database;
    }

  private:
    const QSqlDatabase m_database;
};
