// ==============================================================================
// core/Look.cpp
//
// isIdentity uses a small float epsilon (1e-4) — smaller than any meaningful
// slider increment but safely above float round-trip noise from preset I/O.
//
// clampRanges enforces documented parameter bounds so engines can rely on
// them without re-checking. The pipeline calls clampRanges() once per
// render, so per-engine bounds checks are unnecessary.
// ==============================================================================
#include "core/Look.h"

#include "util/ColorMath.h"

#include <algorithm>
#include <cmath>

namespace lps {

using math::clamp;
using math::nearZero;

// ---- Tone -------------------------------------------------------------------
bool ToneParams::isIdentity() const
{
    return nearZero(exposure)   && nearZero(contrast)
        && nearZero(highlights) && nearZero(shadows)
        && nearZero(whites)     && nearZero(blacks)
        && nearZero(brightness);
}

void ToneParams::clampRanges()
{
    exposure   = clamp(exposure,   -10.0f, +10.0f);
    contrast   = clamp(contrast,   -100.0f, +100.0f);
    highlights = clamp(highlights, -100.0f, +100.0f);
    shadows    = clamp(shadows,    -100.0f, +100.0f);
    whites     = clamp(whites,     -100.0f, +100.0f);
    blacks     = clamp(blacks,     -100.0f, +100.0f);
    brightness = clamp(brightness, -100.0f, +100.0f);
}

// ---- WhiteBalance -----------------------------------------------------------
bool WhiteBalanceParams::isIdentity() const
{
    return nearZero(temperature) && nearZero(tint);
}

void WhiteBalanceParams::clampRanges()
{
    temperature = clamp(temperature, -100.0f, +100.0f);
    tint        = clamp(tint,        -100.0f, +100.0f);
}

// ---- HSLChannel / HSLParams -------------------------------------------------
bool HSLChannel::isIdentity() const
{
    return nearZero(hue) && nearZero(saturation) && nearZero(luminance);
}

void HSLChannel::clampRanges()
{
    hue        = clamp(hue,        -100.0f, +100.0f);
    saturation = clamp(saturation, -100.0f, +100.0f);
    luminance  = clamp(luminance,  -100.0f, +100.0f);
}

bool HSLParams::isIdentity() const
{
    return red.isIdentity()    && orange.isIdentity() && yellow.isIdentity()
        && green.isIdentity()  && aqua.isIdentity()   && blue.isIdentity()
        && purple.isIdentity() && magenta.isIdentity();
}

void HSLParams::clampRanges()
{
    red.clampRanges();    orange.clampRanges();  yellow.clampRanges();
    green.clampRanges();  aqua.clampRanges();    blue.clampRanges();
    purple.clampRanges(); magenta.clampRanges();
}

// ---- RGBMixer ---------------------------------------------------------------
bool RGBMixerParams::isIdentity() const
{
    return nearZero(redOutput.r - 1.0f)   && nearZero(redOutput.g)        && nearZero(redOutput.b)
        && nearZero(greenOutput.r)        && nearZero(greenOutput.g - 1.0f) && nearZero(greenOutput.b)
        && nearZero(blueOutput.r)         && nearZero(blueOutput.g)        && nearZero(blueOutput.b  - 1.0f);
}

void RGBMixerParams::clampRanges()
{
    auto clampRow = [](Row& r) {
        r.r = clamp(r.r, -2.0f, 2.0f);
        r.g = clamp(r.g, -2.0f, 2.0f);
        r.b = clamp(r.b, -2.0f, 2.0f);
    };
    clampRow(redOutput);
    clampRow(greenOutput);
    clampRow(blueOutput);
}

// ---- Color ------------------------------------------------------------------
bool ColorParams::isIdentity() const
{
    return whiteBalance.isIdentity()
        && nearZero(vibrance) && nearZero(saturation)
        && hsl.isIdentity()
        && rgbMixer.isIdentity();
}

void ColorParams::clampRanges()
{
    whiteBalance.clampRanges();
    vibrance   = clamp(vibrance,   -100.0f, +100.0f);
    saturation = clamp(saturation, -100.0f, +100.0f);
    hsl.clampRanges();
    rgbMixer.clampRanges();
}

// ---- CurvePoints / CurveParams ----------------------------------------------
bool CurvePoints::isIdentity() const
{
    if (points.size() != 2) return false;
    return nearZero(static_cast<float>(points[0].x() - 0.0))
        && nearZero(static_cast<float>(points[0].y() - 0.0))
        && nearZero(static_cast<float>(points[1].x() - 1.0))
        && nearZero(static_cast<float>(points[1].y() - 1.0));
}

void CurvePoints::clampRanges()
{
    if (points.size() < 2) {
        points = { {0.0, 0.0}, {1.0, 1.0} };
        return;
    }
    // Clamp each point to [0,1] x [0,1]; force endpoints.
    for (QPointF& p : points) {
        p.setX(std::clamp(p.x(), 0.0, 1.0));
        p.setY(std::clamp(p.y(), 0.0, 1.0));
    }
    std::sort(points.begin(), points.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    points.front().setX(0.0);
    points.back().setX(1.0);
}

bool CurveParams::isIdentity() const
{
    return master.isIdentity() && red.isIdentity()
        && green.isIdentity()  && blue.isIdentity();
}

void CurveParams::clampRanges()
{
    master.clampRanges();
    red.clampRanges();
    green.clampRanges();
    blue.clampRanges();
}

// ---- Grading ----------------------------------------------------------------
bool GradingParams::isIdentity() const
{
    // LUT counts as active only when enabled, set, and visible.
    const bool lutActive  = lutEnabled && !lutPath.isEmpty() && lutOpacity > 1e-4f;
    const bool filmActive = !filmProfileId.isEmpty() && filmProfileOpacity > 1e-4f;

    // Per-wheel activity check: a wheel produces a color tint only when BOTH
    // saturation and strength are non-zero. Either at zero produces no
    // *tint*, but luminance is independent — a non-zero luminance still
    // changes the image even with sat=0.
    auto wheelActive = [](float sat, float str, float lum) {
        return (sat > 1e-4f && str > 1e-4f) || !nearZero(lum);
    };
    const bool wheelsActive =
        wheelActive(shadowsSaturation,    shadowsStrength,    shadowsLuminance)    ||
        wheelActive(midtonesSaturation,   midtonesStrength,   midtonesLuminance)   ||
        wheelActive(highlightsSaturation, highlightsStrength, highlightsLuminance) ||
        wheelActive(globalSaturation,     globalStrength,     globalLuminance);

    // Advanced + filmic placeholders: V1 doesn't render them, but the
    // identity check honors them anyway so an "active" non-zero advanced
    // value still marks the Look non-identity (relevant for "Reset all"
    // detection and unsaved-changes prompts).
    const bool advActive =
        !nearZero(lift) || !nearZero(gamma) ||
        !nearZero(gain) || !nearZero(offset);
    const bool filmicActive =
        !nearZero(filmicContrast)   || !nearZero(highlightRolloff) ||
        !nearZero(shadowLift)       || !nearZero(fadeBlacks)       ||
        !nearZero(colorSeparation);

    return !lutActive && !filmActive && !wheelsActive
        && !advActive && !filmicActive;
}

void GradingParams::clampRanges()
{
    lutOpacity         = clamp(lutOpacity,         0.0f, 1.0f);
    filmProfileOpacity = clamp(filmProfileOpacity, 0.0f, 1.0f);

    // Hues are angular — wrap to [0, 360) instead of clamping (so a slider
    // landing at 360 doesn't snap to 0 as if it were out of range).
    auto wrapHue = [](float h) {
        h = std::fmod(h, 360.0f);
        return h < 0.0f ? h + 360.0f : h;
    };
    shadowsHue    = wrapHue(shadowsHue);
    midtonesHue   = wrapHue(midtonesHue);
    highlightsHue = wrapHue(highlightsHue);
    globalHue     = wrapHue(globalHue);

    shadowsSaturation    = clamp(shadowsSaturation,    0.0f, 100.0f);
    shadowsStrength      = clamp(shadowsStrength,      0.0f, 100.0f);
    shadowsLuminance     = clamp(shadowsLuminance,    -100.0f, 100.0f);
    midtonesSaturation   = clamp(midtonesSaturation,   0.0f, 100.0f);
    midtonesStrength     = clamp(midtonesStrength,     0.0f, 100.0f);
    midtonesLuminance    = clamp(midtonesLuminance,   -100.0f, 100.0f);
    highlightsSaturation = clamp(highlightsSaturation, 0.0f, 100.0f);
    highlightsStrength   = clamp(highlightsStrength,   0.0f, 100.0f);
    highlightsLuminance  = clamp(highlightsLuminance, -100.0f, 100.0f);
    globalSaturation     = clamp(globalSaturation,     0.0f, 100.0f);
    globalStrength       = clamp(globalStrength,       0.0f, 100.0f);
    globalLuminance      = clamp(globalLuminance,    -100.0f, 100.0f);

    balance  = clamp(balance,  -100.0f, 100.0f);
    blending = clamp(blending,    0.0f, 100.0f);

    // Advanced + filmic placeholders.
    lift   = clamp(lift,   -100.0f, 100.0f);
    gamma  = clamp(gamma,  -100.0f, 100.0f);
    gain   = clamp(gain,   -100.0f, 100.0f);
    offset = clamp(offset, -100.0f, 100.0f);

    filmicContrast   = clamp(filmicContrast,   -100.0f, 100.0f);
    highlightRolloff = clamp(highlightRolloff, -100.0f, 100.0f);
    shadowLift       = clamp(shadowLift,       -100.0f, 100.0f);
    fadeBlacks       = clamp(fadeBlacks,       -100.0f, 100.0f);
    colorSeparation  = clamp(colorSeparation,  -100.0f, 100.0f);
}

// ---- Effects ----------------------------------------------------------------
bool VignetteParams::isIdentity() const { return nearZero(amount); }

void VignetteParams::clampRanges()
{
    amount    = clamp(amount,    -100.0f, +100.0f);
    midpoint  = clamp(midpoint,     0.0f,  100.0f);
    feather   = clamp(feather,      0.0f,  100.0f);
    roundness = clamp(roundness, -100.0f, +100.0f);
}

bool GrainParams::isIdentity() const { return amount < 1e-4f; }

void GrainParams::clampRanges()
{
    amount = clamp(amount, 0.0f, 100.0f);
    size   = clamp(size,   0.0f, 100.0f);
}

bool ClarityParams::isIdentity() const { return nearZero(amount); }

void ClarityParams::clampRanges()
{
    amount = clamp(amount, -100.0f, +100.0f);
}

bool EffectsParams::isIdentity() const
{
    return vignette.isIdentity() && grain.isIdentity() && clarity.isIdentity();
}

void EffectsParams::clampRanges()
{
    vignette.clampRanges();
    grain.clampRanges();
    clarity.clampRanges();
}

// ---- LocalAdjustment --------------------------------------------------------
// A mask is "identity" (skippable) when disabled, or when all adjustment
// values are at zero — in either case it can't affect the output.
//
// We don't check geometry here; an enabled mask with non-zero adjustments
// is always considered active, even if its mask weight is zero everywhere
// (a degenerate gradient with start==end, or a radial with radius=0). The
// engine handles those edge cases at apply time.
bool LocalAdjustment::isIdentity() const
{
    if (!enabled) return true;
    return nearZero(exposure)   && nearZero(brightness)
        && nearZero(contrast)   && nearZero(saturation)
        && nearZero(temperature) && nearZero(tint);
}

void LocalAdjustment::clampRanges()
{
    // Geometry coordinates must stay finite but aren't strictly bounded —
    // a user could drag a gradient endpoint outside the image area for a
    // wider falloff. We clamp to a generous range that still keeps the math
    // well-behaved.
    auto clampPoint = [](QPointF& p) {
        p.setX(clamp(static_cast<float>(p.x()), -10.0f, 10.0f));
        p.setY(clamp(static_cast<float>(p.y()), -10.0f, 10.0f));
    };
    clampPoint(startPoint);
    clampPoint(endPoint);
    clampPoint(center);
    radius  = clamp(radius,  1e-4f, 10.0f);   // tiny min keeps math finite
    feather = clamp(feather, 0.0f,  1.0f);
    density = clamp(density, 0.0f,  1.0f);
    flow    = clamp(flow,    0.0f,  1.0f);
    // invert is bool; nothing to clamp.

    exposure    = clamp(exposure,    -10.0f, +10.0f);
    brightness  = clamp(brightness,  -100.0f, +100.0f);
    contrast    = clamp(contrast,    -100.0f, +100.0f);
    saturation  = clamp(saturation,  -100.0f, +100.0f);
    temperature = clamp(temperature, -100.0f, +100.0f);
    tint        = clamp(tint,        -100.0f, +100.0f);
}

// ---- LayerAdjustmentData ---------------------------------------------------
// Flat per-layer adjustment payload. Identity = every sub-struct is identity.
// clampRanges and reset just delegate to each sub-struct's helper.
bool LayerAdjustmentData::isIdentity() const
{
    return tone.isIdentity()
        && color.isIdentity()
        && curves.isIdentity()
        && grading.isIdentity()
        && effects.isIdentity();
}

void LayerAdjustmentData::clampRanges()
{
    tone.clampRanges();
    color.clampRanges();
    curves.clampRanges();
    grading.clampRanges();
    effects.clampRanges();
}

void LayerAdjustmentData::reset()
{
    *this = LayerAdjustmentData{};
}

// ---- AdjustmentLayer --------------------------------------------------------
// A layer is "identity" (skippable) when disabled, has zero opacity, or
// its payload is identity. Layer rendering can fast-path skip on this.
bool AdjustmentLayer::isIdentity() const
{
    if (!enabled) return true;
    if (opacity <= 1e-4f) return true;
    return adjustmentData.isIdentity();
}

void AdjustmentLayer::clampRanges()
{
    opacity = clamp(opacity, 0.0f, 1.0f);
    adjustmentData.clampRanges();
    // blendMode is a discrete enum; values outside the known set are
    // clamped at deserialize time, not here.
}

// ---- Look -------------------------------------------------------------------
bool Look::isIdentity() const
{
    if (!tone.isIdentity())    return false;
    if (!color.isIdentity())   return false;
    if (!curves.isIdentity())  return false;
    if (!grading.isIdentity()) return false;
    if (!effects.isIdentity()) return false;
    // Any active local adjustment makes the Look non-identity.
    for (const auto& la : localAdjustments) {
        if (!la.isIdentity()) return false;
    }
    // Any active adjustment layer makes the Look non-identity.
    for (const auto& al : adjustmentLayers) {
        if (!al.isIdentity()) return false;
    }
    return true;
}

void Look::clampRanges()
{
    tone.clampRanges();
    color.clampRanges();
    curves.clampRanges();
    grading.clampRanges();
    effects.clampRanges();
    for (auto& la : localAdjustments) la.clampRanges();
    for (auto& al : adjustmentLayers) al.clampRanges();
}

void Look::reset()
{
    const QString keepName = name;
    const int keepSchema = schemaVersion;
    *this = Look{};
    name = keepName;
    schemaVersion = keepSchema;
}

} // namespace lps
