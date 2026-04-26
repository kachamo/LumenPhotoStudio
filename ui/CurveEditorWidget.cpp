// ==============================================================================
// ui/CurveEditorWidget.cpp
// ==============================================================================
#include "CurveEditorWidget.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

// Margin inside the widget bounds. Leaves room for the border strokes and
// stops control points at the edges from being clipped.
constexpr int    kMargin     = 8;

// Pixel radius for click hit-testing against control points.
constexpr double kHitRadius  = 9.0;

// Drawn radius of control points.
constexpr double kPointRadius = 4.5;

// Grid divisions (lines per axis — so "4" means 3 interior lines + 2 edges).
constexpr int    kGridDivs   = 4;

// Minimum horizontal separation (in graph coords) between adjacent points
// during drag. Stops points from visually stacking and keeps the x-sort
// strictly increasing — which is important for the engine's Catmull-Rom
// evaluator downstream.
constexpr double kMinXGap    = 0.005;

// Number of evenly-spaced x samples used to draw the spline preview.
// 256 across [0,1] gives ~one sample per widget pixel at typical sizes —
// imperceptible from a smooth curve when antialiased.
constexpr int    kSplineSamples = 256;

// Clamp helper. Using a local rather than pulling the engine's ColorMath
// in; this widget stays self-contained.
inline double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// ---- Spline evaluation ------------------------------------------------------
//
// Smooth preview using Catmull-Rom-style finite-difference slopes plugged
// into a Hermite cubic — y(x) directly, no parametric inversion.
//
// At each interior control point i, the slope is the standard centered
// finite difference:
//     m_i = (y_{i+1} - y_{i-1}) / (x_{i+1} - x_{i-1})
//
// At the endpoints, we mirror the adjacent segment's slope (one-sided),
// which is equivalent to "phantom point" extrapolation:
//     m_0  = (y_1 - y_0)         / (x_1 - x_0)
//     m_n  = (y_n - y_{n-1})     / (x_n - x_{n-1})
//
// For the segment between i and i+1, with width dx = x_{i+1}-x_i and
// parameter s = (x-x_i)/dx, the Hermite cubic is
//     y(s) = h00·y_i + h10·dx·m_i + h01·y_{i+1} + h11·dx·m_{i+1}
// where the basis functions are
//     h00 = 2s³ - 3s² + 1
//     h10 = s³ - 2s² + s
//     h01 = -2s³ + 3s²
//     h11 = s³ - s²
//
// Sanity check: two-point identity {(0,0),(1,1)} → m_0 = m_1 = 1, dx = 1,
// y(s) = 0 + s³-2s²+s + s²(3-2s) + s³-s² = s. Identity line. ✓
//
// This formulation gives y as an explicit function of x (no parametric
// inversion), is C¹-smooth across segment boundaries, and matches the
// shape users expect from "smooth curve editor" — close enough to true
// centripetal Catmull-Rom for typical photographic curves that no human
// will see the difference, but cheaper to compute.
inline double slopeAt(const std::vector<QPointF>& pts, int i)
{
    const int n = static_cast<int>(pts.size());
    if (n < 2) return 0.0;
    if (i <= 0) {
        // Left endpoint — one-sided slope.
        const double dx = pts[1].x() - pts[0].x();
        return (dx > 0.0) ? (pts[1].y() - pts[0].y()) / dx : 0.0;
    }
    if (i >= n - 1) {
        // Right endpoint — one-sided slope.
        const double dx = pts[n - 1].x() - pts[n - 2].x();
        return (dx > 0.0) ? (pts[n - 1].y() - pts[n - 2].y()) / dx : 0.0;
    }
    // Centered finite difference.
    const double dx = pts[i + 1].x() - pts[i - 1].x();
    return (dx > 0.0) ? (pts[i + 1].y() - pts[i - 1].y()) / dx : 0.0;
}

// Evaluate the smooth curve at x ∈ [0,1]. Output clamped to [0,1].
inline double evaluateSpline(const std::vector<QPointF>& pts, double x)
{
    const int n = static_cast<int>(pts.size());
    if (n == 0) return clamp01(x);
    if (n == 1) return clamp01(pts[0].y());

    // x outside the control-point range: extend with the endpoint y.
    // (kMinXGap enforcement plus locked endpoints means pts[0].x() == 0 and
    // pts[n-1].x() == 1 in practice, so this rarely triggers, but defensively.)
    if (x <= pts[0].x())     return clamp01(pts[0].y());
    if (x >= pts[n - 1].x()) return clamp01(pts[n - 1].y());

    // Find the segment containing x. Linear scan is fine for typical
    // 2-8 point curves; std::upper_bound would only matter at large n.
    int i = 0;
    while (i + 1 < n - 1 && pts[i + 1].x() < x) ++i;

    const double x0 = pts[i].x();
    const double y0 = pts[i].y();
    const double x1 = pts[i + 1].x();
    const double y1 = pts[i + 1].y();
    const double dx = x1 - x0;
    if (dx <= 0.0) return clamp01(y0);   // degenerate guard

    const double s  = (x - x0) / dx;
    const double s2 = s * s;
    const double s3 = s2 * s;

    const double h00 =  2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 =        s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 =        s3 -       s2;

    const double m0 = slopeAt(pts, i);
    const double m1 = slopeAt(pts, i + 1);

    const double y = h00 * y0
                   + h10 * dx * m0
                   + h01 * y1
                   + h11 * dx * m1;
    return clamp01(y);
}

} // namespace

// ==============================================================================
// Construction
// ==============================================================================
CurveEditorWidget::CurveEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(false);     // we only care about buttons-down moves
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(180);
}

// ==============================================================================
// Public API
// ==============================================================================
void CurveEditorWidget::setCurve(lps::CurvePoints* curve)
{
    m_curve = curve;
    m_draggingIndex = -1;
    update();
}

void CurveEditorWidget::setCurveColor(QColor color)
{
    m_curveColor = color;
    update();
}

// ==============================================================================
// Coordinate conversion
//
// graphRect() — the inner rect that represents the [0,1]² graph. Inset by
// kMargin so border strokes + point markers don't get clipped.
// ==============================================================================
QRectF CurveEditorWidget::graphRect() const
{
    return QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

QPointF CurveEditorWidget::graphToWidget(const QPointF& g) const
{
    const QRectF r = graphRect();
    // Flip y: graph y=0 at bottom, widget y=0 at top.
    return { r.left() + g.x() * r.width(),
             r.bottom() - g.y() * r.height() };
}

QPointF CurveEditorWidget::widgetToGraph(const QPointF& w) const
{
    const QRectF r = graphRect();
    if (r.width() <= 0.0 || r.height() <= 0.0) return { 0.0, 0.0 };
    const double gx = (w.x() - r.left())   / r.width();
    const double gy = (r.bottom() - w.y()) / r.height();
    return { clamp01(gx), clamp01(gy) };
}

// ==============================================================================
// Hit testing
// ==============================================================================
int CurveEditorWidget::hitTestPoint(const QPointF& widgetPos) const
{
    if (!m_curve) return -1;
    const auto& pts = m_curve->points;
    int best = -1;
    double bestDist2 = kHitRadius * kHitRadius;
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const QPointF wp = graphToWidget(pts[i]);
        const double dx = wp.x() - widgetPos.x();
        const double dy = wp.y() - widgetPos.y();
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestDist2) {
            best = i;
            bestDist2 = d2;
        }
    }
    return best;
}

bool CurveEditorWidget::isEndpoint(int index) const
{
    if (!m_curve) return true;
    const int n = static_cast<int>(m_curve->points.size());
    return index == 0 || index == n - 1;
}

// ==============================================================================
// Mutations
//
// addPoint: insert into the sorted-by-x vector, clamped to [0,1]. Reject if
//    the new point would be within kMinXGap of an existing neighbor — this
//    avoids stacking points on top of each other and keeps the curve well-
//    defined for the engine's interpolator.
//
// movePoint: during drag, reject x values that would cross neighbors. This
//    produces a "sticky" feel at the neighbor boundary that matches what
//    Lightroom / Photoshop do. Endpoints are pinned entirely — we never
//    call movePoint on them (caller checks isEndpoint first).
//
// deletePoint: endpoints are protected. Rejects if we'd leave fewer than two
//    points, which shouldn't happen given endpoint protection, but belt and
//    suspenders.
// ==============================================================================
bool CurveEditorWidget::addPoint(const QPointF& graphPos)
{
    if (!m_curve) return false;
    auto& pts = m_curve->points;

    const double nx = clamp01(graphPos.x());
    const double ny = clamp01(graphPos.y());

    // Find insertion index: first element with x > nx.
    auto it = std::upper_bound(pts.begin(), pts.end(), nx,
        [](double v, const QPointF& p) { return v < p.x(); });

    // Neighbor-gap check. If a left or right neighbor is within kMinXGap,
    // the new point would be effectively redundant.
    if (it != pts.begin()) {
        auto prev = std::prev(it);
        if (std::abs(prev->x() - nx) < kMinXGap) return false;
    }
    if (it != pts.end()) {
        if (std::abs(it->x() - nx) < kMinXGap) return false;
    }

    const int insertIdx = static_cast<int>(std::distance(pts.begin(), it));
    pts.insert(it, QPointF(nx, ny));

    // After insertion the new point is at insertIdx; return it so the caller
    // can start dragging it immediately.
    m_draggingIndex = insertIdx;
    return true;
}

bool CurveEditorWidget::movePoint(int index, const QPointF& graphPos)
{
    if (!m_curve) return false;
    auto& pts = m_curve->points;
    const int n = static_cast<int>(pts.size());
    if (index < 0 || index >= n) return false;
    if (isEndpoint(index)) return false;   // endpoints locked by spec

    const double leftBound  = pts[index - 1].x() + kMinXGap;
    const double rightBound = pts[index + 1].x() - kMinXGap;

    // If neighbors are already very close, leftBound could exceed rightBound —
    // clamp defensively so x stays valid. In practice this shouldn't happen
    // because addPoint enforces kMinXGap.
    const double nx = (leftBound <= rightBound)
        ? std::clamp(graphPos.x(), leftBound, rightBound)
        : pts[index].x();
    const double ny = clamp01(graphPos.y());

    if (pts[index].x() == nx && pts[index].y() == ny) return false;
    pts[index] = QPointF(nx, ny);
    return true;
}

bool CurveEditorWidget::deletePoint(int index)
{
    if (!m_curve) return false;
    auto& pts = m_curve->points;
    const int n = static_cast<int>(pts.size());
    if (index < 0 || index >= n) return false;
    if (isEndpoint(index)) return false;
    if (n <= 2) return false;

    pts.erase(pts.begin() + index);
    return true;
}

// ==============================================================================
// Mouse events
//
// Undo-boundary signaling: every user-initiated edit operation emits exactly
// one editStarted() before mutation, and exactly one editFinished() after
// the operation completes. MainWindow uses these to capture a single undo
// snapshot per operation, regardless of how many curveChanged() pulses fire
// in between (e.g., during a drag).
//
// Three operation shapes:
//   1. Right-click delete      press → editStarted → mutate → editFinished (same event)
//   2. Left-click add+drag     press → editStarted → add-mutation → drag mutations → release → editFinished
//   3. Left-click grab+drag    press → editStarted → drag mutations → release → editFinished
//   4. Left-click on endpoint  press → (no signals; nothing movable happened)
//
// The m_draggingIndex member tracks whether a drag is in progress so
// mouseReleaseEvent knows whether it owes an editFinished.
//
// Left press: if over an existing movable point, start dragging. Otherwise
//             add a new point at the click location and start dragging it.
// Right press: if over a deletable point, remove it.
// Left move: update the dragged point.
// Left release: end the drag.
// ==============================================================================
void CurveEditorWidget::mousePressEvent(QMouseEvent* event)
{
    if (!m_curve) { QWidget::mousePressEvent(event); return; }

    const QPointF widgetPos = event->position();

    if (event->button() == Qt::RightButton) {
        const int hit = hitTestPoint(widgetPos);
        if (hit >= 0 && !isEndpoint(hit)) {
            // Delete is a complete operation in a single event pair.
            emit editStarted();
            if (deletePoint(hit)) {
                emit curveChanged();
                update();
            }
            emit editFinished();
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int hit = hitTestPoint(widgetPos);
    if (hit >= 0) {
        // Grab existing point — but only if it's movable. No editStarted
        // emission for a press on a locked endpoint: nothing will happen,
        // so there's no undo boundary to mark.
        if (!isEndpoint(hit)) {
            emit editStarted();
            m_draggingIndex = hit;
        }
        event->accept();
        return;
    }

    // Empty area — add a new point and immediately start dragging it.
    // The add is itself the first mutation of the operation; editFinished
    // will fire when the user releases.
    emit editStarted();
    const QPointF g = widgetToGraph(widgetPos);
    if (addPoint(g)) {
        emit curveChanged();
        update();
    } else {
        // addPoint refused (e.g., neighbor-gap). Balance the editStarted
        // immediately so we don't leave a dangling undo boundary open.
        emit editFinished();
    }
    event->accept();
}

void CurveEditorWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingIndex < 0 || !m_curve) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPointF g = widgetToGraph(event->position());
    if (movePoint(m_draggingIndex, g)) {
        emit curveChanged();
        update();
    }
    event->accept();
}

void CurveEditorWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_draggingIndex >= 0) {
        m_draggingIndex = -1;
        emit editFinished();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

// ==============================================================================
// Painting
//
// Draw order (back to front):
//   1. Background fill
//   2. Border
//   3. Grid lines
//   4. Identity diagonal (faint reference)
//   5. Smooth spline curve (Hermite cubic with Catmull-Rom slopes)
//   6. Control points (endpoints dimmer; draggable ones brighter)
//
// The spline preview uses finite-difference slopes at each control point
// fed into a Hermite cubic per segment — see evaluateSpline() in the
// anonymous namespace above for the math. y(x) is direct (no parametric
// inversion). This produces the shape users expect from a "curve editor"
// while remaining a pure preview — the engine renders against the same
// stored points using its own interpolator.
// ==============================================================================
void CurveEditorWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // ---- Background ---------------------------------------------------------
    p.fillRect(rect(), QColor(20, 20, 22));

    const QRectF gr = graphRect();
    if (gr.width() <= 1.0 || gr.height() <= 1.0) return;

    // ---- Border -------------------------------------------------------------
    p.setPen(QPen(QColor(58, 58, 63), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(gr);

    // ---- Grid ---------------------------------------------------------------
    p.setPen(QPen(QColor(48, 48, 52), 1.0, Qt::DotLine));
    for (int i = 1; i < kGridDivs; ++i) {
        const double t = static_cast<double>(i) / kGridDivs;
        // Vertical line at graph x = t.
        const double xw = gr.left() + t * gr.width();
        p.drawLine(QPointF(xw, gr.top()), QPointF(xw, gr.bottom()));
        // Horizontal line at graph y = t.
        const double yw = gr.bottom() - t * gr.height();
        p.drawLine(QPointF(gr.left(), yw), QPointF(gr.right(), yw));
    }

    // ---- Identity diagonal --------------------------------------------------
    // Goes from graph (0,0) to (1,1). Drawn faint so it doesn't compete
    // with the active curve.
    p.setPen(QPen(QColor(80, 80, 86), 1.0, Qt::DashLine));
    p.drawLine(graphToWidget(QPointF(0.0, 0.0)),
               graphToWidget(QPointF(1.0, 1.0)));

    if (!m_curve) return;
    const auto& pts = m_curve->points;
    if (pts.size() < 2) return;

    // ---- Curve (smooth Hermite spline with Catmull-Rom slopes) -------------
    // Sample the function y(x) at kSplineSamples evenly-spaced x values
    // across [0, 1] and stroke a single continuous QPainterPath. The
    // sample density (~1px per sample at typical widget sizes) plus
    // antialiasing produces a visibly smooth curve.
    //
    // Note: this is a PREVIEW. The engine's CurveEngine performs its own
    // interpolation against the same control-point list; the displayed
    // shape will closely resemble the rendered tonal response but the two
    // aren't required to match pixel-for-pixel.
    QPainterPath path;
    {
        const QPointF first = graphToWidget(QPointF(0.0, evaluateSpline(pts, 0.0)));
        path.moveTo(first);
        const double step = 1.0 / (kSplineSamples - 1);
        for (int i = 1; i < kSplineSamples; ++i) {
            const double x = i * step;
            const double y = evaluateSpline(pts, x);
            path.lineTo(graphToWidget(QPointF(x, y)));
        }
    }
    p.setPen(QPen(m_curveColor, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // ---- Control points -----------------------------------------------------
    // Endpoints drawn slightly dimmer to signal "locked." Movable points
    // get the accent color.
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const QPointF wp = graphToWidget(pts[i]);
        const bool endpoint = (i == 0 || i == static_cast<int>(pts.size()) - 1);

        QColor fill = endpoint ? QColor(120, 120, 128) : m_curveColor;
        QColor ring = QColor(12, 12, 14);

        p.setPen(QPen(ring, 1.2));
        p.setBrush(fill);
        p.drawEllipse(wp, kPointRadius, kPointRadius);
    }
}
