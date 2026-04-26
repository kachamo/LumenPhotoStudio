// ==============================================================================
// color/WhiteBalance.h
// Temperature (blue<->yellow) and tint (green<->magenta) adjustment.
//
// In linear light, white balance is a per-channel multiplier — the physically
// correct form. This is why linear workflow matters: WB in sRGB space gives
// you hue shifts in shadows; WB in linear gives you true color temperature.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class WhiteBalance
{
public:
    static void apply(PixelBuffer& buffer, const WhiteBalanceParams& params);
};

} // namespace lps
