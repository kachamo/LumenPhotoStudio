// ==============================================================================
// core/ImagePipeline.cpp
//
// Render flow:
//   1. QImage input  -->  QImage::copy()                 (ONE copy, required)
//   2. Copy          -->  PixelBuffer (sRGB->linear LUT, parallel)
//   3. Each engine apply(buffer, params.sub)             (in-place)
//   4. PixelBuffer   -->  QImage (linear->sRGB LUT, parallel)
//
// There is exactly one deep pixel copy of the input (step 1). The sRGB->linear
// conversion in step 2 allocates the float working buffer but does NOT copy
// the bytes of the input a second time — it reads the already-copied buffer
// and writes into the float buffer in a single pass.
// ==============================================================================
#include "core/ImagePipeline.h"

#include "color/ColorEngine.h"
#include "curve/CurveEngine.h"
#include "details/DetailsEngine.h"
#include "effects/EffectsEngine.h"
#include "hdr/HDRToneMapper.h"
#include "lens/LensCorrectionEngine.h"
#include "local/LocalAdjustmentEngine.h"
#include "grading/ColorGrading.h"
#include "tone/ToneEngine.h"
#include "transform/TransformEngine.h"

#include <QElapsedTimer>

namespace lps {

ImagePipeline::ImagePipeline()  = default;
ImagePipeline::~ImagePipeline() = default;

RenderResult ImagePipeline::render(const QImage& input, Look look) const
{
    return renderUpTo(input, std::move(look), Stage::AfterEffects);
}

RenderResult ImagePipeline::renderUpTo(const QImage& input, Look look, Stage stage) const
{
    RenderResult result;
    if (input.isNull()) return result;

    QElapsedTimer timer;
    timer.start();

    // ----- Clamp parameter ranges once. Engines assume inputs are in bounds.
    look.clampRanges();

    // ----- Identity fast path.
    if (look.isIdentity()) {
        result.image       = input.copy();      // THE single guaranteed copy
        result.wasIdentity = true;
        result.elapsedMs   = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    // ----- THE single deep copy of the input. --------------------------------
    // After this line, `workingSrgb` is independent of `input`. Everything
    // downstream reads from it; nothing else touches `input`.
    QImage workingSrgb = input.copy();

    // Stage::Input returns the sRGB-space copy as-is (format may be anything,
    // caller gets what they gave us).
    if (stage == Stage::Input) {
        result.image     = std::move(workingSrgb);
        result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    // ----- Convert to linear-float working buffer (one pass, no extra copy).
    PixelBuffer buffer = PixelBuffer::fromSrgbImage(workingSrgb);

    runEngines(buffer, look, stage);

    result.image     = buffer.toSrgbImage();
    result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
    return result;
}

void ImagePipeline::runEngines(PixelBuffer& buffer, const Look& look, Stage stage) const
{
    // ----- Run engines in fixed order. Each engine:
    //   - Checks its own params.isIdentity() and returns early if so.
    //   - Mutates the shared `buffer` in place.
    //   - Does NOT allocate a second pixel buffer unless documented.

    // Lens correction runs first — vignetting, distortion, and CA are
    // physical defects of the optical chain, so we correct them before
    // any tonal/color adjustments operate on the data. V1 implements
    // vignetting compensation; distortion / CA / fringe are data-only
    // placeholders.
    LensCorrectionEngine::apply(buffer, look.lens);
    if (stage == Stage::AfterLens) {
        return;
    }

    TransformEngine::apply(buffer, look.transform);
    if (stage == Stage::AfterTransform) {
        return;
    }

    HDRToneMapper::apply(buffer, look.hdr);

    ToneEngine::apply(buffer, look.tone);
    if (stage == Stage::AfterTone) {
        return;
    }

    ColorEngine::apply(buffer, look.color);
    if (stage == Stage::AfterColor) {
        return;
    }

    CurveEngine::apply(buffer, look.curves);
    if (stage == Stage::AfterCurve) {
        return;
    }

    // Local masks slot between curves and grading. Per spec: "after global
    // tone/color but before grading." Each mask layers on the previous
    // result via masked lerp; LocalAdjustmentEngine internally early-outs
    // for empty/disabled mask lists.
    LocalAdjustmentEngine::apply(buffer, look.localAdjustments);

    ColorGrading::apply(buffer, look.grading);
    if (stage == Stage::AfterGrading) {
        return;
    }

    DetailsEngine::apply(buffer, look.details);
    if (stage == Stage::AfterDetails) {
        return;
    }

    EffectsEngine::apply(buffer, look.effects);
}

ImagePipeline::BufferResult ImagePipeline::renderToBuffer(const QImage& input, Look look) const
{
    BufferResult result;
    if (input.isNull()) return result;

    QElapsedTimer timer;
    timer.start();

    look.clampRanges();
    result.wasIdentity = look.isIdentity();

    // fromSrgbImage() dispatches on format, so a 16-bit input stays 16-bit
    // through linearization. An identity Look still produces a real buffer —
    // the caller wants pixels, not a short-circuit to a QImage.
    result.buffer = PixelBuffer::fromSrgbImage(input);

    if (!result.wasIdentity)
        runEngines(result.buffer, look, Stage::AfterEffects);

    result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
    return result;
}

} // namespace lps
