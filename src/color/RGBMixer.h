// ==============================================================================
// color/RGBMixer.h
// Channel mixer: rebuild each output channel as a weighted sum of inputs.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class RGBMixer
{
public:
    static void apply(PixelBuffer& buffer, const RGBMixerParams& params);
};

} // namespace lps
