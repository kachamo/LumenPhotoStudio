// ==============================================================================
// transform/TransformEngine.h
//
// Non-destructive crop / rotate / flip / straighten stage. Runs after lens
// correction and before tone/color work so downstream edits operate on the
// transformed image geometry.
// ==============================================================================
#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class TransformEngine
{
public:
    static void apply(PixelBuffer& buffer, const TransformParams& params);
};

} // namespace lps
