#pragma once
#include "LatteTextureLoader.h"
#include "ETC2Encoder.h"

template<auto DecodeBlock>
class TextureDecoder_BC_to_ETC2_RGBA : public TextureDecoder
{
public:
    sint32 getBytesPerTexel(LatteTextureLoaderCtx*) override { return 16; }
    sint32 getTexelCountX(LatteTextureLoaderCtx* tl) override { return (tl->width + 3) / 4; }
    sint32 getTexelCountY(LatteTextureLoaderCtx* tl) override { return (tl->height + 3) / 4; }

    void decode(LatteTextureLoaderCtx* tl, uint8* output) override
    {
        const size_t blocksX = (tl->width + 3) / 4;
        for (sint32 y = 0; y < tl->height; y += 4)
        {
            for (sint32 x = 0; x < tl->width; x += 4)
            {
                float decoded[64];
                uint8 rgba[64];
                DecodeBlock(LatteTextureLoader_GetInput(tl, x, y), decoded);
                // Replicate edge pixels for partial blocks, including 1x1/2x2 mips.
                for (sint32 py = 0; py < 4; ++py)
                    for (sint32 px = 0; px < 4; ++px)
                        for (sint32 c = 0; c < 4; ++c)
                        {
                            const sint32 sx = (std::min)(px, tl->width - x - 1);
                            const sint32 sy = (std::min)(py, tl->height - y - 1);
                            rgba[(py * 4 + px) * 4 + c] = TextureDecoder_BC_floatToUNorm8(decoded[(sy * 4 + sx) * 4 + c]);
                        }
                EncodeETC2RGBA8Block(rgba, output + ((size_t(y) / 4) * blocksX + x / 4) * 16);
            }
        }
    }

    void decodePixelToRGBA(uint8* block, uint8* out, uint8 x, uint8 y) override
    {
        float rgba[64];
        DecodeBlock(block, rgba);
        for (unsigned c = 0; c < 4; ++c)
            out[c] = TextureDecoder_BC_floatToUNorm8(rgba[(y * 4 + x) * 4 + c]);
    }
};

class TextureDecoder_BC1_to_ETC2 : public TextureDecoder_BC_to_ETC2_RGBA<decodeBC1Block>, public SingletonClass<TextureDecoder_BC1_to_ETC2> {};
class TextureDecoder_BC2_to_ETC2 : public TextureDecoder_BC_to_ETC2_RGBA<decodeBC2Block_UNORM>, public SingletonClass<TextureDecoder_BC2_to_ETC2> {};
class TextureDecoder_BC3_to_ETC2 : public TextureDecoder_BC_to_ETC2_RGBA<decodeBC3Block_UNORM>, public SingletonClass<TextureDecoder_BC3_to_ETC2> {};
