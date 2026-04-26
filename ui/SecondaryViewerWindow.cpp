// ==============================================================================
// ui/SecondaryViewerWindow.cpp
// ==============================================================================
#include "SecondaryViewerWindow.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

// ==============================================================================
// DisplayWidget — internal image-painting surface
//
// Paints whatever QImage was last passed via setImage(), scaled to fit
// the widget's interior with aspect ratio preserved. For downscale,
// SmoothPixmapTransform; for upscale beyond 100%, nearest-neighbor so
// users see actual pixels. Same convention as PreviewWidget's fit-mode
// painting.
//
// When the image is null, paints a centered "No image loaded" message.
// ==============================================================================
class SecondaryViewerWindow::DisplayWidget : public QWidget
{
public:
    explicit DisplayWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(320, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void setImage(const QImage& image)
    {
        // Implicit-sharing assignment is cheap — no per-pixel copy. cacheKey
        // comparison early-outs when the same image is set repeatedly (e.g.
        // a parameter that didn't actually change pixel data).
        if (m_image.cacheKey() == image.cacheKey()) return;
        m_image = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter p(this);

        // Background fill — matches the dark surface convention used
        // elsewhere in the UI.
        p.fillRect(rect(), QColor(32, 32, 34));

        if (m_image.isNull()) {
            // Empty state: simple centered text. No need for the full
            // welcome screen here — this window is always opened from
            // an existing main-viewer state, so users already know
            // what's going on.
            p.setPen(QColor(120, 120, 128));
            QFont f = font();
            f.setPointSizeF(font().pointSizeF() * 1.05);
            p.setFont(f);
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("No image loaded"));
            return;
        }

        // Compute aspect-fit destination rect inside the widget.
        const QSizeF src(m_image.size());
        const QSizeF dstAvail(width(), height());
        const double sx = dstAvail.width()  / src.width();
        const double sy = dstAvail.height() / src.height();
        const double s  = std::min(sx, sy);
        const QSizeF dst(src.width() * s, src.height() * s);
        const QPointF origin(
            (dstAvail.width()  - dst.width())  * 0.5,
            (dstAvail.height() - dst.height()) * 0.5);
        const QRectF target(origin, dst);

        // Smooth transform when downscaling; pixel-doubling when upscaling.
        // The threshold matches PreviewWidget's convention (s >= 1.0 ⇒
        // nearest), so the secondary viewer's appearance lines up with
        // a 100%+ main viewer in case users compare them.
        p.setRenderHint(QPainter::SmoothPixmapTransform, s < 1.0);
        p.drawImage(target, m_image, QRectF(m_image.rect()));
    }

private:
    QImage m_image;
};

// ==============================================================================
// SecondaryViewerWindow
// ==============================================================================
SecondaryViewerWindow::SecondaryViewerWindow(QWidget* parent)
    : QMainWindow(parent, Qt::Window)   // explicit Qt::Window so this becomes
                                        // a top-level OS window even though
                                        // it's parented to MainWindow for
                                        // lifetime management.
{
    setWindowTitle(tr("Lumen — Secondary Viewer"));

    // Reasonable default size. The user can resize freely; subsequent
    // opens (after they close it) will preserve the size since we hide
    // rather than destroy on close.
    resize(800, 600);

    m_display = new DisplayWidget(this);
    setCentralWidget(m_display);

    // Default Qt close behavior on a QWidget is to hide, not destroy
    // (WA_DeleteOnClose is off by default). That's exactly what we want
    // — closing the window keeps it alive so MainWindow's menu can
    // re-show it without recreating, and the user's window position
    // and size persist through close→reopen cycles within a session.
}

void SecondaryViewerWindow::setImage(const QImage& image)
{
    if (m_display) m_display->setImage(image);
}
