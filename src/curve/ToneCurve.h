// ==============================================================================
// curve/ToneCurve.h
// Evaluate a list of control points via Catmull-Rom spline. Builds a
// 4097-entry float LUT (matches the tone engine's LUT size for consistency).
// ==============================================================================
#pragma once

#include "core/Look.h"

#include <array>

namespace lps {

class ToneCurve
{
public:
    static constexpr int kLutSize = 4097;
    using FloatLut = std::array<float, kLutSize>;

    // Build the LUT. Returns false if the curve is identity (lut untouched).
    static bool buildLut(const CurvePoints& curve, FloatLut& out);

    // Sample the curve at x in [0,1]. Returns y in [0,1].
    static float evaluate(const CurvePoints& curve, float x);
};

} // namespace lps
