// edge_test.cpp - a decal-like texture (opaque stripe at the left edge, the rest
// transparent) written with every mipmap filter, with and without "tiling", for
// validate_edges.py: without tiling nothing may appear at the right edge.
#include "Resample.h"
#include "VtfWriter.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1];
    const uint32_t W = 512, H = 512;
    std::vector<uint8_t> px(W * H * 4, 255);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) px[(y * W + x) * 4 + 3] = x < 16 ? 255 : 0;
    for (int f = 0; f < vtf::FILTER_COUNT; ++f)
        for (int tiling = 0; tiling < 2; ++tiling) {
            vtf::Options o;
            o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
            o.mipFilter = f;
            o.tiling = tiling != 0;
            std::vector<uint8_t> out; std::string err;
            if (!vtf::Write(px.data(), W, H, o, out, err)) { std::printf("FAIL: %s\n", err.c_str()); return 1; }
            const std::string name = dir + "/" + vtf::FilterName(vtf::Filter(f)) + (tiling ? "_tile" : "_clamp") + ".vtf";
            FILE* fp = std::fopen(name.c_str(), "wb");
            std::fwrite(out.data(), 1, out.size(), fp);
            std::fclose(fp);
        }
    return 0;
}
