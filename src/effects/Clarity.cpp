// ==============================================================================
// effects/Clarity.cpp
//
// Midtone contrast. Evaluate a per-pixel luminance weight that peaks at
// perceptual midgray, then stretch the pixel's values around that pivot by
// an amount scaled by the weight.
//
// Evaluation in linear light with perceptual weighting (sRGB-encoded luma
// for mask peak position) so the effect concentrates where humans see
// "midtones" rather than where linear values cluster.
// ==============================================================================
#include "effects/Clarity.h"

#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <cmath>

namespace lps {

void Clarity::apply(PixelBuffer& buffer, const ClarityParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const float amt = params.amount / 100.0f * 0.3f;   // ±30% contrast at extremes
    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            const float r = p[0], g = p[1], b = p[2];

            // Linear luminance, then perceptual for weight mask.
            const float lumLin = math::luminance(math::clamp01(r), math::clamp01(g), math::clamp01(b));
            const float lumP   = colorspace::linearToSrgb(lumLin);
            // Weight peaks at 0.5, zero at extremes.
            const float mid = 1.0f - std::fabs(lumP - 0.5f) * 2.0f;
            const float mul = 1.0f + amt * mid;

            // Contrast pivot at linear midgray (0.18).
            constexpr float kPivot = 0.18f;
            p[0] = kPivot + (r - kPivot) * mul;
            p[1] = kPivot + (g - kPivot) * mul;
            p[2] = kPivot + (b - kPivot) * mul;
        }
    });
}

} // namespace lps
