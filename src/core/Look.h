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
#include <QVector>

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
    bool    lutEnabled = true;             // master on/off; preserves opacity
                                           // when temporarily disabled
    QString filmProfileId;                 // empty = no profile
    float   filmProfileOpacity = 1.0f;     // [0, 1]

    // ---- 3-way color grading -----------------------------------------------
    // Lightroom/DaVinci-style color wheels for shadows, midtones, highlights,
    // plus a global tint. Each wheel parameterized by:
    //   hue        — 0..360 degrees, the color of the tint
    //   saturation — 0..100 percent, intensity of the tint color (0 = no shift)
    //   strength   — 0..100 percent, how strongly the tint is applied
    //   luminance  — [-100, +100], lightness offset within that tonal zone
    // Both saturation==0 and strength==0 produce no shift (defensive — either
    // way, tint magnitude is zero).
    //
    // Defaults: hue=0 (irrelevant when sat or strength is 0), saturation=0,
    // strength=0, luminance=0 → identity.
    float shadowsHue          = 0.0f;
    float shadowsSaturation   = 0.0f;
    float shadowsStrength     = 0.0f;
    float shadowsLuminance    = 0.0f;
    float midtonesHue         = 0.0f;
    float midtonesSaturation  = 0.0f;
    float midtonesStrength    = 0.0f;
    float midtonesLuminance   = 0.0f;
    float highlightsHue       = 0.0f;
    float highlightsSaturation = 0.0f;
    float highlightsStrength  = 0.0f;
    float highlightsLuminance = 0.0f;
    float globalHue           = 0.0f;
    float globalSaturation    = 0.0f;
    float globalStrength      = 0.0f;
    float globalLuminance     = 0.0f;

    // balance  — [-100, +100]: shifts the tonal pivots between regions.
    //            Negative pulls the shadows/midtones boundary higher (more
    //            of the image counts as "shadows"), positive pushes it
    //            lower. Symmetric effect on the midtones/highlights pivot.
    // blending — [0, 100]: half-width of the smoothstep transition zones
    //            between regions. 0 = hard threshold, 100 = very soft
    //            (regions overlap heavily). Default 50.
    float balance  = 0.0f;
    float blending = 50.0f;

    // ---- DaVinci-style advanced grading (V1: data + UI only, no engine) -----
    // Lift/Gamma/Gain/Offset operate on darks/midtones/brights/all
    // respectively. Engine support is a follow-up — these fields persist
    // and round-trip through save/load and undo/redo so projects authored
    // now will pick up the math when it lands.
    float lift   = 0.0f;   // [-100, +100]
    float gamma  = 0.0f;   // [-100, +100]
    float gain   = 0.0f;   // [-100, +100]
    float offset = 0.0f;   // [-100, +100]

    // ---- Filmic look controls (V1: data + UI only, no engine) ---------------
    // Same model — fields persist, engine math is follow-up.
    float filmicContrast   = 0.0f;   // [-100, +100]
    float highlightRolloff = 0.0f;   // [-100, +100]
    float shadowLift       = 0.0f;   // [-100, +100]
    float fadeBlacks       = 0.0f;   // [-100, +100]
    float colorSeparation  = 0.0f;   // [-100, +100]

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
// Local adjustments (pipeline stage between curves and grading)
//
// A LocalAdjustment is a mask + a small set of tone/color parameters that
// apply only where the mask weight > 0. The mask weight at any pixel is in
// [0, 1] and is computed from the mask's geometry and feather:
//
//   - LinearGradient: weight ramps along a line from startPoint to endPoint
//                     with smooth falloff controlled by feather
//   - RadialGradient: weight is 1 inside center+radius*(1-feather), falls
//                     off smoothly to 0 at center+radius
//   - Brush:          placeholder — no painting yet, falls through as zero
//                     weight everywhere (effectively inert)
//
// All geometry is stored in NORMALIZED image coordinates ∈ [0, 1] so the
// same Look applies cleanly regardless of preview vs. full-resolution
// rendering or rotation/flip operations.
//
// Tagged-union storage (one struct, fields used by type) is the cheapest
// approach to serialize and copy. Wasted bytes per mask are negligible.
// ============================================================================
enum class MaskType : int
{
    Brush          = 0,
    LinearGradient = 1,
    RadialGradient = 2,
};

struct LocalAdjustment
{
    QString  name;                         // user-visible label, e.g. "Sky"
    bool     enabled = true;               // per-mask kill switch
    MaskType type    = MaskType::LinearGradient;

    // ---- Geometry (used fields depend on `type`) ---------------------------
    // Linear: startPoint → endPoint defines the gradient axis; weight ramps
    //         from 0 at startPoint to 1 at endPoint, with feather controlling
    //         transition softness.
    QPointF  startPoint = QPointF(0.5, 0.0);
    QPointF  endPoint   = QPointF(0.5, 0.4);

    // Radial: a circle centered on `center` with radius `radius` (fraction
    //         of the image's smaller edge). Weight is 1 inside the inner
    //         radius (= radius * (1 - feather)) and 0 outside `radius`.
    QPointF  center     = QPointF(0.5, 0.5);
    float    radius     = 0.25f;

    // Common to Linear/Radial: 0 = hard edge, 1 = maximum smoothness.
    float    feather    = 0.5f;

    // ---- Mask modifiers ----------------------------------------------------
    // invert  — mirror the mask weight (w → 1 - w). After invert, density
    //           attenuates the result.
    // density — scales the mask's maximum weight, [0, 1]. 1 = full strength
    //           (default), 0 = no effect anywhere. Lightroom convention —
    //           softens the whole mask without changing geometry.
    // flow    — brush-only painting flow rate placeholder, [0, 1]. V1
    //           does not paint; the field round-trips for forward-compat.
    bool     invert     = false;
    float    density    = 1.0f;
    float    flow       = 1.0f;

    // Brush placeholder — stored stamps for forward-compatible serialization.
    // V1 doesn't paint anything; the engine treats brush masks as zero-weight.
    QVector<QPointF> brushStamps;

    // ---- Adjustment values ------------------------------------------------
    // Subset of ToneParams + WhiteBalance, scaled like the global sliders:
    //   exposure  in stops (typical UI range ±5)
    //   brightness, contrast, saturation, temperature, tint  in [-100, +100]
    float    exposure    = 0.0f;
    float    brightness  = 0.0f;
    float    contrast    = 0.0f;
    float    saturation  = 0.0f;
    float    temperature = 0.0f;
    float    tint        = 0.0f;

    // True iff this mask contributes nothing to the final image — either
    // disabled, or all adjustments at zero. The engine uses this for
    // fast-path skipping.
    bool isIdentity() const;
    void clampRanges();
};

// ============================================================================
// Adjustment layers (stackable, Photoshop/Lightroom-style)
//
// An AdjustmentLayer wraps a flat per-layer adjustment payload plus per-
// layer compositing controls: opacity, blend mode, and an enabled flag.
// Layers stack in render order and composite over the base Look's output.
//
// V1 scope — data and UI plumbing only:
//   - Data structure round-trips through LookSerializer
//   - UI can create / duplicate / delete / toggle layers
//   - Rendering is a placeholder. Full compositing (per-layer payload
//     render + masked blend) is a follow-up step. The base Look
//     continues to render normally.
//
// `adjustmentData` is a flat LayerAdjustmentData (Tone + Color + Curves +
// Grading + Effects), NOT a recursive Look. This avoids self-referential
// types and the stack-overflow risk on deeply nested files. Local masks
// and nested layers are intentionally NOT on layers in V1 — those live
// only on the top-level Look. The shape can grow in a future schema bump
// if needed.
//
// `maskRef` is a forward-compat placeholder — when local masks become
// addressable by ID, this will reference one. V1 leaves it empty.
// ============================================================================
enum class BlendMode : int
{
    Normal     = 0,   // alpha-weighted lerp by opacity
    Multiply   = 1,
    Screen     = 2,
    Overlay    = 3,
    SoftLight  = 4,
    HardLight  = 5,
    ColorDodge = 6,
    ColorBurn  = 7,
    Darken     = 8,
    Lighten    = 9,
    Difference = 10,
};

// Flat adjustment payload for a layer. Same five sub-structs as the top-
// level Look, minus localAdjustments and adjustmentLayers (which would
// make the type recursive). isIdentity / clampRanges / reset mirror the
// Look helpers but skip the recursive-only fields.
struct LayerAdjustmentData
{
    ToneParams    tone;
    ColorParams   color;
    CurveParams   curves;
    GradingParams grading;
    EffectsParams effects;

    bool isIdentity() const;
    void clampRanges();
    void reset();
};

struct AdjustmentLayer
{
    QString  name;
    bool     enabled = true;
    float    opacity = 1.0f;            // [0, 1] — 1.0 = fully opaque
    BlendMode blendMode = BlendMode::Normal;

    // Per-layer adjustment payload. Mutating this drives what the layer
    // "does." Default-constructed payload is identity; new layers start
    // as no-ops until the user adjusts them.
    LayerAdjustmentData adjustmentData;

    // Forward-compat: opaque reference to a mask in some future mask
    // registry. V1 stores an empty string and ignores it on render.
    // When mask IDs become a thing, callers will resolve this string
    // to a mask weight texture.
    QString maskRef;

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

    // Local masks. Applied after global tone/color/curves and before grading.
    // Order matters when masks overlap — later masks layer on top of earlier
    // masks via lerp(prevPixel, adjustedPixel, maskWeight).
    std::vector<LocalAdjustment> localAdjustments;

    // Stackable adjustment layers. Each carries its own complete Look plus
    // compositing controls. Layers render on top of the base Look's output
    // (V1: data only, render is a follow-up).
    std::vector<AdjustmentLayer> adjustmentLayers;

    bool isIdentity() const;
    void clampRanges();
    void reset();   // resets every field to neutral (Look{} assignment)
};

} // namespace lps
