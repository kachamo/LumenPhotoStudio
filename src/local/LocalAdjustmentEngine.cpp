// ==============================================================================
// local/LocalAdjustmentEngine.cpp
//
// Per-mask flow:
//   1. Skip if isIdentity (disabled or all-zero adjustments)
//   2. For each pixel:
//      a. Compute mask weight ∈ [0, 1] from geometry
//      b. If weight ~= 0, skip the pixel entirely
//      c. Compute the fully-adjusted pixel value (full-strength)
//      d. lerp(originalPixel, adjustedPixel, weight) — masked blend
//
// Adjustments operate in linear-light space, mostly via the same primitives
// the global engines use:
//   - exposure: linear multiply by 2^stops  (matches Exposure::multiplier)
//   - brightness: midtone-weighted sine bump (sRGB round-trip)  — same
//     formula as ToneEngine::brightnessShape, applied per-pixel here
//     instead of through a LUT
//   - contrast: scale around perceptual mid-gray with sigmoid steepness
//     (lighter version — full filmic curve here would be expensive)
//   - saturation: lerp toward Rec.709 luminance
//   - temperature/tint: scaled R-shift / G-shift in linear space
//
// All operations are clamped to non-negative; we don't upper-clamp because
// the pipeline downstream handles that. NaN-safe via `!(x > 0)` form.
//
// Performance: ScanlineParallel for the per-pixel pass. Per mask: ~10 ops
// per pixel for weight calc + 30-40 ops for adjustments. At 2-megapixel
// preview with 5 masks, ~500M ops total — runs in a few hundred ms on
// a modern CPU, well under the debounce window.
// ==============================================================================
#include "local/LocalAdjustmentEngine.h"

#include "local/MaskGeometry.h"
#include "tone/Exposure.h"
#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <algorithm>
#include <cmath>

namespace lps {

namespace {

// ============================================================================
// Mask weight evaluation lives in local/MaskGeometry.h — shared with the
// PreviewWidget overlay path. The maskWeight/maskWeightLinear/
// maskWeightRadial functions are inline-defined there in namespace lps,
// so this anonymous namespace block is now adjustments-only.
// ============================================================================

// ============================================================================
// Per-pixel adjustment math
//
// Given a linear-light input pixel and the mask's adjustment values, produce
// the fully-adjusted (mask-weight-1) output pixel. The caller lerps between
// input and output by mask weight.
//
// Operations follow the global engines' conventions:
//   - exposure: linear multiply by 2^stops
//   - temperature/tint: R/G shift scaled to a small fraction of the full
//     range so global +100 doesn't become unusable on local masks
//   - brightness: sRGB round-trip + sine bump (matches ToneEngine V2)
//   - contrast: scale around perceptual mid-gray
//   - saturation: lerp toward luminance
//
// Order: same as global pipeline (exposure first, then color, then tone-
// shape). Saturation last so it operates on the post-WB result.
// ============================================================================

struct Rgb { float r, g, b; };

inline Rgb applyAdjustments(Rgb in, const LocalAdjustment& m)
{
    Rgb p = in;

    // Exposure: linear-light multiply.
    if (std::fabs(m.exposure) > 1e-4f) {
        const float mul = Exposure::multiplier(m.exposure);
        p.r *= mul;
        p.g *= mul;
        p.b *= mul;
    }

    // Temperature/tint: small R/G shifts in linear space.
    // Scale chosen so a +100 local temp matches roughly half the visual
    // strength of a +100 global temp — local masks are usually subtle.
    if (std::fabs(m.temperature) > 1e-4f) {
        const float t = m.temperature / 100.0f * 0.10f;
        p.r *= (1.0f + t);
        p.b *= (1.0f - t);
    }
    if (std::fabs(m.tint) > 1e-4f) {
        const float t = m.tint / 100.0f * 0.10f;
        p.g *= (1.0f - t);
    }

    // Brightness: sRGB round-trip + sine bump (same shape as ToneEngine).
    // Done per-channel for simplicity — a luminance-anchored brightness
    // would be more accurate but adds two more passes. Per-channel is the
    // common convention for local adjustments and gives close enough results.
    if (std::fabs(m.brightness) > 1e-4f) {
        const float t = std::clamp(m.brightness / 100.0f, -1.0f, 1.0f);
        constexpr float kPeak = 0.20f;
        constexpr float kPi   = 3.14159265358979323846f;
        auto bump = [&](float v) {
            const float s = colorspace::linearToSrgb(math::clamp01(v));
            const float bumped = math::clamp01(s + t * kPeak * std::sin(kPi * s));
            return colorspace::srgbToLinear(bumped);
        };
        p.r = bump(p.r);
        p.g = bump(p.g);
        p.b = bump(p.b);
    }

    // Contrast: scale around perceptual mid-gray (sRGB 0.4613). Operate
    // in sRGB-encoded space so the pivot lands at perceptual middle gray
    // and the curve feels natural to photographers.
    if (std::fabs(m.contrast) > 1e-4f) {
        const float t = std::clamp(m.contrast / 100.0f, -1.0f, 1.0f);
        constexpr float kPivot = 0.4613f;
        // Slope at the pivot. Negative t flattens (slope < 1), positive
        // steepens (slope > 1). Bounded to avoid extreme expansion.
        const float slope = (t >= 0.0f) ? (1.0f + t * 1.5f)
                                        : (1.0f / (1.0f - t * 1.5f));
        auto contrast = [&](float v) {
            const float s = colorspace::linearToSrgb(math::clamp01(v));
            const float shifted = (s - kPivot) * slope + kPivot;
            return colorspace::srgbToLinear(math::clamp01(shifted));
        };
        p.r = contrast(p.r);
        p.g = contrast(p.g);
        p.b = contrast(p.b);
    }

    // Saturation: lerp toward Rec.709 luminance.
    if (std::fabs(m.saturation) > 1e-4f) {
        const float s = std::clamp(m.saturation / 100.0f, -1.0f, 1.0f);
        const float Y = math::luminance(p.r, p.g, p.b);
        // s > 0: push away from luma (more saturated)
        // s < 0: pull toward luma (less saturated)
        const float k = 1.0f + s;
        p.r = Y + (p.r - Y) * k;
        p.g = Y + (p.g - Y) * k;
        p.b = Y + (p.b - Y) * k;
    }

    // Floor-clamp at zero (NaN-safe). No upper clamp — linear-light
    // overshoots are fine; downstream stages handle clipping.
    if (!(p.r > 0.0f)) p.r = 0.0f;
    if (!(p.g > 0.0f)) p.g = 0.0f;
    if (!(p.b > 0.0f)) p.b = 0.0f;
    return p;
}

// ============================================================================
// Apply one mask to the buffer
// ============================================================================
void applyOneMask(PixelBuffer& buffer, const LocalAdjustment& mask)
{
    const int W = buffer.width();
    const int H = buffer.height();
    if (W <= 0 || H <= 0) return;

    // Aspect ratio for radial: image width / image height. Used to make
    // a "radius=0.25" radial mask appear as a circle on the displayed
    // image, regardless of the image's aspect.
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const double invW = 1.0 / static_cast<double>(W);
    const double invH = 1.0 / static_cast<double>(H);

    forEachScanline(buffer, [&mask, W, invW, invH, aspect](float* row, int y) {
        // Density and invert are mask-level modifiers applied after the
        // raw geometry weight. density ∈ [0, 1] scales the maximum mask
        // strength (Lightroom convention — softens the whole mask without
        // changing geometry). invert: w → 1 - w. Order: raw → invert →
        // density, so density attenuates the post-invert result.
        const float density = (mask.type == MaskType::Brush)
            ? 1.0f
            : std::clamp(mask.density, 0.0f, 1.0f);
        const bool  invert  = mask.invert;
        const double v = (static_cast<double>(y) + 0.5) * invH;
        for (int x = 0; x < W; ++x) {
            const double u = (static_cast<double>(x) + 0.5) * invW;
            float w = maskWeight(QPointF(u, v), mask, aspect);
            if (invert) w = 1.0f - w;
            w *= density;
            if (w <= 1e-3f) continue;   // pixel outside mask — skip work

            float* px = row + x * 4;
            Rgb in { px[0], px[1], px[2] };
            Rgb adjusted = applyAdjustments(in, mask);

            // Lerp by weight — masked blend.
            px[0] = in.r + (adjusted.r - in.r) * w;
            px[1] = in.g + (adjusted.g - in.g) * w;
            px[2] = in.b + (adjusted.b - in.b) * w;
            // alpha untouched
        }
    });
}

} // namespace

void LocalAdjustmentEngine::apply(PixelBuffer& buffer,
                                  const std::vector<LocalAdjustment>& adjustments)
{
    if (adjustments.empty() || buffer.isNull()) return;

    // Apply masks in the order they appear in the Look. Later masks operate
    // on the result of earlier masks, which is the conventional photo-
    // editor model — masks layer in order.
    for (const auto& mask : adjustments) {
        if (mask.isIdentity()) continue;
        applyOneMask(buffer, mask);
    }
}

} // namespace lps
