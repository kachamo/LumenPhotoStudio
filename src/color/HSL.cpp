// ==============================================================================
// color/HSL.cpp — CINEMATIC REFINEMENT
//
// Fixes over the previous version:
//
//   1. Saturation compounding bug. The old code combined channel saturation
//      contributions multiplicatively:
//            satMult *= (1 + w_i * c.sat[i])
//      When two adjacent channels overlap at a pixel (w_red=0.5, w_orange=0.5),
//      and both have sat slider at +100, the combined multiplier was
//      (1+0.5)(1+0.5) = 2.25 instead of the intuitive (1 + 0.5·1 + 0.5·1) = 2.0.
//      This made pixels in overlap regions (most pixels) over-saturate
//      relative to pure-hue pixels — a visible "banding" between hue ranges.
//      Fixed by switching to weighted-additive combination, normalized by
//      totalWeight the same way hue and luminance already were.
//
//   2. Luminance slider didn't match perceived brightness. The old code
//      adjusted HSL's L (= (max+min)/2), which is very different from
//      Rec.709 luminance. For pure blue (HSL L = 0.5, Rec.709 L ≈ 0.07),
//      +10 on the luminance slider moved HSL L by ~0.015 but that barely
//      changed perceived brightness. Fixed: luminance slider now applies a
//      direct multiplicative gain on the pixel's Rec.709 luminance, which
//      is what users actually want when they say "make the blues brighter."
//
//   3. Better NaN/clamp safety. Output values explicitly protected against
//      NaN propagation from degenerate HSL conversion of black pixels.
//
// What stays the same:
//   - Raised-cosine hue weights with 45° support (smooth blend between
//     adjacent channels, no hard boundaries).
//   - Wrap-around handling via circular distance metric.
//   - Magnitude preservation for HDR-overshoot pixels (values > 1 after
//     upstream exposure boost retain their magnitude through the round-trip).
// ==============================================================================
#include "color/HSL.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <array>
#include <cmath>

namespace lps {

namespace {

// 8 canonical photo hue centers on the normalized hue circle [0, 1).
constexpr std::array<float, 8> kChannelHues = {
    0.0f,     // red
    0.083f,   // orange (~30°)
    0.167f,   // yellow (~60°)
    0.333f,   // green  (~120°)
    0.5f,     // aqua   (180°)
    0.667f,   // blue   (~240°)
    0.778f,   // purple (~280°)
    0.889f    // magenta(~320°)
};

// Raised-cosine falloff width. 0.125 = 45° support — adjacent channels
// overlap smoothly (each channel at distance 0.125/2 from neighbor means
// 50% of the window crosses into the neighbor's region).
constexpr float kHalfWidth = 0.125f;

// Shortest arc distance on the hue circle.
inline float hueDistance(float a, float b)
{
    float d = std::fabs(a - b);
    if (d > 0.5f) d = 1.0f - d;
    return d;
}

// Raised-cosine weight: 1 at channel center, 0 beyond kHalfWidth.
// C1-continuous: slope is 0 at both the center and the boundary, so there
// are no visible kinks when the weight is small.
inline float channelWeight(float pixelHue, float centerHue)
{
    const float d = hueDistance(pixelHue, centerHue);
    if (d >= kHalfWidth) return 0.0f;
    return 0.5f + 0.5f * std::cos(static_cast<float>(M_PI) * d / kHalfWidth);
}

// Packed per-channel slider values, in the units each effect uses.
struct PackedChannels {
    std::array<float, 8> hue;    // hue shift in normalized hue units
    std::array<float, 8> sat;    // saturation multiplier delta (so 0 = identity)
    std::array<float, 8> lum;    // luminance gain delta (0 = identity, +0.5 = +50%)
};

PackedChannels packChannels(const HSLParams& p)
{
    PackedChannels c{};
    const HSLChannel src[8] = {
        p.red, p.orange, p.yellow, p.green, p.aqua, p.blue, p.purple, p.magenta
    };
    for (size_t i = 0; i < 8; ++i) {
        c.hue[i] = src[i].hue        / 100.0f * 0.05f;   // ±5% of hue circle at max
        c.sat[i] = src[i].saturation / 100.0f;           // ±1.0 at max (100% sat delta)
        c.lum[i] = src[i].luminance  / 100.0f * 0.5f;    // ±50% luminance gain at max
    }
    return c;
}

} // namespace

void HSL::apply(PixelBuffer& buffer, const HSLParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const PackedChannels c = packChannels(params);
    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            float r = p[0], g = p[1], b = p[2];

            // ---- Normalize for HSL round-trip ---------------------------
            // rgbToHsl needs values in [0, 1]. For pixels pushed above 1.0
            // by earlier exposure boost, we scale down, do the round-trip,
            // then scale the result back up — preserves HDR headroom.
            const float mx = std::max({r, g, b});
            const float scale = (mx > 1.0f) ? mx : 1.0f;
            const float invScale = 1.0f / scale;
            const float rn = r * invScale;
            const float gn = g * invScale;
            const float bn = b * invScale;

            float h, s, l;
            math::rgbToHsl(rn, gn, bn, h, s, l);

            // ---- Accumulate per-channel contributions -------------------
            // hue: weighted additive (normalized by totalWeight).
            // sat: weighted additive on the MULTIPLIER (not multiplicative
            //      compounding — that was the old bug).
            // lum: weighted additive gain, applied later on Rec.709 luminance.
            float hueDelta    = 0.0f;
            float satDelta    = 0.0f;   // sum of w_i · slider_i
            float lumGainDelta = 0.0f;
            float totalWeight = 0.0f;

            for (size_t i = 0; i < 8; ++i) {
                const float w = channelWeight(h, kChannelHues[i]);
                if (w <= 0.0f) continue;
                totalWeight  += w;
                hueDelta     += w * c.hue[i];
                satDelta     += w * c.sat[i];
                lumGainDelta += w * c.lum[i];
            }

            // ---- Apply hue + saturation via HSL round-trip --------------
            if (totalWeight > 0.0f) {
                const float invW = 1.0f / totalWeight;
                h += hueDelta * invW;
                // Wrap into [0, 1). Use floor-based wrap so negative shifts
                // also land correctly (h + floor-based wrap is robust for
                // any real offset).
                h -= std::floor(h);

                // Saturation multiplier: 1 + weighted-average slider value.
                // Identity (all sat sliders 0): satMult = 1 exactly.
                // Overlap of two channels both at +100: satMult = 1 + 1 = 2,
                // not (1+1)·(1+1) = 4 as the old compounding would produce.
                const float satMult = 1.0f + satDelta * invW;
                s = math::clamp01(s * satMult);
            }

            float nr, ng, nb;
            math::hslToRgb(h, s, l, nr, ng, nb);

            // Re-apply the HDR-overshoot scale.
            nr *= scale;
            ng *= scale;
            nb *= scale;

            // ---- Luminance gain: applied as Rec.709 multiplier ----------
            // Instead of adjusting HSL's L (which doesn't match perceived
            // brightness for saturated hues), we scale the entire RGB by a
            // multiplier that achieves the requested Rec.709 luminance
            // change. This is what the user expects: "blues +20%" should
            // make blue pixels 20% brighter to the eye.
            //
            // We compute the gain as (1 + lumGainDelta·invW) directly rather
            // than trying to hit a specific target luminance — that's simpler
            // and gives predictable slider behavior. Multiplicative form
            // automatically preserves hue (scaling RGB proportionally doesn't
            // change hue angle).
            if (totalWeight > 0.0f && std::fabs(lumGainDelta) > 1e-6f) {
                const float invW = 1.0f / totalWeight;
                const float gain = 1.0f + lumGainDelta * invW;
                nr *= gain;
                ng *= gain;
                nb *= gain;
            }

            // ---- NaN guard ---------------------------------------------
            // hslToRgb is robust for valid inputs, but if upstream feeds
            // NaN (e.g., divide by zero in a prior engine), propagate a
            // safe zero rather than poisoning the buffer.
            if (!std::isfinite(nr)) nr = 0.0f;
            if (!std::isfinite(ng)) ng = 0.0f;
            if (!std::isfinite(nb)) nb = 0.0f;

            // Floor at 0 — no negative light. Don't cap at 1; upstream
            // highlights and final sRGB encode handle the ceiling.
            if (nr < 0.0f) nr = 0.0f;
            if (ng < 0.0f) ng = 0.0f;
            if (nb < 0.0f) nb = 0.0f;

            p[0] = nr;
            p[1] = ng;
            p[2] = nb;
        }
    });
}

} // namespace lps
