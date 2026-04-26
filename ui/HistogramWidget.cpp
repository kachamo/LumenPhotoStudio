// ==============================================================================
// ui/HistogramWidget.cpp
// ==============================================================================
#include "HistogramWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Fraction of pixels at bin 0 (or bin 255) above which we light the
// shadow (or highlight) clipping indicator. 0.5% catches obvious crushing
// without lighting up at the slightest bit of true black, which is fine
// in normal photographic content.
constexpr double kClippingThreshold = 0.005;

// Robust-peak percentile. We sort the bin counts and use the count at this
// percentile as the y-axis ceiling, so a single dominating bin doesn't
// visually crush everything else flat. 99th percentile = ignore the top ~3
// bins of a 256-bin histogram when finding the visual peak.
constexpr double kPeakPercentile = 0.99;

// Margin inside the widget bounds for the histogram graph itself, leaving
// room for the frame and the small clipping indicators.
constexpr int kMarginX = 6;
constexpr int kMarginY = 6;

// Rec.709 luminance weights (matches the engine's ColorMath constants).
constexpr double kLumaR = 0.2126;
constexpr double kLumaG = 0.7152;
constexpr double kLumaB = 0.0722;

// Luminance from 0..255 sRGB-encoded channels. Returns int in [0, 255].
// Note: this is luminance computed in sRGB (gamma-encoded) space, which is
// the right thing for a histogram display — humans perceive sRGB-luminance
// as "brightness," not the linear-light luminance. Most photo editors do
// this in sRGB for the same reason.
inline int srgbLuminance(int r, int g, int b)
{
    const double y = kLumaR * r + kLumaG * g + kLumaB * b;
    if (y <= 0.0)   return 0;
    if (y >= 255.0) return 255;
    return static_cast<int>(y + 0.5);
}

} // namespace

// ==============================================================================
// Construction
// ==============================================================================
HistogramWidget::HistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(72);
}

// ==============================================================================
// Bin computation
//
// One pass over the source pixels with a stride that brings the total
// sample count under kMaxSamples regardless of image size. This is "row
// stride * column stride" sampling — both axes get coarsened together so
// the spatial coverage stays uniform.
//
// Bin storage: int per bin. Even at the maximum sample count, no bin can
// overflow int (2.5×10⁵ << 2³¹). No need for 64-bit counters.
// ==============================================================================
void HistogramWidget::setImage(const QImage& image)
{
    // Clear cached state up front. If the image is null or empty we'll keep
    // the cleared state and paint an empty histogram.
    m_binsR.fill(0);
    m_binsG.fill(0);
    m_binsB.fill(0);
    m_binsY.fill(0);
    m_peak = 0;
    m_clippedShadows = false;
    m_clippedHighlights = false;
    m_sampleCount = 0;

    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        update();
        return;
    }

    // Convert if necessary so we can do a single 4-byte stride read per
    // pixel. Almost all images coming through ImagePipeline are already
    // ARGB32 so this is usually a no-op (QImage::convertToFormat with the
    // same format is cheap).
    QImage src = (image.format() == QImage::Format_ARGB32 ||
                  image.format() == QImage::Format_RGB32)
        ? image
        : image.convertToFormat(QImage::Format_ARGB32);

    const int W = src.width();
    const int H = src.height();
    const long long total = static_cast<long long>(W) * H;

    // Compute per-axis stride so total samples ≈ kMaxSamples. We apply the
    // same factor to both axes (square root of the total ratio) so spatial
    // coverage stays uniform — sampling every other column but every row
    // would over-represent vertical structure.
    int stride = 1;
    if (total > kMaxSamples) {
        const double ratio = static_cast<double>(total) / kMaxSamples;
        stride = std::max(1, static_cast<int>(std::sqrt(ratio)));
    }

    for (int y = 0; y < H; y += stride) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < W; x += stride) {
            const QRgb p = row[x];
            const int r = qRed(p);
            const int g = qGreen(p);
            const int b = qBlue(p);
            ++m_binsR[r];
            ++m_binsG[g];
            ++m_binsB[b];
            ++m_binsY[srgbLuminance(r, g, b)];
            ++m_sampleCount;
        }
    }

    if (m_sampleCount == 0) { update(); return; }

    // ---- Robust peak ------------------------------------------------------
    // Take the largest bin count across all four channels for each bin
    // index, then pick the value at kPeakPercentile of the sorted result.
    // This gives a y-axis ceiling that ignores 1-2 dominating bins and
    // shows real structure in everything else.
    std::array<int, kBinCount> maxPerBin{};
    for (int i = 0; i < kBinCount; ++i) {
        maxPerBin[i] = std::max({ m_binsR[i], m_binsG[i], m_binsB[i], m_binsY[i] });
    }
    std::array<int, kBinCount> sorted = maxPerBin;
    std::sort(sorted.begin(), sorted.end());
    const int idx = std::min(kBinCount - 1,
                             static_cast<int>(kPeakPercentile * (kBinCount - 1)));
    m_peak = sorted[idx];
    if (m_peak <= 0) m_peak = 1;   // avoid divide-by-zero in paint

    // ---- Clipping detection ----------------------------------------------
    // Count pixels at bin 0 (any channel) and bin 255 (any channel). The
    // worst-channel rule is: if R, G, or B is fully crushed/blown, the
    // pixel has lost detail — even if the other channels are fine.
    const int clipShadowsCount = std::max({ m_binsR[0],   m_binsG[0],   m_binsB[0] });
    const int clipHighsCount   = std::max({ m_binsR[255], m_binsG[255], m_binsB[255] });
    const double thresh = kClippingThreshold * m_sampleCount;
    m_clippedShadows    = (clipShadowsCount > thresh);
    m_clippedHighlights = (clipHighsCount   > thresh);

    update();
}

// ==============================================================================
// Painting
//
// Layered draw:
//   1. Background fill
//   2. Subtle vertical grid (at 25/50/75% of the x range)
//   3. R, G, B bin curves with additive blending — overlap regions self-sum
//      to white, matching photographic histogram convention
//   4. Luminance overlay (faint white, on top)
//   5. Clipping indicator triangles at the L/R edges if applicable
//   6. 1px frame around the graph
//
// Per-x-pixel sampling: instead of mapping bins to pixels 1:1 (which would
// alias when the widget is wider or narrower than 256px), each output x
// covers a range of bins and uses the max count in that range. Always
// produces a clean curve regardless of widget size.
// ==============================================================================
void HistogramWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // ---- Background -------------------------------------------------------
    p.fillRect(rect(), QColor(20, 20, 22));

    const QRect graph = rect().adjusted(kMarginX, kMarginY, -kMarginX, -kMarginY);
    if (graph.width() <= 1 || graph.height() <= 1) return;

    // ---- Subtle grid: vertical lines at 25/50/75% --------------------------
    p.setPen(QPen(QColor(48, 48, 52), 1.0, Qt::DotLine));
    for (int i = 1; i < 4; ++i) {
        const double t = i / 4.0;
        const int x = graph.left() + static_cast<int>(t * graph.width());
        p.drawLine(x, graph.top(), x, graph.bottom());
    }

    // ---- Build per-x-pixel bin maxima ------------------------------------
    // For each output x in [0, graph.width()), determine the bin range
    // that maps to it, and take the max count in that range. We do this
    // for each of the four channels separately so the additive blending
    // works correctly (each layer is drawn on its own).
    auto perPixel = [&](const std::array<int, kBinCount>& bins) {
        const int W = graph.width();
        std::vector<int> out(W, 0);
        // Bin index that maps to output pixel x:
        //   binStart = floor(x * 256 / W)
        //   binEnd   = floor((x+1) * 256 / W)
        for (int x = 0; x < W; ++x) {
            const int b0 = (x       * kBinCount) / W;
            const int b1 = std::min(kBinCount, ((x + 1) * kBinCount + W - 1) / W);
            int hi = 0;
            for (int b = b0; b < b1; ++b) {
                if (bins[b] > hi) hi = bins[b];
            }
            out[x] = hi;
        }
        return out;
    };

    auto fillCurve = [&](const std::vector<int>& counts, QColor color, bool additive) {
        QPainterPath path;
        path.moveTo(graph.left(), graph.bottom() + 1);
        for (int x = 0; x < graph.width(); ++x) {
            const double n = std::min(1.0, static_cast<double>(counts[x]) / m_peak);
            const int yPx = graph.bottom() - static_cast<int>(n * graph.height());
            path.lineTo(graph.left() + x, yPx);
        }
        path.lineTo(graph.right(), graph.bottom() + 1);
        path.closeSubpath();

        if (additive) {
            // CompositionMode_Plus sums the source and destination colors
            // per channel (clamped to 255). Three channels stacked sum to
            // white where they all overlap — exactly how photo histograms
            // are conventionally displayed.
            p.setCompositionMode(QPainter::CompositionMode_Plus);
        } else {
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(path);
    };

    if (m_sampleCount > 0) {
        const auto rPx = perPixel(m_binsR);
        const auto gPx = perPixel(m_binsG);
        const auto bPx = perPixel(m_binsB);
        const auto yPx = perPixel(m_binsY);

        // Use mid-saturation channel colors so additive overlap reaches
        // a clean light gray rather than blowing out to pure white.
        fillCurve(rPx, QColor(220,  60,  60, 200), /*additive=*/true);
        fillCurve(gPx, QColor( 60, 200,  90, 200), /*additive=*/true);
        fillCurve(bPx, QColor( 80, 130, 230, 200), /*additive=*/true);

        // Luminance: faint white overlay on top, no additive blend.
        fillCurve(yPx, QColor(220, 220, 225,  90), /*additive=*/false);
    }

    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // ---- Clipping indicators ---------------------------------------------
    // Small triangles at the left/right edges of the graph area. Subtle —
    // the indicator is informational, not alarming. Color picked to read
    // clearly on the dark background without adding chart-junk.
    auto drawClipIndicator = [&](bool atRight) {
        const int size = 5;
        const int yMid = graph.top() + size + 1;
        QPolygon tri;
        if (atRight) {
            tri << QPoint(graph.right(),         yMid - size)
                << QPoint(graph.right(),         yMid + size)
                << QPoint(graph.right() - size,  yMid);
        } else {
            tri << QPoint(graph.left(),          yMid - size)
                << QPoint(graph.left(),          yMid + size)
                << QPoint(graph.left() + size,   yMid);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 200, 90, 220));
        p.drawPolygon(tri);
    };
    if (m_clippedShadows)    drawClipIndicator(/*atRight=*/false);
    if (m_clippedHighlights) drawClipIndicator(/*atRight=*/true);

    // ---- Frame -----------------------------------------------------------
    p.setPen(QPen(QColor(58, 58, 63), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(graph);
}
