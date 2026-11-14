#include <gtest/gtest.h>

#include <QSqlQuery>
#include <QVariant>

#include "library/dao/stemqueuedao.h"
#include "library/dao/stemsetdao.h"
#include "test/librarytest.h"

class StemSetDaoTest : public LibraryTest {};

TEST_F(StemSetDaoTest, Revision41CreatesStemSetTable) {
    QSqlQuery query(dbConnection());
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT id, track_id, set_name, directory, format, model, "
            "sample_rate, frame_count, aac_bitrate_kbps, created_at "
            "FROM stem_set LIMIT 1")));
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT track_id, output_directory, set_name, format, "
            "aac_bitrate_kbps, created_at FROM stem_queue LIMIT 1")));
}

TEST_F(StemSetDaoTest, InsertLoadAndDelete) {
    QSqlDatabase db = dbConnection();
    QSqlQuery query(db);
    ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO library (id, artist, title) "
                           "VALUES (1, 'Artist', 'Title')")));

    StemSetDao dao(db);
    StemSetRecord record;
    record.trackId = TrackId(QVariant(1));
    record.setName = QStringLiteral("Title");
    record.directory = QStringLiteral("C:/Stems");
    record.format = QStringLiteral("flac");
    record.model = QStringLiteral("htdemucs");
    record.sampleRate = 44100;
    record.frameCount = 123456;
    int id = -1;
    ASSERT_TRUE(dao.insert(record, &id));
    EXPECT_GT(id, 0);

    const QList<StemSetRecord> records = dao.loadForTrack(TrackId(QVariant(1)));
    ASSERT_EQ(1, records.size());
    EXPECT_EQ(QStringLiteral("Title"), records.first().setName);
    EXPECT_EQ(QStringLiteral("C:/Stems"), records.first().directory);
    EXPECT_EQ(QStringLiteral("flac"), records.first().format);
    EXPECT_EQ(44100, records.first().sampleRate);
    EXPECT_EQ(123456, records.first().frameCount);
    EXPECT_EQ(0, records.first().aacBitrateKbps);

    // A set name is unique per track and directory.
    EXPECT_FALSE(dao.insert(record));

    ASSERT_TRUE(StemSetDao::deleteForTracks(db, QList<TrackId>{TrackId(QVariant(1))}));
    EXPECT_TRUE(dao.loadForTrack(TrackId(QVariant(1))).isEmpty());
}

TEST_F(StemSetDaoTest, RecordsAacBitrate) {
    QSqlDatabase db = dbConnection();
    QSqlQuery query(db);
    ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO library (id, artist, title) "
                           "VALUES (2, 'Artist', 'Title')")));

    StemSetDao dao(db);
    StemSetRecord record;
    record.trackId = TrackId(QVariant(2));
    record.setName = QStringLiteral("Lossy");
    record.directory = QStringLiteral("C:/Stems");
    record.format = QStringLiteral("aac");
    record.model = QStringLiteral("htdemucs");
    record.sampleRate = 44100;
    record.frameCount = 4410;
    record.aacBitrateKbps = 192;
    ASSERT_TRUE(dao.insert(record));
    ASSERT_EQ(1, dao.loadForTrack(TrackId(QVariant(2))).size());
    EXPECT_EQ(192, dao.loadForTrack(TrackId(QVariant(2))).first().aacBitrateKbps);
}

TEST_F(StemSetDaoTest, StemQueueRoundTrip) {
    QSqlDatabase db = dbConnection();
    QSqlQuery query(db);
    ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO library (id, artist, title) "
                           "VALUES (3, 'Artist', 'Title')")));

    StemQueueDao dao(db);
    StemQueueRecord record;
    record.trackId = TrackId(QVariant(3));
    record.outputDirectory = QStringLiteral("C:/Stems");
    record.setName = QStringLiteral("Title");
    record.format = QStringLiteral("aac");
    record.aacBitrateKbps = 256;
    ASSERT_TRUE(dao.upsert(record));
    ASSERT_EQ(1, dao.loadAll().size());
    EXPECT_EQ(QStringLiteral("Title"), dao.loadAll().first().setName);
    EXPECT_EQ(256, dao.loadAll().first().aacBitrateKbps);

    // A second request for the same track replaces the row.
    record.setName = QStringLiteral("Title (2)");
    ASSERT_TRUE(dao.upsert(record));
    ASSERT_EQ(1, dao.loadAll().size());
    EXPECT_EQ(QStringLiteral("Title (2)"), dao.loadAll().first().setName);

    ASSERT_TRUE(dao.remove(TrackId(QVariant(3))));
    EXPECT_TRUE(dao.loadAll().isEmpty());
}
