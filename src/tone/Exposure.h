// ==============================================================================
// tone/Exposure.h
// Exposure in photographic stops. Returns the multiplier 2^stops.
// Caller composes with other tone operations into the fused float LUT.
// ==============================================================================
#pragma once

namespace lps {

class Exposure
{
public:
    // Returns the multiplier to apply to linear-light RGB.
    // stops in [-10, +10]; 0 = identity (returns 1.0f).
    static float multiplier(float stops);
};

} // namespace lps
