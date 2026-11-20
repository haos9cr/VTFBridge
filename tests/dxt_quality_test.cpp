// dxt_quality_test.cpp - DXT1/DXT5 at every quality level: decodes the files again
// and checks the error (PSNR) goes down from Fast to High to Best to the VTFBridge
// modes, that flat colours stay exact (a bad 4-colour/3-colour block would turn
// pixels black) and that smooth gradients do not turn into flat 4x4 steps.
#include "VtfReader.h"
#include "VtfWriter.h"
#include "../third_party/icbc.h"  // GPU-like reference decoders (implementation is in VtfWriter.cpp)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int c0, int c1) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = c0; c <= c1; ++c) { const double d = double(a[i + c]) - b[i + c]; se += d * d; ++n; }
    return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / n));
}

// Colour PSNR as GPUs decode it: average squared error over icbc's D3D10, NVIDIA
// and AMD decoders (they round the in-between colours differently). File: 7.2,
// no thumbnail, no mipmaps -> blocks start at byte 80. w = channel weights.
static double GpuPsnr(const std::vector<uint8_t>& file, int32_t fmt, const std::vector<uint8_t>& img,
                      uint32_t W, uint32_t H, const double w[3]) {
    const size_t bs = fmt == vtf::FMT_DXT1 ? 8 : 16, colorAt = fmt == vtf::FMT_DXT1 ? 0 : 8;
    const icbc::Decoder dec[3] = {icbc::Decoder_D3D10, icbc::Decoder_NVIDIA, icbc::Decoder_AMD};
    double se = 0, n = 0;
    for (uint32_t by = 0; by < H / 4; ++by)
        for (uint32_t bx = 0; bx < W / 4; ++bx) {
            const uint8_t* b = &file[80 + (size_t(by) * (W / 4) + bx) * bs + colorAt];
            for (const icbc::Decoder d : dec) {
                uint8_t px[64];
                icbc::decode_bc1(b, px, d);
                for (int i = 0; i < 16; ++i)
                    for (int c = 0; c < 3; ++c) {
                        const double t = double(px[i * 4 + c]) - img[((by * 4 + i / 4) * W + bx * 4 + i % 4) * 4 + c];
                        se += w[c] * t * t; n += w[c];
                    }
            }
        }
    return 10.0 * std::log10(255.0 * 255.0 / (se / n));
}

// PSNR with the eye's channel weights (as used by the perceptual mode).
static double WeightedPsnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const double w[3] = {0.3, 0.59, 0.11};
    double se = 0, n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - b[i + c]; se += w[c] * d * d; n += w[c]; }
    return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / n));
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const uint32_t W = 256, H = 256;
    std::vector<uint8_t> img(W * H * 4), flat(W * H * 4);
    std::srand(7);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &img[(y * W + x) * 4];
            p[0] = uint8_t(255 * x / (W - 1));
            p[1] = uint8_t(255 * y / (H - 1));
            p[2] = uint8_t(128 + 127 * std::sin(x / 17.0 + y / 29.0));
            const double r = std::hypot(double(x) - 128, double(y) - 128) / 128;
            int a = int(255 * std::max(0.0, 1 - r)) + std::rand() % 21 - 10;
            p[3] = uint8_t(a < 0 ? 0 : a > 255 ? 255 : a);
            uint8_t* f = &flat[(y * W + x) * 4];
            const int band = int((x / 32 + y / 32) % 4);
            const uint8_t cols[4][3] = {{255, 255, 255}, {0, 0, 0}, {200, 30, 90}, {12, 180, 255}};
            f[0] = cols[band][0]; f[1] = cols[band][1]; f[2] = cols[band][2]; f[3] = 255;
        }
    const char* qn[] = {"fast", "high", "best", "max", "perceptual"};
    for (int32_t fmt : {vtf::FMT_DXT1, vtf::FMT_DXT5, vtf::FMT_DXT3}) {
        double prevColor = 0, prevAlpha = 0, prevWeighted = 0;
        for (int q = 0; q < vtf::DXT_QUALITY_COUNT; ++q) {
            vtf::Options o;
            o.colorFormat = o.alphaFormat = fmt;
            o.dxtQuality = q;
            o.mipmaps = false;
            o.thumbnail = false;
            o.versionMinor = 2;
            std::vector<uint8_t> file, back; std::string err; vtf::ReadInfo info;
            CHECK(vtf::Write(img.data(), W, H, o, file, err), "write: %s", err.c_str());
            CHECK(vtf::Read(file.data(), file.size(), back, info, err), "read: %s", err.c_str());
            static const double kUniform[3] = {1, 1, 1}, kEye[3] = {0.3, 0.59, 0.11};
            const double pc = GpuPsnr(file, fmt, img, W, H, kUniform), pa = fmt == vtf::FMT_DXT1 ? 0 : Psnr(img, back, 3, 3);
            const double pw = GpuPsnr(file, fmt, img, W, H, kEye);
            std::printf("%s %-10s colour %.2f dB  eye-weighted %.2f dB  alpha %.2f dB\n", vtf::FormatName(fmt), qn[q], pc, pw, pa);
            if (q == vtf::DXT_QUALITY_PERCEPTUAL) {
                CHECK(pw > prevWeighted + 0.05, "%s perceptual not better eye-weighted", vtf::FormatName(fmt));
                // Normal maps: perceptual weighting is not used (same output as Max).
                vtf::Options n1 = o, n2 = o;
                n1.flags = n2.flags = vtf::FLAG_NORMAL;
                n2.dxtQuality = vtf::DXT_QUALITY_MAX;
                std::vector<uint8_t> f1, f2;
                CHECK(vtf::Write(img.data(), W, H, n1, f1, err) && vtf::Write(img.data(), W, H, n2, f2, err) && f1 == f2,
                      "normal map with perceptual differs from max");
            } else if (q > 0) {
                CHECK(pc >= prevColor - 0.02, "%s %s colour worse than the previous level", vtf::FormatName(fmt), qn[q]);
                CHECK(pa >= prevAlpha - 0.02, "%s %s alpha worse than the previous level", vtf::FormatName(fmt), qn[q]);
            }
            if (q == vtf::DXT_QUALITY_HIGH) CHECK(pc > prevColor + 0.1, "%s high is not better than fast", vtf::FormatName(fmt));
            if (q == vtf::DXT_QUALITY_BEST && fmt == vtf::FMT_DXT5) CHECK(pa > prevAlpha + 0.5, "DXT5 best alpha not better");
            if (q == vtf::DXT_QUALITY_MAX) CHECK(pc > prevColor + 0.1, "%s max not better than best", vtf::FormatName(fmt));
            if (q != vtf::DXT_QUALITY_PERCEPTUAL) { prevColor = pc; prevAlpha = pa; prevWeighted = pw; }
            FILE* f = std::fopen((dir + "/q_" + vtf::FormatName(fmt) + "_" + qn[q] + ".vtf").c_str(), "wb");
            std::fwrite(file.data(), 1, file.size(), f); std::fclose(f);

            // Flat colours: every block must decode exactly (up to 565 rounding).
            std::vector<uint8_t> ff, fb;
            CHECK(vtf::Write(flat.data(), W, H, o, ff, err) && vtf::Read(ff.data(), ff.size(), fb, info, err), "flat");
            int worst = 0;
            for (size_t i = 0; i < fb.size(); i += 4)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(fb[i + c]) - int(flat[i + c])));
            CHECK(worst <= 8, "%s %s flat colours off by %d", vtf::FormatName(fmt), qn[q], worst);
        }
    }
    // Smooth gradient (sky / wall shading): icbc tends to flatten such blocks into
    // single colours (visible 4x4 steps); the VTFBridge modes must stay smooth.
    {
        const uint32_t GW = 256, GH = 192;
        std::vector<uint8_t> g(GW * GH * 4);
        for (uint32_t y = 0; y < GH; ++y)
            for (uint32_t x = 0; x < GW; ++x) {
                uint8_t* p = &g[(y * GW + x) * 4];
                p[0] = uint8_t(118 + 30.0 * y / GH - 10.0 * x / GW);
                p[1] = uint8_t(108 + 40.0 * y / GH - 6.0 * x / GW);
                p[2] = uint8_t(118 + 45.0 * y / GH);
                p[3] = 255;
            }
        double psnr[vtf::DXT_QUALITY_COUNT];
        double lowFreq[vtf::DXT_QUALITY_COUNT];  // PSNR of the error averaged over 8x8: steps / bands, not noise
        for (int q = 0; q < vtf::DXT_QUALITY_COUNT; ++q) {
            vtf::Options o;
            o.colorFormat = o.alphaFormat = vtf::FMT_DXT1;
            o.dxtQuality = q;
            o.mipmaps = false;
            o.thumbnail = false;
            o.versionMinor = 2;
            std::vector<uint8_t> file, back; std::string err; vtf::ReadInfo info;
            CHECK(vtf::Write(g.data(), GW, GH, o, file, err) && vtf::Read(file.data(), file.size(), back, info, err), "gradient");
            psnr[q] = Psnr(g, back, 0, 2);
            double se = 0; int n = 0;
            for (uint32_t y = 0; y + 8 <= GH; y += 2)
                for (uint32_t x = 0; x + 8 <= GW; x += 2)
                    for (int c = 0; c < 3; ++c) {
                        double sum = 0;
                        for (int j = 0; j < 8; ++j)
                            for (int i = 0; i < 8; ++i) {
                                const size_t k = ((y + j) * GW + x + i) * 4 + c;
                                sum += double(back[k]) - g[k];
                            }
                        se += (sum / 64) * (sum / 64); ++n;
                    }
            lowFreq[q] = 10.0 * std::log10(255.0 * 255.0 / (se / n));
            std::printf("gradient %-10s %.2f dB, banding %.2f dB\n", qn[q], psnr[q], lowFreq[q]);
        }
        CHECK(psnr[vtf::DXT_QUALITY_MAX] > psnr[vtf::DXT_QUALITY_FAST] + 1.0, "gradient: accurate not better than fast");
        CHECK(psnr[vtf::DXT_QUALITY_MAX] > psnr[vtf::DXT_QUALITY_HIGH] + 0.5, "gradient: accurate not better than high");
        CHECK(psnr[vtf::DXT_QUALITY_MAX] > psnr[vtf::DXT_QUALITY_BEST] + 0.5, "gradient: accurate not better than best");
        for (int q : {vtf::DXT_QUALITY_FAST, vtf::DXT_QUALITY_HIGH, vtf::DXT_QUALITY_BEST})
            CHECK(lowFreq[vtf::DXT_QUALITY_MAX] > lowFreq[q] + 1.0, "gradient: accurate bands more than %s", qn[q]);
    }

    std::printf("failures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
