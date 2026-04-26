// ==============================================================================
// ui/CurveEditorWidget.h
// Interactive tone-curve editor.
//
// Edits a lps::CurvePoints in place via a non-owning pointer: MainWindow holds
// the Look, sets the active channel via setCurve(&m_look.curves.red) etc.,
// and connects the curveChanged() signal to its own reprocess trigger.
//
// Widget responsibilities:
//   - Draw a graph area with grid, identity diagonal, curve line, control points.
//   - Handle left-click to add/drag points, right-click to delete.
//   - Keep the first and last points locked at (0,0) and (1,1).
//   - Clamp x,y to [0,1] and prevent points from crossing their neighbors.
//   - Emit curveChanged() when the active CurvePoints has been modified.
//
// Non-responsibilities:
//   - Owning curve data (the pointer is non-owning; lifetime is MainWindow's).
//   - Re-rendering the preview (that's MainWindow's debounce + pipeline).
//   - Tab/channel selection UI (that's a row of buttons in MainWindow).
//
// A single CurveEditorWidget instance handles all four channels by swapping
// the active curve pointer — no need to instantiate one per channel.
// ==============================================================================
#pragma once

#include "core/Look.h"

#include <QColor>
#include <QWidget>

class CurveEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CurveEditorWidget(QWidget* parent = nullptr);

    // Swap the active curve. Non-owning pointer — caller guarantees the
    // pointee outlives the widget. Passing nullptr puts the widget in a
    // neutral display-only state (still paints, doesn't respond to clicks).
    //
    // The widget will repaint to reflect the new curve's points. This call
    // does NOT emit curveChanged() — only user-initiated edits do.
    void setCurve(lps::CurvePoints* curve);

    // Optional accent color for the curve line — lets MainWindow tint the
    // line red/green/blue when those channels are active, white for master.
    // Defaults to white.
    void setCurveColor(QColor color);

    QSize sizeHint()        const override { return QSize(300, 260); }
    QSize minimumSizeHint() const override { return QSize(220, 180); }

signals:
    // Emitted when the active curve's points have been mutated by the user.
    // MainWindow connects this to restart the preview debounce.
    void curveChanged();

    // Emitted at the beginning of a user-initiated edit (mouse press that
    // leads to an add, a grab for drag, or a delete). MainWindow uses this
    // to capture a single undo snapshot at the start of each editing
    // operation — so one drag gesture = one undo step, regardless of how
    // many curveChanged() pulses fire during the drag.
    //
    // Semantics: fires BEFORE the widget mutates m_curve, so a snapshot
    // taken in response reflects the pre-edit state.
    void editStarted();

    // Emitted when an editing operation ends (mouse release after a drag,
    // or the synthetic "end" after a single-click add/delete). Pairs with
    // editStarted — every emitted editStarted is followed by exactly one
    // editFinished.
    void editFinished();

protected:
    void paintEvent      (QPaintEvent*  event) override;
    void mousePressEvent (QMouseEvent*  event) override;
    void mouseMoveEvent  (QMouseEvent*  event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // ---- Coordinate conversion -----------------------------------------------
    // The graph lives inside a margin-inset rect. Graph (0,0) is at the
    // bottom-left of that inset, (1,1) at the top-right. Widget coords are
    // standard Qt (0,0 top-left, y grows downward), so y is flipped.
    QRectF graphRect()           const;
    QPointF graphToWidget(const QPointF& g) const;
    QPointF widgetToGraph(const QPointF& w) const;

    // ---- Hit testing ---------------------------------------------------------
    // Find the index of the point nearest `widgetPos` within kHitRadius
    // pixels, or -1 if none. Endpoints (0 and size-1) ARE returned — callers
    // decide whether to act on them.
    int hitTestPoint(const QPointF& widgetPos) const;

    // Is the given index an endpoint (first or last)?
    bool isEndpoint(int index) const;

    // ---- Mutations (on the active curve) -------------------------------------
    // Each returns true if a mutation actually occurred (which triggers a
    // curveChanged signal and a repaint).
    bool addPoint(const QPointF& graphPos);
    bool movePoint(int index, const QPointF& graphPos);
    bool deletePoint(int index);

    // Non-owning. Null until setCurve() is called.
    lps::CurvePoints* m_curve = nullptr;

    // Drag state: -1 when not dragging.
    int m_draggingIndex = -1;

    QColor m_curveColor = QColor(230, 230, 235);
};
