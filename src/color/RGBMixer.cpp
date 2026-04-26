// ==============================================================================
// color/RGBMixer.cpp — CINEMATIC REFINEMENT
//
// The matrix math is the same — that's the semantic contract of a channel
// mixer. What changed is stability and safety:
//
//   1. Floor clamp. Matrix rows with negative coefficients can produce
//      negative output values. For pixels where input channels happen to
//      line up against those negatives, the result goes below 0 and
//      propagates through downstream engines as "negative light." The final
//      sRGB encode clips it, but intermediate HSL or saturation math on
//      negative values produces artifacts. Clamping to ≥0 here isolates the
//      damage.
//
//   2. Soft shoulder for overshoot. A creative mixer like
//        redOutput = (1.5, 0.3, 0.0)   // red channel takes green too
//      can produce output > 1.0 for pixels with bright red AND green.
//      Left uncapped, these overshoot into the highlight shoulder of the
//      downstream tone stage — but the tone stage is already past us in the
//      pipeline. So we apply a Reinhard-style shoulder here to keep the
//      overshoot controlled, preserving more highlight detail at sRGB encode.
//
//   3. NaN guard. Cheap defensive check — matrix multiply with finite inputs
//      can't produce NaN on its own, but if upstream fed us one we'd rather
//      stop it here than let it propagate.
//
// Luminance preservation is NOT automatic. A user setting
//   redOutput = (0.5, 0.5, 0.0)
// is deliberately choosing a mix that darkens red-dominant regions; fighting
// that with auto-normalization would defeat the creative intent. "Allow
// creative color shifts without destroying luminance too easily" (per spec)
// means the MATH is stable, not that luminance is forced to stay constant.
// ==============================================================================
#include "color/RGBMixer.h"

#include "util/ScanlineParallel.h"

#include <cmath>

namespace lps {

namespace {

// Same shoulder shape used in WhiteBalance — consistent highlight behavior
// across the color stage.
constexpr float kShoulderKnee = 0.95f;

inline float softShoulder(float v)
{
    if (!(v > kShoulderKnee)) return v;   // also catches NaN
    const float u = v - kShoulderKnee;
    constexpr float k = 2.0f;
    const float y_above = (1.0f - kShoulderKnee) * u
                        / ((1.0f - kShoulderKnee) + k * u);
    return kShoulderKnee + y_above;
}

} // namespace

void RGBMixer::apply(PixelBuffer& buffer, const RGBMixerParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const auto& mr = params.redOutput;
    const auto& mg = params.greenOutput;
    const auto& mb = params.blueOutput;
    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            const float r = p[0], g = p[1], b = p[2];

            float outR = mr.r * r + mr.g * g + mr.b * b;
            float outG = mg.r * r + mg.g * g + mg.b * b;
            float outB = mb.r * r + mb.g * g + mb.b * b;

            // NaN guard — zero rather than poison the buffer.
            if (!std::isfinite(outR)) outR = 0.0f;
            if (!std::isfinite(outG)) outG = 0.0f;
            if (!std::isfinite(outB)) outB = 0.0f;

            // Floor at 0 — negative coefficients can produce negatives.
            if (outR < 0.0f) outR = 0.0f;
            if (outG < 0.0f) outG = 0.0f;
            if (outB < 0.0f) outB = 0.0f;

            // Soft shoulder for overshoot.
            outR = softShoulder(outR);
            outG = softShoulder(outG);
            outB = softShoulder(outB);

            p[0] = outR;
            p[1] = outG;
            p[2] = outB;
        }
    });
}

} // namespace lps
