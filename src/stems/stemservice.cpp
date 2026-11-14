#include "stems/stemservice.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSqlDatabase>
#include <QStorageInfo>
#include <algorithm>
#include <utility>
#include <vector>

#include "library/dao/stemqueuedao.h"
#include "library/dao/stemsetdao.h"
#include "library/trackcollectionmanager.h"
#include "moc_stemservice.cpp"
#include "preferences/configobject.h"
#include "sources/audiosource.h"
#include "sources/soundsourceproxy.h"
#include "stems/demucslimiter.h"
#include "stems/stemcache.h"
#include "stems/stemfilewriter.h"
#include "stems/stemseparatorfactory.h"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"
#include "util/samplebuffer.h"

namespace mixxx::stems {
namespace {

const mixxx::Logger kLogger("StemService");

constexpr char kGroup[] = "[Stems]";
constexpr char kModelPathKey[] = "ModelPath";
constexpr char kOutputDirectoryKey[] = "OutputDirectory";

/// Leave at least this much free space after a job, so a full disk does not
/// take the library database with it.
constexpr qint64 kMinFreeBytes = 1024LL * 1024 * 1024;
constexpr SINT kReadBlockFrames = 8192;

QString estimateTooLarge(qint64 estimatedBytes, qint64 availableBytes) {
    return QStringLiteral(
            "Not enough free space for the stem set: about %1 MB are needed "
            "and only %2 MB are available.")
            .arg((estimatedBytes + 999999) / 1000000)
            .arg(availableBytes / 1000000);
}

} // namespace

StemService::StemService(
        UserSettingsPointer pConfig, mixxx::DbConnectionPoolPtr pDbConnectionPool)
        : m_pConfig(std::move(pConfig)),
          m_pDbConnectionPool(std::move(pDbConnectionPool)) {
    qRegisterMetaType<StemService::Status>("mixxx::stems::StemService::Status");
    setObjectName(QStringLiteral("StemService"));
}

StemService::~StemService() {
    {
        QMutexLocker locker(&m_mutex);
        m_stopRequested = true;
        m_jobs.clear();
        // A running job polls this and unwinds at the next output block.
        m_cancelRequested = true;
        m_waitCondition.wakeAll();
    }
    wait();
}

// static
StemService* StemService::createInstance(
        UserSettingsPointer pConfig, mixxx::DbConnectionPoolPtr pDbConnectionPool) {
    return Singleton<StemService>::createInstance(
            std::move(pConfig), std::move(pDbConnectionPool));
}

bool StemService::queueExport(const StemExportRequest& request) {
    if (!request.track || !request.track->getId().isValid()) {
        return false;
    }
    const TrackId trackId = request.track->getId();
    {
        QMutexLocker locker(&m_mutex);
        for (const auto& job : std::as_const(m_jobs)) {
            if (job.trackId == trackId) {
                return false;
            }
        }
    }
    m_cancelRequested = false;
    const StemExportSettings settings = readStemExportSettings(m_pConfig);
    const QString outputDirectory = request.outputDirectory.trimmed().isEmpty()
            ? defaultOutputDirectory()
            : request.outputDirectory;
    const QString setName = request.setName.trimmed().isEmpty()
            ? uniqueStemSetName(outputDirectory, defaultSetName(request.track))
            : sanitizeStemSetName(request.setName);
    const QString modelPathValue = modelPath();
    StemQueueRecord queued;
    queued.trackId = trackId;
    queued.outputDirectory = outputDirectory;
    queued.setName = setName;
    queued.format = stemFileFormatName(settings.format);
    queued.aacBitrateKbps = settings.format == StemFileFormat::Aac
            ? settings.aacBitrateKbps
            : 0;
    // Live outside the lock: it may open a database connection on this thread.
    const QSqlDatabase database = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (database.isOpen()) {
        StemQueueDao(database).upsert(queued);
    }
    {
        QMutexLocker locker(&m_mutex);
        for (const auto& job : std::as_const(m_jobs)) {
            if (job.trackId == trackId) {
                return false;
            }
        }
        m_jobs.enqueue(Job{request,
                trackId,
                outputDirectory,
                setName,
                modelPathValue,
                settings});
    }
    setStatus(trackId, Status::Queued, 0);
    m_waitCondition.wakeAll();
    if (!isRunning()) {
        // Low priority keeps the GUI, waveform and audio callback responsive.
        start(QThread::LowPriority);
    }
    return true;
}

void StemService::pause() {
    m_paused = true;
}

void StemService::resume() {
    m_paused = false;
    m_waitCondition.wakeAll();
}

bool StemService::isPaused() const {
    return m_paused.load();
}

void StemService::cancelAll() {
    m_cancelRequested = true;
    QQueue<Job> canceled;
    {
        QMutexLocker locker(&m_mutex);
        canceled.swap(m_jobs);
    }
    while (!canceled.isEmpty()) {
        setStatus(canceled.dequeue().trackId, Status::NotSeparated, 0);
    }
    const QSqlDatabase database = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (database.isOpen()) {
        StemQueueDao(database).clear();
    }
    m_waitCondition.wakeAll();
}

void StemService::restoreQueuedJobs(TrackCollectionManager* pTrackCollectionManager) {
    if (pTrackCollectionManager == nullptr) {
        return;
    }
    const QSqlDatabase database = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (!database.isOpen()) {
        return;
    }
    StemQueueDao queueDao(database);
    const QList<StemQueueRecord> records = queueDao.loadAll();
    for (const auto& record : records) {
        TrackPointer pTrack = pTrackCollectionManager->getTrackById(record.trackId);
        if (!pTrack) {
            queueDao.remove(record.trackId);
            continue;
        }
        StemExportRequest request;
        request.track = pTrack;
        request.outputDirectory = record.outputDirectory;
        request.setName = record.setName;
        queueExport(request);
    }
}

void StemService::removeStems(TrackId trackId) {
    const QSqlDatabase database = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (!database.isOpen() || !trackId.isValid()) {
        return;
    }
    StemSetDao dao(database);
    const QList<StemSetRecord> records = dao.loadForTrack(trackId);
    for (const auto& record : records) {
        const QString setPath = QDir(record.directory).filePath(record.setName);
        // Never delete a directory that is not recognisably a stem set.
        if (!QFileInfo(QDir(setPath).filePath(QStringLiteral("stem-set.json"))).exists()) {
            kLogger.warning() << "Skipping non-stem directory" << setPath;
            continue;
        }
        if (!QDir(setPath).removeRecursively()) {
            kLogger.warning() << "Failed to remove stem set" << setPath;
        }
    }
    StemSetDao::deleteForTracks(database, QList<TrackId>{trackId});
    StemCache::removeCache(trackId);
    setStatus(trackId, Status::NotSeparated, 0, QStringLiteral("Stems removed."));
}

StemService::Status StemService::statusForTrack(TrackId trackId) const {
    {
        QMutexLocker locker(&m_mutex);
        const auto it = m_statuses.constFind(trackId);
        if (it != m_statuses.constEnd() && it.value() != Status::Ready) {
            return it.value();
        }
    }
    // No live status: an exported set may still be on disk from an earlier
    // session. The row is a rebuildable index, so this is a cheap lookup.
    const QSqlDatabase database = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (!database.isOpen()) {
        return Status::NotSeparated;
    }
    const QList<StemSetRecord> records = StemSetDao(database).loadForTrack(trackId);
    if (records.isEmpty()) {
        return Status::NotSeparated;
    }
    for (const auto& record : records) {
        if (QFileInfo(QDir(record.directory).filePath(record.setName)).exists()) {
            return Status::Ready;
        }
    }
    return Status::MissingStemFile;
}

QString StemService::lastMessageForTrack(TrackId trackId) const {
    QMutexLocker locker(&m_mutex);
    return m_messages.value(trackId);
}

void StemService::setStatus(
        TrackId trackId, Status status, int progress, const QString& message) {
    {
        QMutexLocker locker(&m_mutex);
        if (status == Status::NotSeparated) {
            m_statuses.remove(trackId);
        } else {
            m_statuses.insert(trackId, status);
        }
        m_messages.insert(trackId, message);
    }
    emit trackStatusChanged(trackId, status, progress, message);
}

QString StemService::modelPath() const {
    return m_pConfig->getValue(ConfigKey(kGroup, kModelPathKey), QString());
}

QString StemService::defaultOutputDirectory() const {
    return m_pConfig->getValue(ConfigKey(kGroup, kOutputDirectoryKey),
            QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("Stems")));
}

QString StemService::defaultSetName(const TrackPointer& pTrack) const {
    const QString title = pTrack->getTitle();
    return title.trimmed().isEmpty() ? QStringLiteral("stems") : title;
}

void StemService::run() {
    // A worker-owned connection, because TrackCollection is main-thread only.
    mixxx::DbConnectionPooler connectionPooler(m_pDbConnectionPool);
    const QSqlDatabase daoConnection = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    StemSetDao dao(daoConnection);
    StemQueueDao queueDao(daoConnection);

    std::unique_ptr<DemucsOnnxSeparator> pSeparator;
    QString loadedModelPath;

    while (true) {
        Job job;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_stopRequested && (m_jobs.isEmpty() || m_paused.load())) {
                m_waitCondition.wait(&m_mutex, 200);
            }
            if (m_stopRequested) {
                return;
            }
            job = m_jobs.dequeue();
        }
        processJob(job, &pSeparator, &loadedModelPath, dao);
        // The job is finished either way; only a crash leaves the row for the
        // next start to resume.
        queueDao.remove(job.trackId);
    }
}

void StemService::processJob(Job& job,
        std::unique_ptr<DemucsOnnxSeparator>* ppSeparator,
        QString* pLoadedModelPath,
        StemSetDao& dao) {
    const TrackId trackId = job.trackId;
    const TrackPointer pTrack = job.request.track;
    if (!pTrack) {
        setStatus(trackId, Status::Failed, 0, QStringLiteral("The track is gone."));
        return;
    }

    const StemExportSettings& settings = job.settings;
    const QString& outputDirectory = job.outputDirectory;
    const QString setName = job.setName;
    const QString setPath = QDir(outputDirectory).filePath(setName);

    if (QDir(setPath).exists()) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral("A stem set named %1 already exists.").arg(setName));
        return;
    }

    // Free-space preflight. The estimate is deliberately generous: FLAC and
    // ALAC are 24-bit, AAC scales with the requested bitrate.
    constexpr int kExpectedModelSampleRate = 44100;
    qint64 estimatedBytes = 0;
    {
        const double frames = pTrack->getDuration() * kExpectedModelSampleRate;
        if (settings.format == StemFileFormat::Aac) {
            estimatedBytes = static_cast<qint64>(4.0 * frames * settings.aacBitrateKbps / 8.0);
        } else {
            estimatedBytes = static_cast<qint64>(frames * kStemChannelCount * 3.0);
        }
        estimatedBytes = static_cast<qint64>(estimatedBytes * 1.05) + 4096;
    }
    const QString existingAncestor = QFileInfo(outputDirectory).absolutePath();
    const QStorageInfo storage(existingAncestor);
    if (storage.isValid() && storage.isReady() &&
            storage.bytesAvailable() - estimatedBytes < kMinFreeBytes) {
        setStatus(trackId,
                Status::Failed,
                0,
                estimateTooLarge(estimatedBytes, storage.bytesAvailable()));
        return;
    }

    const QString path = job.modelPath;
    if (path.isEmpty()) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral("No stem separation model is selected in Settings -> Stems."));
        return;
    }
    if (!*ppSeparator || *pLoadedModelPath != path) {
        ppSeparator->reset();
        std::string error;
        DemucsOnnxConfig config;
        config.modelPath = path.toStdString();
        *ppSeparator = createDemucsOnnxSeparator(config, &error);
        if (!*ppSeparator) {
            setStatus(trackId,
                    Status::Failed,
                    0,
                    QStringLiteral("Could not load the stem separation model: %1")
                            .arg(QString::fromStdString(error)));
            return;
        }
        *pLoadedModelPath = path;
    }

    const int sampleRate = (*ppSeparator)->sampleRate();

    setStatus(trackId, Status::Separating, 0);
    SoundSourceProxy proxy(pTrack);
    mixxx::AudioSource::OpenParams openParams;
    openParams.setChannelCount(mixxx::audio::ChannelCount::stereo());
    openParams.setSampleRate(mixxx::audio::SampleRate(sampleRate));
    const mixxx::AudioSourcePointer pSource = proxy.openAudioSource(openParams);
    if (!pSource) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral("Could not open the track for reading."));
        return;
    }
    if (pSource->getSignalInfo().getSampleRate() != mixxx::audio::SampleRate(sampleRate)) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral(
                        "Stem separation needs %1 Hz audio; resampling from %2 Hz is "
                        "not supported yet.")
                        .arg(sampleRate)
                        .arg(pSource->getSignalInfo().getSampleRate().value()));
        return;
    }

    const SINT channels = pSource->getSignalInfo().getChannelCount().value();
    if (channels != 1 && channels != 2) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral("Stem separation needs mono or stereo audio."));
        return;
    }

    const SINT frames = pSource->frameLength();
    if (frames <= 0) {
        setStatus(trackId,
                Status::Failed,
                0,
                QStringLiteral("The track contains no audio."));
        return;
    }
    kLogger.info() << "Separating track" << trackId << "at"
                   << pSource->getSignalInfo().getSampleRate().value() << "Hz,"
                   << frames << "frames; model expects" << sampleRate << "Hz";

    std::vector<float> planar(
            static_cast<std::size_t>(2) * static_cast<std::size_t>(frames), 0.0f);
    {
        mixxx::SampleBuffer chunk(static_cast<SINT>(
                static_cast<std::size_t>(kReadBlockFrames) * channels));
        for (SINT offset = 0; offset < frames;) {
            if (m_cancelRequested.load()) {
                setStatus(trackId, Status::NotSeparated, 0, QStringLiteral("Canceled."));
                return;
            }
            const SINT count = std::min<SINT>(kReadBlockFrames, frames - offset);
            const auto readable = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                    mixxx::IndexRange::forward(offset, count),
                    mixxx::SampleBuffer::WritableSlice(chunk)));
            const SINT read = readable.frameLength();
            if (read <= 0) {
                setStatus(trackId,
                        Status::Failed,
                        0,
                        QStringLiteral("Could not read the track audio."));
                return;
            }
            for (SINT frame = 0; frame < read; ++frame) {
                const std::size_t sourceIndex =
                        static_cast<std::size_t>(frame) * channels;
                const std::size_t destinationIndex =
                        static_cast<std::size_t>(offset + frame);
                // The separator takes planar [channels][frames], not interleaved
                // stereo. Interleaving here time-squeezes the whole track.
                planar[destinationIndex] = readable.readableData()[sourceIndex];
                planar[frames + destinationIndex] = channels == 1
                        ? readable.readableData()[sourceIndex]
                        : readable.readableData()[sourceIndex + 1];
            }
            offset += read;
        }
    }
    pSource->close();

    std::vector<CSAMPLE> separated(
            static_cast<std::size_t>(frames) * kStemChannelCount, 0.0f);
    bool outputValid = true;
    SINT outputEnd = 0;
    const bool completed = (*ppSeparator)->separate(planar.data(), frames, [&](SINT offset, const CSAMPLE* pBuffer, SINT count) {
                if (offset < 0 || count <= 0 || offset + count > frames) {
                    outputValid = false;
                    return false;
                }
                outputEnd = std::max(outputEnd, offset + count);
                std::copy(pBuffer,
                        pBuffer + static_cast<std::size_t>(count) * kStemChannelCount,
                        separated.begin() + static_cast<std::size_t>(offset) *
                                        kStemChannelCount);
                return true; }, [this, trackId](double progress) { setStatus(trackId,
                                                                                                                                                                                Status::Separating,
                                                                                                                                                                                static_cast<int>(std::clamp(progress, 0.0, 1.0) * 90.0)); }, [this]() {
                if (m_cancelRequested.load()) {
                    return true;
                }
                while (m_paused.load() && !m_cancelRequested.load()) {
                    QThread::msleep(50);
                }
                return m_cancelRequested.load(); });
    if (!completed || !outputValid) {
        if (m_cancelRequested.load()) {
            setStatus(trackId, Status::NotSeparated, 0, QStringLiteral("Canceled."));
        } else {
            setStatus(trackId,
                    Status::Failed,
                    0,
                    QStringLiteral("Stem separation failed: %1")
                            .arg(QString::fromStdString((*ppSeparator)->lastError())));
        }
        return;
    }
    kLogger.info() << "Separator covered frames" << outputEnd << "of" << frames;

    // One scalar gain over all stems and their sum keeps the mix reconstructible
    // and inside [-1, 1]; the writer then encodes without clipping.
    const float gain = demucsLimiterGain(separated.data(), frames);
    if (gain < 1.0f) {
        demucsApplyGain(separated.data(), separated.size(), gain);
    }

    setStatus(trackId, Status::Separating, 95);
    StemSetInfo info;
    info.baseName = setName;
    info.modelName = QFileInfo(path).completeBaseName();
    info.sampleRate = sampleRate;
    info.frames = frames;
    info.format = settings.format;
    info.aacBitrateKbps = settings.aacBitrateKbps;
    QString error;
    if (!writeStemSet(outputDirectory, info, separated.data(), &error)) {
        setStatus(trackId, Status::Failed, 0, error);
        return;
    }

    // The export is the user-facing artifact; the 8-channel cache is what the
    // decks and the waveform renderer consume.
    QString cacheError;
    if (!StemCache::writeCache(trackId,
                separated.data(),
                frames,
                sampleRate,
                &cacheError)) {
        kLogger.warning() << "Stem export written but cache failed for track"
                          << trackId << cacheError;
    } else {
        emit stemsReady(trackId);
    }

    StemSetRecord record;
    record.trackId = trackId;
    record.setName = setName;
    record.directory = QDir(outputDirectory).absolutePath();
    record.format = stemFileFormatName(settings.format);
    record.model = info.modelName;
    record.sampleRate = sampleRate;
    record.frameCount = frames;
    record.aacBitrateKbps = settings.format == StemFileFormat::Aac
            ? settings.aacBitrateKbps
            : 0;
    if (!dao.insert(record)) {
        kLogger.warning() << "Stem set written but not indexed for track" << trackId;
    }

    setStatus(trackId, Status::Ready, 100);
}

} // namespace mixxx::stems
