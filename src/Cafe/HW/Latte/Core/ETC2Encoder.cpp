#include "ETC2Encoder.h"
#include <cstddef>
#include <cstring>
#include "ProcessRGB.hpp"

void EncodeETC2RGBA8Block(const uint8_t* rgba, uint8_t* output)
{
    // etcpak expects BGRA bytes, and returns blocks already in GPU byte order.
    alignas(32) uint32_t bgra[16];
    auto* bytes = reinterpret_cast<uint8_t*>(bgra);
    for (unsigned i = 0; i < 16; ++i)
    {
        bytes[i * 4 + 0] = rgba[i * 4 + 2];
        bytes[i * 4 + 1] = rgba[i * 4 + 1];
        bytes[i * 4 + 2] = rgba[i * 4 + 0];
        bytes[i * 4 + 3] = rgba[i * 4 + 3];
    }
    alignas(32) uint64_t encoded[2];
    CompressEtc2Rgba(bgra, encoded, 1, 4, false);
    std::memcpy(output, encoded, sizeof(encoded));
}
