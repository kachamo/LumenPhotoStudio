// ==============================================================================
// local/LocalAdjustmentEngine.h
//
// Applies the masked local-adjustment stage of the pipeline. Operates on
// the linear-light float PixelBuffer in place. Per-mask cost: one mask-
// weight evaluation per pixel + a small cluster of arithmetic ops.
//
// Pipeline position: between curves and grading (see ImagePipeline.cpp).
// Each mask layers on top of the previous via lerp(prev, adjusted, weight)
// — so multiple masks compose multiplicatively in the order they appear
// in look.localAdjustments.
//
// Mask types (V1):
//   - LinearGradient: weight ramps from 0 at startPoint to 1 at endPoint
//                     along the projection axis, with smoothstep falloff
//                     softness controlled by feather.
//   - RadialGradient: weight is 1 inside the inner radius (= radius * (1
//                     - feather)) and 0 outside `radius`, smooth falloff.
//   - Brush:          placeholder. V1 treats brush masks as zero-weight
//                     everywhere — they're inert until the brush UI lands.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class LocalAdjustmentEngine
{
public:
    // Applies all enabled, non-identity local adjustments in look-defined
    // order. Caller is responsible for ensuring buffer is in linear-light
    // float format with valid dimensions. Identity / disabled masks are
    // skipped.
    static void apply(PixelBuffer& buffer,
                      const std::vector<LocalAdjustment>& adjustments);
};

} // namespace lps
