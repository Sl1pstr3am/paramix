#pragma once

#include <QMap>
#include <QMetaType>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>

#include "preferences/usersettings.h"
#include "stems/stemfileformat.h"
#include "track/track_decl.h"
#include "track/trackid.h"
#include "util/db/dbconnectionpool.h"
#include "util/singleton.h"

class StemSetDao;
class TrackCollectionManager;

namespace mixxx::stems {

class DemucsOnnxSeparator;

/// One queued export. The set is written to `outputDirectory/<setName>`.
struct StemExportRequest {
    TrackPointer track;
    /// Parent directory of the set. Empty selects a default below the
    /// settings directory.
    QString outputDirectory;
    /// Directory name of the set. Empty derives a name from the track title.
    QString setName;
};

/// Low-priority background worker that separates tracks and writes native
/// stem sets. Owns the model session and runs the whole-track loop, so it is
/// never called from the real-time audio thread.
class StemService : public QThread, public Singleton<StemService> {
    Q_OBJECT

  public:
    enum class Status {
        NotSeparated,
        Queued,
        Separating,
        Ready,
        MissingStemFile,
        Failed,
    };
    Q_ENUM(Status)

    static StemService* createInstance(
            UserSettingsPointer pConfig, mixxx::DbConnectionPoolPtr pDbConnectionPool);

    ~StemService() override;

    /// Appends a job to the queue and starts the worker if needed. Thread-safe.
    bool queueExport(const StemExportRequest& request);

    /// Re-queues jobs that were still pending when the app exited. Must be
    /// called on the main thread, because it resolves tracks by id.
    void restoreQueuedJobs(TrackCollectionManager* pTrackCollectionManager);

    /// Stops dequeuing new jobs. A running job pauses at its next block.
    void pause();
    void resume();
    bool isPaused() const;

    /// Cancels the running job and clears the queue. The current output is
    /// discarded. Thread-safe.
    void cancelAll();

    /// Last known status of a track. `NotSeparated` when the service has never
    /// seen it.
    Status statusForTrack(TrackId trackId) const;

    /// Message attached to the last status change, if any.
    QString lastMessageForTrack(TrackId trackId) const;

  signals:
    /// `progress` is a percentage in [0, 100].
    void trackStatusChanged(TrackId trackId,
            mixxx::stems::StemService::Status status,
            int progress,
            const QString& message);

  protected:
    void run() override;

  private:
    struct Job {
        StemExportRequest request;
        TrackId trackId;
        // Resolved on the caller's thread so the worker never touches ConfigObject.
        QString outputDirectory;
        QString setName;
        QString modelPath;
        StemExportSettings settings;
    };

    friend class Singleton<StemService>;

    StemService(UserSettingsPointer pConfig, mixxx::DbConnectionPoolPtr pDbConnectionPool);

    void processJob(Job& job,
            std::unique_ptr<DemucsOnnxSeparator>* ppSeparator,
            QString* pLoadedModelPath,
            StemSetDao& dao);
    void setStatus(
            TrackId trackId, Status status, int progress, const QString& message = QString());
    QString defaultOutputDirectory() const;
    QString defaultSetName(const TrackPointer& pTrack) const;
    QString modelPath() const;

    const UserSettingsPointer m_pConfig;
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;

    mutable QMutex m_mutex;
    QWaitCondition m_waitCondition;
    QQueue<Job> m_jobs;
    QMap<TrackId, Status> m_statuses;
    QMap<TrackId, QString> m_messages;
    bool m_stopRequested = false;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_cancelRequested{false};
};

} // namespace mixxx::stems

Q_DECLARE_METATYPE(mixxx::stems::StemService::Status)
