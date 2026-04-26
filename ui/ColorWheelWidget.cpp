// ==============================================================================
// ui/ColorWheelWidget.cpp
// ==============================================================================
#include "ColorWheelWidget.h"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace {

// Inner radius (fraction of full radius) below which clicks are
// treated as "reset to sat=0" rather than as a normal drag. Keeps
// users from getting stuck at sat=0.001 from mis-clicks at center.
constexpr float kCenterDeadZoneFrac = 0.04f;

// Accent color drawn around the puck when it's the active (selected)
// wheel. Brand color — same #CCFF00 used elsewhere in the UI.
const QColor kAccentColor(0xCC, 0xFF, 0x00);

// Convert HSV (h ∈ [0,360), s,v ∈ [0,1]) to QColor. We do this per-pixel
// during the cache rebuild; QColor::fromHsvF is slightly faster than
// rolling our own and produces sRGB-encoded values which is what we
// want for display.
inline QColor hsvToQColor(float h, float s, float v)
{
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    return QColor::fromHsvF(h / 360.0f,
                             std::clamp(s, 0.0f, 1.0f),
                             std::clamp(v, 0.0f, 1.0f));
}

} // namespace

ColorWheelWidget::ColorWheelWidget(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::ClickFocus);
}

void ColorWheelWidget::setHueSaturation(float hueDeg, float sat01)
{
    // Wrap hue to [0, 360); clamp sat to [0, 1].
    hueDeg = std::fmod(hueDeg, 360.0f);
    if (hueDeg < 0.0f) hueDeg += 360.0f;
    sat01 = std::clamp(sat01, 0.0f, 1.0f);

    if (std::fabs(m_hue - hueDeg) < 1e-3f && std::fabs(m_sat - sat01) < 1e-3f) {
        return;
    }
    m_hue = hueDeg;
    m_sat = sat01;
    update();
    // No signal — programmatic update.
}

QPointF ColorWheelWidget::wheelCenter() const noexcept
{
    return QPointF(width() * 0.5, height() * 0.5);
}

float ColorWheelWidget::wheelRadius() const noexcept
{
    // Leave a few pixels of margin so the puck and accent ring don't
    // clip against the widget edge.
    return std::max(1.0f, std::min(width(), height()) * 0.5f - 4.0f);
}

bool ColorWheelWidget::pointToHueSat(const QPointF& p,
                                     float& outHue,
                                     float& outSat) const
{
    const QPointF c = wheelCenter();
    const double dx = p.x() - c.x();
    const double dy = p.y() - c.y();
    const double dist = std::sqrt(dx*dx + dy*dy);
    const double r = wheelRadius();

    if (dist < r * kCenterDeadZoneFrac) {
        // Center dead-zone: report sat=0 and keep the previous hue.
        outHue = m_hue;
        outSat = 0.0f;
        return true;
    }

    // atan2 returns angle in radians, +X axis is 0, increasing
    // counter-clockwise. Convert to degrees in [0, 360).
    // Note: Qt's y-axis points DOWN, but HSV's hue convention is for a
    // standard math y-up. So we negate dy when computing the angle to
    // get the user-expected "red right, green up, blue down-left" layout.
    double angle = std::atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
    if (angle < 0.0) angle += 360.0;

    outHue = static_cast<float>(angle);
    outSat = static_cast<float>(std::min(dist / r, 1.0));
    return true;
}

void ColorWheelWidget::resizeEvent(QResizeEvent* /*event*/)
{
    rebuildWheelCache();
}

void ColorWheelWidget::rebuildWheelCache()
{
    const int W = width();
    const int H = height();
    if (W <= 0 || H <= 0) {
        m_wheelCache = QImage();
        return;
    }

    // Pre-multiplied for fast blit. ARGB32 is fine — this is a small
    // image (~120x120 typical) so the format choice doesn't matter
    // for performance.
    QImage img(W, H, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    const QPointF c = wheelCenter();
    const double r = wheelRadius();

    for (int y = 0; y < H; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        const double dy = y - c.y();
        for (int x = 0; x < W; ++x) {
            const double dx = x - c.x();
            const double dist = std::sqrt(dx*dx + dy*dy);
            if (dist > r) {
                row[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            // Anti-alias the rim: 1px feather to avoid jaggies.
            int alpha = 255;
            if (dist > r - 1.0) {
                alpha = static_cast<int>(255 * (r - dist));
                if (alpha < 0) alpha = 0;
                if (alpha > 255) alpha = 255;
            }

            double angle = std::atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
            if (angle < 0.0) angle += 360.0;
            const double sat = std::min(dist / r, 1.0);

            const QColor c2 = hsvToQColor(static_cast<float>(angle),
                                           static_cast<float>(sat), 1.0f);
            row[x] = qRgba(c2.red(), c2.green(), c2.blue(), alpha);
        }
    }
    m_wheelCache = std::move(img);
}

void ColorWheelWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background: dark surface tile so the wheel sits on a card-like
    // panel rather than on the raw widget background.
    const QPointF c = wheelCenter();
    const float   r = wheelRadius();

    // Outer ring shadow / inset look — subtle.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(24, 24, 26));
    p.drawEllipse(c, r + 3.0, r + 3.0);

    // The cached wheel.
    if (!m_wheelCache.isNull()) {
        p.drawImage(0, 0, m_wheelCache);
    }

    // Outer rim line — subtle but defines the boundary clearly.
    p.setPen(QPen(QColor(0, 0, 0, 120), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);

    // Puck position. sat=0 → at center; sat=1 → at rim.
    const double angleRad = m_hue * 3.14159265358979323846 / 180.0;
    const QPointF puckPos(
        c.x() + r * m_sat * std::cos(angleRad),
        c.y() - r * m_sat * std::sin(angleRad));

    // Puck: dark inner circle with accent ring. Size scales slightly with
    // the wheel so it stays visible on small wheels.
    const double puckR = std::max(4.0, r * 0.08);

    // Accent ring (brand color).
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kAccentColor, 2.0));
    p.drawEllipse(puckPos, puckR, puckR);

    // Inner dark dot for contrast against the wheel background.
    p.setPen(QPen(QColor(20, 20, 22), 1.0));
    p.setBrush(QColor(255, 255, 255, 200));
    p.drawEllipse(puckPos, puckR * 0.45, puckR * 0.45);

    // Optional center crosshair — helps users locate sat=0 visually.
    p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
    p.drawLine(QPointF(c.x() - 3, c.y()), QPointF(c.x() + 3, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - 3), QPointF(c.x(), c.y() + 3));
}

void ColorWheelWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Only intercept clicks inside the wheel circle. Outside clicks
    // fall through to the parent (which lets the user click panel
    // backgrounds without dragging the puck off-screen).
    const QPointF c = wheelCenter();
    const QPointF p = event->position();
    const double dx = p.x() - c.x();
    const double dy = p.y() - c.y();
    if (dx*dx + dy*dy > wheelRadius() * wheelRadius()) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging = true;
    emit dragStarted();

    float h, s;
    if (pointToHueSat(p, h, s)) {
        m_hue = h;
        m_sat = s;
        emit hueSaturationChanged(m_hue, m_sat);
        update();
    }
    event->accept();
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) { QWidget::mouseMoveEvent(event); return; }

    float h, s;
    if (pointToHueSat(event->position(), h, s)) {
        m_hue = h;
        m_sat = s;
        emit hueSaturationChanged(m_hue, m_sat);
        update();
    }
    event->accept();
}

void ColorWheelWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ColorWheelWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click anywhere → reset request. Caller decides semantics
    // (typically: hue=0, sat=0) and pushes the undo snapshot.
    if (event->button() == Qt::LeftButton) {
        emit resetRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
