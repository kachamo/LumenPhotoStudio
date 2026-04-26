// ==============================================================================
// color/Vibrance.cpp — CINEMATIC REFINEMENT
//
// Addresses three real issues in the previous implementation:
//
//   1. Wrong saturation estimate. The old code used max(|r-L|, |g-L|, |b-L|)
//      as "current saturation." That's proportional to saturation but not
//      bounded the way true HSL saturation is — it caps out around 0.7 for
//      pure reds, not 1.0, which made the protection curve kick in at the
//      wrong spot. New version uses (max - min) / max, the standard HSV/HSL
//      saturation measure, bounded cleanly in [0,1].
//
//   2. No skin-tone protection. Vibrance is specifically marketed as
//      "smart saturation that protects skin" in every editing tool. The
//      old code treated skin tones like any other hue — pushing +80
//      vibrance on a portrait made faces fluorescent orange. New version
//      has an explicit smooth attenuation mask centered on skin-tone hues
//      (~20-50° on the hue circle).
//
//   3. Inverted behavior for negative vibrance. Old code: boost factor =
//      1 + t·protect, where protect is "1 for gray, 0 for saturated". With
//      negative t, that means gray pixels got desaturated MORE than already-
//      saturated ones — the opposite of intuitive. New version: negative
//      vibrance targets saturated pixels first (drains the most colorful
//      areas), leaving near-grays mostly untouched.
//
// Saturation (the global slider) stays luminance-preserving as before — it
// scales each channel's offset from the pixel's Rec.709 luminance. That's
// mathematically correct and doesn't need changing.
// ==============================================================================
#include "color/Vibrance.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <algorithm>
#include <cmath>

namespace lps {

namespace {

constexpr float kEps = 0.01f;

// ---- Skin-tone protection ---------------------------------------------------
// Target the orange/red hue band where skin tones cluster across ethnicities.
// In normalized hue [0,1), skin sits roughly between 0.0 (red) and 0.1 (orange/
// yellow boundary). Mask shape: full protection (weight 0 → no vibrance effect)
// at the center, smoothly fading to no protection (weight 1 → full effect)
// outside a window.
//
// Smoothstep falloff — no hard edges. At slider-level use this is enough;
// for a "protect skin harder" user slider we'd expose the strength as a
// param, but the spec doesn't ask for one.
constexpr float kSkinCenter     = 0.055f;   // ~20° — middle of skin-tone band
constexpr float kSkinInnerWidth = 0.055f;   // full protection within ±0.055
constexpr float kSkinOuterWidth = 0.110f;   // zero protection beyond ±0.110

inline float skinProtectionWeight(float hue)
{
    // Distance from skin center on the circle (shortest arc).
    float d = std::fabs(hue - kSkinCenter);
    if (d > 0.5f) d = 1.0f - d;

    if (d <= kSkinInnerWidth) return 0.0f;                     // fully protected
    if (d >= kSkinOuterWidth) return 1.0f;                     // no protection
    // Smoothstep from inner (weight 0) to outer (weight 1).
    const float u = (d - kSkinInnerWidth)
                  / (kSkinOuterWidth - kSkinInnerWidth);
    return u * u * (3.0f - 2.0f * u);
}

// ---- Fast HSL-style saturation estimate -------------------------------------
// Returns saturation in [0, 1]. Uses (max - min) / max (the HSV form) because
// it's cheaper than full HSL and gives the same "how colorful is this pixel"
// signal for the vibrance decision.
//
// For black pixels (max = 0) returns 0 — no color information, no boost.
inline float hsvSaturation(float r, float g, float b)
{
    const float mx = std::max({r, g, b});
    if (mx <= 1e-6f) return 0.0f;
    const float mn = std::min({r, g, b});
    return (mx - mn) / mx;
}

// ---- Fast hue estimate (for skin protection only) ---------------------------
// Returns hue in [0, 1). Uses the standard HSV hue formula. Only called when
// vibrance is non-zero, so the cost is amortized.
inline float hsvHue(float r, float g, float b)
{
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    if (d <= 1e-6f) return 0.0f;   // achromatic — hue undefined, doesn't matter

    float h;
    if (mx == r)       h = ((g - b) / d) + (g < b ? 6.0f : 0.0f);
    else if (mx == g)  h = ((b - r) / d) + 2.0f;
    else               h = ((r - g) / d) + 4.0f;
    return h * (1.0f / 6.0f);
}

} // namespace

void Vibrance::apply(PixelBuffer& buffer, float vibrance, float globalSaturation)
{
    const bool hasVib = std::fabs(vibrance)         >= kEps;
    const bool hasSat = std::fabs(globalSaturation) >= kEps;
    if ((!hasVib && !hasSat) || buffer.isNull()) return;

    const float vibFactor = math::clamp(vibrance / 100.0f, -1.0f, 1.0f);
    const float satFactor = math::clamp(globalSaturation / 100.0f, -1.0f, 1.0f);
    const float satMul    = 1.0f + satFactor;   // 0 at -100, 2 at +100

    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            float r = p[0], g = p[1], b = p[2];

            // Rec.709 luminance — this is the pivot for both vibrance and
            // saturation, ensuring both are luminance-preserving.
            const float lum = math::luminance(r, g, b);

            float dr = r - lum;
            float dg = g - lum;
            float db = b - lum;

            // ---- Vibrance (smart) ----------------------------------------
            if (hasVib) {
                // Estimate current saturation. 0 = gray, 1 = fully saturated.
                const float sat = hsvSaturation(math::clamp01(r),
                                                math::clamp01(g),
                                                math::clamp01(b));

                // Build a per-pixel effect weight shaped differently for
                // positive vs negative vibrance:
                //
                // Positive vibrance: boost LESS-saturated pixels MORE.
                //   weight = (1 - sat)^1.5
                //   — At sat=0 (gray): weight=1.0, max boost
                //   — At sat=1 (fully saturated): weight=0.0, no boost
                //   The power >1 creates an aggressive falloff so "already
                //   colorful" regions are strongly protected from further
                //   saturation — prevents neon artifacts.
                //
                // Negative vibrance: drain MORE-saturated pixels MORE.
                //   weight = sat
                //   — At sat=0: weight=0, no effect on gray (correct — nothing
                //     to drain).
                //   — At sat=1: weight=1, full drain on saturated regions.
                //   Linear falloff feels predictable.
                float weight;
                if (vibFactor >= 0.0f) {
                    // protect ∈ [0, 1] because sat ∈ [0, 1], so no clamp needed.
                    const float protect = 1.0f - sat;
                    weight = protect * protect * std::sqrt(protect);
                    // Equivalent to (1 - sat)^2.5. The actual shape isn't
                    // critical — what matters is monotonic falloff from gray
                    // (full effect) to saturated (no effect), with a rapid
                    // drop so already-colorful regions are strongly protected
                    // from further saturation. Prevents neon artifacts.
                } else {
                    weight = sat;
                }

                // Skin-tone protection — only applies to positive vibrance.
                // Desaturating skin is a valid artistic choice, so we don't
                // block it when the slider is negative.
                if (vibFactor > 0.0f) {
                    const float hue = hsvHue(r, g, b);
                    weight *= skinProtectionWeight(hue);
                }

                // Apply: scale the color axis by (1 + vibFactor * weight).
                // Multiplicative so identity (factor=0 or weight=0) is exact.
                const float boost = 1.0f + vibFactor * weight;
                dr *= boost;
                dg *= boost;
                db *= boost;
            }

            // ---- Global saturation (luminance-preserving) ---------------
            // Scaling the offsets preserves Rec.709 luminance exactly because
            // Σw_i · (c_i - L) = 0 by construction of L = Σw_i · c_i.
            if (hasSat) {
                dr *= satMul;
                dg *= satMul;
                db *= satMul;
            }

            // Reassemble. Don't clamp — downstream engines may pull values
            // back into range; we preserve headroom here. The final sRGB
            // encode is where the hard floor/ceiling lives.
            p[0] = lum + dr;
            p[1] = lum + dg;
            p[2] = lum + db;
        }
    });
}

} // namespace lps
