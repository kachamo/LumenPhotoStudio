// ==============================================================================
// ui/PreviewWidget.cpp
// ==============================================================================
#include "PreviewWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

// Hard limits on the user scale. 0.05 = "5% of native preview size" is far
// enough to fit very large images comfortably; 8.0 = "800%" is far enough
// to inspect individual pixels at substantial zoom. Beyond these the
// experience degrades: too zoomed-out and the image is unreadable, too
// zoomed-in and rendering is just stretched pixels.
constexpr double kMinUserScale = 0.05;
constexpr double kMaxUserScale = 8.0;

// Wheel zoom multiplier per notch. 1.15 gives ~7 notches to double the zoom
// — fast enough to feel responsive, slow enough to hit specific zoom levels
// like 100% by overshooting and back.
constexpr double kWheelZoomFactor = 1.15;

// Threshold below which preview scaling uses smooth (bilinear) sampling and
// at/above which it switches to nearest-neighbor (so users can see actual
// pixels at high zoom — the standard photo-editor convention).
constexpr double kPixelDoublingThreshold = 1.0;

// Margin inside which the image must keep at least one anchor visible
// during pan. Prevents users from accidentally panning the entire image
// off-screen with no way to navigate back.
constexpr double kPanEdgeKeep = 32.0;

} // namespace

// ==============================================================================
// Construction
// ==============================================================================
PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(480, 360);
    setMouseTracking(false);   // we only need drags-with-button-down
    setAttribute(Qt::WA_StyledBackground, false);
    setFocusPolicy(Qt::ClickFocus);
}

// ==============================================================================
// Public API — image state
// ==============================================================================
void PreviewWidget::setOriginalImage(const QImage& image)
{
    if (m_originalImage.cacheKey() == image.cacheKey()) return;
    m_originalImage = image;
    update();
}

void PreviewWidget::setEditedImage(const QImage& image)
{
    if (m_editedImage.cacheKey() == image.cacheKey()) return;
    m_editedImage = image;
    update();
}

void PreviewWidget::setShowOriginal(bool showOriginal)
{
    if (m_showOriginal == showOriginal) return;
    m_showOriginal = showOriginal;
    update();
}

bool PreviewWidget::hasImage() const
{
    return !m_originalImage.isNull() || !m_editedImage.isNull();
}

// ==============================================================================
// Public API — zoom modes
// ==============================================================================
void PreviewWidget::zoomToFit()
{
    m_fitMode = true;
    // Pan offset is recomputed in paint when in fit mode, but we reset it
    // here so any subsequent toggle to 100% starts from a centered position
    // rather than a stale free-mode offset.
    m_panOffset = QPointF(width() * 0.5, height() * 0.5);
    update();
}

void PreviewWidget::zoomTo100()
{
    m_fitMode = false;
    m_userScale = 1.0;
    m_panOffset = QPointF(width() * 0.5, height() * 0.5);
    update();
}

// Multiplicative zoom anchored at the widget center. Used by menu items
// and shortcuts. Cursor-anchored zoom is the wheel handler's job; this
// version doesn't have a cursor position to work with.
void PreviewWidget::zoomBy(double factor)
{
    if (!hasImage() || !std::isfinite(factor) || factor <= 0.0) return;
    if (m_fitMode) {
        m_userScale = effectiveScale();
        m_fitMode = false;
    }
    m_userScale = std::clamp(m_userScale * factor, kMinUserScale, kMaxUserScale);
    // Center anchoring: image-space point under the view center stays at
    // the view center. With pan = view-center, that's automatic — no math.
    m_panOffset = QPointF(width() * 0.5, height() * 0.5);
    update();
}

// ==============================================================================
// Color sampling
//
// The Targeted Color Adjustment tool reads pixels directly from the
// cached image. No render trigger — this is a pure observation. The
// spec is explicit on this: "Do NOT trigger render when sampling pixel."
//
// We always sample the EDITED image (m_editedImage) regardless of which
// is currently displayed. This matches the spec's "Sample pixel from
// m_processed image, NOT the original," and avoids the surprise of
// "what you click isn't what you get" when before/after is showing
// the original.
// ==============================================================================
void PreviewWidget::setColorSamplingActive(bool on)
{
    if (m_colorSamplingActive == on) return;
    m_colorSamplingActive = on;

    // Cursor change is purely visual feedback. Crosshair when active, no
    // explicit cursor when inactive (so the platform default + any
    // pan-drag handler can claim it). We only claim/un-claim our cursor
    // when not currently mid-pan, since a pan in progress installs its
    // own ClosedHandCursor that should remain.
    if (m_panButton == Qt::NoButton) {
        if (on) setCursor(Qt::CrossCursor);
        else    unsetCursor();
    }
}

QColor PreviewWidget::sampleAt(const QPoint& imagePos, bool* valid) const
{
    auto setValid = [valid](bool v) { if (valid) *valid = v; };

    // Prefer the edited image (post-render output). Fall back to the
    // original only if the edited cache is null — useful in transient
    // states where a render hasn't completed yet.
    const QImage& src = !m_editedImage.isNull() ? m_editedImage : m_originalImage;
    if (src.isNull()) { setValid(false); return Qt::transparent; }

    if (imagePos.x() < 0 || imagePos.y() < 0 ||
        imagePos.x() >= src.width() || imagePos.y() >= src.height()) {
        setValid(false);
        return Qt::transparent;
    }

    setValid(true);
    return QColor(src.pixel(imagePos));
}

// ==============================================================================
// Zoom label
//
// Returns "Fit" in fit mode (regardless of what the computed fit scale is).
// In free mode returns the current scale rounded to whole percent — matches
// the labels in the spec ("25%", "50%", "100%", "200%") without forcing the
// user's scale to land on those exact values.
// ==============================================================================
QString PreviewWidget::zoomLabelText() const
{
    if (m_fitMode) return QStringLiteral("Fit");
    const int pct = static_cast<int>(std::lround(effectiveScale() * 100.0));
    return QString::number(pct) + QLatin1Char('%');
}

// ==============================================================================
// Coordinate conversion
//
// effectiveScale() returns the scale actually applied to image-pixel
// distances when computing widget coords. In fit mode this is the ratio
// that makes the image just fit inside the widget; in free mode it's
// m_userScale clamped.
// ==============================================================================
const QImage& PreviewWidget::displayImage() const
{
    // Fall back to whichever image we have if the requested side is null.
    // This matters during the brief window between "first render finished"
    // and "user holds Spacebar" — m_originalImage might be set before
    // m_editedImage or vice versa depending on which path runs first.
    if (m_showOriginal) {
        return !m_originalImage.isNull() ? m_originalImage : m_editedImage;
    }
    return !m_editedImage.isNull() ? m_editedImage : m_originalImage;
}

double PreviewWidget::computeFitScale() const
{
    const QImage& img = displayImage();
    if (img.isNull()) return 1.0;
    const double iw = img.width();
    const double ih = img.height();
    if (iw <= 0.0 || ih <= 0.0) return 1.0;
    const double sx = static_cast<double>(width())  / iw;
    const double sy = static_cast<double>(height()) / ih;
    return std::min(sx, sy);
}

double PreviewWidget::effectiveScale() const
{
    if (m_fitMode) return computeFitScale();
    return std::clamp(m_userScale, kMinUserScale, kMaxUserScale);
}

QPointF PreviewWidget::imageCenter() const
{
    const QImage& img = displayImage();
    if (img.isNull()) return QPointF(0, 0);
    return QPointF(img.width() * 0.5, img.height() * 0.5);
}

QPointF PreviewWidget::imageToWidget(const QPointF& imagePoint) const
{
    const double s = effectiveScale();
    const QPointF c = imageCenter();
    const QPointF off = m_fitMode
        ? QPointF(width() * 0.5, height() * 0.5)
        : m_panOffset;
    return QPointF((imagePoint.x() - c.x()) * s + off.x(),
                   (imagePoint.y() - c.y()) * s + off.y());
}

QPointF PreviewWidget::widgetToImage(const QPointF& widgetPoint) const
{
    const double s = effectiveScale();
    if (s <= 0.0) return QPointF(0, 0);
    const QPointF c = imageCenter();
    const QPointF off = m_fitMode
        ? QPointF(width() * 0.5, height() * 0.5)
        : m_panOffset;
    return QPointF((widgetPoint.x() - off.x()) / s + c.x(),
                   (widgetPoint.y() - off.y()) / s + c.y());
}

QRectF PreviewWidget::imageRectInWidget() const
{
    const QImage& img = displayImage();
    if (img.isNull()) return QRectF();
    const QPointF tl = imageToWidget(QPointF(0, 0));
    const QPointF br = imageToWidget(QPointF(img.width(), img.height()));
    return QRectF(tl, br);
}

// Clamp pan so the image doesn't slide entirely off-screen. We allow the
// user to push the image to where only a small strip remains visible at
// any edge, but no further. In fit mode this isn't called (pan is
// recomputed per paint).
void PreviewWidget::clampPanOffset()
{
    if (m_fitMode) return;
    const QRectF imgRect = imageRectInWidget();
    if (imgRect.isEmpty()) return;

    const double minX = -imgRect.width() * 0.5 + kPanEdgeKeep;
    const double maxX =  width()  + imgRect.width() * 0.5 - kPanEdgeKeep;
    const double minY = -imgRect.height() * 0.5 + kPanEdgeKeep;
    const double maxY =  height() + imgRect.height() * 0.5 - kPanEdgeKeep;

    // The constraint is on m_panOffset (image center). Image center =
    // panOffset, so we clamp panOffset directly. If the image is smaller
    // than the widget, min > max — in that case just center it.
    if (minX > maxX) m_panOffset.setX(width()  * 0.5);
    else             m_panOffset.setX(std::clamp(m_panOffset.x(), minX, maxX));
    if (minY > maxY) m_panOffset.setY(height() * 0.5);
    else             m_panOffset.setY(std::clamp(m_panOffset.y(), minY, maxY));
}

// ==============================================================================
// Painting
//
// Three layers:
//   1. Background fill (even when no image, so the widget reads as a
//      photographic surface, not a hole).
//   2. The displayed image, scaled and translated. Smooth transform
//      below 100%, nearest-neighbor at 100%+.
//   3. Zoom-label chip in the top-left corner (skipped if no image).
// ==============================================================================
void PreviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(32, 32, 34));

    const QImage& img = displayImage();
    if (img.isNull()) return;

    const double s = effectiveScale();
    p.setRenderHint(QPainter::SmoothPixmapTransform,
                    s < kPixelDoublingThreshold);

    const QRectF target = imageRectInWidget();
    p.drawImage(target, img, QRectF(img.rect()));

    // Zoom-label chip. Top-left corner, small rounded rect with text.
    QFont chipFont = font();
    chipFont.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(chipFont);
    const QString label = zoomLabelText();
    const QFontMetrics fm(chipFont);
    const int padX = 8;
    const int padY = 3;
    const QSize textSize = fm.size(0, label);
    const QRectF chip(8, 8, textSize.width() + padX * 2, textSize.height() + padY * 2);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 140));
    p.drawRoundedRect(chip, 4, 4);

    p.setPen(QColor(220, 220, 225));
    p.drawText(chip, Qt::AlignCenter, label);
}

// ==============================================================================
// Resize
//
// Fit mode: nothing to do — paint recomputes the fit scale from the new
// widget size. Free mode: recenter the pan if the image's anchor is now
// off-screen, otherwise leave it alone (don't yank the user's view around
// just because the window resized).
// ==============================================================================
void PreviewWidget::resizeEvent(QResizeEvent* event)
{
    if (!m_fitMode) clampPanOffset();
    QWidget::resizeEvent(event);
}

// ==============================================================================
// Mouse — wheel zoom (Ctrl+wheel only)
//
// Other wheel events (no Ctrl) fall through to the base class. This lets
// future scroll-style behavior (e.g. scroll up the panel) coexist if a
// scrollable parent is added. Today there's no parent that does anything
// with wheel events, so unmodified wheels are inert.
//
// Cursor anchoring: compute the image-space point under the cursor BEFORE
// the zoom change, then after applying the new scale, adjust pan so the
// same image point is still under the cursor.
// ==============================================================================
void PreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        event->ignore();
        return;
    }
    if (displayImage().isNull()) { event->ignore(); return; }

    const QPointF cursor = event->position();
    const QPointF imagePosUnderCursor = widgetToImage(cursor);

    // angleDelta().y() is in 1/8-degree units, typically ±120 per notch.
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) { event->ignore(); return; }

    // Switch into free mode at current effective scale, then apply notches.
    // This way Ctrl+wheel from Fit mode starts at fit-scale and zooms from
    // there, instead of jumping to some unrelated user scale.
    if (m_fitMode) {
        m_userScale = effectiveScale();
        m_fitMode = false;
    }
    const double factor = std::pow(kWheelZoomFactor, notches);
    m_userScale = std::clamp(m_userScale * factor, kMinUserScale, kMaxUserScale);

    // Re-anchor: pan offset must move so imageToWidget(imagePosUnderCursor)
    // still equals cursor.
    //   imageToWidget(p) = (p - center) * s + panOffset
    // Solve for panOffset given target == cursor:
    //   panOffset = cursor - (p - center) * s
    const QPointF c = imageCenter();
    m_panOffset = cursor - QPointF((imagePosUnderCursor.x() - c.x()) * effectiveScale(),
                                    (imagePosUnderCursor.y() - c.y()) * effectiveScale());
    clampPanOffset();
    update();
    event->accept();
}

// ==============================================================================
// Mouse — pan
//
// Pan triggers: middle button, OR Alt + left button. Other buttons are
// ignored (and pass to the base class so child widgets / event filters
// still see them).
//
// Pan implicitly switches to free mode at current effective scale — same
// reasoning as wheel zoom, so the first pan from Fit doesn't snap to some
// unrelated zoom level.
// ==============================================================================
namespace {
inline bool isPanPress(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton) return true;
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) return true;
    return false;
}
}

void PreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (!hasImage()) { QWidget::mousePressEvent(event); return; }

    // Pan gestures (middle-button or Alt+left) win over sampling — even
    // when sampling is active, the user may want to reposition before
    // their next sample. Sampling intercepts only plain left-click.
    if (isPanPress(event)) {
        if (m_fitMode) {
            m_userScale = effectiveScale();
            m_fitMode = false;
        }
        m_panButton = event->button();
        m_lastDragPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // Color-sampling intercept. Only fires for plain left-click (modifiers
    // empty); modifier+left combinations are reserved for future tools or
    // for the pan path above.
    if (m_colorSamplingActive && event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier) {
        // Map widget coordinates → image coordinates via the existing
        // pan/zoom transform. The result may be outside image bounds (if
        // the user clicked the letterbox area around a fit-mode image);
        // sampleAt returns valid=false in that case and we silently
        // ignore — spec rule: "If click occurs outside image bounds:
        // ignore click safely."
        const QPointF imageF = widgetToImage(event->position());
        const QPoint imagePos(static_cast<int>(std::floor(imageF.x())),
                              static_cast<int>(std::floor(imageF.y())));
        bool valid = false;
        const QColor c = sampleAt(imagePos, &valid);
        if (valid) {
            emit colorSampled(c, imagePos);
        }
        // We accept the click either way — a click in letterbox area
        // shouldn't fall through to other handlers (e.g. the parent's
        // context menu handler, though right-click is what triggers that).
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panButton != Qt::NoButton) {
        const QPoint delta = event->pos() - m_lastDragPos;
        m_lastDragPos = event->pos();
        m_panOffset += QPointF(delta);
        clampPanOffset();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == m_panButton) {
        m_panButton = Qt::NoButton;
        // Restore the right cursor: crosshair if sampling is still active,
        // platform default otherwise. Without this, pan-release while in
        // sampling mode would drop us back to the default cursor and the
        // user would lose the visual cue.
        if (m_colorSamplingActive) setCursor(Qt::CrossCursor);
        else                       unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

// ==============================================================================
// Double-click — toggle Fit ↔ 100%
// ==============================================================================
void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !hasImage()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (m_fitMode) {
        // Anchor the 100% view so the image pixel under the cursor stays
        // there. Same trick as the wheel handler.
        const QPointF cursor = event->position();
        const QPointF imagePosUnderCursor = widgetToImage(cursor);
        m_fitMode = false;
        m_userScale = 1.0;
        const QPointF c = imageCenter();
        m_panOffset = cursor - QPointF((imagePosUnderCursor.x() - c.x()) * effectiveScale(),
                                        (imagePosUnderCursor.y() - c.y()) * effectiveScale());
        clampPanOffset();
    } else {
        zoomToFit();
    }
    update();
    event->accept();
}
