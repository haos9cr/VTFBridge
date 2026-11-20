// imageload_test.cpp - the preview's reference-image loader (runs under Wine):
// TGA (uncompressed / RLE, bottom-up / top-down, grey, 16-bit), BMP through Windows
// Imaging Component, and VTF through our reader.
// Usage: imageload_test <folder for the test files>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ImageLoad.h"
#include "VtfWriter.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static const uint32_t W = 3, H = 2;
// Expected RGBA, top-down.
static const uint8_t kPx[W * H][4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 0},
                                      {10, 20, 30, 255}, {200, 150, 100, 64}, {255, 255, 255, 255}};

static std::wstring Path(const std::string& dir, const char* name) {
    std::string p = dir + "\\" + name;
    return std::wstring(p.begin(), p.end());
}

static void Save(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::vector<uint8_t> TgaHeader(int type, int bpp, int desc) {
    std::vector<uint8_t> h(18, 0);
    h[2] = uint8_t(type);
    h[12] = W; h[14] = H;
    h[16] = uint8_t(bpp);
    h[17] = uint8_t(desc);
    return h;
}

static void Expect(const char* what, const std::wstring& path, bool alpha, bool grey = false, int tolerance = 0) {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const bool ok = LoadImageFile(path, rgba, w, h);
    CHECK(ok && w == W && h == H, "%s: load failed or wrong size (%u x %u)", what, w, h);
    if (!ok || w != W || h != H) return;
    int worst = 0;
    for (uint32_t i = 0; i < W * H; ++i)
        for (int c = 0; c < 4; ++c) {
            int e = kPx[i][c];
            if (grey && c < 3) e = kPx[i][0];
            if (c == 3 && !alpha) e = 255;
            worst = std::max(worst, std::abs(int(rgba[i * 4 + c]) - e));
        }
    CHECK(worst <= tolerance, "%s: pixels off by %d", what, worst);
    std::printf("%-26s ok (max error %d)\n", what, worst);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";

    {  // 24-bit, uncompressed, bottom-up (the usual TGA)
        std::vector<uint8_t> f = TgaHeader(2, 24, 0);
        for (int y = H - 1; y >= 0; --y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* p = kPx[y * W + x];
                f.insert(f.end(), {p[2], p[1], p[0]});
            }
        Save(Path(dir, "t24.tga"), f);
        Expect("TGA 24-bit bottom-up", Path(dir, "t24.tga"), false);
    }
    {  // 32-bit RLE, top-down: first two pixels as a run of raw pixels, then the rest
        std::vector<uint8_t> f = TgaHeader(10, 32, 0x28);
        f.push_back(0x01);  // raw packet, 2 pixels
        for (int i = 0; i < 2; ++i) f.insert(f.end(), {kPx[i][2], kPx[i][1], kPx[i][0], kPx[i][3]});
        for (int i = 2; i < int(W * H); ++i) {
            f.push_back(0x80);  // run packet of 1 pixel
            f.insert(f.end(), {kPx[i][2], kPx[i][1], kPx[i][0], kPx[i][3]});
        }
        Save(Path(dir, "t32rle.tga"), f);
        Expect("TGA 32-bit RLE top-down", Path(dir, "t32rle.tga"), true);
    }
    {  // 8-bit grey, top-down
        std::vector<uint8_t> f = TgaHeader(3, 8, 0x20);
        for (uint32_t i = 0; i < W * H; ++i) f.push_back(kPx[i][0]);
        Save(Path(dir, "tgrey.tga"), f);
        Expect("TGA 8-bit grey", Path(dir, "tgrey.tga"), false, true);
    }
    {  // 16-bit (5-5-5 + 1-bit alpha), top-down
        std::vector<uint8_t> f = TgaHeader(2, 16, 0x21);
        for (uint32_t i = 0; i < W * H; ++i) {
            const int v = ((kPx[i][0] >> 3) << 10) | ((kPx[i][1] >> 3) << 5) | (kPx[i][2] >> 3) | (kPx[i][3] >= 128 ? 0x8000 : 0);
            f.insert(f.end(), {uint8_t(v), uint8_t(v >> 8)});
        }
        Save(Path(dir, "t16.tga"), f);
        std::vector<uint8_t> rgba; uint32_t w, h;
        CHECK(LoadImageFile(Path(dir, "t16.tga"), rgba, w, h) && w == W && h == H && rgba[2 * 4 + 3] == 0 && rgba[3] == 255,
              "TGA 16-bit");
        std::printf("%-26s ok\n", "TGA 16-bit");
    }
    {  // Broken TGA must fail, not crash
        std::vector<uint8_t> f = TgaHeader(10, 32, 0);
        f.push_back(0xFF);
        Save(Path(dir, "broken.tga"), f);
        std::vector<uint8_t> rgba; uint32_t w, h;
        CHECK(!LoadImageFile(Path(dir, "broken.tga"), rgba, w, h), "broken TGA accepted");
    }
    {  // BMP (24-bit, bottom-up) through WIC
        const uint32_t stride = (W * 3 + 3) & ~3u;
        std::vector<uint8_t> f(54 + stride * H, 0);
        f[0] = 'B'; f[1] = 'M';
        const uint32_t size = uint32_t(f.size());
        std::memcpy(&f[2], &size, 4);
        f[10] = 54; f[14] = 40; f[18] = W; f[22] = H; f[26] = 1; f[28] = 24;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* p = kPx[(H - 1 - y) * W + x];
                uint8_t* d = &f[54 + y * stride + x * 3];
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
            }
        Save(Path(dir, "t.bmp"), f);
        Expect("BMP via WIC", Path(dir, "t.bmp"), false);
    }
    {  // VTF (BGRA8888, lossless)
        std::vector<uint8_t> rgba(W * H * 4);
        std::memcpy(rgba.data(), kPx, rgba.size());
        vtf::Options o;
        o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
        o.mipmaps = false;
        std::vector<uint8_t> file; std::string err;
        CHECK(vtf::Write(rgba.data(), W, H, o, file, err), "VTF write");
        Save(Path(dir, "t.vtf"), file);
        Expect("VTF", Path(dir, "t.vtf"), true);
    }
    {  // VTF with mipmaps: the stored levels come back too
        const uint32_t S = 8;
        std::vector<uint8_t> rgba(S * S * 4);
        for (uint32_t i = 0; i < S * S; ++i) { rgba[i * 4] = uint8_t(i * 4); rgba[i * 4 + 1] = 0; rgba[i * 4 + 2] = uint8_t(255 - i * 4); rgba[i * 4 + 3] = 255; }
        vtf::Options o;
        o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
        o.mipFilter = 0;  // point
        std::vector<uint8_t> file; std::string err;
        CHECK(vtf::Write(rgba.data(), S, S, o, file, err), "VTF with mips write");
        Save(Path(dir, "tmips.vtf"), file);
        std::vector<uint8_t> top; std::vector<std::vector<uint8_t>> mips; uint32_t w = 0, h = 0;
        const bool ok = LoadImageFile(Path(dir, "tmips.vtf"), top, w, h, &mips);
        CHECK(ok && w == S && h == S && top == rgba, "VTF mips: top level");
        CHECK(ok && mips.size() == 3 && mips[0].size() == 4 * 4 * 4 && mips[1].size() == 2 * 2 * 4 && mips[2].size() == 4,
              "VTF mips: %zu levels", mips.size());
        std::printf("%-26s ok (%zu smaller levels)\n", "VTF mips", mips.size());
    }
    {  // VTF animation: every frame, each with its own mips
        const uint32_t S = 8;
        std::vector<std::vector<uint8_t>> fr(3, std::vector<uint8_t>(S * S * 4, 255));
        for (size_t f = 0; f < fr.size(); ++f)
            for (uint32_t i = 0; i < S * S; ++i) fr[f][i * 4] = uint8_t(f * 100), fr[f][i * 4 + 1] = uint8_t(i);
        std::vector<const uint8_t*> ptrs;
        for (auto& f : fr) ptrs.push_back(f.data());
        vtf::Options o;
        o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
        std::vector<uint8_t> file; std::string err;
        CHECK(vtf::Write(ptrs, S, S, o, file, err), "animated VTF write");
        Save(Path(dir, "tanim.vtf"), file);
        std::vector<LoadedFrame> frames; uint32_t w = 0, h = 0;
        const bool ok = LoadImageFrames(Path(dir, "tanim.vtf"), frames, w, h);
        CHECK(ok && frames.size() == 3, "animation: %zu frames", frames.size());
        for (size_t f = 0; ok && f < frames.size() && f < 3; ++f) {
            CHECK(frames[f].rgba == fr[f], "animation: frame %zu pixels", f);
            CHECK(frames[f].mips.size() == 3 && frames[f].mips[2].size() == 4 && frames[f].mips[2][0] == uint8_t(f * 100),
                  "animation: frame %zu mips", f);
        }
        std::printf("%-26s ok (%zu frames)\n", "VTF animation", frames.size());
    }
    {  // Missing file
        std::vector<uint8_t> rgba; uint32_t w, h;
        CHECK(!LoadImageFile(Path(dir, "missing.png"), rgba, w, h), "missing file accepted");
    }
    std::printf("failures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
