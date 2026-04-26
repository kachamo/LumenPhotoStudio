// ==============================================================================
// tone/ToneEngine.cpp — CINEMATIC REFINEMENT
//
// Same architecture as before (PixelBuffer float32 linear-light, 4097-entry
// fused LUT, single pass, identity fast-path). What changed is the SHAPE of
// each primitive so the tonal response feels like a grading tool instead of
// a math paper.
//
// What's different from the previous pass:
//
//   1. filmicCurve (was filmicContrast): replaced the symmetric smoothstep
//      S with a proper sigmoid-based S pivoted at LINEAR middle gray (0.18).
//      Steeper slope at the pivot, asymptotic toward [0,1], continuously
//      tunable from gentle to strong without ever clipping.
//
//   2. shadowShape (was shadowLift): now has an explicit falloff — effect
//      is strongest at x=0 and smoothly decays to zero by the midtones. No
//      more gamma bleeding into mids. Black point always preserved.
//
//   3. highlightShape (was highlightRolloff): matching symmetric falloff —
//      effect strongest at x=1, zero by midtones. Soft Reinhard shoulder
//      preserved for the "recover" direction.
//
//   4. whitesShape / blacksShape: fixed the washed-out-blacks bug. Previous
//      version of +blacks did `y = lift + y*(1-lift)` which moved f(0) away
//      from zero. New version confines the effect to a toe region so f(0)=0
//      and f(1)=1 always. Same fix applied to whites.
//
// All operations run in PERCEPTUAL space (sRGB-encoded) because that's where
// photographic tone shaping lives. Round-trip is per-LUT-entry, not per-pixel,
// so runtime cost is zero.
//
// Build order per LUT entry:
//   1. Exposure          (physical — linear 2^stops multiply)
//   2. → sRGB            (enter perceptual space)
//   3. blacksShape       (toe, preserves 0→0)
//   4. whitesShape       (shoulder, preserves 1→1)
//   5. shadowShape       (lift/crush with midtone falloff)
//   6. highlightShape    (recover/push with midtone falloff)
//   7. brightnessShape   (midtone-weighted bump, preserves both endpoints)
//   8. filmicCurve       (S-curve around perceptual 0.46 ≈ linear 0.18)
//   9. → linear          (back to working color space)
//
// Ordering matters: W/B first so the endpoints are shaped before the H/S
// regional sliders and the overall S-curve contour them. Brightness sits
// just before the S-curve because its bump is widest at midtones — placing
// it after the regional sliders lets H/S remain the source of truth for
// their respective regions, while brightness adds a global mid-anchor lift
// the S-curve can then re-shape into its final filmic contour.
// ==============================================================================
#include "tone/ToneEngine.h"

#include "tone/Exposure.h"
#include "util/ColorMath.h"
#include "util/ColorSpace.h"
#include "util/ScanlineParallel.h"

#include <array>
#include <cmath>

namespace lps {

namespace {

constexpr int kLutSize = 4097;   // 4096 intervals + 1 endpoint

struct FusedLut {
    std::array<float, kLutSize> table;
};

// ============================================================================
// Constants
// ============================================================================
// Linear middle gray is 0.18. In sRGB-encoded space this lands at ~0.4613.
// We pre-compute this so the contrast pivot stays exactly on the perceptual
// representative of linear 0.18, not the naive 0.5.
// (sRGB encode of 0.18: 1.055 * 0.18^(1/2.4) - 0.055 ≈ 0.4613)
constexpr float kPivotPerceptual = 0.4613f;

// Tiny epsilon for "slider parked at zero" checks.
constexpr float kEps = 1e-4f;

// ============================================================================
// Smooth falloff mask — used to confine endpoint operations to their region
//
// Returns a weight in [0, 1]:
//   falloffFromZero(x):   1 at x=0, 0 at x ≥ boundary. Smooth cubic decay.
//   falloffFromOne(x):    0 at x ≤ (1 - boundary), 1 at x=1. Smooth cubic.
//
// These let each endpoint operation ramp its strength down as the input
// approaches midtones, so edits made to shadows don't bleed into the
// midtones (and vice versa for highlights). This is the single biggest
// quality improvement over the previous tone pass — gamma-style lifts
// visibly flatten midtones, whereas masked lifts feel local.
// ============================================================================
inline float falloffFromZero(float x, float boundary)
{
    // t ramps from 1 at x=0 to 0 at x=boundary, with smoothstep shaping.
    if (x <= 0.0f) return 1.0f;
    if (x >= boundary) return 0.0f;
    const float u = 1.0f - (x / boundary);   // u: 1 → 0
    return u * u * (3.0f - 2.0f * u);         // smoothstep
}

inline float falloffFromOne(float x, float boundary)
{
    // t ramps from 0 at x=(1-boundary) to 1 at x=1.
    const float start = 1.0f - boundary;
    if (x <= start) return 0.0f;
    if (x >= 1.0f)  return 1.0f;
    const float u = (x - start) / boundary;   // u: 0 → 1
    return u * u * (3.0f - 2.0f * u);
}

// ============================================================================
// filmicCurve — sigmoid-based S-curve pivoted at linear middle gray (0.18)
//
// The canonical "filmic" tone response is a sigmoid. Smoothstep-based S-curves
// (x²(3-2x)) are monotonic and clean but their slope at the pivot is only
// 1.5×, giving an S that feels soft and digital. Real film curves have
// steeper pivot slopes (2-4×) with asymptotic shoulders.
//
// Implementation: we use a centered sigmoid of the form
//
//     S(u) = 1 / (1 + exp(-k * u))
//
// normalized so that S(-∞) = 0, S(+∞) = 1, S(0) = 0.5. Inside [0,1] the raw
// sigmoid never quite reaches 0 or 1, so we renormalize: subtract the
// sigmoid evaluated at the endpoints and rescale. The result is a smooth
// S that exactly maps 0→0, pivot→pivot, 1→1, with a tunable steepness k.
//
// k is driven by the contrast slider:
//   contrast =  0   → k → 0        (identity line, but we early-out for this)
//   contrast = +25  → k ≈ 2        (gentle "natural" S)
//   contrast = +50  → k ≈ 4        (moderate cinematic S)
//   contrast = +100 → k ≈ 8        (strong, still monotonic, no clipping)
//
// Negative contrast "flattens" toward the pivot (de-contrast). We build the
// flatten operation as a linear interpolation toward the pivot, same as
// before — this is mathematically the right inverse of "add S-curve shape".
//
// Pivot handling: the sigmoid pivots at u=0. We want it pivoted at
// kPivotPerceptual. So we center the input around the pivot, evaluate, then
// center the output the same way — which is a simple affine transform.
// ============================================================================
float filmicCurve(float x, float contrast)
{
    if (std::fabs(contrast) < kEps) return x;

    const float t = math::clamp(contrast / 100.0f, -1.0f, 1.0f);

    if (t < 0.0f) {
        // De-contrast: lerp toward the pivot. Preserves pivot exactly.
        // At t=-1 the image collapses to uniform kPivotPerceptual (extreme
        // but mathematically well-defined).
        return math::lerp(x, kPivotPerceptual, -t);
    }

    // Positive contrast: build a sigmoid-based S pivoted at kPivotPerceptual.
    //
    // Steepness k grows with contrast slider. We use a gentle quadratic
    // mapping so small slider moves give small contrast changes (the spec's
    // "small contrast changes should be subtle" requirement) while the top
    // of the range still reaches a cinematic intensity.
    //
    // k = 1.5 at t=0+, 8.5 at t=1:
    const float k = 1.5f + t * t * 7.0f;

    // Rescale x around the pivot so that x=pivot maps to u=0, and the
    // endpoints 0 and 1 map to symmetric negative/positive values. We map
    // 0 → -1 and 1 → +1 relative to the pivot:
    //   u = (x - pivot) / (pivot if x < pivot else 1 - pivot)
    // This keeps the curve symmetric on each side of the pivot in the space
    // where the sigmoid is evaluated, even though the pivot isn't at 0.5.
    const float u = (x < kPivotPerceptual)
        ? (x - kPivotPerceptual) / kPivotPerceptual               // -1 .. 0
        : (x - kPivotPerceptual) / (1.0f - kPivotPerceptual);     //  0 .. +1

    // Sigmoid of the scaled input, normalized to [0,1] with pivot→pivot:
    //   s_at_pivot = 0.5, s_at_minus1 = sigmoid(-k), s_at_plus1 = sigmoid(+k)
    // We want:
    //   0 → 0  (need to subtract s_at_minus1 and rescale)
    //   1 → 1
    // The clean form: map the raw sigmoid S(u) into [S(-k), S(+k)] then
    // stretch to [0, 1]. Because sigmoid is symmetric, S(-k) = 1 - S(+k).
    const float sigmoid_u = 1.0f / (1.0f + std::exp(-k * u));
    const float sigmoid_neg1 = 1.0f / (1.0f + std::exp(k));
    const float sigmoid_pos1 = 1.0f / (1.0f + std::exp(-k));
    const float stretched = (sigmoid_u - sigmoid_neg1)
                          / (sigmoid_pos1 - sigmoid_neg1);    // now in [0,1]

    // Now stretched is a clean S on [0,1] pivoted at the "middle" (u=0) of
    // the sigmoid. But our pivot in the OUTPUT domain is kPivotPerceptual,
    // not 0.5. So we remap: below pivot, stretched ∈ [0, 0.5] → output
    // ∈ [0, kPivotPerceptual]; above pivot, stretched ∈ [0.5, 1] → output
    // ∈ [kPivotPerceptual, 1].
    float y;
    if (stretched < 0.5f) {
        y = stretched * (kPivotPerceptual / 0.5f);
    } else {
        y = kPivotPerceptual
          + (stretched - 0.5f) * ((1.0f - kPivotPerceptual) / 0.5f);
    }
    return y;
}

// ============================================================================
// shadowShape — toe with midtone falloff
//
// Lifts or deepens values near zero. Effect strength falls off to zero by
// x = kShadowBoundary (~0.5), so midtones are preserved. Black point always
// stays at 0 regardless of slider direction.
//
// Mechanism: compute a pure gamma shift (lift or crush), then blend between
// identity and the shifted value using falloffFromZero as the mask weight.
// Result:
//   - At x = 0:  full gamma effect, but f(0) = 0 since pow(0, anything) = 0
//   - At x = boundary: zero effect, f = identity
//   - In between: smooth blend, no visible kink
// ============================================================================
float shadowShape(float x, float amount)
{
    if (std::fabs(amount) < kEps) return x;
    const float sx = math::clamp01(x);

    const float t = math::clamp(amount / 100.0f, -1.0f, 1.0f);

    // Effect region: 0 through ~halfway-to-midtones.
    constexpr float kShadowBoundary = 0.5f;
    const float weight = falloffFromZero(sx, kShadowBoundary);
    if (weight <= 0.0f) return sx;

    // Gamma-style lift/crush. Scaled gently — full slider is noticeable but
    // not extreme, because "professional cinematic" means the slider has
    // headroom beyond typical use.
    //   t = +1 → exponent = 0.5  (square-root: strong lift)
    //   t = -1 → exponent = 2.0  (square: strong crush)
    const float gamma = 1.0f + std::fabs(t);
    const float exponent = (t > 0.0f) ? (1.0f / gamma) : gamma;
    const float shifted = std::pow(sx, exponent);

    // Blend: only the masked region is shifted.
    return math::lerp(sx, shifted, weight);
}

// ============================================================================
// highlightShape — broad highlight region shaping with midtone falloff
//
// Mirror of shadowShape. Effect confined to x ≥ ~0.5, peaks at 1.
// Positive amount pushes near-whites brighter; negative "recovers" (compresses
// bright values toward the midtones to preserve detail near the ceiling).
//
// Uses a gamma shift identical in form to shadowShape, mirrored around x=1:
//   - define y = 1 - pow(1 - x, exponent)
//   - this preserves f(1) = 1 and creates symmetric behavior vs shadowShape
//   - positive t → exponent < 1 → pushes brights UP (brighter near-whites)
//   - negative t → exponent > 1 → compresses brights DOWN (recovery)
//
// Why this is better than a Reinhard shoulder here:
//   - Exact symmetry with shadowShape — predictable for photographers
//   - Clean f(1)=1 anchor, unlike Reinhard which needs rescaling
//   - Falloff mask handles the midtone transition so there's no visible knee
//   - Bounded: pow preserves [0,1] for base in [0,1]
// ============================================================================
float highlightShape(float x, float amount)
{
    if (std::fabs(amount) < kEps) return x;
    const float sx = math::clamp01(x);

    const float t = math::clamp(amount / 100.0f, -1.0f, 1.0f);

    constexpr float kHighlightBoundary = 0.5f;
    const float weight = falloffFromOne(sx, kHighlightBoundary);
    if (weight <= 0.0f) return sx;

    // Mirror-of-shadow gamma shift. Operating on (1 - x):
    //   base = 1 - x   (so base ranges 0 at x=1 → 1 at x=0)
    //   shifted = 1 - pow(base, exponent)
    // Positive t (push) → exponent < 1 → pow(base) bigger → 1 - bigger = smaller base
    //   actually we want push = brighter, so this direction needs exponent > 1.
    //   exponent > 1 → pow(base) smaller → 1 - smaller = closer to 1 (brighter). ✓
    // Negative t (recover) → exponent < 1 → 1 - pow bigger = darker near-whites. ✓
    const float gamma = 1.0f + std::fabs(t);
    const float exponent = (t > 0.0f) ? gamma : (1.0f / gamma);
    const float base = 1.0f - sx;
    const float shifted = 1.0f - std::pow(base, exponent);

    return math::lerp(sx, shifted, weight);
}

// ============================================================================
// blacksShape — deep shadow endpoint shaping, preserves black point
//
// Different from shadowShape: blacksShape operates on a much tighter region
// near x=0 (boundary ~0.2 instead of ~0.5). Photographers use it for the
// "floor" of the image — how dark the darkest tones get. Shadow is for the
// overall shadow region, blacks is for the crush point itself.
//
// Positive: lift deep blacks (for "faded" looks)
// Negative: deepen (crush deeper)
//
// CRITICAL: the previous version did `y = lift + y*(1-lift)` which moved
// f(0) off zero. That's the "washed-out gray shadow" bug the spec flags.
// Here we use the falloff-masked approach so f(0) stays exactly 0 unless
// the slider is literally asking for lifted blacks (and even then, it's
// a subtle near-zero lift, not a global offset).
// ============================================================================
float blacksShape(float x, float amount)
{
    if (std::fabs(amount) < kEps) return x;
    const float sx = math::clamp01(x);

    const float t = math::clamp(amount / 100.0f, -1.0f, 1.0f);

    // Tighter region than shadows — blacks affects only the bottom ~20%.
    constexpr float kBlacksBoundary = 0.2f;
    const float weight = falloffFromZero(sx, kBlacksBoundary);
    if (weight <= 0.0f) return sx;

    // Stronger gamma curve for the narrow region.
    //   t = +1 → exponent = 0.4  (aggressive lift within the tight region)
    //   t = -1 → exponent = 2.5  (deep crush)
    const float gamma = 1.0f + std::fabs(t) * 1.5f;
    const float exponent = (t > 0.0f) ? (1.0f / gamma) : gamma;
    const float shifted = std::pow(sx, exponent);

    return math::lerp(sx, shifted, weight);
}

// ============================================================================
// whitesShape — bright endpoint shaping, preserves white point
//
// Same philosophy as blacksShape — tight region near x=1, preserves f(1)=1.
// Positive pushes near-whites brighter (more of them reach the ceiling),
// negative compresses (less of them do). Distinct from highlightShape:
// highlights shapes the broader highlight region; whites is the ceiling.
// ============================================================================
float whitesShape(float x, float amount)
{
    if (std::fabs(amount) < kEps) return x;
    const float sx = math::clamp01(x);

    const float t = math::clamp(amount / 100.0f, -1.0f, 1.0f);

    constexpr float kWhitesBoundary = 0.2f;
    const float weight = falloffFromOne(sx, kWhitesBoundary);
    if (weight <= 0.0f) return sx;

    const float gamma = 1.0f + std::fabs(t) * 1.5f;
    const float exponent = (t > 0.0f) ? (1.0f / gamma) : gamma;
    const float shifted = std::pow(sx, exponent);

    return math::lerp(sx, shifted, weight);
}

// ============================================================================
// brightnessShape — midtone-weighted lift, preserves both endpoints
//
// Distinct from exposure (linear multiply that can blow highlights or push
// shadows below zero) and from contrast (S-curve that lifts highs while
// crushing lows). Brightness is a midtone-weighted offset:
//
//   shifted = x + δ · sin(π·x)
//
// sin(π·x) is exactly zero at x=0 and x=1, peaks at x=0.5 — a clean bump
// that preserves both endpoints without any masking gymnastics. The shape
// matches what photographers expect from a "brightness" slider: at +50,
// midtones lift visibly while highlights and shadows stay where they are.
//
// Range mapping: amount ∈ [-100, +100] → δ ∈ [-0.20, +0.20]. At δ=0.20,
// a perceptual mid-gray (x=0.5) shifts to 0.70 — strong but not extreme.
// We don't need a separate clamp01 at the output because the magnitude
// is bounded by construction (sin·δ_max = 0.20 < 0.5), but we apply one
// defensively to absorb floating-point noise at the endpoints.
// ============================================================================
float brightnessShape(float x, float amount)
{
    if (std::fabs(amount) < kEps) return x;
    const float sx = math::clamp01(x);
    const float t = math::clamp(amount / 100.0f, -1.0f, 1.0f);

    // Peak shift at midpoint. Chosen so +100 produces a substantial but
    // not-extreme lift; matches the perceptual feel of Lightroom's
    // brightness slider at full deflection.
    constexpr float kPeak = 0.20f;
    constexpr float kPi   = 3.14159265358979323846f;

    // sin(π·x): 0 at x=0, 1 at x=0.5, 0 at x=1.
    const float bump = std::sin(kPi * sx);
    return math::clamp01(sx + t * kPeak * bump);
}

// ============================================================================
// LUT build
// ============================================================================
FusedLut buildLut(const ToneParams& p)
{
    FusedLut lut{};
    const float expMul = Exposure::multiplier(p.exposure);

    for (int i = 0; i < kLutSize; ++i) {
        // Input: linear-light value in [0, 1].
        float v = static_cast<float>(i) / (kLutSize - 1);

        // 1) Exposure — physically correct linear 2^stops multiply.
        v = v * expMul;

        // 2) Enter perceptual space for shaping. Clamp defensively; anything
        //    pushed past 1 by exposure will be clipped here, which is the
        //    correct behavior (highlight shoulder couldn't recover detail
        //    that was never there).
        float s = colorspace::linearToSrgb(math::clamp01(v));

        // 3) Endpoint shaping — tight regions near 0 and 1.
        s = blacksShape(s, p.blacks);
        s = whitesShape(s, p.whites);

        // 4) Regional shaping — wider falloff into midtones.
        s = shadowShape(s, p.shadows);
        s = highlightShape(s, p.highlights);

        // 5) Brightness — midtone-weighted bump with endpoint preservation.
        //    Sits before the filmic S-curve so its bump becomes part of the
        //    contour the S-curve operates on, rather than being squashed by
        //    the S-curve's pivot slope.
        s = brightnessShape(s, p.brightness);

        // 6) Filmic S-curve — the global contour.
        s = filmicCurve(s, p.contrast);

        // 7) Safe clamp + back to linear for the pipeline.
        s = math::clamp01(s);
        v = colorspace::srgbToLinear(s);

        lut.table[static_cast<size_t>(i)] = v;
    }
    return lut;
}

// ============================================================================
// LUT sampling — linear interpolation, overshoot-safe
// ============================================================================
inline float sample(const FusedLut& lut, float v)
{
    if (!(v > 0.0f)) return lut.table[0];   // catches 0, negative, NaN

    const float idx = v * (kLutSize - 1);
    if (idx >= static_cast<float>(kLutSize - 1)) {
        // Above-range: extend with last-slope extrapolation. Inputs > 1
        // can happen if an earlier engine boosted a channel; we don't want
        // a hard clamp mid-pipeline.
        const float last = lut.table[kLutSize - 1];
        const float prev = lut.table[kLutSize - 2];
        const float slope = last - prev;
        return last + (idx - static_cast<float>(kLutSize - 1)) * slope;
    }

    const int i0 = static_cast<int>(idx);
    const int i1 = i0 + 1;
    const float t = idx - static_cast<float>(i0);
    return lut.table[static_cast<size_t>(i0)] * (1.0f - t)
         + lut.table[static_cast<size_t>(i1)] * t;
}

} // namespace

// ============================================================================
// apply — unchanged interface. Scanline iteration with pointer access.
// ============================================================================
void ToneEngine::apply(PixelBuffer& buffer, const ToneParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    const FusedLut lut = buildLut(params);
    const int width = buffer.width();

    forEachScanline(buffer, [&](float* row, int /*y*/) {
        for (int x = 0; x < width; ++x) {
            float* p = row + x * 4;
            p[0] = sample(lut, p[0]);   // R
            p[1] = sample(lut, p[1]);   // G
            p[2] = sample(lut, p[2]);   // B
            // alpha untouched
        }
    });
}

} // namespace lps
