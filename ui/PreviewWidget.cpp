// ==============================================================================
// ui/PreviewWidget.cpp
// ==============================================================================
#include "PreviewWidget.h"

#include "core/Look.h"
#include "local/MaskGeometry.h"

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
    const bool sizeChanged = m_editedImage.size() != image.size();
    m_editedImage = image;
    if (sizeChanged) invalidateMaskOverlayCache();
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

    // Mask overlay + handles. Painted on top of the image but in image-
    // local widget coordinates, so they scale and pan with the photo.
    // The overlay layer is independent of the image and does not affect
    // the photo pixels — render output / export is unchanged.
    if (m_activeMask && m_showMaskOverlay
        && m_maskViewMode != MaskViewMode::Off) {
        paintMaskOverlay(p, target);
    }
    if (m_activeMask) {
        paintMaskHandles(p);
    }

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

    // Mask handle interception. When an active mask is set and the user
    // left-clicks on one of its handles, start a drag. This wins over
    // color sampling because the visible handles are an explicit user
    // choice; clicking ON one is unambiguous.
    if (m_activeMask && event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier) {
        const int hit = hitTestHandle(event->position());
        if (hit >= 0) {
            m_grabbedHandle = hit;
            // Capture the offset from the click point to the geometry's
            // anchor for "move whole gradient" so the gradient doesn't
            // jump on grab. Store in image-normalized coords.
            const QPointF imgF = widgetToImage(event->position());
            const QImage& img = displayImage();
            if (!img.isNull()) {
                const QPointF normClick(imgF.x() / img.width(),
                                         imgF.y() / img.height());
                m_handleDragOffsetImageCoords = normClick;
            }
            emit maskHandleDragStarted();
            event->accept();
            return;
        }
    }

    // Color-sampling intercept. Only fires for plain left-click (modifiers
    // empty); modifier+left combinations are reserved for future tools or
    // for the pan path above.
    if (m_colorSamplingActive && event->button() == Qt::LeftButton &&
        event->modifiers() == Qt::NoModifier) {
        const QPointF imageF = widgetToImage(event->position());
        const QPoint imagePos(static_cast<int>(std::floor(imageF.x())),
                              static_cast<int>(std::floor(imageF.y())));
        bool valid = false;
        const QColor c = sampleAt(imagePos, &valid);
        if (valid) {
            emit colorSampled(c, imagePos);
        }
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

    // Handle drag in progress.
    if (m_grabbedHandle >= 0 && m_activeMask) {
        applyHandleDrag(m_grabbedHandle, event->position());
        invalidateMaskOverlayCache();
        update();
        emit maskGeometryChanged();
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == m_panButton) {
        m_panButton = Qt::NoButton;
        if (m_colorSamplingActive) setCursor(Qt::CrossCursor);
        else                       unsetCursor();
        event->accept();
        return;
    }
    if (m_grabbedHandle >= 0 && event->button() == Qt::LeftButton) {
        m_grabbedHandle = -1;
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

// ==============================================================================
// Mask overlay
//
// The active mask is rendered as a colored translucent layer over the
// image. Per-pixel mask weight (from local/MaskGeometry.h, the same math
// the engine uses) is converted to an alpha multiplier on the overlay
// color. Result: areas the mask affects are visibly tinted; areas it
// doesn't are clear.
//
// We cache the alpha mask at source-image dimensions so zoom/pan reuse
// the cache — Qt scales it during paint via the same image→widget
// transform as the photo. The cache invalidates when the active mask
// changes, geometry changes (handle drag, slider edit), or image size
// changes (load / rotation / flip).
//
// Overlay is UI-only — never written into m_editedImage, never seen by
// export. The engine continues to render the actual mask effect into
// the photo via LocalAdjustmentEngine; the overlay just shows users
// where the mask hits.
// ==============================================================================
void PreviewWidget::setActiveMask(const lps::LocalAdjustment* mask)
{
    // Always invalidate the cache. Callers may pass the same pointer to
    // signal "the mask's data changed" (e.g. after a slider edit) — we
    // can't tell from the pointer alone whether the underlying data is
    // stale, so a cache rebuild on every call is the safe default. The
    // rebuild itself early-outs on null images / null masks, so the
    // cost when "nothing meaningful changed" is tiny.
    m_activeMask = mask;
    invalidateMaskOverlayCache();
    update();
}

void PreviewWidget::setShowMaskOverlay(bool on)
{
    if (m_showMaskOverlay == on) return;
    m_showMaskOverlay = on;
    update();
}

void PreviewWidget::setMaskOverlayOpacity(float op01)
{
    op01 = std::clamp(op01, 0.0f, 1.0f);
    if (std::fabs(m_maskOverlayAlpha - op01) < 1e-3f) return;
    m_maskOverlayAlpha = op01;
    update();
}

void PreviewWidget::setMaskOverlayColor(const QColor& color)
{
    if (m_maskOverlayColor == color) return;
    m_maskOverlayColor = color;
    update();
}

void PreviewWidget::setMaskViewMode(MaskViewMode mode)
{
    if (m_maskViewMode == mode) return;
    m_maskViewMode = mode;
    update();
}

void PreviewWidget::invalidateMaskOverlayCache()
{
    m_maskOverlayCacheDirty = true;
}

// Build the overlay alpha image at source-image dimensions. Each pixel's
// alpha is set proportional to the mask weight at that image coordinate.
// Cost: ~width × height float ops + a few per-pixel arithmetic ops. At
// 2-megapixel preview, runs in tens of ms — acceptable on geometry-change
// events (which are rate-limited by mouse drag throttling anyway).
void PreviewWidget::rebuildMaskOverlayCache()
{
    m_maskOverlayCacheDirty = false;
    const QImage& src = displayImage();
    if (src.isNull() || !m_activeMask) {
        m_maskOverlayCache = QImage();
        return;
    }

    const int W = src.width();
    const int H = src.height();
    if (W <= 0 || H <= 0) {
        m_maskOverlayCache = QImage();
        return;
    }

    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const double invW = 1.0 / static_cast<double>(W);
    const double invH = 1.0 / static_cast<double>(H);

    const lps::LocalAdjustment& mask = *m_activeMask;
    const float density = std::clamp(mask.density, 0.0f, 1.0f);
    const bool  invert  = mask.invert;
    const bool  bw      = (m_maskViewMode == MaskViewMode::BlackAndWhite);

    // Tint color components, premultiplied with full alpha at the end
    // (per-pixel alpha is the actual modulation).
    int tr = m_maskOverlayColor.red();
    int tg = m_maskOverlayColor.green();
    int tb = m_maskOverlayColor.blue();
    if (bw) { tr = 255; tg = 255; tb = 255; }   // grayscale mode

    for (int y = 0; y < H; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        const double v = (static_cast<double>(y) + 0.5) * invH;
        for (int x = 0; x < W; ++x) {
            const double u = (static_cast<double>(x) + 0.5) * invW;
            float w = lps::maskWeight(QPointF(u, v), mask, aspect);
            if (invert) w = 1.0f - w;
            w *= density;
            if (w <= 0.005f) {
                row[x] = 0;
                continue;
            }
            // Premultiplied: rgb scaled by alpha. Alpha comes from the
            // mask weight directly (range [0, 255]).
            const int a = static_cast<int>(w * 255.0f + 0.5f);
            const int rr = (tr * a) / 255;
            const int gg = (tg * a) / 255;
            const int bb = (tb * a) / 255;
            row[x] = qRgba(rr, gg, bb, a);
        }
    }
    m_maskOverlayCache = std::move(img);
}

void PreviewWidget::paintMaskOverlay(QPainter& p, const QRectF& target)
{
    if (m_maskOverlayCacheDirty) rebuildMaskOverlayCache();
    if (m_maskOverlayCache.isNull()) return;

    p.save();
    p.setOpacity(m_maskOverlayAlpha);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(target, m_maskOverlayCache, QRectF(m_maskOverlayCache.rect()));
    p.restore();
}

// Handle layout per mask type:
//   Linear:  0=start, 1=end, 2=midpoint (move-whole-gradient)
//   Radial:  0=center, 1=radius edge, 2=feather edge
//   Brush:   no handles in V1 (placeholder)
namespace {

// Convert a mask's normalized coord to image-pixel coord.
inline QPointF normToImagePx(const QPointF& norm, const QImage& img)
{
    return QPointF(norm.x() * img.width(), norm.y() * img.height());
}

} // namespace

void PreviewWidget::paintMaskHandles(QPainter& p)
{
    const QImage& img = displayImage();
    if (img.isNull() || !m_activeMask) return;

    const lps::LocalAdjustment& mask = *m_activeMask;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor accent(0xCC, 0xFF, 0x00);
    const QColor shadow(0, 0, 0, 180);
    const float handleR = 6.0f;

    auto drawHandle = [&](const QPointF& widgetPos) {
        p.setPen(QPen(shadow, 2.0));
        p.setBrush(accent);
        p.drawEllipse(widgetPos, handleR, handleR);
    };

    switch (mask.type) {
    case lps::MaskType::LinearGradient: {
        const QPointF s = imageToWidget(normToImagePx(mask.startPoint, img));
        const QPointF e = imageToWidget(normToImagePx(mask.endPoint, img));

        // Connecting line from start to end. Drawn first so handles
        // overlay it.
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5,
                      Qt::DashLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawLine(s, e);

        // Perpendicular guide lines at start and end (the gradient
        // boundaries). Length proportional to viewport extent so they
        // remain visible at any zoom.
        const QPointF dir = e - s;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 1e-3) {
            const QPointF perp(-dir.y() / len, dir.x() / len);
            const double half = std::min<double>(width(), height()) * 0.4;
            p.setPen(QPen(QColor(255, 255, 255, 160), 1.0, Qt::SolidLine));
            p.drawLine(s + perp * half, s - perp * half);
            p.setPen(QPen(QColor(255, 255, 255, 220), 1.5, Qt::SolidLine));
            p.drawLine(e + perp * half, e - perp * half);
        }

        drawHandle(s);
        drawHandle(e);
        // Midpoint handle for "move whole gradient" — drawn slightly
        // smaller so it's distinct from the endpoints.
        p.setPen(QPen(shadow, 2.0));
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse((s + e) * 0.5, handleR * 0.7, handleR * 0.7);
        break;
    }
    case lps::MaskType::RadialGradient: {
        const QPointF c = imageToWidget(normToImagePx(mask.center, img));
        // Radius is normalized fraction of the smaller image edge in
        // image space. To draw on screen, convert to widget coords.
        // We pick a representative direction (right) and use the
        // distance from center to that point as the on-screen radius.
        const float aspect = static_cast<float>(img.width()) /
                              static_cast<float>(img.height());
        // Radial radius is multiplied by aspect ratio handling in the
        // mask weight; for the handle we'll use the unscaled normalized
        // radius along the X-axis as the on-screen circle's reference,
        // matching what the user sees in the overlay.
        Q_UNUSED(aspect);
        const QPointF edgeImage = normToImagePx(
            mask.center + QPointF(mask.radius, 0.0), img);
        const QPointF edgeW = imageToWidget(edgeImage);
        const double r = std::hypot(edgeW.x() - c.x(), edgeW.y() - c.y());

        // Outer circle (mask boundary).
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.5, Qt::SolidLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r);

        // Inner circle (full-strength region — radius * (1 - feather)).
        const double rInner = r * std::max(0.0,
            static_cast<double>(1.0 - std::clamp(mask.feather, 0.0f, 1.0f)));
        if (rInner > 1.0) {
            p.setPen(QPen(QColor(255, 255, 255, 120), 1.0, Qt::DashLine));
            p.drawEllipse(c, rInner, rInner);
        }

        drawHandle(c);
        // Radius handle at "right" edge.
        drawHandle(edgeW);
        // Feather handle on the inner circle, also at the right.
        if (rInner > 1.0) {
            p.setPen(QPen(shadow, 2.0));
            p.setBrush(QColor(255, 255, 255));
            p.drawEllipse(QPointF(c.x() + rInner, c.y()),
                          handleR * 0.7, handleR * 0.7);
        }
        break;
    }
    case lps::MaskType::Brush: {
        // V1 placeholder: a small circle indicator at the brush "anchor"
        // (center if no stamps). Brush painting itself isn't implemented.
        const QPointF c = imageToWidget(normToImagePx(QPointF(0.5, 0.5), img));
        p.setPen(QPen(QColor(255, 255, 255, 180), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 24.0, 24.0);
        break;
    }
    }

    p.restore();
}

// Hit-test handles at the given widget position. Returns the handle
// index (mask-type specific) or -1 for "no handle there." Hit radius
// is generous (12 widget px) for easy grabbing.
int PreviewWidget::hitTestHandle(const QPointF& widgetPos) const
{
    if (!m_activeMask) return -1;
    const QImage& img = displayImage();
    if (img.isNull()) return -1;

    const lps::LocalAdjustment& mask = *m_activeMask;
    const double hitR = 12.0;

    auto hit = [&](const QPointF& q) {
        return std::hypot(widgetPos.x() - q.x(), widgetPos.y() - q.y()) <= hitR;
    };

    switch (mask.type) {
    case lps::MaskType::LinearGradient: {
        const QPointF s = imageToWidget(normToImagePx(mask.startPoint, img));
        const QPointF e = imageToWidget(normToImagePx(mask.endPoint, img));
        if (hit(s)) return 0;
        if (hit(e)) return 1;
        if (hit((s + e) * 0.5)) return 2;
        return -1;
    }
    case lps::MaskType::RadialGradient: {
        const QPointF c = imageToWidget(normToImagePx(mask.center, img));
        const QPointF edge = imageToWidget(
            normToImagePx(mask.center + QPointF(mask.radius, 0.0), img));
        const double r = std::hypot(edge.x() - c.x(), edge.y() - c.y());
        const double rInner = r * std::max(0.0,
            static_cast<double>(1.0 - std::clamp(mask.feather, 0.0f, 1.0f)));
        if (hit(c)) return 0;
        if (hit(edge)) return 1;
        if (rInner > 1.0 && hit(QPointF(c.x() + rInner, c.y()))) return 2;
        return -1;
    }
    case lps::MaskType::Brush:
    default:
        return -1;
    }
}

// Apply a handle drag. Updates the mask geometry in place via the
// non-const cast — m_activeMask is const-typed so external code can't
// accidentally mutate it, but the widget owns the editing intent here
// and pushes geometry changes back to MainWindow via the
// maskGeometryChanged signal (which then refreshes UI / kicks render).
void PreviewWidget::applyHandleDrag(int handleIndex, const QPointF& widgetPos)
{
    if (!m_activeMask) return;
    const QImage& img = displayImage();
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) return;

    const QPointF imgPx = widgetToImage(widgetPos);
    const QPointF norm(imgPx.x() / img.width(), imgPx.y() / img.height());

    // The const_cast is safe here: m_activeMask points into m_look in
    // MainWindow, which is heap-stable and writable. The const tag on
    // the member is purely an external-mutation barrier.
    auto* mask = const_cast<lps::LocalAdjustment*>(m_activeMask);

    switch (mask->type) {
    case lps::MaskType::LinearGradient:
        switch (handleIndex) {
        case 0: mask->startPoint = norm; break;
        case 1: mask->endPoint   = norm; break;
        case 2: {
            // Move whole gradient: translate both start and end by the
            // delta from current midpoint to the cursor.
            const QPointF mid = (mask->startPoint + mask->endPoint) * 0.5;
            const QPointF d   = norm - mid;
            mask->startPoint += d;
            mask->endPoint   += d;
            break;
        }
        default: break;
        }
        break;
    case lps::MaskType::RadialGradient:
        switch (handleIndex) {
        case 0: mask->center = norm; break;
        case 1: {
            // Radius drag — distance from center to cursor in normalized
            // coords. Floor at a tiny minimum to keep math finite.
            const double dx = norm.x() - mask->center.x();
            const double dy = norm.y() - mask->center.y();
            const double r  = std::sqrt(dx*dx + dy*dy);
            mask->radius = std::max(1e-3f, static_cast<float>(r));
            break;
        }
        case 2: {
            // Feather drag — the inner circle handle. Compute current
            // distance and convert to a feather fraction:
            //   innerFrac = innerDist / radius
            //   feather   = 1 - innerFrac
            const double dx = norm.x() - mask->center.x();
            const double dy = norm.y() - mask->center.y();
            const double dist = std::sqrt(dx*dx + dy*dy);
            if (mask->radius > 1e-4f) {
                const float innerFrac = std::clamp(
                    static_cast<float>(dist / mask->radius), 0.0f, 1.0f);
                mask->feather = 1.0f - innerFrac;
            }
            break;
        }
        default: break;
        }
        break;
    case lps::MaskType::Brush:
    default:
        break;
    }
}
