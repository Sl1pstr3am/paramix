#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "track/trackid.h"

/// A stem export that has been requested but not finished. The row is removed
/// when the job succeeds, fails or is canceled, so a leftover row means "resume
/// this after a restart".
struct StemQueueRecord {
    TrackId trackId;
    QString outputDirectory;
    QString setName;
    QString format;
    int aacBitrateKbps = 0;
};

class StemQueueDao final {
  public:
    explicit StemQueueDao(QSqlDatabase database = QSqlDatabase());

    /// Adds or replaces the job for this track. A track is only queued once.
    bool upsert(const StemQueueRecord& record);

    /// Queued jobs, oldest first.
    QList<StemQueueRecord> loadAll() const;

    bool remove(TrackId trackId);

    /// Removes queued jobs for tracks that are about to be purged.
    static bool deleteForTracks(
            const QSqlDatabase& database, const QList<TrackId>& trackIds);

    bool clear();

  private:
    const QSqlDatabase m_database;
};
