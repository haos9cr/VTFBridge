// normal_test.cpp - writes a bumpy normal map with and without the Normal Map flag
// (BGRA8888, so no compression error) for validate_normal.py.
#include "VtfWriter.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    const uint32_t W = 64, H = 64;
    std::vector<uint8_t> px(W * H * 4);
    // Strongly tilted vectors that alternate per pixel: averaging them shortens the result.
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const float a = ((x + y) & 1) ? 0.8f : -0.8f, b = (y & 1) ? 0.5f : -0.5f;
            float n[3] = {a, b, 0.3f};
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            uint8_t* p = &px[(y * W + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround((n[c] / l * .5f + .5f) * 255.f));
            p[3] = 255;
        }
    struct Case { const char* name; uint32_t flags; bool gamma; };
    const Case cases[] = {{"plain", 0, false}, {"normal", 0x80, true}, {"ssbump", 0x80 | 0x08000000, false}};
    for (const Case& c : cases) {
        vtf::Options o;
        o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
        o.flags = c.flags;
        o.gammaFilter = c.gamma;  // must be ignored for normal maps
        std::vector<uint8_t> out; std::string err;
        if (!vtf::Write(px.data(), W, H, o, out, err)) { std::printf("FAIL %s: %s\n", c.name, err.c_str()); return 1; }
        FILE* f = std::fopen((dir + "/" + c.name + ".vtf").c_str(), "wb");
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
    return 0;
}
