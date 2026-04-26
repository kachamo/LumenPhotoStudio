// ==============================================================================
// ui/SecondaryViewerWindow.h
//
// Secondary preview window — a minimal floating window that mirrors the
// main preview's processed image. Used for second-monitor previewing,
// reference views, or comparing fit-to-screen to a zoomed main viewer.
//
// The window:
//   - Shows the processed image only. No sliders, no menus, no histogram.
//   - Always renders fit-to-screen with smooth scaling. No zoom/pan
//     controls — those would conflict with the spec's "fit always"
//     requirement and add complexity for no benefit.
//   - Updates whenever MainWindow calls setImage() with a new QImage.
//     The window does NOT independently render — it's a pure consumer
//     of the main pipeline's output.
//   - Is hidden (not destroyed) when the user closes it via the X button.
//     MainWindow's menu slot can call show() again to bring it back.
//
// Lifetime: parented to MainWindow but flagged Qt::Window so it lives as
// an independent OS window. Auto-destroyed when MainWindow exits via
// Qt's parent-child cleanup. Top-level window flag means the user can
// freely move it to any monitor.
// ==============================================================================
#pragma once

#include <QImage>
#include <QMainWindow>

class SecondaryViewerWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit SecondaryViewerWindow(QWidget* parent = nullptr);

    // Push a new image into the viewer. Null QImage → blank state.
    // Stores a copy (Qt's implicit-sharing makes this cheap), schedules
    // a repaint. NEVER triggers a render — this window only displays
    // what MainWindow already rendered.
    void setImage(const QImage& image);

private:
    // Internal display widget. A small QWidget subclass that paints the
    // current image scaled-to-fit in its center, with smooth transform
    // for downscaling and nearest-neighbor for upscaling above 100%.
    class DisplayWidget;
    DisplayWidget* m_display = nullptr;
};
