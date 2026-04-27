// ==============================================================================
// ui/NodeGraphWidget.h
//
// Visual-only node graph foundation — DaVinci-style node workflow preview.
// V1 displays the canonical pipeline as connected nodes:
//
//   Input -> Lens -> Transform -> Tone -> Color -> Curves -> Grading
//         -> Details -> Effects -> Output
//
// The graph is purely informational. It does NOT drive rendering — the
// actual pipeline is still hard-wired in ImagePipeline.cpp. The widget
// exists so users can see the pipeline's structure and so future work
// (custom node types, branching, mask compositing) has a concrete UI
// surface to grow into.
//
// Implemented on QGraphicsView/QGraphicsScene because that already gives
// us pan/zoom/selection/hit-testing for free, and node items can be
// QGraphicsItem subclasses without the widget having to manage their
// paint or input directly.
//
// Reusable beyond this V1: when the pipeline becomes node-driven, each
// NodeItem will gain input/output sockets and a parameter payload, and
// the connection lines will become real edges in a DAG. The widget's
// interaction model (pan, zoom, select) carries forward unchanged.
// ==============================================================================
#pragma once

#include <QGraphicsView>
#include <QString>

class QGraphicsScene;

class NodeGraphWidget : public QGraphicsView
{
    Q_OBJECT

public:
    explicit NodeGraphWidget(QWidget* parent = nullptr);

protected:
    // Pan: middle-mouse-drag (matches PreviewWidget's convention).
    void mousePressEvent  (QMouseEvent* event) override;
    void mouseMoveEvent   (QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    // Zoom: Ctrl + wheel. Plain wheel falls through to the default
    // QGraphicsView vertical scroll behavior.
    void wheelEvent(QWheelEvent* event) override;

private:
    // Builds the default pipeline layout as connected nodes in a row.
    // straight lines. Called once at construction. Future versions can
    // load a graph from a Look or other source.
    void buildDefaultGraph();

    QGraphicsScene* m_scene = nullptr;

    // Pan drag state. -1 button when not panning.
    bool   m_panning = false;
    QPoint m_lastPanPos;
};
