// ==============================================================================
// curve/ToneCurve.cpp
// ==============================================================================
#include "curve/ToneCurve.h"

#include "util/ColorMath.h"

#include <cmath>

namespace lps {

static float catmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
    );
}

float ToneCurve::evaluate(const CurvePoints& curve, float x)
{
    x = math::clamp01(x);

    const auto& pts = curve.points;
    const size_t n = pts.size();
    if (n == 0) return x;
    if (n == 1) return static_cast<float>(pts[0].y());

    if (n == 2) {
        const float x0 = static_cast<float>(pts[0].x());
        const float y0 = static_cast<float>(pts[0].y());
        const float x1 = static_cast<float>(pts[1].x());
        const float y1 = static_cast<float>(pts[1].y());
        const float t = (x1 - x0) > 1e-6f ? (x - x0) / (x1 - x0) : 0.0f;
        return math::clamp01(y0 + (y1 - y0) * t);
    }

    size_t i = 0;
    while (i + 1 < n && x > pts[i + 1].x()) ++i;
    if (i + 1 >= n) i = n - 2;

    const size_t i0 = (i == 0) ? 0 : i - 1;
    const size_t i1 = i;
    const size_t i2 = i + 1;
    const size_t i3 = (i + 2 < n) ? i + 2 : n - 1;

    const float x1 = static_cast<float>(pts[i1].x());
    const float x2 = static_cast<float>(pts[i2].x());
    const float t = (x2 - x1) > 1e-6f ? (x - x1) / (x2 - x1) : 0.0f;

    const float y = catmullRom(
        static_cast<float>(pts[i0].y()),
        static_cast<float>(pts[i1].y()),
        static_cast<float>(pts[i2].y()),
        static_cast<float>(pts[i3].y()),
        t);
    return math::clamp01(y);
}

bool ToneCurve::buildLut(const CurvePoints& curve, FloatLut& out)
{
    if (curve.isIdentity()) return false;
    for (int i = 0; i < kLutSize; ++i) {
        const float x = static_cast<float>(i) / (kLutSize - 1);
        out[static_cast<size_t>(i)] = evaluate(curve, x);
    }
    return true;
}

} // namespace lps
