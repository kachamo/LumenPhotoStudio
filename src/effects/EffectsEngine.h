// ==============================================================================
// effects/EffectsEngine.h
// Orchestrates clarity -> grain -> vignette.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class EffectsEngine
{
public:
    static void apply(PixelBuffer& buffer, const EffectsParams& params);
};

} // namespace lps
