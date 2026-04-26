// ==============================================================================
// tone/Contrast.h
// Linear-space contrast: stretches values around middle gray.
//
// Important: "middle gray" in LINEAR light is ~0.18, not 0.5. In sRGB-encoded
// space, middle gray is 0.5 (which encodes to 0.18 linear). So our pivot is
// 0.18 for the linear pipeline.
// ==============================================================================
#pragma once

namespace lps {

class Contrast
{
public:
    // Evaluate f(v) = pivot + (v - pivot) * (1 + amount/100)
    // v and return in linear light, amount in [-100, +100].
    static float evaluate(float v, float amount);
};

} // namespace lps
