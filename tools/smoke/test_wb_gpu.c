/* GPU writeback packing model: raw bits, row pitches, padding and overlapping guest surfaces. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RSX_WB_GPU_C_REFERENCE
#include "rsx_wb_gpu.h"

static uint32_t rng = 0x63D83595u;
static uint32_t next_u32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint32_t load_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le(uint8_t* p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i));
}

static void reference_word(const uint8_t* src, uint8_t* dst, unsigned mode)
{
    if (mode == RSX_WB_GPU_RGBA8) {
        dst[0] = src[3]; dst[1] = src[0]; dst[2] = src[1]; dst[3] = src[2];
    } else if (mode == RSX_WB_GPU_HALF) {
        dst[0] = src[1]; dst[1] = src[0]; dst[2] = src[3]; dst[3] = src[2];
    } else {
        dst[0] = src[3]; dst[1] = src[2]; dst[2] = src[1]; dst[3] = src[0];
    }
}

static int words_test(void)
{
    static const uint32_t special[] = {
        0, 0xFFFFFFFFu, 0x80000000u, 0x7F800000u, 0x7F800001u, 0x7FC00001u,
        0xFF800001u, 0x00000001u, 0x00010000u, 0x12345678u
    };
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned i = 0; i < 1000000u + sizeof special / sizeof special[0]; i++) {
            const uint32_t v = i < sizeof special / sizeof special[0] ? special[i] : next_u32();
            uint8_t src[4], want[4], got[4];
            store_le(src, v);
            reference_word(src, want, mode);
            store_le(got, rsx_wb_gpu_word(v, mode));
            if (memcmp(want, got, sizeof got)) {
                printf("[FAIL] word mode=%u value=%08X\n", mode, v);
                return 0;
            }
        }
    }
    puts("[PASS] 3 million words, including float and half special bits");
    return 1;
}

static int rows_test(void)
{
    uint8_t guest_cpu[8192], guest_gpu[8192], source[2560], packed[2560];
    for (unsigned round = 0; round < 1000u; round++) {
        for (unsigned i = 0; i < sizeof guest_cpu; i++)
            guest_cpu[i] = guest_gpu[i] = (uint8_t)next_u32();
        for (unsigned surface = 0; surface < 5u; surface++) {
            const unsigned mode = next_u32() % 3u;
            const unsigned texel = mode == RSX_WB_GPU_HALF ? 8u : 4u;
            const unsigned width = 1u + next_u32() % 31u;
            const unsigned height = 1u + next_u32() % 10u;
            const unsigned row_bytes = width * texel;
            const unsigned src_pitch = 256u;
            const unsigned dst_pitch = row_bytes + 4u * (next_u32() % 13u);
            const unsigned offset = 512u + next_u32() % 1024u;
            for (unsigned i = 0; i < src_pitch * height; i++) source[i] = (uint8_t)next_u32();
            memcpy(packed, source, src_pitch * height);
            for (unsigned y = 0; y < height; y++) {
                for (unsigned x = 0; x < row_bytes; x += 4u) {
                    const uint8_t* s = source + y * src_pitch + x;
                    reference_word(s, guest_cpu + offset + y * dst_pitch + x, mode);
                    store_le(packed + y * src_pitch + x, rsx_wb_gpu_word(load_le(s), mode));
                }
                memcpy(guest_gpu + offset + y * dst_pitch, packed + y * src_pitch, row_bytes);
            }
            if (memcmp(guest_cpu, guest_gpu, sizeof guest_cpu)) {
                printf("[FAIL] rows round=%u surface=%u mode=%u width=%u height=%u\n",
                       round, surface, mode, width, height);
                return 0;
            }
        }
    }
    puts("[PASS] 5,000 pitched and overlapping surface stores");
    return 1;
}

int main(void)
{
    if (!words_test() || !rows_test()) return 1;
    puts("[PASS] test_wb_gpu");
    return 0;
}
