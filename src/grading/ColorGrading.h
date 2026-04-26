// ==============================================================================
// grading/ColorGrading.h
// Applies LUT-based grading + film profile.
//
// .cube LUTs are conventionally authored in SDR sRGB space (inputs are sRGB
// values in [0,1]). So we sRGB-encode each pixel for lookup, sample the LUT
// with trilinear interpolation, then linearize back. This is the same
// pattern as the curve engine.
//
// LUTs are cached by path; re-applying the same LUT across renders avoids
// re-parsing.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class ColorGrading
{
public:
    static void apply(PixelBuffer& buffer, const GradingParams& params);
};

} // namespace lps
