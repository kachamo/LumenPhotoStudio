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
#include "effects/EffectsEngine.h"
#include "local/LocalAdjustmentEngine.h"
#include "grading/ColorGrading.h"
#include "tone/ToneEngine.h"

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

    // ----- Run engines in fixed order. Each engine:
    //   - Checks its own params.isIdentity() and returns early if so.
    //   - Mutates the shared `buffer` in place.
    //   - Does NOT allocate a second pixel buffer unless documented.

    ToneEngine::apply(buffer, look.tone);
    if (stage == Stage::AfterTone) {
        result.image     = buffer.toSrgbImage();
        result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    ColorEngine::apply(buffer, look.color);
    if (stage == Stage::AfterColor) {
        result.image     = buffer.toSrgbImage();
        result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    CurveEngine::apply(buffer, look.curves);
    if (stage == Stage::AfterCurve) {
        result.image     = buffer.toSrgbImage();
        result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    // Local masks slot between curves and grading. Per spec: "after global
    // tone/color but before grading." Each mask layers on the previous
    // result via masked lerp; LocalAdjustmentEngine internally early-outs
    // for empty/disabled mask lists.
    LocalAdjustmentEngine::apply(buffer, look.localAdjustments);

    ColorGrading::apply(buffer, look.grading);
    if (stage == Stage::AfterGrading) {
        result.image     = buffer.toSrgbImage();
        result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
        return result;
    }

    EffectsEngine::apply(buffer, look.effects);

    // ----- Convert back to sRGB for display / export.
    result.image     = buffer.toSrgbImage();
    result.elapsedMs = timer.nsecsElapsed() / 1'000'000.0;
    return result;
}

} // namespace lps
