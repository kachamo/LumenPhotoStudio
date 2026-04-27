// ==============================================================================
// core/ImagePipeline.h
// The central renderer.
//
// Memory discipline (STRICT):
//   - Exactly ONE input -> working-buffer conversion per render() call.
//   - All engines mutate the same PixelBuffer in place.
//   - No engine makes additional image copies. (Grain and effects that need
//     a read-only copy of the pre-effect state stage that locally and are
//     documented at the engine.)
//
// Color space:
//   - The working buffer is LINEAR-LIGHT float RGBA.
//   - sRGB->Linear happens at PixelBuffer::fromSrgbImage().
//   - Linear->sRGB happens at PixelBuffer::toSrgbImage().
//   - Every engine operates on linear values. They must NOT re-encode sRGB
//     internally.
//
// Identity fast path:
//   - Look::isIdentity() short-circuits the entire pipeline.
//   - Each engine's apply() also checks its own params.isIdentity() and
//     returns without touching pixels.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

#include <QImage>

namespace lps {

// Forward-compatible result type. Holds the rendered image today; designed
// to carry histograms, metadata, and timing info in future steps without
// breaking callers.
struct RenderResult
{
    QImage  image;

    // When true, the pipeline skipped processing because the Look was
    // identity. Callers that want to distinguish "rendered as no-op" from
    // "rendered with edits" can use this for logging / caching decisions.
    bool    wasIdentity = false;

    // Populated for debugging / perf tuning. Milliseconds for the full
    // render(), including sRGB<->linear conversions. Zero if not measured.
    double  elapsedMs = 0.0;
};

class ImagePipeline
{
public:
    ImagePipeline();
    ~ImagePipeline();

    // Run the full pipeline. Returns a RenderResult; input is unchanged.
    // The returned image is sRGB-encoded ARGB32.
    //
    // The Look is passed by value so clampRanges() can be applied locally
    // without mutating the caller's copy. This is cheap — Look is <1 KB.
    RenderResult render(const QImage& input, Look look) const;

    // Stage-granular variants (for A/B debugging, histograms at intermediate
    // stages). The returned image is always sRGB-encoded.
    enum class Stage {
        Input,
        AfterLens,
        AfterTransform,
        AfterTone,
        AfterColor,
        AfterCurve,
        AfterGrading,
        AfterDetails,
        AfterEffects,
    };
    RenderResult renderUpTo(const QImage& input, Look look, Stage stage) const;
};

} // namespace lps
