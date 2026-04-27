#include "hdr/HDRToneMapper.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <algorithm>
#include <cmath>

namespace lps {

namespace {

inline float safeChannel(float v)
{
    return std::isfinite(v) && v > 0.0f ? v : 0.0f;
}

float toneMapValue(float x, const HDRParams& p)
{
    x = safeChannel(x);
    if (x <= 0.0f) return 0.0f;

    const float pivot = std::max(0.05f, p.midtonePivot);
    const float compression = p.highlightCompression / 100.0f;
    const float shoulder = p.shoulderStrength / 100.0f;

    const float strength = compression * (0.20f + shoulder * 4.0f);
    if (strength <= 1e-5f) return x;

    const float compressed = x
        * (1.0f + strength * pivot)
        / (1.0f + strength * x);

    const float shoulderStart = pivot;
    const float shoulderEnd = std::max(pivot + 1e-4f,
                                       pivot * (2.0f + (1.0f - shoulder) * 4.0f));
    const float weight = math::smoothstep(shoulderStart, shoulderEnd, x);

    return math::lerp(x, compressed, weight);
}

} // namespace

void HDRToneMapper::apply(PixelBuffer& buffer, const HDRParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const float exposureMul = std::pow(2.0f, params.exposureBias);
    const float preserve = math::clamp(params.saturationPreserve / 100.0f,
                                       0.0f, 1.0f);
    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* px = row + x * 4;

            const float r = safeChannel(px[0] * exposureMul);
            const float g = safeChannel(px[1] * exposureMul);
            const float b = safeChannel(px[2] * exposureMul);

            const float lum = math::luminance(r, g, b);
            const float mappedLum = toneMapValue(lum, params);
            const float scale = lum > 1e-6f ? mappedLum / lum : 0.0f;

            const float lr = r * scale;
            const float lg = g * scale;
            const float lb = b * scale;

            const float cr = toneMapValue(r, params);
            const float cg = toneMapValue(g, params);
            const float cb = toneMapValue(b, params);

            px[0] = safeChannel(math::lerp(cr, lr, preserve));
            px[1] = safeChannel(math::lerp(cg, lg, preserve));
            px[2] = safeChannel(math::lerp(cb, lb, preserve));
        }
    });
}

} // namespace lps
