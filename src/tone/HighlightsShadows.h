// ==============================================================================
// tone/HighlightsShadows.h
// Per-luminance-band tonal shaping.
//
// In LINEAR-LIGHT working space, the "tone bands" are centered on perceptual
// midpoints. We use sRGB-encoded reference positions (0.5 middle, 0.85
// highlight, 0.15 shadow) converted to linear for the mask centers — this
// makes sliders feel intuitive: "shadows = 50" lifts the visually dark
// regions, which happen to live in a narrow linear-light band near the
// bottom of the range.
// ==============================================================================
#pragma once

namespace lps {

struct ToneShapingParams
{
    float highlights;
    float shadows;
    float whites;
    float blacks;
};

class HighlightsShadows
{
public:
    // v in linear-light [0, 1]. Returns the shaped value.
    // params values in [-100, +100].
    static float evaluate(float v, const ToneShapingParams& p);
};

} // namespace lps
