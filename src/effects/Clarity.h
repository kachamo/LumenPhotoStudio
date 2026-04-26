// ==============================================================================
// effects/Clarity.h
// Midtone contrast proxy. Positive = "punch"; negative = softer look.
// True unsharp-mask clarity requires a blur pass; future step.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class Clarity
{
public:
    static void apply(PixelBuffer& buffer, const ClarityParams& params);
};

} // namespace lps
