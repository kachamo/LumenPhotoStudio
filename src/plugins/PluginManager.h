#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace lps {

struct PluginInfo
{
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString type;
    bool enabled = false;
    bool valid = false;
    bool packaged = false;
    QString path;
    QString manifestPath;
    QString error;
};

class PluginManager
{
public:
    explicit PluginManager(QString pluginsDirPath = QString());

    QString pluginsDirPath() const { return m_pluginsDirPath; }
    QString lastError() const { return m_lastError; }
    QList<PluginInfo> installedPlugins() const { return m_plugins; }

    bool ensurePluginsFolder();
    void reload();
    bool installPluginFromPath(const QString& sourcePath,
                               QString* errorMessage = nullptr);
    bool setPluginEnabled(const QString& pluginId,
                          bool enabled,
                          QString* errorMessage = nullptr);

    static QStringList supportedTypes();

private:
    PluginInfo readPluginFolder(const QString& folderPath) const;
    PluginInfo readPluginManifest(const QString& manifestPath,
                                  const QString& pluginPath) const;
    PluginInfo readPackagedPlugin(const QString& filePath) const;

    bool copyDirectory(const QString& sourceDir,
                       const QString& destDir,
                       QString* errorMessage) const;
    QString uniqueDestinationPath(const QString& baseName) const;
    void setLastError(const QString& message,
                      QString* errorMessage = nullptr);

    QString m_pluginsDirPath;
    QString m_lastError;
    QList<PluginInfo> m_plugins;
};

} // namespace lps
