// ==============================================================================
// tone/Contrast.cpp
// ==============================================================================
#include "tone/Contrast.h"

namespace lps {

// Middle gray in linear light (sRGB 0.5 -> linear 0.2140; round to the
// standard "18% gray" reference that matches photographic practice).
static constexpr float kPivotLinear = 0.18f;

float Contrast::evaluate(float v, float amount)
{
    const float factor = 1.0f + (amount / 100.0f);
    return kPivotLinear + (v - kPivotLinear) * factor;
}

} // namespace lps
