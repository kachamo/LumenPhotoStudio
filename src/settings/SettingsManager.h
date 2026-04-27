// ==============================================================================
// src/settings/SettingsManager.h
// ==============================================================================
#pragma once

#include <QString>
#include <QStringList>

namespace lps {

class SettingsManager
{
public:
    SettingsManager();

    bool showWelcomeOnStartup() const;
    void setShowWelcomeOnStartup(bool on);

    QStringList recentImages() const;
    void setRecentImages(const QStringList& images);
    void addRecentImage(const QString& path);

    QStringList recentFiles() const;
    void setRecentFiles(const QStringList& files);
    void addRecentFile(const QString& path);

    QStringList recentProjects() const;
    void setRecentProjects(const QStringList& projects);
    void addRecentProject(const QString& path);

    QString themeName() const;
    void setThemeName(const QString& name);

    bool bottomWorkspaceVisible() const;
    void setBottomWorkspaceVisible(bool visible);

    bool bottomWorkspaceCollapsed() const;
    void setBottomWorkspaceCollapsed(bool collapsed);

    bool analysisPanelCollapsed() const;
    void setAnalysisPanelCollapsed(bool collapsed);

    QString renderBackend() const;
    void setRenderBackend(const QString& backend);

    QString defaultExportFolder() const;
    void setDefaultExportFolder(const QString& folder);

    QString lastOpenFolder() const;
    void setLastOpenFolder(const QString& folder);

private:
    static QStringList normalizedRecentList(const QStringList& paths);
};

} // namespace lps
