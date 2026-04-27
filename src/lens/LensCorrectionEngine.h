// ==============================================================================
// lens/LensCorrectionEngine.h
//
// Applies lens-correction stage to the working PixelBuffer. Position in
// the pipeline: very early — before tone/color/curves — so all downstream
// stages see geometrically and brightness-corrected pixels. Matches
// Lightroom's convention.
//
// V1 scope:
//   - Vignetting correction:      real radial brightness compensation
//                                  (gain = 1 + amount × distance²)
//   - Distortion:                 placeholder no-op. Real barrel/pincushion
//                                  needs a separate read buffer + bilinear
//                                  resample, which is a non-trivial
//                                  follow-up.
//   - Chromatic-aberration / fringe: placeholder no-op. UI persists and
//                                  values round-trip; engine ignores them.
//
// `params.enabled` is a master kill switch. Identity check on params
// short-circuits the apply.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class LensCorrectionEngine
{
public:
    // Applies lens correction in place on a linear-light float buffer.
    // Caller is responsible for ensuring buffer is non-null and in linear
    // space. The engine internally early-outs on identity params.
    static void apply(PixelBuffer& buffer, const LensParams& params);
};

} // namespace lps
