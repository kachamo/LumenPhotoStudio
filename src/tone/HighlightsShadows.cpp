// ==============================================================================
// tone/HighlightsShadows.cpp
//
// Masks are defined in PERCEPTUAL (sRGB-encoded) space so the sliders behave
// intuitively — but we're operating on linear-light values. So we evaluate
// the mask against a stand-in that approximates sRGB perception: we apply
// the sRGB encoding to the linear value just for the mask calculation. The
// actual tonal shift is added back in linear space.
//
// This matters: if we compute masks directly on linear-light values, "shadows"
// would affect a much wider range than expected (most of the visually dark
// region sits in 0.0..0.05 linear).
// ==============================================================================
#include "tone/HighlightsShadows.h"

#include "util/ColorMath.h"
#include "util/ColorSpace.h"

#include <cmath>

namespace lps {

namespace { constexpr float kEps = 0.01f; }

float HighlightsShadows::evaluate(float v, const ToneShapingParams& p)
{
    const bool any = std::fabs(p.highlights) >= kEps
                  || std::fabs(p.shadows)    >= kEps
                  || std::fabs(p.whites)     >= kEps
                  || std::fabs(p.blacks)     >= kEps;
    if (!any) return v;

    // Perceptual stand-in for mask evaluation only.
    const float perceptual = colorspace::linearToSrgb(math::clamp01(v));

    float result = v;

    if (std::fabs(p.shadows) >= kEps) {
        const float mask = 1.0f - math::smoothstep(0.0f, 0.5f, perceptual);
        // Scale target amount — larger in linear because we're adding into
        // potentially very small values. 0.05 linear @ +100 shadows ~= visible lift.
        result += (p.shadows / 100.0f) * 0.25f * mask;
    }
    if (std::fabs(p.highlights) >= kEps) {
        const float mask = math::smoothstep(0.5f, 1.0f, perceptual);
        result += (p.highlights / 100.0f) * 0.4f * mask;
    }
    if (std::fabs(p.blacks) >= kEps) {
        const float mask = 1.0f - math::smoothstep(0.0f, 0.3f, perceptual);
        result += (p.blacks / 100.0f) * 0.15f * mask;
    }
    if (std::fabs(p.whites) >= kEps) {
        const float mask = math::smoothstep(0.7f, 1.0f, perceptual);
        result += (p.whites / 100.0f) * 0.35f * mask;
    }

    return result;
}

} // namespace lps
