// ==============================================================================
// curve/CurveEngine.h
// Applies master curve + per-channel RGB curves. Per-channel LUTs are fused
// (master composed with channel's own curve) so one pass does everything.
//
// Curves defined in PERCEPTUAL space (sRGB-encoded) by the user — that's
// how Lightroom's curve dialog works ("drag the midpoint up to brighten").
// So we sRGB-encode each channel, sample the curve, then linearize back.
// This preserves the linear-light pipeline while letting curve edits feel
// natural to photographers.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class CurveEngine
{
public:
    static void apply(PixelBuffer& buffer, const CurveParams& params);
};

} // namespace lps
