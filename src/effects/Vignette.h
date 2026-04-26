// ==============================================================================
// effects/Vignette.h
// Radial darken/brighten from edges inward. Multiplicative in linear light.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class Vignette
{
public:
    static void apply(PixelBuffer& buffer, const VignetteParams& params);
};

} // namespace lps
