// ==============================================================================
// effects/EffectsEngine.cpp
// ==============================================================================
#include "effects/EffectsEngine.h"

#include "effects/Clarity.h"
#include "effects/Grain.h"
#include "effects/Vignette.h"

namespace lps {

void EffectsEngine::apply(PixelBuffer& buffer, const EffectsParams& params)
{
    if (params.isIdentity() || buffer.isNull()) return;

    Clarity::apply(buffer, params.clarity);
    Grain::apply(buffer, params.grain);
    Vignette::apply(buffer, params.vignette);
}

} // namespace lps
