// ==============================================================================
// tone/Exposure.cpp
// ==============================================================================
#include "tone/Exposure.h"

#include <cmath>

namespace lps {

float Exposure::multiplier(float stops)
{
    return std::pow(2.0f, stops);
}

} // namespace lps
