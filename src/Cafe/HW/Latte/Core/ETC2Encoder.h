#pragma once
#include <cstdint>

// Encodes one row-major 4x4 RGBA8 block into 16 bytes of ETC2 RGB + EAC alpha.
// The destination doesn't need to be aligned. sRGB conversion is performed by the GPU view.
void EncodeETC2RGBA8Block(const uint8_t* rgba, uint8_t* output);
