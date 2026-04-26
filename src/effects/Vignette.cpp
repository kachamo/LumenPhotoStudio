// ==============================================================================
// effects/Vignette.cpp
//
// In linear light, vignette becomes a true exposure-like multiplier — a
// clean ×0.5 at the corners looks like -1 stop of light, matching what a
// physical vignette does. In sRGB space, the same multiplier would crush
// shadows non-linearly and look muddy.
//
// Roundness not yet implemented (stays planned for future step).
// ==============================================================================
#include "effects/Vignette.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <algorithm>
#include <cmath>

namespace lps {

void Vignette::apply(PixelBuffer& buffer, const VignetteParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const int w = buffer.width();
    const int h = buffer.height();
    if (w <= 0 || h <= 0) return;

    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float maxDist = std::sqrt(cx * cx + cy * cy);   // half-diagonal

    const float inner   = math::clamp(params.midpoint / 100.0f, 0.0f, 0.99f);
    const float feather = math::clamp(params.feather / 100.0f, 0.0f, 1.0f);
    const float outer   = std::min(1.0f, inner + feather * (1.0f - inner) + 0.01f);

    // amt < 0 darkens (multiplier < 1); amt > 0 brightens (> 1).
    const float amt = params.amount / 100.0f;

    forEachScanline(buffer, [&](float* row, int y) {
        const float dy = (static_cast<float>(y) - cy) / maxDist;
        for (int x = 0; x < w; ++x) {
            const float dx = (static_cast<float>(x) - cx) / maxDist;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float t = math::smoothstep(inner, outer, d);
            const float m = 1.0f + amt * t;

            float* p = row + x * 4;
            p[0] *= m;
            p[1] *= m;
            p[2] *= m;
        }
    });
}

} // namespace lps
