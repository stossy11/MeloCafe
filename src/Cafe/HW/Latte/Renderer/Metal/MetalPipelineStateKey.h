#pragma once

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalAttachmentsInfo.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include <algorithm>
#include <array>

struct MetalPipelineStateKey
{
    std::array<uint64, 9> shaderHashes{};
    uint64 fetchHash{};
    std::array<uint32, LATTE_MAX_VERTEX_BUFFERS> vertexStrides{};
    size_t vertexBufferCount{};
    std::array<Latte::E_GX2SURFFMT, LATTE_NUM_COLOR_TARGET> lastColorFormats{};
    std::array<Latte::E_GX2SURFFMT, LATTE_NUM_COLOR_TARGET> activeColorFormats{};
    Latte::E_GX2SURFFMT depthFormat{};
    bool hasStencil{};
    bool activeDepth{};
    uint32 streamoutEnable{};
    uint32 polygonControl{};
    uint32 clipControl{};
    uint32 colorControl{};
    uint32 targetMask{};
    std::array<uint32, LATTE_NUM_COLOR_TARGET> blendControls{};
    uint32 meshPrimitive{};

    bool operator==(const MetalPipelineStateKey&) const = default;

    static MetalPipelineStateKey Capture(const LatteFetchShader* fetchShader,
        const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader,
        const LatteDecompilerShader* pixelShader, const MetalAttachmentsInfo& lastAttachments,
        const MetalAttachmentsInfo& activeAttachments, const LatteContextRegister& lcr)
    {
        MetalPipelineStateKey key{};
        const LatteDecompilerShader* shaders[] = {vertexShader, geometryShader, pixelShader};
        for (size_t i = 0; i < 3; ++i)
        {
            if (shaders[i])
            {
                key.shaderHashes[i * 3] = 1;
                key.shaderHashes[i * 3 + 1] = shaders[i]->baseHash;
                key.shaderHashes[i * 3 + 2] = shaders[i]->auxHash;
            }
        }
        key.fetchHash = fetchShader->getVkPipelineHashFragment();
        key.vertexBufferCount = fetchShader->bufferGroups.size();
        // Oversized layouts bypass the fast path in GetRenderPipelineState.
        for (size_t i = 0; i < std::min(key.vertexBufferCount, key.vertexStrides.size()); ++i)
            key.vertexStrides[i] = fetchShader->bufferGroups[i].getCurrentBufferStride(lcr.GetRawView());
        for (size_t i = 0; i < LATTE_NUM_COLOR_TARGET; ++i)
        {
            key.lastColorFormats[i] = lastAttachments.colorFormats[i];
            key.activeColorFormats[i] = activeAttachments.colorFormats[i];
        }
        key.depthFormat = lastAttachments.depthFormat;
        key.hasStencil = lastAttachments.hasStencil;
        key.activeDepth = activeAttachments.depthFormat != Latte::E_GX2SURFFMT::INVALID_FORMAT;
        const auto* registers = lcr.GetRawView();
        key.streamoutEnable = registers[mmVGT_STRMOUT_EN];
        key.polygonControl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
        key.clipControl = lcr.PA_CL_CLIP_CNTL.getRawValue();
        key.colorControl = registers[Latte::REGADDR::CB_COLOR_CONTROL];
        key.targetMask = registers[Latte::REGADDR::CB_TARGET_MASK];
        const uint32 blendEnableMask = (key.colorControl >> 8) & 0xFF;
        for (size_t i = 0; i < key.blendControls.size(); ++i)
        {
            if (blendEnableMask & (1u << i))
                key.blendControls[i] = registers[Latte::REGADDR::CB_BLEND0_CONTROL + i];
        }
        const auto primitiveMode = lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
        if (geometryShader || primitiveMode == LattePrimitiveMode::RECTS)
            key.meshPrimitive = registers[Latte::REGADDR::VGT_PRIMITIVE_TYPE];
        return key;
    }
};
