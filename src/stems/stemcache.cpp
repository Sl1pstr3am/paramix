#include "stems/stemcache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#ifdef _WIN32
#include <windows.h>
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#endif
#include <sndfile.h>

#include <algorithm>

#include "preferences/configobject.h"
#include "stems/stemcache.h"
#include "stems/stemseparatorbackend.h"
#include "util/logger.h"

namespace mixxx::stems {
namespace {

const mixxx::Logger kLogger("StemCache");

constexpr char kGroup[] = "[Stems]";
constexpr char kCacheDirectoryKey[] = "CacheDirectory";
constexpr int kBlockFrames = 8192;
constexpr int kChannelCount = kStemChannelCount;

QString& rootDirectoryRef() {
    static QString s_rootDirectory;
    return s_rootDirectory;
}

SNDFILE* openSndFileForWrite(const QString& path, SF_INFO* pInfo) {
#ifdef _WIN32
    return sf_wchar_open(path.toStdWString().c_str(), SFM_WRITE, pInfo);
#else
    return sf_open(path.toUtf8().constData(), SFM_WRITE, pInfo);
#endif
}

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

} // namespace

void StemCache::initialize(const UserSettingsPointer& pConfig) {
    if (!pConfig) {
        return;
    }
    rootDirectoryRef() = pConfig->getValue(ConfigKey(kGroup, kCacheDirectoryKey),
            QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("StemCache")));
}

QString StemCache::rootDirectory() {
    return rootDirectoryRef();
}

QString StemCache::cacheFilePath(TrackId trackId) {
    return QDir(QDir(rootDirectoryRef()).filePath(trackId.toString()))
            .filePath(QStringLiteral("stems.flac"));
}

bool StemCache::hasCache(TrackId trackId) {
    if (!trackId.isValid() || rootDirectoryRef().isEmpty()) {
        return false;
    }
    return QFileInfo::exists(cacheFilePath(trackId));
}

bool StemCache::writeCache(TrackId trackId,
        const CSAMPLE* pInterleavedStems,
        SINT frames,
        int sampleRate,
        QString* pError) {
    if (!trackId.isValid() || pInterleavedStems == nullptr || frames <= 0 ||
            sampleRate <= 0 || rootDirectoryRef().isEmpty()) {
        setError(pError, QStringLiteral("Invalid stem cache arguments"));
        return false;
    }
    const QString directory = QDir(rootDirectoryRef()).filePath(trackId.toString());
    if (!QDir().mkpath(directory)) {
        setError(pError, QStringLiteral("Could not create %1").arg(directory));
        return false;
    }
    const QString path = cacheFilePath(trackId);
    const QString tempPath = QDir(directory).filePath(
            QStringLiteral(".stems-%1.tmp.flac")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels = kChannelCount;
    info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
    SNDFILE* pFile = openSndFileForWrite(tempPath, &info);
    if (pFile == nullptr) {
        setError(pError,
                QStringLiteral("Could not create %1: %2")
                        .arg(tempPath, QString::fromUtf8(sf_strerror(nullptr))));
        QFile::remove(tempPath);
        return false;
    }
    sf_command(pFile, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);
    sf_command(pFile, SFC_SET_CLIPPING, nullptr, SF_TRUE);

    bool written = true;
    for (SINT offset = 0; offset < frames; offset += kBlockFrames) {
        const SINT count = std::min<SINT>(kBlockFrames, frames - offset);
        const sf_count_t expected = count;
        if (sf_writef_float(pFile,
                    pInterleavedStems + static_cast<std::size_t>(offset) * kChannelCount,
                    expected) != expected) {
            setError(pError,
                    QStringLiteral("Could not write the stem cache: %1")
                            .arg(QString::fromUtf8(sf_strerror(pFile))));
            written = false;
            break;
        }
    }
    if (sf_close(pFile) != 0 && written) {
        setError(pError, QStringLiteral("Could not finalize the stem cache"));
        written = false;
    }
    if (!written) {
        QFile::remove(tempPath);
        return false;
    }

    // Windows refuses to rename over an existing file.
    QFile::remove(path);
    if (!QFile::rename(tempPath, path)) {
        setError(pError, QStringLiteral("Could not move the stem cache into place"));
        QFile::remove(tempPath);
        return false;
    }
    kLogger.info() << "Wrote stem cache for track" << trackId << path;
    return true;
}

bool StemCache::removeCache(TrackId trackId) {
    if (!trackId.isValid() || rootDirectoryRef().isEmpty()) {
        return false;
    }
    const QString directory = QDir(rootDirectoryRef()).filePath(trackId.toString());
    if (!QFileInfo::exists(QDir(directory).filePath(QStringLiteral("stems.flac")))) {
        return false;
    }
    return QDir(directory).removeRecursively();
}

QList<StemInfo> StemCache::stemInfo() {
    return {StemInfo(QStringLiteral("Drums"), QColor(QStringLiteral("#E85D4A"))),
            StemInfo(QStringLiteral("Bass"), QColor(QStringLiteral("#7A5AF8"))),
            StemInfo(QStringLiteral("Other"), QColor(QStringLiteral("#F2C94C"))),
            StemInfo(QStringLiteral("Vocals"), QColor(QStringLiteral("#2BB673")))};
}

} // namespace mixxx::stems
