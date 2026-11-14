#include "library/dao/stemsetdao.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtDebug>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemSetDao");
} // namespace

StemSetDao::StemSetDao(QSqlDatabase database)
        : m_database(std::move(database)) {
}

bool StemSetDao::insert(const StemSetRecord& record, int* pId) {
    if (!record.trackId.isValid() || record.setName.isEmpty() ||
            record.directory.isEmpty() || record.format.isEmpty()) {
        kLogger.warning() << "Refusing to index an incomplete stem set record";
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "INSERT INTO stem_set (track_id, set_name, directory, format, model, "
            "sample_rate, frame_count, aac_bitrate_kbps) "
            "VALUES (:track_id, :set_name, :directory, :format, :model, "
            ":sample_rate, :frame_count, :aac_bitrate_kbps)"));
    query.bindValue(QStringLiteral(":track_id"), record.trackId.toVariant());
    query.bindValue(QStringLiteral(":set_name"), record.setName);
    query.bindValue(QStringLiteral(":directory"), record.directory);
    query.bindValue(QStringLiteral(":format"), record.format);
    query.bindValue(QStringLiteral(":model"), record.model);
    query.bindValue(QStringLiteral(":sample_rate"), record.sampleRate);
    query.bindValue(QStringLiteral(":frame_count"), static_cast<qint64>(record.frameCount));
    const QVariant aacBitrate = record.aacBitrateKbps > 0
            ? QVariant(record.aacBitrateKbps)
            : QVariant();
    query.bindValue(QStringLiteral(":aac_bitrate_kbps"), aacBitrate);
    if (!query.exec()) {
        kLogger.warning() << "Failed to index stem set" << record.setName << ":"
                          << query.lastError();
        return false;
    }
    if (pId != nullptr) {
        *pId = query.lastInsertId().toInt();
    }
    return true;
}

QList<StemSetRecord> StemSetDao::loadForTrack(TrackId trackId) const {
    QList<StemSetRecord> records;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "SELECT id, track_id, set_name, directory, format, model, "
            "sample_rate, frame_count, aac_bitrate_kbps, created_at "
            "FROM stem_set WHERE track_id = :track_id ORDER BY created_at"));
    query.bindValue(QStringLiteral(":track_id"), trackId.toVariant());
    if (!query.exec()) {
        kLogger.warning() << "Failed to load stem sets for track" << trackId
                          << ":" << query.lastError();
        return records;
    }
    while (query.next()) {
        StemSetRecord record;
        record.id = query.value(0).toInt();
        record.trackId = TrackId(query.value(1));
        record.setName = query.value(2).toString();
        record.directory = query.value(3).toString();
        record.format = query.value(4).toString();
        record.model = query.value(5).toString();
        record.sampleRate = query.value(6).toInt();
        record.frameCount = static_cast<SINT>(query.value(7).toLongLong());
        record.aacBitrateKbps = query.value(8).toInt();
        record.createdAt = query.value(9).toDateTime();
        records.append(record);
    }
    return records;
}

// static
bool StemSetDao::deleteForTracks(
        const QSqlDatabase& database, const QList<TrackId>& trackIds) {
    if (trackIds.isEmpty()) {
        return true;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "DELETE FROM stem_set WHERE track_id = :track_id"));
    for (const auto& trackId : trackIds) {
        query.bindValue(QStringLiteral(":track_id"), trackId.toVariant());
        if (!query.exec()) {
            kLogger.warning() << "Failed to delete stem sets for track" << trackId
                              << ":" << query.lastError();
            return false;
        }
    }
    return true;
}
