// ==============================================================================
// color/Vibrance.h
// Smart saturation + global saturation.
//
// Vibrance boosts low-saturation pixels more than high-saturation ones —
// protects already-saturated colors (like skin tones) from becoming neon.
// Global saturation is a uniform multiplier on the color axis.
//
// Both operate on linear-light RGB using Rec. 709 luminance.
// ==============================================================================
#pragma once

#include "core/PixelBuffer.h"

namespace lps {

class Vibrance
{
public:
    // vibrance and globalSaturation both in [-100, +100].
    static void apply(PixelBuffer& buffer, float vibrance, float globalSaturation);
};

} // namespace lps
