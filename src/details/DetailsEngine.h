// ==============================================================================
// details/DetailsEngine.h
//
// Lightroom-style Details stage: sharpening plus luminance/color noise
// reduction. Runs after grading and before effects.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class DetailsEngine
{
public:
    static void apply(PixelBuffer& buffer, const DetailsParams& params);
};

} // namespace lps
