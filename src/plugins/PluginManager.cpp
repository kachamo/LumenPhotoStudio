#include "plugins/PluginManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace lps {

namespace {

QString valueAsString(const QJsonObject& obj, const QString& key)
{
    const auto v = obj.value(key);
    return v.isString() ? v.toString().trimmed() : QString();
}

bool isZipPackage(const QFileInfo& fi)
{
    return fi.isFile() && fi.suffix().compare(QStringLiteral("zip"),
                                              Qt::CaseInsensitive) == 0;
}

} // namespace

PluginManager::PluginManager(QString pluginsDirPath)
{
    if (pluginsDirPath.trimmed().isEmpty()) {
        pluginsDirPath = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins");
    }

    m_pluginsDirPath = QDir::cleanPath(pluginsDirPath);
    ensurePluginsFolder();
    reload();
}

QStringList PluginManager::supportedTypes()
{
    return {
        QStringLiteral("adjustment"),
        QStringLiteral("export"),
        QStringLiteral("presetPack"),
        QStringLiteral("lutPack"),
        QStringLiteral("panel"),
        QStringLiteral("node"),
    };
}

bool PluginManager::ensurePluginsFolder()
{
    QDir dir(m_pluginsDirPath);
    if (dir.exists()) {
        m_lastError.clear();
        return true;
    }

    if (!dir.mkpath(QStringLiteral("."))) {
        setLastError(QStringLiteral("Could not create plugins folder: %1")
                         .arg(m_pluginsDirPath));
        return false;
    }

    m_lastError.clear();
    return true;
}

void PluginManager::reload()
{
    m_plugins.clear();
    if (!ensurePluginsFolder()) return;

    QDir dir(m_pluginsDirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);

    for (const QFileInfo& entry : entries) {
        if (entry.isDir()) {
            m_plugins.append(readPluginFolder(entry.absoluteFilePath()));
        } else if (entry.fileName().compare(QStringLiteral("plugin.json"),
                                            Qt::CaseInsensitive) == 0) {
            m_plugins.append(readPluginManifest(entry.absoluteFilePath(),
                                                entry.absolutePath()));
        } else if (isZipPackage(entry)) {
            m_plugins.append(readPackagedPlugin(entry.absoluteFilePath()));
        }
    }
}

bool PluginManager::installPluginFromPath(const QString& sourcePath,
                                          QString* errorMessage)
{
    if (sourcePath.trimmed().isEmpty()) {
        setLastError(QStringLiteral("No plugin path selected."), errorMessage);
        return false;
    }
    if (!ensurePluginsFolder()) {
        if (errorMessage) *errorMessage = m_lastError;
        return false;
    }

    const QFileInfo src(sourcePath);
    if (!src.exists()) {
        setLastError(QStringLiteral("Plugin path does not exist: %1")
                         .arg(sourcePath), errorMessage);
        return false;
    }

    const QString dest = uniqueDestinationPath(src.completeBaseName().isEmpty()
        ? src.fileName()
        : src.completeBaseName());

    bool ok = false;
    if (src.isDir()) {
        ok = copyDirectory(src.absoluteFilePath(), dest, errorMessage);
    } else if (isZipPackage(src)) {
        QDir dir(m_pluginsDirPath);
        QString zipDest = dir.filePath(src.fileName());
        int suffix = 1;
        while (QFileInfo::exists(zipDest)) {
            zipDest = dir.filePath(QStringLiteral("%1-%2.zip")
                .arg(src.completeBaseName())
                .arg(suffix++));
        }
        ok = QFile::copy(src.absoluteFilePath(), zipDest);
        if (!ok) {
            setLastError(QStringLiteral("Could not copy plugin package to: %1")
                             .arg(zipDest), errorMessage);
        }
    } else {
        setLastError(QStringLiteral("Select a plugin folder or .zip package."),
                     errorMessage);
        return false;
    }

    if (ok) {
        m_lastError.clear();
        reload();
    }
    return ok;
}

bool PluginManager::setPluginEnabled(const QString& pluginId,
                                     bool enabled,
                                     QString* errorMessage)
{
    const auto it = std::find_if(m_plugins.cbegin(), m_plugins.cend(),
        [&pluginId](const PluginInfo& p) { return p.id == pluginId; });
    if (it == m_plugins.cend() || !it->valid || it->manifestPath.isEmpty()) {
        setLastError(QStringLiteral("Plugin manifest is not editable."),
                     errorMessage);
        return false;
    }

    QFile file(it->manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        setLastError(QStringLiteral("Could not read manifest: %1")
                         .arg(it->manifestPath), errorMessage);
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        setLastError(QStringLiteral("Invalid plugin manifest: %1")
                         .arg(it->manifestPath), errorMessage);
        return false;
    }

    QJsonObject obj = doc.object();
    obj[QStringLiteral("enabled")] = enabled;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setLastError(QStringLiteral("Could not write manifest: %1")
                         .arg(it->manifestPath), errorMessage);
        return false;
    }

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();
    reload();
    m_lastError.clear();
    return true;
}

PluginInfo PluginManager::readPluginFolder(const QString& folderPath) const
{
    const QString manifest = QDir(folderPath).filePath(QStringLiteral("plugin.json"));
    if (QFileInfo::exists(manifest))
        return readPluginManifest(manifest, folderPath);

    PluginInfo info;
    const QFileInfo fi(folderPath);
    info.id = fi.fileName();
    info.name = fi.fileName();
    info.version = QStringLiteral("-");
    info.type = QStringLiteral("-");
    info.path = folderPath;
    info.valid = false;
    info.error = QStringLiteral("Missing plugin.json manifest.");
    return info;
}

PluginInfo PluginManager::readPluginManifest(const QString& manifestPath,
                                             const QString& pluginPath) const
{
    PluginInfo info;
    info.path = pluginPath;
    info.manifestPath = manifestPath;

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        info.name = QFileInfo(pluginPath).fileName();
        info.version = QStringLiteral("-");
        info.type = QStringLiteral("-");
        info.error = QStringLiteral("Could not read plugin.json.");
        return info;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        info.name = QFileInfo(pluginPath).fileName();
        info.version = QStringLiteral("-");
        info.type = QStringLiteral("-");
        info.error = QStringLiteral("Invalid JSON manifest.");
        return info;
    }

    const QJsonObject obj = doc.object();
    info.id = valueAsString(obj, QStringLiteral("id"));
    info.name = valueAsString(obj, QStringLiteral("name"));
    info.version = valueAsString(obj, QStringLiteral("version"));
    info.author = valueAsString(obj, QStringLiteral("author"));
    info.description = valueAsString(obj, QStringLiteral("description"));
    info.type = valueAsString(obj, QStringLiteral("type"));
    info.enabled = obj.value(QStringLiteral("enabled")).toBool(false);

    QStringList missing;
    const QStringList requiredFields = {
        QStringLiteral("id"),
        QStringLiteral("name"),
        QStringLiteral("version"),
        QStringLiteral("author"),
        QStringLiteral("description"),
        QStringLiteral("type"),
    };
    for (const QString& key : requiredFields) {
        if (valueAsString(obj, key).isEmpty())
            missing.append(key);
    }
    if (!obj.value(QStringLiteral("enabled")).isBool())
        missing.append(QStringLiteral("enabled"));

    if (!missing.isEmpty()) {
        info.error = QStringLiteral("Missing or invalid fields: %1")
            .arg(missing.join(QStringLiteral(", ")));
        return info;
    }

    if (!supportedTypes().contains(info.type)) {
        info.error = QStringLiteral("Unsupported plugin type: %1")
            .arg(info.type);
        return info;
    }

    info.valid = true;
    return info;
}

PluginInfo PluginManager::readPackagedPlugin(const QString& filePath) const
{
    PluginInfo info;
    const QFileInfo fi(filePath);
    info.id = fi.completeBaseName();
    info.name = fi.fileName();
    info.version = QStringLiteral("-");
    info.type = QStringLiteral("package");
    info.path = filePath;
    info.packaged = true;
    info.valid = false;
    info.error = QStringLiteral("ZIP package installed; extraction is not enabled yet.");
    return info;
}

bool PluginManager::copyDirectory(const QString& sourceDir,
                                  const QString& destDir,
                                  QString* errorMessage) const
{
    const QDir source(sourceDir);
    if (!source.exists()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Source folder does not exist.");
        return false;
    }

    QDir dest;
    if (!dest.mkpath(destDir)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Could not create destination folder: %1")
                .arg(destDir);
        return false;
    }

    const QFileInfoList entries = source.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name);

    for (const QFileInfo& entry : entries) {
        const QString outPath = QDir(destDir).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectory(entry.absoluteFilePath(), outPath, errorMessage))
                return false;
        } else {
            if (!QFile::copy(entry.absoluteFilePath(), outPath)) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Could not copy %1 to %2")
                        .arg(entry.absoluteFilePath(), outPath);
                }
                return false;
            }
        }
    }

    return true;
}

QString PluginManager::uniqueDestinationPath(const QString& baseName) const
{
    const QString safeBase = baseName.trimmed().isEmpty()
        ? QStringLiteral("plugin")
        : baseName.trimmed();

    QDir dir(m_pluginsDirPath);
    QString candidate = dir.filePath(safeBase);
    int suffix = 1;
    while (QFileInfo::exists(candidate)) {
        candidate = dir.filePath(QStringLiteral("%1-%2").arg(safeBase).arg(suffix++));
    }
    return candidate;
}

void PluginManager::setLastError(const QString& message,
                                 QString* errorMessage)
{
    m_lastError = message;
    if (errorMessage) *errorMessage = message;
}

} // namespace lps
