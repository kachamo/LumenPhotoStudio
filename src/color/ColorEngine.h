// ==============================================================================
// color/ColorEngine.h
// Orchestrates the four color sub-modules in canonical order:
//   WhiteBalance -> Vibrance/Saturation -> HSL -> RGBMixer
//
// WB first so downstream edits operate on balanced base.
// Vibrance/sat before HSL so HSL targets post-saturation image.
// RGBMixer last so it acts as a final creative channel rewire.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class ColorEngine
{
public:
    static void apply(PixelBuffer& buffer, const ColorParams& params);
};

} // namespace lps
