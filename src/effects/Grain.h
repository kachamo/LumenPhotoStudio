// ==============================================================================
// effects/Grain.h
// Deterministic additive film grain. Same image + same Look = same output.
// Noise injected in PERCEPTUAL (sRGB-encoded) space so grain amplitude
// matches photographic expectations (grain looks uniform across tones
// instead of being invisible in shadows).
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class Grain
{
public:
    static void apply(PixelBuffer& buffer, const GrainParams& params);
};

} // namespace lps
