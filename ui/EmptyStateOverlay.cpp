// ==============================================================================
// ui/EmptyStateOverlay.cpp
// ==============================================================================
#include "EmptyStateOverlay.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QStringList>
#include <QUrl>

namespace {

// Supported image extensions for drag-and-drop. Matches the filter Open
// Image's dialog uses, including .tif / .tiff and .webp. Lower-case
// matching is done at lookup time.
const QStringList& supportedExts()
{
    static const QStringList exts = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("webp"),
    };
    return exts;
}

inline bool isSupportedExt(const QString& ext)
{
    return supportedExts().contains(ext.toLower());
}

} // namespace

// ==============================================================================
// Construction
// ==============================================================================
EmptyStateOverlay::EmptyStateOverlay(QWidget* parent)
    : QWidget(parent)
{
    Q_ASSERT(parent != nullptr);   // parent is required — overlay tracks its size

    setAcceptDrops(true);

    // Geometry: cover the full parent on construction, then keep tracking
    // via the event filter below.
    setGeometry(parent->rect());
    parent->installEventFilter(this);

    // Styled background painted in paintEvent — we deliberately don't use
    // the QWidget stylesheet system here so background color stays under
    // our explicit control (in particular, so it can be transparent in
    // future variants without fighting the stylesheet cascade).
    setAttribute(Qt::WA_StyledBackground, false);

    // Mouse events should always pass through to us when we're visible —
    // even if the parent has its own handler.
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
}

// ==============================================================================
// Geometry tracking
//
// Filter the parent's resize events so the overlay always covers the same
// rect as its parent. This is the trick that makes "overlay" work without
// subclassing the parent: the parent doesn't need to know about us.
// ==============================================================================
bool EmptyStateOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parent() && event->type() == QEvent::Resize) {
        if (auto* w = qobject_cast<QWidget*>(parent())) {
            setGeometry(w->rect());
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ==============================================================================
// Painting
//
// Layout (vertical center stack):
//   - Logo (140 px square, scaled smoothly from 512 px source)
//   - 16 px gap
//   - "Drop image here"
//   - 8 px gap
//   - "or click to open"
//   - 12 px gap
//   - "JPG · PNG · TIFF · WEBP"
//
// Drag-hover state adds a subtle dashed accent border so users get visible
// feedback when their drag is being accepted.
// ==============================================================================
void EmptyStateOverlay::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Background — solid fill, matches the preview's dark surface.
    p.fillRect(rect(), QColor(32, 32, 34));

    if (m_dragHovering) {
        QPen pen(QColor(140, 200, 255, 160), 2.0, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(6, 6, -6, -6));
    }

    // Static so the QPixmap is loaded once per process. Falls through
    // silently if the resource fails to load — paint still produces a
    // usable empty state with text only.
    static const QPixmap logo(QStringLiteral(":/icons/lumen_logo_512.png"));
    const int logoSize = 140;

    const int W = width();
    const int H = height();

    const int gap1 = 16;
    const int gap2 = 8;
    const int gap3 = 12;

    QFont primaryFont = font();
    QFont secondaryFont = font();
    secondaryFont.setPointSizeF(font().pointSizeF() * 0.92);
    QFont formatsFont = font();
    formatsFont.setPointSizeF(font().pointSizeF() * 0.82);

    const QFontMetrics fmPrimary  (primaryFont);
    const QFontMetrics fmSecondary(secondaryFont);
    const QFontMetrics fmFormats  (formatsFont);

    const int primaryH   = fmPrimary.height();
    const int secondaryH = fmSecondary.height();
    const int formatsH   = fmFormats.height();
    const int totalH = logoSize + gap1 + primaryH + gap2 + secondaryH + gap3 + formatsH;

    int y = (H - totalH) / 2;
    if (y < 8) y = 8;

    if (!logo.isNull()) {
        const QPixmap scaled = logo.scaled(logoSize, logoSize,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        const int x = (W - scaled.width()) / 2;
        p.drawPixmap(x, y, scaled);
    }
    y += logoSize + gap1;

    p.setPen(QColor(210, 210, 215));
    p.setFont(primaryFont);
    p.drawText(QRect(0, y, W, primaryH),
               Qt::AlignHCenter | Qt::AlignVCenter,
               tr("Drop image here"));
    y += primaryH + gap2;

    p.setPen(QColor(140, 140, 148));
    p.setFont(secondaryFont);
    p.drawText(QRect(0, y, W, secondaryH),
               Qt::AlignHCenter | Qt::AlignVCenter,
               tr("or click to open"));
    y += secondaryH + gap3;

    p.setPen(QColor(105, 105, 112));
    p.setFont(formatsFont);
    p.drawText(QRect(0, y, W, formatsH),
               Qt::AlignHCenter | Qt::AlignVCenter,
               tr("JPG  ·  PNG  ·  TIFF  ·  WEBP"));
}

// ==============================================================================
// Mouse — left click anywhere triggers Open Image
// ==============================================================================
void EmptyStateOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit openRequested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// ==============================================================================
// Drag-and-drop
//
// Accept exactly one URL pointing at a local file with a supported image
// extension. Anything else is silently rejected — the cursor will show
// "no" and dropEvent won't fire.
//
// Drag-and-drop is only registered on this overlay, which means once the
// overlay is hidden (after image load), the parent QLabel won't accept
// drops by default — the existing image stays untouched. That's a
// deliberate design choice: replacing a loaded image via drag would clash
// with the unsaved-changes prompt and any future zoom/pan drag gestures.
// ==============================================================================
bool EmptyStateOverlay::eventHasSupportedImageFile(const QMimeData* mime,
                                                   QString* outPath)
{
    if (!mime || !mime->hasUrls()) return false;
    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1) return false;
    const QUrl& url = urls.first();
    if (!url.isLocalFile()) return false;

    const QString path = url.toLocalFile();
    const QFileInfo fi(path);
    if (!fi.isFile()) return false;
    if (!isSupportedExt(fi.suffix())) return false;

    if (outPath) *outPath = path;
    return true;
}

void EmptyStateOverlay::dragEnterEvent(QDragEnterEvent* event)
{
    if (eventHasSupportedImageFile(event->mimeData())) {
        m_dragHovering = true;
        update();
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void EmptyStateOverlay::dragMoveEvent(QDragMoveEvent* event)
{
    if (eventHasSupportedImageFile(event->mimeData())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void EmptyStateOverlay::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_dragHovering = false;
    update();
    QWidget::dragLeaveEvent(event);
}

void EmptyStateOverlay::dropEvent(QDropEvent* event)
{
    m_dragHovering = false;
    update();

    QString path;
    if (eventHasSupportedImageFile(event->mimeData(), &path)) {
        event->acceptProposedAction();
        emit imageFileDropped(path);
    } else {
        event->ignore();
    }
}
