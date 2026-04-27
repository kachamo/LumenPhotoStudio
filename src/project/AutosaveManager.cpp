// ==============================================================================
// src/project/AutosaveManager.cpp
// ==============================================================================
#include "project/AutosaveManager.h"

#include "project/ProjectSerializer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kAutosaveDelayMs = 5000;
constexpr int kAutosavesToKeep = 3;

QString fallbackAppDataPath()
{
    return QDir::homePath() + QStringLiteral("/.lumen-photo-studio");
}

} // namespace

namespace lps {

AutosaveManager::AutosaveManager(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kAutosaveDelayMs);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        saveNow();
    });
}

QString AutosaveManager::autosaveDirPath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = fallbackAppDataPath();
    return QDir(base).filePath(QStringLiteral("autosave"));
}

QString AutosaveManager::latestAutosavePath() const
{
    const QDir dir(autosaveDirPath());
    if (!dir.exists())
        return QString();

    QFileInfoList files = dir.entryInfoList(
        QStringList{ QStringLiteral("autosave_*.lpsproj") },
        QDir::Files,
        QDir::Name);
    std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() > b.lastModified();
    });
    return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

bool AutosaveManager::hasAutosave() const
{
    return !latestAutosavePath().isEmpty();
}

void AutosaveManager::schedule(const ProjectDocument& document)
{
    m_pendingDocument = document;
    m_hasPendingDocument = true;
    m_timer.start();
}

bool AutosaveManager::saveNow(QString* error)
{
    if (error) error->clear();
    if (!m_hasPendingDocument)
        return true;

    QDir dir(autosaveDirPath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("Could not create autosave folder.");
        return false;
    }

    ProjectDocument document = m_pendingDocument;
    document.modifiedDate = QDateTime::currentDateTimeUtc();
    if (document.projectName.trimmed().isEmpty()) {
        document.projectName = document.projectPathReference.isEmpty()
            ? QStringLiteral("Recovered Session")
            : QFileInfo(document.projectPathReference).completeBaseName();
    }

    const QString path = makeAutosavePath();
    const ProjectSaveResult result = ProjectSerializer::saveToFile(document, path);
    if (!result.ok) {
        if (error) *error = result.errorMessage;
        return false;
    }

    m_lastAutosavePath = path;
    pruneOldAutosaves(kAutosavesToKeep);
    return true;
}

void AutosaveManager::clearPending()
{
    m_timer.stop();
    m_hasPendingDocument = false;
    m_pendingDocument = ProjectDocument{};
}

bool AutosaveManager::deleteAutosave(const QString& path, QString* error)
{
    if (error) error->clear();
    const QString target = path.isEmpty() ? latestAutosavePath() : path;
    if (target.isEmpty())
        return true;
    if (!QFile::remove(target)) {
        if (error) *error = QStringLiteral("Could not delete autosave file.");
        return false;
    }
    if (m_lastAutosavePath == target)
        m_lastAutosavePath.clear();
    return true;
}

void AutosaveManager::deleteAllAutosaves()
{
    clearPending();
    const QDir dir(autosaveDirPath());
    const QFileInfoList files = dir.entryInfoList(
        QStringList{ QStringLiteral("autosave_*.lpsproj") },
        QDir::Files,
        QDir::Time);
    for (const QFileInfo& file : files)
        QFile::remove(file.absoluteFilePath());
    m_lastAutosavePath.clear();
}

QString AutosaveManager::makeAutosavePath() const
{
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return QDir(autosaveDirPath()).filePath(
        QStringLiteral("autosave_%1.lpsproj").arg(stamp));
}

void AutosaveManager::pruneOldAutosaves(int keepCount)
{
    const QDir dir(autosaveDirPath());
    QFileInfoList files = dir.entryInfoList(
        QStringList{ QStringLiteral("autosave_*.lpsproj") },
        QDir::Files,
        QDir::Name);
    std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() > b.lastModified();
    });
    for (int i = keepCount; i < files.size(); ++i)
        QFile::remove(files.at(i).absoluteFilePath());
}

} // namespace lps
