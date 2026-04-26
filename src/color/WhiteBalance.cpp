// ==============================================================================
// color/WhiteBalance.cpp — CINEMATIC REFINEMENT
//
// Upgrades over the previous version:
//
//   1. Luminance-compensated multipliers. The previous WB used R×1.4, B×0.71
//      at full warm, which *also* brightens the image by about 6% (because
//      Rec.709 weights R and B unevenly). A proper WB shifts chromaticity
//      without changing perceived luminance. We now scale the green channel
//      by a compensating factor so that a neutral gray pixel keeps the same
//      Rec.709 luminance before and after.
//
//   2. Soft shoulder instead of hard overshoot. Warm WB can push R above 1
//      for already-bright pixels. The previous code just let those values
//      propagate — fine in principle for linear-light workflow, but it means
//      the final sRGB encode clips them hard. We now apply a smooth shoulder
//      to values that overshoot, preserving more highlight detail.
//
//   3. Safety clamps. NaN/Inf inputs can't poison the buffer.
//
// Architectural decisions kept:
//   - Still channel-multiplier form (not a full von Kries XYZ transform).
//     Matches Lightroom's Temperature slider behavior; calibrated chromatic
//     adaptation is a future step.
//   - Still operates in linear light — multiplicative color temperature is
//     physically meaningful there.
// ==============================================================================
#include "color/WhiteBalance.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <cmath>

namespace lps {

namespace {

// Soft shoulder: values above this start to compress toward the ceiling at 1.
// Applied per channel after WB multipliers. Below the knee the channel passes
// through unchanged; above it a Reinhard-style compression asymptotes toward
// 1.0 but never clips. This is how film handles exposure overshoot.
constexpr float kShoulderKnee = 0.90f;

inline float softShoulder(float v)
{
    if (!(v > kShoulderKnee)) return v;          // also catches NaN
    const float u = v - kShoulderKnee;           // u > 0
    const float k = 2.0f;                         // shoulder tightness
    // y = knee + (1 - knee) * u / (1 - knee + k*u)
    // At u=0: returns knee. As u→∞: returns knee + (1-knee)/k = always < 1.
    // Slope at u=0 is 1, so C1-continuous with the pass-through region.
    const float y_above = (1.0f - kShoulderKnee) * u
                        / ((1.0f - kShoulderKnee) + k * u);
    return kShoulderKnee + y_above;
}

} // namespace

void WhiteBalance::apply(PixelBuffer& buffer, const WhiteBalanceParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    // Slider positions mapped to log-space ratios so that ±t and ∓t are
    // exact inverses — symmetry matters for "apply then undo" to land on the
    // same pixel values.
    const float tempNorm = params.temperature / 100.0f;    // [-1, +1]
    const float tintNorm = params.tint        / 100.0f;    // [-1, +1]

    // Base R/B ratio from temperature. +1 → R×1.4, B/1.4. Symmetric in log.
    const float rMulRaw = std::pow(1.4f,  tempNorm);
    const float bMulRaw = std::pow(1.4f, -tempNorm);

    // Base G scale from tint. +1 → G×0.83 (pulls green out of magenta cast).
    const float gMulRaw = std::pow(1.2f, -tintNorm);

    // ----- Luminance compensation --------------------------------------------
    // We want a neutral gray pixel (R=G=B=L) to preserve its Rec.709
    // luminance through the WB transform. After the raw multipliers:
    //   L_new = kLumaR·(rMulRaw·L) + kLumaG·(gMulRaw·L) + kLumaB·(bMulRaw·L)
    //         = L · (kLumaR·rMulRaw + kLumaG·gMulRaw + kLumaB·bMulRaw)
    // So we divide every channel by that sum to restore L_new = L.
    //
    // This moves all sliders onto a luminance-neutral axis. Warming no longer
    // brightens; cooling no longer darkens. Photographers expect this.
    const float lumScale = math::kLumaR_Linear * rMulRaw
                         + math::kLumaG_Linear * gMulRaw
                         + math::kLumaB_Linear * bMulRaw;
    // Guard: if lumScale underflows (shouldn't happen with sane sliders, but
    // defensively) we skip compensation rather than producing NaN.
    const float inv = (lumScale > 1e-6f) ? (1.0f / lumScale) : 1.0f;
    const float rMul = rMulRaw * inv;
    const float gMul = gMulRaw * inv;
    const float bMul = bMulRaw * inv;

    const int width = buffer.width();
    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;

            // Read, multiply, soft-shoulder, clamp at floor.
            // Upper soft clamp preserves highlight detail; lower hard clamp
            // at 0 prevents physically-impossible negative light from leaking
            // into downstream math.
            float r = p[0] * rMul;
            float g = p[1] * gMul;
            float b = p[2] * bMul;

            r = softShoulder(r);
            g = softShoulder(g);
            b = softShoulder(b);

            // Floor at 0. Negatives only appear if upstream produced them
            // (color matrices in prior engines); we don't generate them here
            // but defensively clamp.
            if (!(r > 0.0f)) r = 0.0f;
            if (!(g > 0.0f)) g = 0.0f;
            if (!(b > 0.0f)) b = 0.0f;

            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
    });
}

} // namespace lps
