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
    const bool lutActive  = !lutPath.isEmpty()       && lutOpacity        > 1e-4f;
    const bool filmActive = !filmProfileId.isEmpty() && filmProfileOpacity > 1e-4f;

    // Per-wheel activity check: a wheel produces a tint only when BOTH
    // saturation and strength are non-zero. Either at zero is identity.
    auto wheelActive = [](float sat, float str) {
        return sat > 1e-4f && str > 1e-4f;
    };
    const bool wheelsActive =
        wheelActive(shadowsSaturation,    shadowsStrength)    ||
        wheelActive(midtonesSaturation,   midtonesStrength)   ||
        wheelActive(highlightsSaturation, highlightsStrength) ||
        wheelActive(globalSaturation,     globalStrength);

    return !lutActive && !filmActive && !wheelsActive;
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
    midtonesSaturation   = clamp(midtonesSaturation,   0.0f, 100.0f);
    midtonesStrength     = clamp(midtonesStrength,     0.0f, 100.0f);
    highlightsSaturation = clamp(highlightsSaturation, 0.0f, 100.0f);
    highlightsStrength   = clamp(highlightsStrength,   0.0f, 100.0f);
    globalSaturation     = clamp(globalSaturation,     0.0f, 100.0f);
    globalStrength       = clamp(globalStrength,       0.0f, 100.0f);

    balance  = clamp(balance,  -100.0f, 100.0f);
    blending = clamp(blending,    0.0f, 100.0f);
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

// ---- Look -------------------------------------------------------------------
bool Look::isIdentity() const
{
    return tone.isIdentity()
        && color.isIdentity()
        && curves.isIdentity()
        && grading.isIdentity()
        && effects.isIdentity();
}

void Look::clampRanges()
{
    tone.clampRanges();
    color.clampRanges();
    curves.clampRanges();
    grading.clampRanges();
    effects.clampRanges();
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
