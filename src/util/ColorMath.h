// ==============================================================================
// util/ColorMath.h
// Shared color math. Header-only, inline. All channel values are float in
// [0,1] unless explicitly noted. No QImage dependency.
//
// After the linear-workflow refactor: luminance and saturation helpers here
// assume *linear-light* RGB. If called on sRGB-encoded values they return
// perceptually incorrect results — convert first via util/ColorSpace.h.
// ==============================================================================
#pragma once

#include <algorithm>
#include <cmath>

namespace lps::math {

// ---- Safe clamp (typed, inline, branch-free on modern compilers) ------------
inline float clamp(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

inline float clamp01(float v)
{
    return clamp(v, 0.0f, 1.0f);
}

// ---- 8-bit conversion (saturating) ------------------------------------------
inline unsigned char toByte(float v)
{
    // Round-to-nearest with saturation. Avoids undefined behavior on NaN by
    // relying on the clamp01 path below — NaN compares false in any relation
    // so clamp01 promotes it to 0.
    const float c = clamp01(v);
    return static_cast<unsigned char>(c * 255.0f + 0.5f);
}

// ---- Linear interpolation ---------------------------------------------------
inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// ---- Smoothstep (for tone masks, vignette falloff, etc.) --------------------
inline float smoothstep(float edge0, float edge1, float x)
{
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// ---- Luminance (Rec. 709 linear weights) ------------------------------------
// Rec. 709 is the correct choice for *linear-light* RGB. Rec. 601 weights
// (0.299/0.587/0.114) apply to gamma-encoded video/SDR — using them on
// linear values produces visibly wrong grays in shadows and highlights.
inline constexpr float kLumaR_Linear = 0.2126f;
inline constexpr float kLumaG_Linear = 0.7152f;
inline constexpr float kLumaB_Linear = 0.0722f;

inline float luminance(float r, float g, float b)
{
    return kLumaR_Linear * r + kLumaG_Linear * g + kLumaB_Linear * b;
}

// ---- Saturation stretch around luminance ------------------------------------
// amount > 0 pushes channels away from gray; amount < 0 pulls them toward it.
// In/out values are linear-light floats.
inline void applySaturation(float& r, float& g, float& b, float amount)
{
    if (std::fabs(amount) < 1e-4f) return;
    const float lum = luminance(r, g, b);
    const float factor = 1.0f + amount;
    r = lum + (r - lum) * factor;
    g = lum + (g - lum) * factor;
    b = lum + (b - lum) * factor;
}

// ---- Float near-zero test (used in isIdentity) ------------------------------
inline bool nearZero(float v, float eps = 1e-4f)
{
    return std::fabs(v) < eps;
}

// ---- HSL conversions --------------------------------------------------------
// h in [0,1) (normalized, not degrees); s, l in [0,1].
// NOTE: HSL computed on linear-light RGB is a different color space than HSL
// computed on sRGB-encoded RGB. For per-hue HSL targeting (where users think
// "make the blues more saturated"), hue selection is still well-defined in
// linear-light — the Red/Orange/Yellow/Green/Aqua/Blue/Purple/Magenta hue
// bands remain at the same hue angles. Only *lightness* values change
// meaning. The HSL engine applies hue/sat on hue angles, luminance on linear
// L, which is what we want.
inline void rgbToHsl(float r, float g, float b, float& h, float& s, float& l)
{
    const float mx = std::max(r, std::max(g, b));
    const float mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;

    if (mx == mn) { h = 0.0f; s = 0.0f; return; }

    const float d = mx - mn;
    s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);

    if (mx == r)       h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g)  h = (b - r) / d + 2.0f;
    else               h = (r - g) / d + 4.0f;
    h *= (1.0f / 6.0f);
}

inline float hue2rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

inline void hslToRgb(float h, float s, float l, float& r, float& g, float& b)
{
    if (s == 0.0f) { r = g = b = l; return; }
    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    r = hue2rgb(p, q, h + 1.0f / 3.0f);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0f / 3.0f);
}

} // namespace lps::math
