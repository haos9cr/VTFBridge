// preview_test.cpp - mipmap preview data and mipmap sharpening.
#include "VtfReader.h"
#include "VtfWriter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// Sum of |laplacian| of the green channel: grows with local contrast (sharpness).
static double Detail(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    double s = 0;
    for (uint32_t y = 1; y + 1 < h; ++y)
        for (uint32_t x = 1; x + 1 < w; ++x) {
            auto g = [&](uint32_t xx, uint32_t yy) { return double(px[(size_t(yy) * w + xx) * 4 + 1]); };
            s += std::fabs(4 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1));
        }
    return s;
}

int main() {
    const uint32_t W = 128, H = 64;
    std::vector<uint8_t> img(W * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &img[(y * W + x) * 4];
            p[0] = uint8_t(((x / 4 + y / 4) & 1) ? 230 : 20);  // checker detail
            p[1] = uint8_t(128 + 100 * std::sin(x * 0.7) * std::cos(y * 0.5));
            p[2] = uint8_t(x * 2);
            p[3] = uint8_t(x < 64 ? 255 : 128);
        }

    // 1) Uncompressed: decoded == original on every level; sizes follow the chain.
    {
        vtf::Options o; o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888;
        std::vector<vtf::PreviewLevel> L; int32_t fmt = 0;
        CHECK(vtf::BuildPreview(img.data(), W, H, o, L, fmt), "preview BGRA8888");
        CHECK(fmt == vtf::FMT_BGRA8888 && L.size() == 8, "levels %zu fmt %d", L.size(), fmt);
        for (size_t m = 0; m < L.size(); ++m) {
            CHECK(L[m].w == std::max(1u, W >> m) && L[m].h == std::max(1u, H >> m), "level %zu size", m);
            CHECK(L[m].decoded == L[m].original && L[m].psnr >= 99, "level %zu lossless", m);
        }
    }
    // 2) DXT: level 0 matches what Write() puts into the file (decoded with VtfReader).
    {
        vtf::Options o;  // Auto DXT1/DXT5 -> DXT5 (has alpha)
        o.dxtQuality = vtf::DXT_QUALITY_BEST;
        std::vector<vtf::PreviewLevel> L; int32_t fmt = 0;
        CHECK(vtf::BuildPreview(img.data(), W, H, o, L, fmt), "preview DXT");
        CHECK(fmt == vtf::FMT_DXT5, "auto format with alpha must be DXT5");
        std::vector<uint8_t> file, top; std::string err; vtf::ReadInfo info;
        CHECK(vtf::Write(img.data(), W, H, o, file, err) && vtf::Read(file.data(), file.size(), top, info, err), "write/read");
        CHECK(L[0].decoded == top, "preview level 0 differs from the saved file");
        CHECK(L[0].psnr > 20 && L[0].psnr < 99, "DXT psnr %.2f", L[0].psnr);
        std::printf("DXT5 preview: level 0 PSNR %.2f dB, %zu levels\n", L[0].psnr, L.size());
        // Only one level requested.
        std::vector<vtf::PreviewLevel> one;
        CHECK(vtf::BuildPreview(img.data(), W, H, o, one, fmt, 2), "single level");
        CHECK(one.size() == L.size() && one[2].decoded == L[2].decoded && one[0].decoded.empty(), "single level content");
        // No mipmaps: one level.
        o.mipmaps = false;
        CHECK(vtf::BuildPreview(img.data(), W, H, o, one, fmt) && one.size() == 1, "no mipmaps");
    }
    // 3) Sharpening: top level unchanged, smaller levels more detailed, 0 = unchanged.
    {
        vtf::Options o; o.colorFormat = o.alphaFormat = vtf::FMT_BGRA8888; o.mipFilter = 1;  // Box
        std::vector<vtf::PreviewLevel> a, b; int32_t fmt;
        vtf::BuildPreview(img.data(), W, H, o, a, fmt);
        o.mipSharpen = 60;
        vtf::BuildPreview(img.data(), W, H, o, b, fmt);
        CHECK(a[0].original == b[0].original, "sharpening must not touch the top level");
        for (int m = 1; m <= 3; ++m) {
            const double da = Detail(a[m].original, a[m].w, a[m].h), db = Detail(b[m].original, b[m].w, b[m].h);
            std::printf("level %d detail: plain %.0f  sharpened %.0f\n", m, da, db);
            CHECK(db > da * 1.1, "level %d not sharper", m);
        }
        for (size_t i = 3; i < a[1].original.size(); i += 4)
            if (a[1].original[i] != b[1].original[i]) { CHECK(false, "sharpening changed alpha"); break; }
        // "Alpha too": the alpha of smaller levels gets sharper as well; colour as without it.
        {
            std::vector<vtf::PreviewLevel> c;
            o.sharpenAlpha = true;
            vtf::BuildPreview(img.data(), W, H, o, c, fmt);
            o.sharpenAlpha = false;
            double ea = 0, ec = 0;
            for (size_t i = 3; i + 4 < a[1].original.size(); i += 4) {
                ea += std::abs(int(a[1].original[i]) - int(a[1].original[i + 4]));
                ec += std::abs(int(c[1].original[i]) - int(c[1].original[i + 4]));
            }
            std::printf("level 1 alpha detail: plain %.0f  sharpened %.0f\n", ea, ec);
            CHECK(ec > ea * 1.05, "alpha not sharper with sharpenAlpha");
            bool sameColour = true;
            for (size_t i = 0; i < b[1].original.size(); i += 4)
                for (int k = 0; k < 3; ++k) sameColour &= b[1].original[i + k] == c[1].original[i + k];
            CHECK(sameColour, "sharpenAlpha changed colour");
            CHECK(c[0].original == a[0].original, "sharpenAlpha touched the top level");
        }
        // Written file follows the same chain.
        std::vector<uint8_t> f1, f2; std::string err;
        o.mipSharpen = 0; vtf::Write(img.data(), W, H, o, f1, err);
        o.mipSharpen = 60; vtf::Write(img.data(), W, H, o, f2, err);
        CHECK(f1 != f2 && f1.size() == f2.size(), "sharpening must change the saved mipmaps");
    }
    std::printf("failures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
