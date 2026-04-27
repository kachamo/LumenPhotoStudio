#pragma once

#include "core/Look.h"
#include "core/PixelBuffer.h"

namespace lps {

class HDRToneMapper
{
public:
    static void apply(PixelBuffer& buffer, const HDRParams& params);
};

} // namespace lps
