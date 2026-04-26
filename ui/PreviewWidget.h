// ==============================================================================
// ui/PreviewWidget.h
//
// Zoomable, pannable image preview surface. Replaces the QLabel that
// previously hosted the scaled-pixmap display.
//
// Two cached QImages, swapped by a flag:
//   - m_originalImage — pre-edit (Spacebar shows this)
//   - m_editedImage   — post-edit (default)
// Switching between them is a flag flip + repaint; no re-render involved.
//
// Zoom modes:
//   - Fit (default): image scaled to fit the widget's interior, preserving
//     aspect ratio. Window resize automatically recomputes the fit scale.
//   - Free: explicit user scale (set by Ctrl+wheel or "100%" toggle), with
//     pan offset that the user can drag around.
//
// Interaction:
//   - Ctrl + mouse wheel: zoom in/out, anchored to the cursor (the image
//     pixel under the cursor stays under the cursor).
//   - Double-click: toggle Fit ↔ 100%.
//   - Middle-button drag, OR Alt+left-button drag: pan.
//   - Spacebar before/after is handled by MainWindow, which calls
//     setShowOriginal(); the widget just flips the displayed image.
//
// What this widget does NOT do:
//   - Drag-and-drop (handled by EmptyStateOverlay parented on top).
//   - Open-image dialog (handled by EmptyStateOverlay → MainWindow).
//   - Re-rendering pixels (this widget never asks the engine to do work;
//     it only displays what MainWindow gives it).
// ==============================================================================
#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QWidget>

class PreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    // ---- Image state -----------------------------------------------------
    // Both copies live alongside each other so Spacebar before/after can
    // flip the displayed image without forcing MainWindow to re-feed us.
    // Setting either to a null QImage is fine — the widget paints empty
    // when both are null (which is also when EmptyStateOverlay is shown).
    void setOriginalImage(const QImage& image);
    void setEditedImage  (const QImage& image);
    void setShowOriginal (bool showOriginal);

    // True when at least one of the cached images is non-null.
    bool hasImage() const;

    // ---- Zoom controls ---------------------------------------------------
    // Programmatic zoom — used by the double-click handler and could be
    // used by future menu items / keyboard shortcuts. The Ctrl+wheel handler
    // adjusts the user scale directly.
    void zoomToFit();
    void zoomTo100();

    // Multiplicative zoom — used by Zoom In / Zoom Out menu actions and
    // keyboard shortcuts. factor > 1 zooms in, < 1 zooms out. Cursor
    // anchoring is skipped for menu-driven zoom (no cursor position
    // available); the zoom anchors at the current view center.
    void zoomBy(double factor);

    // For the inline zoom-display chip. Updated automatically on every
    // zoom or resize event; no explicit caller required.
    QString zoomLabelText() const;

    // ---- Color sampling (Targeted Color Adjustment Tool) ----------------
    // When sampling mode is enabled, left-clicks within the image area
    // emit colorSampled(QColor, QPoint) instead of doing any other action.
    // Cursor switches to a crosshair so users can see the tool is active.
    //
    // The mode is sticky — it stays on across multiple samples until the
    // caller flips it off. This matches the spec's "Tool remains active
    // until user disables Target Color button."
    //
    // Pan, zoom, and double-click-toggle all continue to work in sampling
    // mode. The middle-button / Alt+left-button pan gestures are not
    // affected. Right-click context menu is also unaffected.
    void setColorSamplingActive(bool on);
    bool isColorSamplingActive() const { return m_colorSamplingActive; }

    QSize sizeHint()        const override { return QSize(640, 480); }
    QSize minimumSizeHint() const override { return QSize(320, 240); }

signals:
    // Emitted when the user clicks a valid pixel inside the image area
    // while sampling mode is active. The QColor is read from the edited
    // image (m_editedImage), falling back to the original if edited is
    // null. imagePos is the pixel coordinate within the source image.
    //
    // Designed for reuse beyond the HSL eyedropper — same primitive
    // serves color-grading wheel sampling, mask color-range selection,
    // skin-tone detection, etc. Slots that only need the color can
    // declare `void slot(QColor)`; Qt drops unused trailing args.
    void colorSampled(QColor color, QPoint imagePos);

protected:
    void paintEvent       (QPaintEvent*  event) override;
    void resizeEvent      (QResizeEvent* event) override;
    void wheelEvent       (QWheelEvent*  event) override;
    void mousePressEvent  (QMouseEvent*  event) override;
    void mouseMoveEvent   (QMouseEvent*  event) override;
    void mouseReleaseEvent(QMouseEvent*  event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    // ---- Coordinate conversion ------------------------------------------
    // The displayed image's center lives at m_panOffset (in widget coords).
    // The effective scale is computeFitScale() in fit mode, m_userScale
    // otherwise. Convert: widget = (image - imageCenter) * scale + panOffset.
    double  effectiveScale() const;
    double  computeFitScale() const;
    QPointF imageCenter()    const;   // image-space center, in image pixels
    QPointF imageToWidget(const QPointF& imagePoint) const;
    QPointF widgetToImage(const QPointF& widgetPoint) const;

    // Image rect in widget coords for the current scale + pan.
    QRectF  imageRectInWidget() const;

    // After any zoom change, ensure the pan offset keeps the image at least
    // partially on screen (with some forgiveness so users can pan an image
    // off the edge if they want a closer look at one corner).
    void clampPanOffset();

    // The currently displayed image — picks original or edited based on
    // m_showOriginal. May be null if neither has been set.
    const QImage& displayImage() const;

    // ---- State -----------------------------------------------------------
    QImage m_originalImage;
    QImage m_editedImage;
    bool   m_showOriginal = false;

    // Zoom: m_fitMode == true ignores m_userScale and uses computeFitScale().
    bool   m_fitMode  = true;
    double m_userScale = 1.0;

    // Pan offset: where the image's center lands in widget coords. In fit
    // mode this is always the widget center (recomputed on every paint).
    // In free mode it tracks user drag input. Reset to widget center when
    // toggling to 100% or changing images.
    QPointF m_panOffset;

    // Drag state: -1 when not panning, otherwise the button being held.
    Qt::MouseButton m_panButton = Qt::NoButton;
    QPoint          m_lastDragPos;

    // ---- Color sampling state -----------------------------------------------
    bool m_colorSamplingActive = false;

    // Read the pixel at imagePos from the EDITED image (falling back to
    // original if edited is null). Returns Qt::transparent and *valid=false
    // if imagePos is outside the image rect or both images are null.
    //
    // Reusable across future tools (color grading wheels, mask range
    // selection, skin-tone detection). Pure read — no side effects, no
    // render trigger. Spec rule: sampling does NOT trigger render.
    QColor sampleAt(const QPoint& imagePos, bool* valid = nullptr) const;
};
