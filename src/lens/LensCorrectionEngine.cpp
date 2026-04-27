// ==============================================================================
// lens/LensCorrectionEngine.cpp
//
// V1 implements vignetting compensation only; distortion + CA + fringe
// are placeholder no-ops with their data fields persisted. This file is
// where real implementations land in follow-up steps.
//
// Vignetting compensation model:
//   normalized distance d = sqrt(dx² + dy²) / maxRadius, where dx/dy are
//   distances from the image center in the smaller-edge unit. d ∈ [0, 1]
//   at corners (in landscape; ranges over a different shape for non-
//   square aspects but stays bounded near 1 at the visible-corner zone).
//
//   gain(d) = 1 + (amount/100) × d²
//
//   amount = +100 brightens the corners by 2× compared to center
//   (gain at d=1 is 2.0). amount = -100 darkens corners to ~0.
//   The d² weighting concentrates the effect near the edges; the center
//   stays untouched (gain at d=0 is exactly 1).
//
//   Operates per channel multiplicatively in linear space — same model
//   as the existing global vignette effect, just driven by lens.vignetting
//   instead of effects.vignette.amount.
//
// Performance: one pass through the buffer with a simple per-pixel
// multiply. Sub-millisecond on preview-sized images.
// ==============================================================================
#include "lens/LensCorrectionEngine.h"

#include "util/ColorMath.h"
#include "util/ScanlineParallel.h"

#include <cmath>

namespace lps {

namespace {

// Apply vignetting compensation in place. amount ∈ [-100, +100].
// No-op when amount is at zero.
void applyVignetting(PixelBuffer& buffer, float amount)
{
    if (std::fabs(amount) < 1e-4f) return;

    const int W = buffer.width();
    const int H = buffer.height();
    if (W <= 0 || H <= 0) return;

    // Compensation strength. We map [-100, +100] to a unit-scale gain
    // delta at the corners. Sign convention:
    //   positive amount brightens corners (fills darkened vignette)
    //   negative amount darkens corners (adds vignette)
    const float strength = amount / 100.0f;   // [-1, +1]

    // Center in pixel coords. Use sub-pixel center for symmetry.
    const float cx = static_cast<float>(W) * 0.5f;
    const float cy = static_cast<float>(H) * 0.5f;

    // Normalize distances by the half-diagonal so d=1 lands at the
    // image corners. This makes "amount=100 doubles corner brightness"
    // a stable definition regardless of aspect ratio.
    const float halfDiag = std::sqrt(cx*cx + cy*cy);
    const float invHalfDiag = (halfDiag > 1e-3f) ? (1.0f / halfDiag) : 0.0f;

    forEachScanline(buffer, [W, cx, cy, invHalfDiag, strength]
                            (float* row, int y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        for (int x = 0; x < W; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float d = std::sqrt(dx*dx + dy*dy) * invHalfDiag;
            // d² weighting concentrates effect near the edges; center
            // gain is exactly 1.0 (no shift).
            const float gain = 1.0f + strength * d * d;
            // Floor at zero — when amount = -100 and d = 1, gain = 0 and
            // pixels go to black, which is the user's explicit choice.
            // Negative gains can't occur with this clamp range, but the
            // !(g > 0) guard catches NaN safely.
            const float g = (gain > 0.0f) ? gain : 0.0f;

            float* px = row + x * 4;
            px[0] *= g;
            px[1] *= g;
            px[2] *= g;
            // alpha untouched
        }
    });
}

} // namespace

void LensCorrectionEngine::apply(PixelBuffer& buffer, const LensParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    // Vignetting is the only V1-active correction. Distortion, CA, and
    // fringe controls are placeholders — their values are read from
    // params for round-trip but do not affect pixels yet.
    applyVignetting(buffer, params.vignetting);

    // Distortion placeholder. Real correction would:
    //   1. Allocate a read-only copy of the buffer (or two: one for source,
    //      one for destination).
    //   2. For each output pixel (x, y), compute its corresponding source
    //      sample point via the inverse distortion equation:
    //         r' = r * (1 + k1*r² + k2*r⁴)  for k from `distortion`.
    //   3. Bilinearly sample the source at (sx, sy) → write to dest.
    //   4. Black-pad pixels that fall outside the source rect.
    // Skipped for V1 to avoid the second-buffer cost and the considerable
    // edge-handling complexity. Data field round-trips through save/load.
    (void)params.distortion;

    // Chromatic aberration / fringe placeholders. Real CA correction
    // would shift the R and B channels radially by tiny fractions to
    // reduce purple/green fringes near high-contrast edges. UI present,
    // engine inert.
    (void)params.removeChromaticAberration;
    (void)params.purpleFringe;
    (void)params.greenFringe;
}

} // namespace lps
