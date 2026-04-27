// ==============================================================================
// src/settings/SettingsManager.cpp
// ==============================================================================
#include "settings/SettingsManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

constexpr int kMaxRecentItems = 10;

QString defaultPicturesPath()
{
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return path.isEmpty() ? QDir::homePath() : path;
}

QString cleanFolder(const QString& folder, const QString& fallback)
{
    const QString path = QDir::fromNativeSeparators(folder.trimmed());
    if (path.isEmpty()) return fallback;
    return QFileInfo(path).isDir() ? path : fallback;
}

} // namespace

namespace lps {

SettingsManager::SettingsManager() = default;

bool SettingsManager::showWelcomeOnStartup() const
{
    return QSettings().value(QStringLiteral("startup/showWelcomeOnStartup"),
                             true).toBool();
}

void SettingsManager::setShowWelcomeOnStartup(bool on)
{
    QSettings().setValue(QStringLiteral("startup/showWelcomeOnStartup"), on);
}

QStringList SettingsManager::recentImages() const
{
    QSettings settings;
    const QString key = QStringLiteral("recent/images");
    QStringList raw = settings.value(key).toStringList();
    bool migrated = false;
    if (raw.isEmpty()) {
        raw = settings.value(QStringLiteral("recent/files")).toStringList();
        migrated = !raw.isEmpty();
    }
    const QStringList cleaned = normalizedRecentList(raw);
    if (migrated || cleaned != raw)
        settings.setValue(key, cleaned);
    return cleaned;
}

void SettingsManager::setRecentImages(const QStringList& images)
{
    QSettings().setValue(QStringLiteral("recent/images"),
                         normalizedRecentList(images));
}

void SettingsManager::addRecentImage(const QString& path)
{
    QStringList images = recentImages();
    images.prepend(path);
    setRecentImages(images);
}

QStringList SettingsManager::recentFiles() const
{
    return recentImages();
}

void SettingsManager::setRecentFiles(const QStringList& files)
{
    setRecentImages(files);
}

void SettingsManager::addRecentFile(const QString& path)
{
    addRecentImage(path);
}

QStringList SettingsManager::recentProjects() const
{
    QSettings settings;
    const QString key = QStringLiteral("recent/projects");
    const QStringList raw = settings.value(key).toStringList();
    const QStringList cleaned = normalizedRecentList(raw);
    if (cleaned != raw)
        settings.setValue(key, cleaned);
    return cleaned;
}

void SettingsManager::setRecentProjects(const QStringList& projects)
{
    QSettings().setValue(QStringLiteral("recent/projects"),
                         normalizedRecentList(projects));
}

void SettingsManager::addRecentProject(const QString& path)
{
    QStringList projects = recentProjects();
    projects.prepend(path);
    setRecentProjects(projects);
}

QString SettingsManager::themeName() const
{
    return QSettings().value(QStringLiteral("appearance/themeName"),
                             QStringLiteral("Dark")).toString();
}

void SettingsManager::setThemeName(const QString& name)
{
    const QString clean = name.trimmed().isEmpty()
        ? QStringLiteral("Dark")
        : name.trimmed();
    QSettings().setValue(QStringLiteral("appearance/themeName"), clean);
}

bool SettingsManager::bottomWorkspaceVisible() const
{
    return QSettings().value(QStringLiteral("workspace/bottomWorkspaceVisible"),
                             true).toBool();
}

void SettingsManager::setBottomWorkspaceVisible(bool visible)
{
    QSettings().setValue(QStringLiteral("workspace/bottomWorkspaceVisible"),
                         visible);
}

bool SettingsManager::bottomWorkspaceCollapsed() const
{
    return QSettings().value(QStringLiteral("workspace/bottomWorkspaceCollapsed"),
                             false).toBool();
}

void SettingsManager::setBottomWorkspaceCollapsed(bool collapsed)
{
    QSettings().setValue(QStringLiteral("workspace/bottomWorkspaceCollapsed"),
                         collapsed);
}

bool SettingsManager::analysisPanelCollapsed() const
{
    return QSettings().value(QStringLiteral("workspace/analysisPanelCollapsed"),
                             false).toBool();
}

void SettingsManager::setAnalysisPanelCollapsed(bool collapsed)
{
    QSettings().setValue(QStringLiteral("workspace/analysisPanelCollapsed"),
                         collapsed);
}

QString SettingsManager::renderBackend() const
{
    return QSettings().value(QStringLiteral("performance/renderBackend"),
                             QStringLiteral("CPU")).toString();
}

void SettingsManager::setRenderBackend(const QString& backend)
{
    const QString clean = backend.trimmed().isEmpty()
        ? QStringLiteral("CPU")
        : backend.trimmed();
    QSettings().setValue(QStringLiteral("performance/renderBackend"), clean);
}

QString SettingsManager::defaultExportFolder() const
{
    const QString fallback = defaultPicturesPath();
    return cleanFolder(QSettings().value(QStringLiteral("folders/defaultExportFolder"),
                                         fallback).toString(),
                       fallback);
}

void SettingsManager::setDefaultExportFolder(const QString& folder)
{
    QSettings().setValue(QStringLiteral("folders/defaultExportFolder"),
                         cleanFolder(folder, defaultPicturesPath()));
}

QString SettingsManager::lastOpenFolder() const
{
    const QString fallback = defaultPicturesPath();
    return cleanFolder(QSettings().value(QStringLiteral("folders/lastOpenFolder"),
                                         fallback).toString(),
                       fallback);
}

void SettingsManager::setLastOpenFolder(const QString& folder)
{
    QSettings().setValue(QStringLiteral("folders/lastOpenFolder"),
                         cleanFolder(folder, defaultPicturesPath()));
}

QStringList SettingsManager::normalizedRecentList(const QStringList& paths)
{
    QStringList out;
    for (const QString& path : paths) {
        const QString input = QDir::fromNativeSeparators(path.trimmed());
        if (input.isEmpty()) continue;
        const QFileInfo fi(input);
        if (!fi.isFile()) continue;
        const QString clean = QDir::fromNativeSeparators(fi.absoluteFilePath());
        if (out.contains(clean)) continue;
        out.append(clean);
    }
    while (out.size() > kMaxRecentItems)
        out.removeLast();
    return out;
}

} // namespace lps
