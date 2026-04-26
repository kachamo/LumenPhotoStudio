// ==============================================================================
// core/Look.h
// The central data model. Every edit the user makes lives here.
//
// Principles:
//   - Plain data. No QObject, no signals. Trivially copyable.
//   - Every sub-struct has a neutral default (Look{} is identity).
//   - Every sub-struct has isIdentity() for fast-path skipping in engines.
//   - Parameter ranges are DOCUMENTED inline; engines assume inputs respect
//     them. The Look::clampRanges() method enforces limits on the whole
//     struct before use — pipeline does this automatically.
//
// Serialization (.lxp JSON) lives in preset/LookSerializer. Adding a field
// here requires updating three places: this header, Look.cpp's isIdentity,
// and the serializer.
// ==============================================================================
#pragma once

#include <QPointF>
#include <QString>

#include <vector>

namespace lps {

// ============================================================================
// Tone (pipeline stage 1)
// ============================================================================
struct ToneParams
{
    float exposure   = 0.0f;   // stops, [-10, +10]. ±5 is a sane UI range.
    float contrast   = 0.0f;   // [-100, +100]
    float highlights = 0.0f;   // [-100, +100] — affects upper half of tone curve
    float shadows    = 0.0f;   // [-100, +100] — affects lower half
    float whites     = 0.0f;   // [-100, +100] — clipping point for brights
    float blacks     = 0.0f;   // [-100, +100] — clipping point for darks
    float brightness = 0.0f;   // [-100, +100] — midtone-weighted shift,
                               //   preserves both endpoints (0→0, 1→1).
                               //   Distinct from exposure (which is a multiply
                               //   that can blow highlights) — brightness is
                               //   a curve adjustment localized to midtones.

    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Color (pipeline stage 2)
// ============================================================================
struct WhiteBalanceParams
{
    float temperature = 0.0f;  // [-100, +100] (blue <-> yellow shift)
    float tint        = 0.0f;  // [-100, +100] (green <-> magenta)

    bool isIdentity() const;
    void clampRanges();
};

// Per-hue HSL channel. Applied to pixels whose hue falls in this channel's band.
struct HSLChannel
{
    float hue        = 0.0f;   // [-100, +100] hue shift along the wheel
    float saturation = 0.0f;   // [-100, +100] saturation multiplier
    float luminance  = 0.0f;   // [-100, +100] lightness offset

    bool isIdentity() const;
    void clampRanges();
};

// 8 canonical photo hues. More than 6 gives skin-tone (orange/yellow) and
// foliage (green/aqua) separation without being unwieldy.
struct HSLParams
{
    HSLChannel red;
    HSLChannel orange;
    HSLChannel yellow;
    HSLChannel green;
    HSLChannel aqua;
    HSLChannel blue;
    HSLChannel purple;
    HSLChannel magenta;

    bool isIdentity() const;
    void clampRanges();
};

// Channel mixer: each output = weighted sum of the three inputs.
// Identity is the {1,0,0}/{0,1,0}/{0,0,1} matrix.
struct RGBMixerParams
{
    struct Row { float r = 1.0f, g = 0.0f, b = 0.0f; };
    Row redOutput   = { 1.0f, 0.0f, 0.0f };
    Row greenOutput = { 0.0f, 1.0f, 0.0f };
    Row blueOutput  = { 0.0f, 0.0f, 1.0f };

    bool isIdentity() const;
    void clampRanges();   // coefficients clamped to [-2, +2] (reasonable range)
};

struct ColorParams
{
    WhiteBalanceParams whiteBalance;
    float              vibrance   = 0.0f;   // [-100, +100] smart saturation
    float              saturation = 0.0f;   // [-100, +100] global saturation
    HSLParams          hsl;
    RGBMixerParams     rgbMixer;

    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Curves (pipeline stage 3)
// ============================================================================
// Control points in [0,1] x [0,1]. Identity curve = {(0,0), (1,1)}.
struct CurvePoints
{
    std::vector<QPointF> points = { {0.0, 0.0}, {1.0, 1.0} };

    bool isIdentity() const;
    void clampRanges();   // x and y coordinates clamped to [0,1], points sorted
};

struct CurveParams
{
    CurvePoints master;   // applies equally to R, G, B
    CurvePoints red;
    CurvePoints green;
    CurvePoints blue;

    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Grading (pipeline stage 4)
// ============================================================================
struct GradingParams
{
    // ---- LUT / film profile ------------------------------------------------
    QString lutPath;                       // empty = no LUT
    float   lutOpacity = 1.0f;             // [0, 1]
    QString filmProfileId;                 // empty = no profile
    float   filmProfileOpacity = 1.0f;     // [0, 1]

    // ---- 3-way color grading -----------------------------------------------
    // Lightroom/DaVinci-style color wheels for shadows, midtones, highlights,
    // plus a global tint. Each wheel parameterized by:
    //   hue        — 0..360 degrees, the color of the tint
    //   saturation — 0..100 percent, intensity of the tint color (0 = no shift)
    //   strength   — 0..100 percent, how strongly the tint is applied
    // Both saturation==0 and strength==0 produce no shift (defensive — either
    // way, tint magnitude is zero).
    //
    // Defaults: hue=0 (irrelevant when sat or strength is 0), saturation=0,
    // strength=0 → identity.
    float shadowsHue          = 0.0f;
    float shadowsSaturation   = 0.0f;
    float shadowsStrength     = 0.0f;
    float midtonesHue         = 0.0f;
    float midtonesSaturation  = 0.0f;
    float midtonesStrength    = 0.0f;
    float highlightsHue       = 0.0f;
    float highlightsSaturation = 0.0f;
    float highlightsStrength  = 0.0f;
    float globalHue           = 0.0f;
    float globalSaturation    = 0.0f;
    float globalStrength      = 0.0f;

    // balance  — [-100, +100]: shifts the tonal pivots between regions.
    //            Negative pulls the shadows/midtones boundary higher (more
    //            of the image counts as "shadows"), positive pushes it
    //            lower. Symmetric effect on the midtones/highlights pivot.
    // blending — [0, 100]: half-width of the smoothstep transition zones
    //            between regions. 0 = hard threshold, 100 = very soft
    //            (regions overlap heavily). Default 50.
    float balance  = 0.0f;
    float blending = 50.0f;

    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Effects (pipeline stage 5)
// ============================================================================
struct VignetteParams
{
    float amount    = 0.0f;    // [-100, +100] (negative = dark, positive = bright)
    float midpoint  = 50.0f;   // [0, 100]
    float feather   = 50.0f;   // [0, 100]
    float roundness = 0.0f;    // [-100, +100]

    bool isIdentity() const;
    void clampRanges();
};

struct GrainParams
{
    float amount = 0.0f;       // [0, 100]
    float size   = 25.0f;      // [0, 100]

    bool isIdentity() const;
    void clampRanges();
};

struct ClarityParams
{
    float amount = 0.0f;       // [-100, +100]

    bool isIdentity() const;
    void clampRanges();
};

struct EffectsParams
{
    VignetteParams vignette;
    GrainParams    grain;
    ClarityParams  clarity;

    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Look — aggregate
// ============================================================================
struct Look
{
    QString name;
    int     schemaVersion = 1;

    ToneParams    tone;
    ColorParams   color;
    CurveParams   curves;
    GradingParams grading;
    EffectsParams effects;

    bool isIdentity() const;
    void clampRanges();
    void reset();   // resets every field to neutral (Look{} assignment)
};

} // namespace lps
