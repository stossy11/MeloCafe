#pragma once

#include <Metal/Metal.hpp>
#include <array>
#include <map>

#include "HW/Latte/Core/LatteConst.h"
#include "HW/Latte/ISA/LatteReg.h"

class MetalSamplerCache
{
public:
    MetalSamplerCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalSamplerCache();

    MTL::SamplerState* GetSamplerState(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords);

private:
    class MetalRenderer* m_mtlr;

    // The register words overlap when combined with shifts and addition. A hash
    // alone can alias clamp modes with LOD settings, so compare the full state.
    using SamplerKey = std::array<uint32, 4>;
    std::map<SamplerKey, MTL::SamplerState*> m_samplerCache;
};
