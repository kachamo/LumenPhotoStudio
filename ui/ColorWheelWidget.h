// ==============================================================================
// ui/ColorWheelWidget.h
//
// A circular hue/saturation control. Background paints an HSV wheel
// (value=1, hue around the angle, saturation along the radius). A puck
// draws at the current (hue, sat) position. Drag updates live.
//
// Conventions:
//   - Hue 0° at the +X axis (right of center), increasing counter-clockwise.
//     This matches HSV's classic mathematical convention. Display-wise it
//     means red is to the right, green at top, blue at bottom-left.
//   - Saturation 0 at center, 1 at the rim. Distances beyond the rim
//     clamp to 1.
//   - Value (V in HSV) is fixed at 1.0 for the wheel's background. The
//     "Strength" and "Luminance" sliders that pair with this widget
//     handle dimming and lightness offsetting separately.
//
// Reusable beyond color grading: same widget could drive future "wheel"-
// based tools (skin-tone qualifier, white-balance picker, etc.). The
// design is deliberately stateless beyond the (hue, sat) pair — no
// per-instance UI mode flags, no "shadow vs highlight" semantics.
// ==============================================================================
#pragma once

#include <QImage>
#include <QPointF>
#include <QWidget>

class ColorWheelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ColorWheelWidget(QWidget* parent = nullptr);

    // Programmatic update — does NOT emit signals (avoids feedback loops
    // when MainWindow's refresh function syncs widgets after undo/redo).
    void setHueSaturation(float hueDeg, float sat01);

    float hueDeg() const noexcept { return m_hue; }
    float sat()    const noexcept { return m_sat; }

    QSize sizeHint()        const override { return QSize(120, 120); }
    QSize minimumSizeHint() const override { return QSize(80, 80); }

signals:
    // Emitted at the start of a drag gesture (mouse press inside the
    // wheel area). MainWindow uses this to push an undo snapshot — one
    // snapshot per drag, regardless of how many move events fire.
    void dragStarted();

    // Emitted on every move event during a drag. hue is in [0, 360);
    // sat is in [0, 1]. Caller updates Look fields and kicks the
    // render debounce.
    void hueSaturationChanged(float hueDeg, float sat01);

    // Emitted on double-click anywhere in the widget. Caller decides
    // what "reset" means (typically: hue=0, sat=0). MainWindow pushes
    // an undo snapshot before applying.
    void resetRequested();

protected:
    void paintEvent     (QPaintEvent*  event) override;
    void mousePressEvent(QMouseEvent*  event) override;
    void mouseMoveEvent (QMouseEvent*  event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent    (QResizeEvent* event) override;

private:
    // Rebuild the cached background wheel image at the current widget
    // size. Called from resizeEvent; cheap (~few ms at typical sizes).
    void rebuildWheelCache();

    // Translate a widget-local point into (hue, sat). Returns false if
    // the point is at the exact center (sat=0, hue undefined — we keep
    // the existing hue in that case).
    bool pointToHueSat(const QPointF& p, float& outHue, float& outSat) const;

    // Geometry helpers — center and radius in widget coordinates.
    QPointF wheelCenter() const noexcept;
    float   wheelRadius() const noexcept;

    QImage  m_wheelCache;            // cached HSV background, updated on resize
    float   m_hue = 0.0f;            // [0, 360)
    float   m_sat = 0.0f;            // [0, 1]
    bool    m_dragging = false;
};
