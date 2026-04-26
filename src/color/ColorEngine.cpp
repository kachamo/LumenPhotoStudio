// ==============================================================================
// color/ColorEngine.cpp
// ==============================================================================
#include "color/ColorEngine.h"

#include "color/HSL.h"
#include "color/RGBMixer.h"
#include "color/Vibrance.h"
#include "color/WhiteBalance.h"

namespace lps {

void ColorEngine::apply(PixelBuffer& buffer, const ColorParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    WhiteBalance::apply(buffer, params.whiteBalance);
    Vibrance::apply(buffer, params.vibrance, params.saturation);
    HSL::apply(buffer, params.hsl);
    RGBMixer::apply(buffer, params.rgbMixer);
}

} // namespace lps
