#include "library/dao/stemqueuedao.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtDebug>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemQueueDao");
} // namespace

StemQueueDao::StemQueueDao(QSqlDatabase database)
        : m_database(std::move(database)) {
}

bool StemQueueDao::upsert(const StemQueueRecord& record) {
    if (!record.trackId.isValid() || record.outputDirectory.isEmpty() ||
            record.setName.isEmpty()) {
        kLogger.warning() << "Refusing to persist an incomplete stem job";
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO stem_queue "
            "(track_id, output_directory, set_name, format, aac_bitrate_kbps) "
            "VALUES (:track_id, :output_directory, :set_name, :format, "
            ":aac_bitrate_kbps)"));
    query.bindValue(QStringLiteral(":track_id"), record.trackId.toVariant());
    query.bindValue(QStringLiteral(":output_directory"), record.outputDirectory);
    query.bindValue(QStringLiteral(":set_name"), record.setName);
    query.bindValue(QStringLiteral(":format"), record.format);
    query.bindValue(QStringLiteral(":aac_bitrate_kbps"),
            record.aacBitrateKbps > 0 ? QVariant(record.aacBitrateKbps) : QVariant());
    if (!query.exec()) {
        kLogger.warning() << "Failed to persist stem job for track"
                          << record.trackId << ":" << query.lastError();
        return false;
    }
    return true;
}

QList<StemQueueRecord> StemQueueDao::loadAll() const {
    QList<StemQueueRecord> records;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
                "SELECT track_id, output_directory, set_name, format, "
                "aac_bitrate_kbps FROM stem_queue ORDER BY created_at"))) {
        kLogger.warning() << "Failed to load the stem queue:" << query.lastError();
        return records;
    }
    while (query.next()) {
        StemQueueRecord record;
        record.trackId = TrackId(query.value(0));
        record.outputDirectory = query.value(1).toString();
        record.setName = query.value(2).toString();
        record.format = query.value(3).toString();
        record.aacBitrateKbps = query.value(4).toInt();
        records.append(record);
    }
    return records;
}

bool StemQueueDao::remove(TrackId trackId) {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "DELETE FROM stem_queue WHERE track_id = :track_id"));
    query.bindValue(QStringLiteral(":track_id"), trackId.toVariant());
    if (!query.exec()) {
        kLogger.warning() << "Failed to remove stem job for track" << trackId
                          << ":" << query.lastError();
        return false;
    }
    return true;
}

// static
bool StemQueueDao::deleteForTracks(
        const QSqlDatabase& database, const QList<TrackId>& trackIds) {
    if (trackIds.isEmpty()) {
        return true;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "DELETE FROM stem_queue WHERE track_id = :track_id"));
    for (const auto& trackId : trackIds) {
        query.bindValue(QStringLiteral(":track_id"), trackId.toVariant());
        if (!query.exec()) {
            kLogger.warning() << "Failed to delete stem job for track" << trackId
                              << ":" << query.lastError();
            return false;
        }
    }
    return true;
}

bool StemQueueDao::clear() {
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DELETE FROM stem_queue"))) {
        kLogger.warning() << "Failed to clear the stem queue:" << query.lastError();
        return false;
    }
    return true;
}
