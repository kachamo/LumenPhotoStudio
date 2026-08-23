// ==============================================================================
// ui/CatalogGridView.cpp
//
// Architecture
// ------------
// QListView (IconMode) + a flat QAbstractListModel + a custom
// QStyledItemDelegate. One widget for the whole grid, never one widget per
// photo, so a 50,000-image catalog costs 50,000 POD rows instead of 50,000
// QWidgets. QListView already provides pixel-smooth scrolling, rubber-band
// multi-select, keyboard navigation and accessibility; a hand-painted view
// would have to reimplement all of that to arrive at the same place.
//
// Thumbnails
// ----------
// ThumbnailCache::getOrGenerate() decodes (and for RAW that is slow), so it is
// never called from the UI thread. Instead:
//
//   scroll / resize / model reset
//        -> coalescing timer (kScanIntervalMs)
//        -> scanVisible(): sample the viewport for the tiles actually on
//           screen and queue only those with no pixmap yet
//        -> pump(): keep at most kMaxActiveLoads runnables alive in a private
//           QThreadPool; each decodes and scales off-thread, then posts the
//           finished QImage back with a queued invokeMethod
//        -> handleThumbnailReady(): QImage to QPixmap on the UI thread, into
//           the bounded cache, dataChanged() for that one row.
//
// Nothing off-screen is ever generated, and queued-but-not-started requests
// are dropped on the next scan, so a fast flick never leaves thousands of
// stale decodes behind.
//
// Memory: decoded tiles live in a QCache with a byte budget, not in a vector.
// 50,000 tiles at 256px would be several GB; the cache holds the working set
// and re-requests evicted tiles, which is a cheap disk-cache hit.
// ==============================================================================
#include "CatalogGridView.h"

#include "catalog/ThumbnailCache.h"

#include <QAbstractListModel>
#include <QCache>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLatin1Char>
#include <QList>
#include <QListView>
#include <QLocale>
#include <QMetaObject>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QRunnable>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace {

// ---- Tile geometry ----------------------------------------------------------
constexpr int kTileGap        = 8;    // space between neighbouring tiles
constexpr int kTilePad        = 6;    // tile border to content
constexpr int kFooterHeight   = 16;   // stars / flag / colour label strip
constexpr int kMinThumbPx     = 72;
constexpr int kMaxThumbPx     = 360;
constexpr int kDefaultThumbPx = 168;
constexpr int kCaptionMinPx   = 120;  // below this the filename is dropped

// ---- Scheduling -------------------------------------------------------------
constexpr int kScanIntervalMs = 24;         // coalesces scroll/resize storms
constexpr int kMaxActiveLoads = 4;          // decodes in flight at once
constexpr int kPixmapBudgetKb = 96 * 1024;  // roughly 96 MB of decoded tiles

// ---- Palette (mirrors lumenDarkTheme() in ui/MainWindow.cpp) -----------------
const QColor kAccent     (0xCC, 0xFF, 0x00);
const QColor kTileBg     (0x16, 0x18, 0x1D);
const QColor kTileBgSel  (0xCC, 0xFF, 0x00, 36);
const QColor kTileBorder (0x2A, 0x2D, 0x35);
const QColor kWellBg     (0x0E, 0x0F, 0x12);
const QColor kTextPrimary(0xDD, 0xE0, 0xE7);
// kTextDim was used by an earlier delegate layout; kept out of the build
// rather than left dangling — clang warns where MinGW does not.
const QColor kStarOff    (0x3D, 0x42, 0x4E);
const QColor kGlyph      (0x26, 0x2A, 0x31);
const QColor kRejectRed  (0xE2, 0x60, 0x5F);

constexpr double kPi = 3.14159265358979323846;   // M_PI is not ISO C++

QColor colorForLabel(lps::ColorLabel label)
{
    switch (label) {
    case lps::ColorLabel::Red:    return QColor(0xE2, 0x60, 0x5F);
    case lps::ColorLabel::Yellow: return QColor(0xE6, 0xC5, 0x4A);
    case lps::ColorLabel::Green:  return QColor(0x6B, 0xD1, 0x7A);
    case lps::ColorLabel::Blue:   return QColor(0x6C, 0x9C, 0xE8);
    case lps::ColorLabel::Purple: return QColor(0xB0, 0x82, 0xE8);
    case lps::ColorLabel::None:   break;
    }
    return QColor();
}

// Five-pointed star inscribed in `box`. Drawn by hand rather than as a text
// glyph so the shape is identical on every platform and at every tile size.
QPainterPath starPath(const QRectF& box)
{
    QPainterPath path;
    const QPointF c   = box.center();
    const qreal outer = std::min(box.width(), box.height()) / 2.0;
    const qreal inner = outer * 0.44;
    for (int i = 0; i < 10; ++i) {
        const qreal r = (i % 2 == 0) ? outer : inner;
        const qreal a = -kPi / 2.0 + (static_cast<qreal>(i) * kPi / 5.0);
        const QPointF p(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        if (i == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    path.closeSubpath();
    return path;
}

// Pennant on a staff, used for the "picked" flag.
QPainterPath flagPath(const QRectF& box)
{
    QPainterPath path;
    const qreal x = box.left() + box.width() * 0.24;
    path.moveTo(x, box.bottom());
    path.lineTo(x, box.top());
    path.lineTo(box.right(), box.top() + box.height() * 0.28);
    path.lineTo(x, box.top() + box.height() * 0.56);
    return path;
}

} // namespace

// ==============================================================================
// CatalogImageModel
//
// Flat list of catalog rows plus a bounded, id-keyed pixmap cache. The
// delegate reads the row struct straight off the model rather than through a
// QVariant, so a 300-byte struct is never copied to paint a tile. data() only
// implements the roles Qt itself needs (accessible text and tooltip).
// ==============================================================================
class CatalogImageModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit CatalogImageModel(QObject* parent = nullptr)
        : QAbstractListModel(parent)
    {
        m_pixmaps.setMaxCost(kPixmapBudgetKb);
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_images.size());
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!isValidRow(index.row()) || index.parent().isValid())
            return QVariant();

        const lps::CatalogImage& img = m_images.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
        case Qt::AccessibleTextRole:
            return img.fileName;
        case Qt::ToolTipRole:
            return tooltipFor(img);
        default:
            return QVariant();
        }
    }

    // ---- rows ---------------------------------------------------------------
    void setImages(const QVector<lps::CatalogImage>& images)
    {
        beginResetModel();
        m_images = images;
        m_rowById.clear();
        m_rowById.reserve(m_images.size());
        for (int row = 0; row < m_images.size(); ++row)
            m_rowById.insert(m_images.at(row).id, row);
        m_failed.clear();
        endResetModel();
    }

    void clearImages()
    {
        beginResetModel();
        m_images.clear();
        m_rowById.clear();
        m_failed.clear();
        m_pixmaps.clear();
        endResetModel();
    }

    bool isValidRow(int row) const { return row >= 0 && row < m_images.size(); }
    const lps::CatalogImage& imageAt(int row) const { return m_images.at(row); }
    int  rowForId(qint64 id) const { return m_rowById.value(id, -1); }

    // Replaces one row in place (rating/flag/label edits) and repaints just it.
    bool replaceImage(const lps::CatalogImage& image)
    {
        const int row = rowForId(image.id);
        if (row < 0)
            return false;
        m_images[row] = image;
        touch(row);
        return true;
    }

    void applyRating(qint64 id, int rating)
    {
        const int row = rowForId(id);
        if (row < 0)
            return;
        m_images[row].rating = std::clamp(rating, 0, 5);
        touch(row);
    }

    void applyFlag(qint64 id, lps::ImageFlag flag)
    {
        const int row = rowForId(id);
        if (row < 0)
            return;
        m_images[row].flag = flag;
        touch(row);
    }

    // ---- thumbnails ---------------------------------------------------------
    const QPixmap* pixmapFor(qint64 id) const { return m_pixmaps.object(id); }

    void setPixmap(qint64 id, const QPixmap& pixmap)
    {
        const int row = rowForId(id);
        if (row < 0 || pixmap.isNull())
            return;
        const int cost = std::max(1, (pixmap.width() * pixmap.height() * 4) / 1024);
        m_pixmaps.insert(id, new QPixmap(pixmap), cost);
        m_failed.remove(id);
        touch(row);
    }

    // Called when the tile size changes: every cached pixmap is now the wrong
    // size, so drop them and let the next visibility scan re-request.
    void dropPixmaps()
    {
        m_pixmaps.clear();
        m_failed.clear();
        if (!m_images.isEmpty())
            emit dataChanged(index(0), index(static_cast<int>(m_images.size()) - 1));
    }

    bool hasFailed(qint64 id) const { return m_failed.contains(id); }

    void markFailed(qint64 id)
    {
        const int row = rowForId(id);
        if (row < 0)
            return;
        m_failed.insert(id);
        touch(row);
    }

private:
    void touch(int row)
    {
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx);
    }

    static QString tooltipFor(const lps::CatalogImage& img)
    {
        QStringList lines;
        lines << img.fileName;
        if (img.width > 0 && img.height > 0) {
            lines << QCoreApplication::translate("CatalogGridView", "%1 x %2 px")
                         .arg(img.width)
                         .arg(img.height);
        }
        if (!img.cameraModel.isEmpty())
            lines << img.cameraModel;
        if (!img.lensModel.isEmpty())
            lines << img.lensModel;

        QStringList exposure;
        if (!img.focalLength.isEmpty())  exposure << img.focalLength;
        if (!img.aperture.isEmpty())     exposure << img.aperture;
        if (!img.shutterSpeed.isEmpty()) exposure << img.shutterSpeed;
        if (!img.iso.isEmpty())          exposure << img.iso;
        if (!exposure.isEmpty())
            lines << exposure.join(QStringLiteral("  |  "));

        if (img.captureTime.isValid())
            lines << QLocale().toString(img.captureTime, QLocale::ShortFormat);
        if (!img.lookJson.isEmpty())
            lines << QCoreApplication::translate("CatalogGridView", "Edited");

        return lines.join(QLatin1Char('\n'));
    }

    QVector<lps::CatalogImage> m_images;
    QHash<qint64, int>         m_rowById;
    QSet<qint64>               m_failed;    // generation came back empty
    QCache<qint64, QPixmap>    m_pixmaps;   // bounded LRU, keyed by image id
};

// ==============================================================================
// GridTileDelegate
//
// Paints a whole tile: thumbnail well, filename, rating stars, pick/reject
// flag and colour label. Layout is derived from the cell rect so the delegate
// and the view can never disagree about geometry.
// ==============================================================================
class GridTileDelegate final : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit GridTileDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {}

    void setCellSize(const QSize& size) { m_cell = size; }
    void setShowCaption(bool show) { m_showCaption = show; }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return m_cell;
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto* model = qobject_cast<const CatalogImageModel*>(index.model());
        if (!model || !model->isValidRow(index.row())) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const lps::CatalogImage& img = model->imageAt(index.row());

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool rejected = (img.flag == lps::ImageFlag::Rejected);

        // ---- frame ----------------------------------------------------------
        const qreal half = kTileGap / 2.0;
        const QRectF tile = QRectF(option.rect).adjusted(half, half, -half, -half);
        if (tile.width() < 8.0 || tile.height() < 8.0) {
            painter->restore();
            return;
        }

        QPainterPath frame;
        frame.addRoundedRect(tile, 8.0, 8.0);
        painter->fillPath(frame, selected ? kTileBgSel : kTileBg);
        painter->setPen(QPen(selected ? kAccent : kTileBorder, selected ? 1.6 : 1.0));
        painter->drawPath(frame);

        // ---- content bands --------------------------------------------------
        const QRectF inner = tile.adjusted(kTilePad, kTilePad, -kTilePad, -kTilePad);
        const QFontMetrics fm(option.font);
        const qreal captionH = m_showCaption ? fm.height() : 0.0;
        const qreal wellH = inner.height() - kFooterHeight - captionH
                          - (m_showCaption ? 3.0 : 0.0);
        if (wellH < 8.0) {
            painter->restore();
            return;
        }

        const QRectF well(inner.left(), inner.top(), inner.width(), wellH);
        painter->setOpacity(rejected ? 0.42 : 1.0);
        paintWell(painter, well, img, model);
        painter->setOpacity(1.0);

        // ---- caption --------------------------------------------------------
        qreal y = well.bottom();
        if (m_showCaption) {
            y += 3.0;
            const QRectF captionRect(inner.left(), y, inner.width(), captionH);
            painter->setPen(selected ? QColor(0xFF, 0xFF, 0xFF) : kTextPrimary);
            painter->setFont(option.font);
            painter->drawText(captionRect,
                              Qt::AlignLeft | Qt::AlignVCenter,
                              fm.elidedText(img.fileName,
                                            Qt::ElideMiddle,
                                            static_cast<int>(captionRect.width())));
            y += captionH;
        }

        // ---- footer: stars left, label + flag right --------------------------
        const QRectF footer(inner.left(), y, inner.width(), kFooterHeight);
        paintFooter(painter, footer, img);

        painter->restore();
    }

private:
    void paintWell(QPainter* painter,
                   const QRectF& well,
                   const lps::CatalogImage& img,
                   const CatalogImageModel* model) const
    {
        QPainterPath wellPath;
        wellPath.addRoundedRect(well, 6.0, 6.0);
        painter->fillPath(wellPath, kWellBg);

        const QPixmap* pixmap = model->pixmapFor(img.id);
        if (pixmap && !pixmap->isNull()) {
            QSizeF size = QSizeF(pixmap->size());
            size.scale(well.size(), Qt::KeepAspectRatio);
            QRectF target(QPointF(0.0, 0.0), size);
            target.moveCenter(well.center());
            painter->drawPixmap(target, *pixmap, QRectF(pixmap->rect()));
        } else {
            paintPlaceholder(painter, well, img, model->hasFailed(img.id));
        }

        // Small accent dot marks an image that already carries edits.
        if (!img.lookJson.isEmpty()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kAccent);
            painter->drawEllipse(QPointF(well.right() - 7.0, well.top() + 7.0), 3.0, 3.0);
            painter->setBrush(Qt::NoBrush);
        }
    }

    // Pending and failed tiles both get a quiet photo glyph rather than a hole
    // in the grid; a failed one also names the file type so the user can see
    // what could not be decoded.
    static void paintPlaceholder(QPainter* painter,
                                 const QRectF& well,
                                 const lps::CatalogImage& img,
                                 bool failed)
    {
        const qreal side = std::min(well.width(), well.height()) * 0.42;
        if (side < 10.0)
            return;

        QRectF glyph(0.0, 0.0, side, side * 0.78);
        glyph.moveCenter(well.center());

        painter->setPen(QPen(kGlyph, 1.4));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(glyph, 3.0, 3.0);
        painter->drawEllipse(QPointF(glyph.left() + glyph.width() * 0.28,
                                     glyph.top() + glyph.height() * 0.30),
                             glyph.width() * 0.08, glyph.width() * 0.08);

        QPainterPath ridge;
        ridge.moveTo(glyph.left() + glyph.width() * 0.08, glyph.bottom() - 2.0);
        ridge.lineTo(glyph.left() + glyph.width() * 0.40, glyph.center().y());
        ridge.lineTo(glyph.left() + glyph.width() * 0.60, glyph.bottom() - glyph.height() * 0.28);
        ridge.lineTo(glyph.left() + glyph.width() * 0.78, glyph.center().y() - glyph.height() * 0.10);
        ridge.lineTo(glyph.right() - 2.0, glyph.bottom() - 2.0);
        painter->drawPath(ridge);

        if (failed) {
            const QString ext = img.fileName.section(QLatin1Char('.'), -1).toUpper();
            if (!ext.isEmpty() && ext != img.fileName.toUpper()) {
                painter->setPen(kStarOff);
                painter->drawText(QRectF(well.left(), glyph.bottom() + 2.0,
                                         well.width(), well.bottom() - glyph.bottom() - 2.0),
                                  Qt::AlignHCenter | Qt::AlignTop,
                                  ext);
            }
        }
    }

    static void paintFooter(QPainter* painter,
                            const QRectF& footer,
                            const lps::CatalogImage& img)
    {
        const qreal starSide = std::min<qreal>(12.0, footer.height());
        const qreal step = starSide + 2.0;

        painter->setPen(Qt::NoPen);
        for (int i = 0; i < 5; ++i) {
            const QRectF box(footer.left() + i * step,
                             footer.center().y() - starSide / 2.0,
                             starSide,
                             starSide);
            if (box.right() > footer.right())
                break;
            const QPainterPath star = starPath(box.adjusted(1.0, 1.0, -1.0, -1.0));
            if (i < img.rating) {
                painter->fillPath(star, kAccent);
            } else {
                painter->setPen(QPen(kStarOff, 1.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(star);
                painter->setPen(Qt::NoPen);
            }
        }

        qreal right = footer.right();

        if (img.flag != lps::ImageFlag::None) {
            const QRectF box(right - starSide, footer.center().y() - starSide / 2.0,
                             starSide, starSide);
            if (img.flag == lps::ImageFlag::Picked) {
                painter->setPen(Qt::NoPen);
                painter->fillPath(flagPath(box.adjusted(1.0, 1.0, -1.0, -1.0)), kAccent);
            } else {
                painter->setPen(QPen(kRejectRed, 1.6, Qt::SolidLine, Qt::RoundCap));
                const QRectF x = box.adjusted(3.0, 3.0, -3.0, -3.0);
                painter->drawLine(x.topLeft(), x.bottomRight());
                painter->drawLine(x.topRight(), x.bottomLeft());
                painter->setPen(Qt::NoPen);
            }
            right -= starSide + 3.0;
        }

        const QColor label = colorForLabel(img.colorLabel);
        if (label.isValid()) {
            const qreal side = std::min<qreal>(9.0, footer.height());
            const QRectF box(right - side, footer.center().y() - side / 2.0, side, side);
            painter->setPen(Qt::NoPen);
            painter->setBrush(label);
            painter->drawRoundedRect(box, 2.0, 2.0);
            painter->setBrush(Qt::NoBrush);
        }
    }

    QSize m_cell        = QSize(kDefaultThumbPx, kDefaultThumbPx);
    bool  m_showCaption = true;
};

// ==============================================================================
// GridEventHook
//
// Tiny event filter so the grid can intercept the view's key presses (QListView
// would otherwise swallow 0-5 / P / X as type-ahead search) and learn about
// viewport resizes, without changing CatalogGridView's declared interface.
// ==============================================================================
class GridEventHook final : public QObject
{
public:
    using Handler = std::function<bool(QObject*, QEvent*)>;

    GridEventHook(Handler handler, QObject* parent)
        : QObject(parent)
        , m_handler(std::move(handler))
    {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_handler && m_handler(watched, event))
            return true;
        return QObject::eventFilter(watched, event);
    }

private:
    Handler m_handler;
};

// ==============================================================================
// CatalogGridView::Impl
// ==============================================================================
struct CatalogGridView::Impl
{
    explicit Impl(CatalogGridView* owner) : q(owner) {}

    // The grid's event filter. Held so ~CatalogGridView can detach it before
    // freeing this Impl — see the destructor for why that ordering matters.
    QObject* hook = nullptr;

    // ---- thumbnail scheduling ----------------------------------------------
    void scheduleScan();
    void scanVisible();
    void pump();
    void submit(qint64 id, const QString& path, const QSize& box, int epoch);
    void handleThumbnailReady(qint64 id, const QImage& image, int epoch);
    void resetQueue();

    // ---- geometry -----------------------------------------------------------
    QSize cellSize() const;
    QSize thumbBox() const { return QSize(thumbPx, thumbPx); }
    bool  showCaption() const { return thumbPx >= kCaptionMinPx; }
    void  applyGeometry();

    // ---- interaction --------------------------------------------------------
    bool handleKey(QKeyEvent* event);
    QVector<qint64> selectionOrCurrent() const;
    void emitSelection();
    void activate(const QModelIndex& index);

    CatalogGridView*   q        = nullptr;
    QListView*         view     = nullptr;
    CatalogImageModel* model    = nullptr;
    GridTileDelegate*  delegate = nullptr;
    QTimer*            scanTimer = nullptr;

    QThreadPool      pool;
    QList<qint64>    queue;     // highest priority first
    QSet<qint64>     queued;
    QSet<qint64>     active;
    std::atomic_bool aborted{false};

    int thumbPx = kDefaultThumbPx;
    int epoch   = 0;            // bumped on size change; stale results dropped
};

QSize CatalogGridView::Impl::cellSize() const
{
    const int captionH = showCaption()
        ? QFontMetrics(view ? view->font() : q->font()).height() + 3
        : 0;
    const int w = thumbPx + 2 * kTilePad + kTileGap;
    const int h = thumbPx + 2 * kTilePad + kTileGap + kFooterHeight + captionH;
    return QSize(w, h);
}

void CatalogGridView::Impl::applyGeometry()
{
    const QSize cell = cellSize();
    delegate->setCellSize(cell);
    delegate->setShowCaption(showCaption());
    view->setGridSize(cell);
    view->setIconSize(thumbBox());
    view->doItemsLayout();
    scheduleScan();
}

void CatalogGridView::Impl::scheduleScan()
{
    if (scanTimer && !scanTimer->isActive())
        scanTimer->start();
}

// Samples the viewport on a half-cell lattice. With a uniform grid every
// visible cell contains at least one sample point, so this finds exactly the
// on-screen rows in time proportional to the number of visible tiles, not to
// the catalog size.
void CatalogGridView::Impl::scanVisible()
{
    if (aborted)
        return;

    resetQueue();

    if (!view->isVisible() || model->rowCount() == 0)
        return;

    const QRect vp = view->viewport()->rect();
    const QSize cell = view->gridSize();
    if (vp.isEmpty() || cell.isEmpty())
        return;

    const int stepX = std::max(8, cell.width() / 2);
    const int stepY = std::max(8, cell.height() / 2);

    QSet<int> seen;
    for (int y = 0; y < vp.height(); y += stepY) {
        for (int x = 0; x < vp.width(); x += stepX) {
            const QModelIndex idx = view->indexAt(QPoint(std::min(x, vp.width() - 1),
                                                         std::min(y, vp.height() - 1)));
            if (!idx.isValid() || seen.contains(idx.row()))
                continue;
            seen.insert(idx.row());

            const lps::CatalogImage& img = model->imageAt(idx.row());
            if (model->pixmapFor(img.id) || model->hasFailed(img.id))
                continue;
            if (active.contains(img.id) || queued.contains(img.id))
                continue;

            queued.insert(img.id);
            queue.append(img.id);
        }
    }

    pump();
}

// Drops everything that has been queued but not started. Anything still on
// screen is re-queued by the scan that immediately follows.
void CatalogGridView::Impl::resetQueue()
{
    queue.clear();
    queued.clear();
}

void CatalogGridView::Impl::pump()
{
    while (!aborted && active.size() < kMaxActiveLoads && !queue.isEmpty()) {
        const qint64 id = queue.takeFirst();
        queued.remove(id);

        const int row = model->rowForId(id);
        if (row < 0)
            continue;

        const QString path = model->imageAt(row).absolutePath;
        if (path.isEmpty()) {
            model->markFailed(id);
            continue;
        }

        active.insert(id);
        submit(id, path, thumbBox(), epoch);
    }
}

void CatalogGridView::Impl::submit(qint64 id, const QString& path,
                                   const QSize& box, int requestEpoch)
{
    Impl* self = this;
    QObject* context = model;   // outlives the pool: see ~CatalogGridView

    pool.start(QRunnable::create([self, context, id, path, box, requestEpoch]() {
        QImage image;
        if (!self->aborted) {
            // One cache object per worker thread. ThumbnailCache documents
            // get()/has() as thread-safe but generate() only as "call me off
            // the UI thread", so instances are never shared between threads.
            thread_local std::unique_ptr<lps::ThumbnailCache> cache;
            if (!cache)
                cache = std::make_unique<lps::ThumbnailCache>();

            image = cache->getOrGenerate(path, lps::ThumbnailCache::Size::Grid);
            if (!image.isNull() && box.isValid()) {
                image = image.scaled(box, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
        }

        QMetaObject::invokeMethod(
            context,
            [self, id, image, requestEpoch]() {
                self->handleThumbnailReady(id, image, requestEpoch);
            },
            Qt::QueuedConnection);
    }));
}

void CatalogGridView::Impl::handleThumbnailReady(qint64 id, const QImage& image,
                                                 int requestEpoch)
{
    active.remove(id);

    if (requestEpoch == epoch) {
        if (image.isNull())
            model->markFailed(id);
        else
            model->setPixmap(id, QPixmap::fromImage(image));
    } else {
        // Stale: the tile size changed while this decode was in flight. Drop
        // the wrongly-sized result and rescan — the scan that ran at the moment
        // of the size change skipped this id because it was still active, so
        // without this the tile would stay blank until the next scroll.
        scheduleScan();
    }

    pump();
}

QVector<qint64> CatalogGridView::Impl::selectionOrCurrent() const
{
    QVector<qint64> ids;
    QList<int> rows;

    const QModelIndexList selected = view->selectionModel()->selectedIndexes();
    for (const QModelIndex& idx : selected) {
        if (model->isValidRow(idx.row()))
            rows.append(idx.row());
    }
    if (rows.isEmpty()) {
        const QModelIndex current = view->currentIndex();
        if (model->isValidRow(current.row()))
            rows.append(current.row());
    }

    std::sort(rows.begin(), rows.end());
    ids.reserve(rows.size());
    for (int row : rows)
        ids.append(model->imageAt(row).id);
    return ids;
}

void CatalogGridView::Impl::emitSelection()
{
    QVector<qint64> ids;
    QList<int> rows;
    const QModelIndexList selected = view->selectionModel()->selectedIndexes();
    for (const QModelIndex& idx : selected) {
        if (model->isValidRow(idx.row()))
            rows.append(idx.row());
    }
    std::sort(rows.begin(), rows.end());
    ids.reserve(rows.size());
    for (int row : rows)
        ids.append(model->imageAt(row).id);

    emit q->selectionChanged(ids);
}

void CatalogGridView::Impl::activate(const QModelIndex& index)
{
    if (!model->isValidRow(index.row()))
        return;
    const QString path = model->imageAt(index.row()).absolutePath;
    if (!path.isEmpty())
        emit q->imageActivated(path);
}

// Keyboard contract: 0-5 rate, P pick, X reject, Enter opens. Flags toggle off
// when the item under the cursor already carries them, which is what users who
// come from Lightroom expect.
bool CatalogGridView::Impl::handleKey(QKeyEvent* event)
{
    const Qt::KeyboardModifiers mods =
        event->modifiers() & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
    if (mods != Qt::NoModifier)
        return false;

    const int key = event->key();

    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        activate(view->currentIndex());
        return true;
    }

    const QVector<qint64> ids = selectionOrCurrent();
    if (ids.isEmpty())
        return false;

    if (key >= Qt::Key_0 && key <= Qt::Key_5) {
        const int rating = key - Qt::Key_0;
        for (qint64 id : ids) {
            model->applyRating(id, rating);
            emit q->ratingRequested(id, rating);
        }
        return true;
    }

    if (key == Qt::Key_P || key == Qt::Key_X) {
        const lps::ImageFlag wanted = (key == Qt::Key_P) ? lps::ImageFlag::Picked
                                                         : lps::ImageFlag::Rejected;
        const int currentRow = model->rowForId(ids.first());
        const bool alreadySet = currentRow >= 0
                             && model->imageAt(currentRow).flag == wanted;
        const lps::ImageFlag flag = alreadySet ? lps::ImageFlag::None : wanted;

        for (qint64 id : ids) {
            model->applyFlag(id, flag);
            emit q->flagRequested(id, flag);
        }
        return true;
    }

    return false;
}

// ==============================================================================
// CatalogGridView
// ==============================================================================
CatalogGridView::CatalogGridView(QWidget* parent)
    : QWidget(parent)
    , d(new Impl(this))
{
    setObjectName(QStringLiteral("catalogGridView"));

    d->pool.setMaxThreadCount(kMaxActiveLoads);
    d->pool.setObjectName(QStringLiteral("lps-thumbnails"));

    d->model = new CatalogImageModel(this);

    d->view = new QListView(this);
    d->view->setObjectName(QStringLiteral("catalogGridList"));
    d->view->setModel(d->model);
    d->view->setViewMode(QListView::IconMode);
    d->view->setFlow(QListView::LeftToRight);
    d->view->setWrapping(true);
    d->view->setResizeMode(QListView::Adjust);
    d->view->setMovement(QListView::Static);
    d->view->setUniformItemSizes(true);
    d->view->setSpacing(0);
    d->view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->view->setSelectionRectVisible(true);
    d->view->setDragEnabled(false);
    d->view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    d->view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->view->setFrameShape(QFrame::NoFrame);
    d->view->setAttribute(Qt::WA_MacShowFocusRect, false);
    d->view->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    d->view->setStyleSheet(QStringLiteral(
        "QListView#catalogGridList { background: #0E0F12; border: 0; outline: 0; }"));

    d->delegate = new GridTileDelegate(this);
    d->view->setItemDelegate(d->delegate);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(d->view);

    // Coalesces scroll and resize storms into one visibility scan per frame.
    d->scanTimer = new QTimer(this);
    d->scanTimer->setSingleShot(true);
    d->scanTimer->setInterval(kScanIntervalMs);
    connect(d->scanTimer, &QTimer::timeout, this, [this]() { d->scanVisible(); });

    connect(d->view->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { d->scheduleScan(); });
    connect(d->view, &QListView::doubleClicked,
            this, [this](const QModelIndex& index) { d->activate(index); });
    connect(d->view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection&, const QItemSelection&) {
                d->emitSelection();
            });

    auto* hook = new GridEventHook(
        [this](QObject* watched, QEvent* event) {
            // Defence in depth: the destructor detaches this filter before
            // freeing d, but a null d must never be dereferenced if some
            // path still routes an event here.
            if (!d) return false;
            if (watched == d->view) {
                if (event->type() == QEvent::KeyPress)
                    return d->handleKey(static_cast<QKeyEvent*>(event));
                if (event->type() == QEvent::Show)
                    d->scheduleScan();
            } else if (watched == d->view->viewport()) {
                if (event->type() == QEvent::Resize)
                    d->scheduleScan();
            }
            return false;
        },
        this);
    d->hook = hook;
    d->view->installEventFilter(hook);
    d->view->viewport()->installEventFilter(hook);

    d->applyGeometry();
}

// Order matters: stop handing work to the pool, drop everything queued, then
// wait for the runnables already decoding. Only after waitForDone() can `d`
// (and the model the results are posted to) safely go away.
CatalogGridView::~CatalogGridView()
{
    d->aborted = true;
    d->pool.clear();
    d->pool.waitForDone();

    // Detach the event filter BEFORE freeing d.
    //
    // The hook is parented to `this`, so Qt destroys it inside the base
    // ~QWidget — which runs *after* this body. While ~QWidget tears down our
    // children it sends them events, those events pass through the still
    // installed filter, and the handler dereferences `d` on its first line.
    //
    // macOS caught this as EXC_BAD_ACCESS in QAbstractScrollArea::viewport(),
    // called from GridEventHook::eventFilter during QListView destruction.
    // Windows and Linux only survived because no event happened to be
    // delivered in that window — it was a latent use-after-free on all three.
    if (d->hook) {
        if (d->view) {
            d->view->removeEventFilter(d->hook);
            if (QWidget* vp = d->view->viewport())
                vp->removeEventFilter(d->hook);
        }
        delete d->hook;
        d->hook = nullptr;
    }

    delete d;
    d = nullptr;   // the handler's guard reads this
}

void CatalogGridView::setImages(const QVector<lps::CatalogImage>& images)
{
    d->resetQueue();
    d->model->setImages(images);
    d->view->scrollToTop();
    d->scheduleScan();
    d->emitSelection();
}

void CatalogGridView::clear()
{
    d->resetQueue();
    d->model->clearImages();
    d->emitSelection();
}

QVector<qint64> CatalogGridView::selectedIds() const
{
    QVector<qint64> ids;
    QList<int> rows;
    const QModelIndexList selected = d->view->selectionModel()->selectedIndexes();
    for (const QModelIndex& idx : selected) {
        if (d->model->isValidRow(idx.row()))
            rows.append(idx.row());
    }
    std::sort(rows.begin(), rows.end());
    ids.reserve(rows.size());
    for (int row : rows)
        ids.append(d->model->imageAt(row).id);
    return ids;
}

int CatalogGridView::thumbnailSize() const
{
    return d->thumbPx;
}

void CatalogGridView::setThumbnailSize(int px)
{
    const int clamped = std::clamp(px, kMinThumbPx, kMaxThumbPx);
    if (clamped == d->thumbPx)
        return;

    d->thumbPx = clamped;

    // Cached pixmaps were scaled for the old tile; invalidate them and let the
    // scan re-request at the new size. Bumping the epoch discards results from
    // decodes that are already in flight.
    ++d->epoch;
    d->resetQueue();
    d->model->dropPixmaps();
    d->applyGeometry();
}

// Additive to the declared contract: lets the owner push an authoritative row
// back after a catalog write without resetting the whole model (which would
// lose scroll position and selection).
void CatalogGridView::updateImage(const lps::CatalogImage& image)
{
    d->model->replaceImage(image);
}

#include "CatalogGridView.moc"
