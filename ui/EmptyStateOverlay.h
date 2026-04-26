// ==============================================================================
// ui/EmptyStateOverlay.h
//
// Welcome / drop-zone widget that overlays the preview surface when no
// image is loaded. Designed as an overlay (parented to the preview but
// painted on top) rather than a subclass of the preview itself, so future
// multi-document workflows can re-parent or share the same overlay across
// multiple preview surfaces.
//
// Geometry: the overlay tracks its parent's size automatically via an
// installed event filter. Caller just needs to construct the overlay with
// the preview as its parent and the geometry stays in sync.
//
// Visibility is controlled by the caller via show()/hide(). Typical usage:
//
//   m_emptyState = new EmptyStateOverlay(m_previewLabel);
//   m_emptyState->show();
//   ...
//   // after image loads:
//   m_emptyState->hide();
//   // after image cleared / project reset:
//   m_emptyState->show();
// ==============================================================================
#pragma once

#include <QWidget>

class QMimeData;

class EmptyStateOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit EmptyStateOverlay(QWidget* parent);

signals:
    // User clicked the empty preview area.
    void openRequested();

    // User dropped a single valid image file. The receiver should run the
    // standard "load this path" code path.
    void imageFileDropped(const QString& path);

protected:
    void paintEvent     (QPaintEvent*  event) override;
    void mousePressEvent(QMouseEvent*  event) override;

    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent (QDragMoveEvent*  event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent     (QDropEvent*      event) override;

    // Filters the parent's resize events so this overlay always covers
    // the parent's full client area.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool eventHasSupportedImageFile(const QMimeData* mime,
                                           QString* outPath = nullptr);

    bool m_dragHovering = false;
};
