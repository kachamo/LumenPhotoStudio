// ==============================================================================
// tone/ToneEngine.h
// Orchestrates tone stage. Fuses exposure, highlights/shadows, and contrast
// into a single 4096-entry float LUT. One pass through pixels.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class ToneEngine
{
public:
    // Applies tone stage in place on a linear-light float buffer.
    // params is assumed to have been clampRanges()'d upstream.
    static void apply(PixelBuffer& buffer, const ToneParams& params);
};

} // namespace lps
