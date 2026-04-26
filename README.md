# Lumen Photo Studio V1

Non-destructive photo editing engine. Qt 6 / C++20 / CMake.

Engine-only static library — no UI.

## Core idea

```
RenderResult r = ImagePipeline::render(inputImage, look);
```

A `Look` is plain data capturing every adjustment. The pipeline is stateless:
feed it an image and a Look, get a `RenderResult { image, wasIdentity, elapsedMs }`.

```
   UI  ─mutates─▶  Look  ─read by─▶  ImagePipeline  ─produces─▶  RenderResult
```

## Pipeline (strict order, linear-light internally)

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
     ├──▶ ToneEngine           fused 4097-entry LUT: exposure ∘ H/S/W/B ∘ contrast
     ├──▶ ColorEngine          WhiteBalance → Vibrance/Sat → HSL → RGBMixer
     ├──▶ CurveEngine          master ∘ per-channel curves (sampled in perceptual space)
     ├──▶ ColorGrading         .cube LUT + film profile (trilinear, cached)
     ├──▶ EffectsEngine        Clarity → Grain → Vignette
     │
     │  Linear → sRGB (piecewise, via 4096-entry interpolated LUT)
     ▼
  Output QImage (sRGB ARGB32)
```

Each engine checks `params.isIdentity()` and early-exits. If the whole Look is
identity, the pipeline short-circuits before even creating the float buffer.

## Color accuracy

All editing math happens in **linear-light** float32 RGBA. This matters because:

- Exposure is a true multiplier (×2 = +1 stop), not a gamma-curve lie.
- White balance is physically meaningful per-channel scaling.
- Contrast pivots at linear middle gray (~0.18), not sRGB 0.5.
- Vignette multiplication darkens like real light falloff.
- Blending and LUT sampling produce correct intermediate colors.

The conversions use the real **piecewise IEC 61966-2-1 sRGB transfer function**:

```
sRGB → Linear:  v/12.92                       if v ≤ 0.04045
                ((v + 0.055) / 1.055)^2.4     otherwise
```

NOT `pow(2.2)`, which is wrong by ~5 counts in the shadow region.

## Memory discipline

- Exactly ONE deep copy of the input QImage per `render()`.
- All engines mutate the same `PixelBuffer` in place.
- sRGB↔linear conversions are full-buffer passes, not per-engine.
- 24 MP image ≈ 384 MB float working buffer. For preview (1800px ≈ 2 MP) this
  is ~32 MB, which is fine; full-res export trades memory for speed.

## Identity fast path

Every parameter struct has `isIdentity()` returning true at neutral defaults.
If `Look::isIdentity()` is true, the pipeline copies input → output with no
conversion. Engines self-check as a second line of defense.

Value parameters have documented ranges enforced by `Look::clampRanges()`,
called once per render on a local copy of the Look — corrupt presets can't
damage the pipeline.

## Folder layout

```
LumenPhotoStudio/
├── CMakeLists.txt
├── README.md
└── src/
    ├── core/        Look, ImagePipeline, PixelBuffer
    ├── tone/        ToneEngine, Exposure, Contrast, HighlightsShadows
    ├── color/       ColorEngine, WhiteBalance, Vibrance, HSL, RGBMixer
    ├── curve/       CurveEngine, ToneCurve
    ├── grading/     ColorGrading, LUTLoader, FilmProfiles
    ├── effects/     EffectsEngine, Vignette, Grain, Clarity
    ├── preset/      PresetManager, LookSerializer
    └── util/        ScanlineParallel.h, ColorMath.h, ColorSpace.h
```

## The Look struct

```cpp
struct Look {
    QString name;
    int     schemaVersion;
    ToneParams    tone;     // exposure, contrast, H/S/W/B
    ColorParams   color;    // WB, vibrance, HSL, RGB mixer
    CurveParams   curves;   // master + R/G/B curves
    GradingParams grading;  // LUT path + opacity, film profile id + opacity
    EffectsParams effects;  // clarity, grain, vignette

    bool isIdentity() const;
    void clampRanges();
    void reset();
};
```

Every sub-struct has its own `isIdentity()` and `clampRanges()`. Adding a
field is a three-file change: `Look.h`, `Look.cpp`, and `LookSerializer.cpp`.

## .lxp preset format

Hardened JSON:

- Missing fields default to identity (forward compat when adding fields)
- Wrong-type fields are ignored (robust against corruption)
- NaN/Inf rejected
- `schemaVersion` recorded; bump for breaking changes; migration hook in place
- `clampRanges()` applied to every loaded preset

## Performance

- **LUT fusion** — tone engine fuses exposure+H/S/W/B+contrast into one 4097-
  entry float LUT with linear interpolation; curve engine fuses master+per-
  channel curves per RGB plane.
- **Parallel scanlines** — `forEachScanline` dispatches to QtConcurrent above
  a 200k-pixel threshold; single-threaded below (dispatch overhead).
- **LUT cache** — parsed `.cube` files cached by path; re-applying same LUT
  across renders doesn't re-parse.
- **Identity short-circuit** — skips pipeline entirely, copies input → output.

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
// r.image — sRGB-encoded ARGB32 QImage
// r.wasIdentity — true if Look had no edits
// r.elapsedMs — render time for profiling

lps::LookSerializer::saveToFile(look, "my_look.lxp");
```

## Build

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x
cmake --build build --config Release
```

Produces `lps_engine` static library.

## Known limitations (V1)

- Vignette roundness parameter is stored but not wired into math
- Clarity is a midtone-contrast proxy; true unsharp-mask clarity needs a blur pass
- WB is per-channel multiplication (Lightroom-style); true chromatic adaptation
  (Bradford matrix) is future work
- `.cube` parser handles 1D and 3D; shaper + 3D combined LUTs not yet supported
- Film profile bundles are currently just LUT-paths; curve/response bundling is future
- No masks, no RAW, no UI (all explicitly excluded from V1)
