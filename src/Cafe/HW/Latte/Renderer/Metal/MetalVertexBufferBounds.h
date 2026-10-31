#pragma once

#include <cstdint>

namespace MetalVertexBufferBounds
{
constexpr std::uint64_t AttributeEnd(std::uint32_t lastIndex, std::uint32_t stride,
    std::uint32_t attributeOffset, std::uint32_t attributeSize)
{
    return std::uint64_t(lastIndex) * stride + attributeOffset + attributeSize;
}

constexpr bool Fits(std::uint64_t required, std::uint64_t boundSize,
    std::uint64_t bufferOffset, std::uint64_t bufferLength)
{
    return bufferOffset <= bufferLength && required <= boundSize &&
        required <= bufferLength - bufferOffset;
}
}
