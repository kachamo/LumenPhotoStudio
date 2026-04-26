// ==============================================================================
// color/HSL.h
// 8-channel HSL (red/orange/yellow/green/aqua/blue/purple/magenta).
// Per-pixel: compute pixel hue -> raised-cosine weight per channel ->
// weighted sum of hue shift, saturation multiplier, luminance offset.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class HSL
{
public:
    static void apply(PixelBuffer& buffer, const HSLParams& params);
};

} // namespace lps
