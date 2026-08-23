# Architecture

This document covers the engine's design: the data model, the render
pipeline, the color math, and the module map. It's aimed at contributors
who want to add or modify engine code. For product scope and what's
missing, see [`ROADMAP.md`](ROADMAP.md). For the dev loop and code style,
see [`../CONTRIBUTING.md`](../CONTRIBUTING.md).

## Core idea

```
RenderResult r = ImagePipeline::render(inputImage, look);
```

A `Look` is plain data capturing every adjustment. The pipeline is
stateless: feed it an image and a `Look`, get back a
`RenderResult { image, wasIdentity, elapsedMs }`.

```
   UI  ─mutates─▶  Look  ─read by─▶  ImagePipeline  ─produces─▶  RenderResult
```

## Pipeline (strict order, linear-light internally)

The actual stage order, verified against `ImagePipeline.cpp`:

```
  Input (sRGB QImage)
     │
     │  input.copy()            ← the ONE deep pixel copy
     ▼
  Working QImage (sRGB)
     │
     │  sRGB → Linear (piecewise IEC 61966-2-1, per-channel, via LUT)
     ▼
  PixelBuffer (float32 RGBA, linear-light)
     │
     ├──▶ LensCorrectionEngine   vignetting compensation (real); distortion,
     │                           chromatic aberration, fringe are data-only
     │                           placeholders — no pixel effect yet
     ├──▶ TransformEngine        crop → flip → rotate/straighten (bilinear
     │                           resample, real)
     ├──▶ HDRToneMapper          exposure bias + highlight compression /
     │                           shoulder tone-mapping
     ├──▶ ToneEngine             fused 4097-entry LUT: exposure ∘ H/S/W/B ∘
     │                           contrast ∘ brightness
     ├──▶ ColorEngine            WhiteBalance → Vibrance/Saturation → HSL →
     │                           RGBMixer
     ├──▶ CurveEngine            master ∘ per-channel curves (fused into a
     │                           single sRGB→sRGB LUT per channel, sampled
     │                           in perceptual/sRGB-encoded space)
     ├──▶ LocalAdjustmentEngine  linear-gradient, radial-gradient, and
     │                           brush masks — each masks a small tone/
     │                           color adjustment via per-pixel weight lerp
     ├──▶ ColorGrading           3-way color wheels + global tint, .cube
     │                           LUT + film profile (trilinear, cached).
     │                           Lift/Gamma/Gain/Offset and the filmic
     │                           controls are accepted and serialized but
     │                           NOT yet read by this stage — see below.
     ├──▶ DetailsEngine          sharpening (unsharp-mask with edge
     │                           masking) → luminance NR → color NR
     ├──▶ EffectsEngine          Clarity (midtone-contrast proxy) → Grain
     │                           → Vignette (roundness param unused — see
     │                           ROADMAP good-first-issues)
     │
     │  Linear → sRGB (piecewise, via 4096-entry interpolated LUT)
     ▼
  Output QImage (sRGB ARGB32)
```

Notes on things that are *not* in the render path even though they have UI
and data:

- `Look::adjustmentLayers` (the Photoshop-style layer stack with blend
  modes) is never read by `ImagePipeline::render()`. The data model and UI
  are real and persist correctly; compositing is unimplemented.
- `GradingParams::lift/gamma/gain/offset` and the filmic fields
  (`filmicContrast`, `highlightRolloff`, `shadowLift`, `fadeBlacks`,
  `colorSeparation`) round-trip through `Look.cpp` and
  `LookSerializer.cpp` but `ColorGrading.cpp` does not read them yet.

Each engine checks `params.isIdentity()` and early-exits. If the whole
`Look` is identity, `ImagePipeline::render()` short-circuits before even
creating the float buffer. `ImagePipeline::renderUpTo()` also exposes a
`Stage` enum for rendering up to any intermediate point in the pipeline
(used for stage-by-stage debugging/comparison and by `NodeGraphWidget`'s
diagram of the stage order).

## Color accuracy

All editing math happens in **linear-light** float32 RGBA. This matters
because:

- Exposure is a true multiplier (×2 = +1 stop), not a gamma-curve lie.
- White balance is physically meaningful per-channel scaling (see caveat
  below — it's not full chromatic adaptation).
- Contrast and Clarity pivot at linear middle gray (~0.18), not sRGB 0.5.
- Vignette multiplication darkens like real light falloff.
- Blending and LUT sampling produce correct intermediate colors.

The conversions use the real **piecewise IEC 61966-2-1 sRGB transfer
function**:

```
sRGB → Linear:  v/12.92                       if v ≤ 0.04045
                ((v + 0.055) / 1.055)^2.4     otherwise
```

NOT `pow(2.2)`, which is wrong by roughly 5 counts in the shadow region.

**Known accuracy caveats** (documented honestly rather than silently):

- White balance (`color/WhiteBalance.cpp`) is a luminance-compensated
  per-channel multiplier with a soft highlight shoulder — it matches
  Lightroom's Temperature/Tint slider *feel*, but it is not a full von
  Kries/Bradford chromatic-adaptation transform.
- There is no ICC/color-management layer. The pipeline assumes sRGB
  primaries throughout; there's no wide-gamut working space and no
  soft-proofing.
- RAW files are currently decoded by LibRaw to 8-bit sRGB
  (`io/RawImageLoader.cpp` hardcodes `output_bps = 8`,
  `output_color = 1`, `use_camera_wb = 1`) before entering the linear
  pipeline, so a RAW file's real dynamic range is truncated well before
  the float32 buffer ever sees it. This is tracked as a priority fix in
  `ROADMAP.md`.

## Memory discipline

- Exactly ONE deep copy of the input `QImage` per `render()`.
- All engines mutate the same `PixelBuffer` in place.
- sRGB↔linear conversions are full-buffer passes, not per-engine.
- A 24 MP image is roughly `24M × 4 channels × 4 bytes` ≈ 384 MB as a
  float working buffer. For preview resolution (~2 MP) that's ~32 MB,
  which is fine; full-resolution export trades memory for correctness/
  simplicity over a streaming approach (not implemented).

## Identity fast path

Every parameter struct has `isIdentity()`, true at neutral defaults. If
`Look::isIdentity()` is true, the pipeline copies input → output with no
conversion at all. Individual engines also self-check as a second line of
defense, so a `Look` with only, say, an exposure change still skips every
other engine's per-pixel work.

Value parameters have documented ranges enforced by `Look::clampRanges()`,
called once per render on a local copy of the `Look` (it's passed by
value into `render()` specifically so this local mutation is safe) —
corrupt or hand-edited presets can't push out-of-range values into the
math.

## Performance

- **LUT fusion** — `ToneEngine` fuses exposure + highlights/shadows/
  whites/blacks + contrast + brightness into one 4097-entry float LUT
  with linear interpolation; `CurveEngine` fuses master + per-channel
  curves into one sRGB→sRGB LUT per channel.
- **Parallel scanlines** — `util/ScanlineParallel.h`'s `forEachScanline()`
  dispatches rows to `QtConcurrent::blockingMap` above a 200,000-pixel
  threshold; below that it runs single-threaded (dispatch overhead isn't
  worth it at small sizes). This is the only concurrency/parallelism in
  the engine — there is no GPU path yet (see `ROADMAP.md`).
- **LUT cache** — parsed `.cube` files are cached by path; re-applying the
  same LUT across renders doesn't re-parse the file.
- **Identity short-circuit** — skips the pipeline entirely, copies input →
  output.

## Module map

Verified against `src/` and `CMakeLists.txt`'s `LPS_SOURCES` list.

```
src/
├── core/        Look (the central data model), ImagePipeline, PixelBuffer
├── io/          RawImageLoader (optional LibRaw), ImageMetadataReader
├── hdr/         HDRToneMapper — early linear-stage exposure bias + highlight
│                compression/shoulder tone-mapping
├── tone/        ToneEngine, Exposure, Contrast, HighlightsShadows
├── color/       ColorEngine, WhiteBalance, Vibrance, HSL, RGBMixer
├── curve/       CurveEngine, ToneCurve
├── grading/     ColorGrading, LUTLoader (.cube), FilmProfiles
├── details/     DetailsEngine — sharpening + luminance/color noise reduction
├── transform/   TransformEngine — crop, rotate, flip, straighten
├── effects/     EffectsEngine, Vignette, Grain, Clarity
├── local/       LocalAdjustmentEngine, MaskGeometry (linear/radial/brush
│                mask weight math, shared with the UI's mask overlay paint)
├── lens/        LensCorrectionEngine — vignetting (real); distortion/CA
│                are data-only placeholders
├── preset/      PresetManager (.lxp folder scanning), LookSerializer
├── plugins/     PluginManager — manifest-based plugin folder management;
│                no code-execution runtime yet (no QPluginLoader/QLibrary
│                use anywhere in the codebase)
├── project/     ProjectDocument (single image + single Look today — this
│                is why there's no catalog, see ROADMAP.md), 
│                ProjectSerializer, AutosaveManager
├── settings/    SettingsManager — app-level QSettings wrapper (window
│                state, recent files, theme, export defaults)
└── util/        ScanlineParallel.h, ColorMath.h, ColorSpace.h (header-only)
```

```
ui/
├── MainWindow.{h,cpp}          Main application window (~7,900 lines
│                               combined) — inspector panels, mask editor,
│                               layers panel, plugin manager dialog, undo/
│                               redo stack, rail navigation
├── PreviewWidget.{h,cpp}       Main image viewer; owns brush-stroke input
│                               handling and mask overlay rendering
├── CurveEditorWidget.{h,cpp}   Interactive tone-curve editor
├── ColorWheelWidget.{h,cpp}    3-way color-grading wheel control
├── HistogramWidget.{h,cpp}     Live histogram
├── NodeGraphWidget.{h,cpp}     Read-only diagram of the fixed pipeline
│                               stage order — does not drive rendering
├── ExportDialog.{h,cpp}        Export options (format, resize, color
│                               space — see ROADMAP for what's real there)
├── SecondaryViewerWindow.{h,cpp}  Fit-to-screen mirror of the main preview
│                               for a second monitor
├── WelcomeScreenWidget.{h,cpp}  Startup screen — recent files/projects,
│                               drag-and-drop open
├── EmptyStateOverlay.{h,cpp}   "No image loaded" placeholder view
└── main.cpp                    Application entry point
```

## The `Look` struct

```cpp
struct Look {
    QString name;
    int     schemaVersion;

    HDRParams       hdr;
    ToneParams      tone;
    ColorParams     color;
    CurveParams     curves;
    GradingParams   grading;
    DetailsParams   details;
    EffectsParams   effects;
    LensParams      lens;
    TransformParams transform;

    std::vector<LocalAdjustment>  localAdjustments;  // masks
    std::vector<AdjustmentLayer>  adjustmentLayers;   // NOT yet rendered

    bool isIdentity() const;
    void clampRanges();
    void reset();
};
```

Every sub-struct has its own `isIdentity()` and `clampRanges()`. **Adding a
field is a three-file change**: `core/Look.h` (declare it + document its
range), `core/Look.cpp` (fold it into the parent struct's `isIdentity()`
and `clampRanges()`), and `preset/LookSerializer.cpp` (read + write it in
the JSON serializer). This was verified directly against how the existing
Lift/Gamma/Gain/Offset fields are wired — they appear in exactly those
three files today, which is also *why* they compile and round-trip cleanly
despite having no engine behavior yet: the three-file contract is about
serialization completeness, not about an engine actually consuming the
field.

## `.lxp` preset format

Hardened JSON:

- Missing fields default to identity (forward-compatible when new fields
  are added).
- Wrong-type fields are ignored rather than crashing (robust against
  corruption or hand-editing).
- NaN/Inf values are rejected.
- `schemaVersion` is recorded; bump it for breaking changes (a migration
  hook is in place for that, though no migration has been needed yet).
- `clampRanges()` is applied to every loaded preset before use.

## Usage

```cpp
#include "core/ImagePipeline.h"
#include "preset/LookSerializer.h"

lps::Look look;
look.tone.exposure = 0.3f;
look.color.whiteBalance.temperature = 8.0f;
look.grading.lutPath = "/presets/Portra.cube";
look.grading.lutOpacity = 0.8f;

lps::ImagePipeline pipeline;
lps::RenderResult r = pipeline.render(input, look);
// r.image        — sRGB-encoded ARGB32 QImage
// r.wasIdentity   — true if Look had no edits
// r.elapsedMs     — render time, useful for perf tuning

lps::LookSerializer::saveToFile(look, "my_look.lxp");
```
