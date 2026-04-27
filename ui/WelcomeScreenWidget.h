// ==============================================================================
// ui/WelcomeScreenWidget.h
// ==============================================================================
#pragma once

#include <QStringList>
#include <QWidget>

class QFrame;
class QGridLayout;
class QMimeData;
class QCheckBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;

class WelcomeScreenWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WelcomeScreenWidget(QWidget* parent = nullptr);
    bool showOnStartup() const;
    void setShowOnStartup(bool on);
    void setRecentItems(const QStringList& images,
                        const QStringList& projects);
    void setRecentFiles(const QStringList& files,
                        const QStringList& projects = QStringList());

signals:
    void openImageRequested();
    void openProjectRequested();
    void newProjectRequested();
    void preferencesRequested();
    void pluginsRequested();
    void showOnStartupChanged(bool on);
    void recentImageRequested(const QString& path);
    void recentProjectRequested(const QString& path);
    void imageFileDropped(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    static bool eventHasSupportedImageFile(const QMimeData* mime,
                                           QString* outPath = nullptr);
    void setDropHovering(bool hovering);

    QFrame* m_dropZone = nullptr;
    QCheckBox* m_showOnStartupCheck = nullptr;
    QGridLayout* m_recentImagesGrid = nullptr;
    QGridLayout* m_recentProjectsGrid = nullptr;
    bool m_dropHovering = false;
};
