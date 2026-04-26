// ==============================================================================
// local/MaskGeometry.h
//
// Pure-math mask weight evaluation, shared between LocalAdjustmentEngine
// (which applies the mask to pixels) and PreviewWidget (which paints a
// UI overlay showing where the mask hits).
//
// All inputs are normalized image coordinates ∈ [0, 1]. Weight is in
// [0, 1] where 1 = full effect, 0 = no effect. Soft falloff handled
// per mask type via smoothstep on the feather parameter.
//
// Reusable across:
//   - Engine: per-pixel weight × adjustment in LocalAdjustmentEngine
//   - UI overlay: per-pixel weight → alpha for the colored overlay layer
//   - Hit-testing: future "mask region" picker tools
//
// Header-only inlines so neither caller links a separate translation
// unit and the compiler can inline through the per-pixel inner loop.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "util/ColorMath.h"

#include <QPointF>
#include <algorithm>
#include <cmath>

namespace lps {

// Linear gradient: project the pixel onto the (start → end) axis. The
// projection parameter t is 0 at start, 1 at end. We then run a smoothstep
// from 0 to 1 with the feather controlling band width:
//   - feather=0 → step at exactly 0.5
//   - feather=1 → ramp across the full t range
inline float maskWeightLinear(const QPointF& p, const LocalAdjustment& mask)
{
    const QPointF d = mask.endPoint - mask.startPoint;
    const double  lengthSq = d.x()*d.x() + d.y()*d.y();
    if (lengthSq < 1e-8) return 0.0f;   // degenerate gradient

    const QPointF rel = p - mask.startPoint;
    const double  t = (rel.x()*d.x() + rel.y()*d.y()) / lengthSq;

    // Map t to a smoothstep with feather-controlled width centered at 0.5.
    const float f = std::max(0.05f, mask.feather);   // floor for math safety
    const float lo = 0.5f - 0.5f * f;
    const float hi = 0.5f + 0.5f * f;
    return math::smoothstep(lo, hi, static_cast<float>(t));
}

// Radial gradient: distance from center, normalized by radius. Weight is
// 1 at d=0 ... falls to 0 at d=1 over a smoothstep band whose width is
// controlled by feather:
//   - feather=0 → hard circle at d=1
//   - feather=1 → soft ramp from d=0 to d=1
//
// aspectRatio = imageWidth / imageHeight; corrects so a "radius=0.25"
// radial reads as a true circle on the displayed image regardless of
// the image's aspect.
inline float maskWeightRadial(const QPointF& p,
                              const LocalAdjustment& mask,
                              float aspectRatio)
{
    const double dx = p.x() - mask.center.x();
    const double dy = (p.y() - mask.center.y()) / aspectRatio;
    const double dist = std::sqrt(dx*dx + dy*dy);
    if (mask.radius <= 1e-4f) return 0.0f;

    const float d = static_cast<float>(dist) / mask.radius;
    const float innerFrac = 1.0f - std::clamp(mask.feather, 0.0f, 1.0f);
    return 1.0f - math::smoothstep(innerFrac, 1.0f, d);
}

// Dispatch by mask type. Brush is V1-placeholder (always 0). aspectRatio
// is only used by radial; pass any value for linear/brush.
inline float maskWeight(const QPointF& p,
                        const LocalAdjustment& mask,
                        float aspectRatio)
{
    switch (mask.type) {
        case MaskType::LinearGradient: return maskWeightLinear(p, mask);
        case MaskType::RadialGradient: return maskWeightRadial(p, mask, aspectRatio);
        case MaskType::Brush:          return 0.0f;
    }
    return 0.0f;
}

} // namespace lps
