// VtfWriter.cpp - minimal, dependency-free VTF writer.
#include "VtfWriter.h"
#include "VtfReader.h"
#include "Resample.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>

#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include "../third_party/stb_dxt.h"

// icbc: high quality BC1 (DXT1 colour) encoder by Ignacio Castano (author of
// NVIDIA Texture Tools), MIT. SSE2 only, so the plug-in runs on any x64 CPU and
// gives the same result on every build.
#define ICBC_IMPLEMENTATION
#define ICBC_SIMD 1  // ICBC_SSE2
#include "../third_party/icbc.h"

#include <mutex>
#include <thread>

namespace vtf {

namespace {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px; // RGBA8
};

bool IsCompressed(int32_t f) { return f == FMT_DXT1 || f == FMT_DXT3 || f == FMT_DXT5; }

uint32_t AlphaBits(int32_t f) {
    switch (f) {
    case FMT_RGBA8888: case FMT_BGRA8888: case FMT_IA88: case FMT_A8:
    case FMT_DXT3: case FMT_DXT5: return 8;
    case FMT_BGRA4444: return 4;
    case FMT_BGRA5551: return 1;
    default: return 0;
    }
}

// --- little-endian byte writer ----------------------------------------------
struct Buf {
    std::vector<uint8_t>& v;
    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) { u8(uint8_t(x)); u8(uint8_t(x >> 8)); }
    void u32(uint32_t x) { u16(uint16_t(x)); u16(uint16_t(x >> 16)); }
    void i32(int32_t x) { u32(uint32_t(x)); }
    void f32(float x) { uint32_t b; std::memcpy(&b, &x, 4); u32(b); }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
    void bytes(const std::vector<uint8_t>& b) { v.insert(v.end(), b.begin(), b.end()); }
};

uint8_t ToByte(float v) {
    v = v * 255.0f + 0.5f;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return uint8_t(v);
}

// --- format encoders ------------------------------------------------------------
uint8_t Luminance(const uint8_t* p) {
    return ToByte((0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f);
}

uint32_t Q(uint8_t v, uint32_t bits) { // quantise 8 -> bits with rounding
    const uint32_t maxv = (1u << bits) - 1;
    return (uint32_t(v) * maxv + 127) / 255;
}

// One-time table setup of both encoders (not thread-safe themselves, so done
// before any encoding thread starts).
void InitEncoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        icbc::init(icbc::Decoder_D3D10);
        uint8_t block[64] = {}, out[16];
        stb_compress_dxt_block(out, block, 1, STB_DXT_HIGHQUAL);  // builds stb_dxt's tables
    });
}

// --- Experimental refinement (VTFBridge) ------------------------------------------
// icbc picks endpoints with a fast approximate search. This polishes them: a
// coordinate descent over the six 5:6:5 endpoint components (single and joint
// steps of +-1/+-2) that minimises the block error. The error is measured
// against the exact 4-colour palette in thirds (no rounding), so the result is
// not tuned to one GPU's decoder: in our tests it gains on D3D10, NVIDIA and AMD
// decoding alike (up to ~0.5 dB on gradients).

struct Rgb { int r, g, b; };

Rgb Expand565(int c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

// Error of a block for endpoints c0, c1 with the best index per pixel (palette x3).
int64_t BlockError(const uint8_t block[64], int c0, int c1, const int w[3], uint32_t* indices = nullptr,
                   int64_t bound = INT64_MAX) {
    const Rgb a = Expand565(c0), b = Expand565(c1);
    const int pr[4] = {3 * a.r, 3 * b.r, 2 * a.r + b.r, a.r + 2 * b.r};
    const int pg[4] = {3 * a.g, 3 * b.g, 2 * a.g + b.g, a.g + 2 * b.g};
    const int pb[4] = {3 * a.b, 3 * b.b, 2 * a.b + b.b, a.b + 2 * b.b};
    const int w0 = w[0], w1 = w[1], w2 = w[2];
    int64_t e = 0;
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        const int r = 3 * block[i * 4], g = 3 * block[i * 4 + 1], bl = 3 * block[i * 4 + 2];
        int best = 0x7fffffff, bi = 0;
        for (int s = 0; s < 4; ++s) {
            const int dr = pr[s] - r, dg = pg[s] - g, db = pb[s] - bl;
            const int d = w0 * dr * dr + w1 * dg * dg + w2 * db * db;
            if (d < best) { best = d; bi = s; }
        }
        e += best;
        if (e >= bound) return e;
        idx |= uint32_t(bi) << (2 * i);
    }
    if (indices) *indices = idx;
    return e;
}

void RefineEndpoints(const uint8_t block[64], int& c0, int& c1, const int w[3]) {
    int a[3] = {(c0 >> 11) & 31, (c0 >> 5) & 63, c0 & 31};
    int b[3] = {(c1 >> 11) & 31, (c1 >> 5) & 63, c1 & 31};
    const int maxv[3] = {31, 63, 31};
    auto pack = [](const int v[3]) { return (v[0] << 11) | (v[1] << 5) | v[2]; };
    int64_t best = BlockError(block, c0, c1, w);
    for (int round = 0; round < 24; ++round) {
        bool improved = false;
        for (int ch = 0; ch < 3; ++ch)
            for (int d : {-2, -1, 1, 2})
                for (int move = 0; move < 4; ++move) {  // a, b, both, opposite
                    int na[3] = {a[0], a[1], a[2]}, nb[3] = {b[0], b[1], b[2]};
                    if (move != 1) na[ch] += d;
                    if (move == 1 || move == 2) nb[ch] += d;
                    if (move == 3) nb[ch] -= d;
                    if (na[ch] < 0 || na[ch] > maxv[ch] || nb[ch] < 0 || nb[ch] > maxv[ch]) continue;
                    const int64_t e = BlockError(block, pack(na), pack(nb), w, nullptr, best);  // stops early once worse
                    if (e < best) {
                        best = e;
                        std::copy(na, na + 3, a);
                        std::copy(nb, nb + 3, b);
                        improved = true;
                    }
                }
        if (!improved) break;
    }
    c0 = pack(a);
    c1 = pack(b);
}

// Writes a 4-colour block for endpoints c0, c1 (orders them c0 > c1, as DXT1
// needs for 4-colour mode).
void WriteColorBlock(const uint8_t block[64], int c0, int c1, const int w[3], uint8_t* dst) {
    if (c0 < c1) std::swap(c0, c1);
    uint32_t idx = 0;
    if (c0 != c1) BlockError(block, c0, c1, w, &idx);  // equal endpoints: all index 0 (valid in every mode)
    dst[0] = uint8_t(c0); dst[1] = uint8_t(c0 >> 8);
    dst[2] = uint8_t(c1); dst[3] = uint8_t(c1 >> 8);
    for (int i = 0; i < 4; ++i) dst[4 + i] = uint8_t(idx >> (8 * i));
}

// DXT colour block (8 bytes) in 4-colour mode: DXT1 without 1-bit alpha, and the
// colour half of DXT3/DXT5 (always decoded in 4-colour mode).
void EncodeColorBlock(const uint8_t block[64], int quality, uint8_t* dst) {
    if (quality == DXT_QUALITY_FAST || quality >= DXT_QUALITY_MAX) {
        stb_compress_dxt_block(dst, block, 0, STB_DXT_HIGHQUAL);
        if (quality == DXT_QUALITY_FAST) return;
        // VTFBridge modes: refine stb's endpoints against the exact palette. Starting from
        // stb (not from icbc) keeps smooth gradients free of flat 4x4 steps and is faster.
        static const int kUniform[3] = {1, 1, 1}, kPerceptual[3] = {3, 6, 1};
        const int* w = quality == DXT_QUALITY_PERCEPTUAL ? kPerceptual : kUniform;
        int c0 = dst[0] | (dst[1] << 8), c1 = dst[2] | (dst[3] << 8);
        RefineEndpoints(block, c0, c1, w);
        WriteColorBlock(block, c0, c1, w, dst);
        return;
    }
    // High / Best: icbc as it is (third-party presets).
    float colors[64], weights[16];
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) colors[i * 4 + c] = block[i * 4 + c] / 255.0f;
        colors[i * 4 + 3] = 1.0f;
        weights[i] = 1.0f;
    }
    const float channelWeights[3] = {1.0f, 1.0f, 1.0f};
    const icbc::Quality level = quality == DXT_QUALITY_HIGH ? icbc::Quality_Level5 : icbc::Quality_Level8;
    icbc::compress_bc1(level, colors, weights, channelWeights, false, false, dst);
}

// DXT5 alpha palette for endpoints a0, a1 (8 values; 6-value mode when a0 <= a1),
// same rounding as VtfReader.
void AlphaPalette(int a0, int a1, int p[8]) {
    p[0] = a0;
    p[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) p[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;  // rounded, like the
    } else {                                                                 // exact (float) palette
        for (int i = 1; i < 5; ++i) p[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        p[6] = 0;
        p[7] = 255;
    }
}

// DXT5 alpha block (8 bytes) by searching endpoints around the block's min/max in
// both modes; picks the smallest squared error (better than min/max alone on soft alpha).
void EncodeAlphaBlockSearch(const uint8_t block[64], uint8_t* dst) {
    int a[16], mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) {
        a[i] = block[i * 4 + 3];
        mn = std::min(mn, a[i]);
        mx = std::max(mx, a[i]);
    }
    const int r = 8;
    long best = -1;
    int bestA0 = mx, bestA1 = mn;
    for (int lo = std::max(0, mn - r); lo <= std::min(255, mn + r); ++lo) {
        for (int hi = std::max(0, mx - r); hi <= std::min(255, mx + r); ++hi) {
            for (int mode = 0; mode < 2; ++mode) {
                const int a0 = mode ? lo : hi, a1 = mode ? hi : lo;  // mode 0: 8 values, 1: 6 values + 0/255
                if (mode == 0 && a0 <= a1) continue;
                int p[8];
                AlphaPalette(a0, a1, p);
                long e = 0;
                for (int i = 0; i < 16 && (best < 0 || e < best); ++i) {
                    int d = 1 << 30;
                    for (int k = 0; k < 8; ++k) d = std::min(d, (a[i] - p[k]) * (a[i] - p[k]));
                    e += d;
                }
                if (best < 0 || e < best) { best = e; bestA0 = a0; bestA1 = a1; }
            }
        }
    }
    int p[8];
    AlphaPalette(bestA0, bestA1, p);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int bi = 0, bd = 1 << 30;
        for (int k = 0; k < 8; ++k) {
            const int d = (a[i] - p[k]) * (a[i] - p[k]);
            if (d < bd) { bd = d; bi = k; }
        }
        bits |= uint64_t(bi) << (3 * i);
    }
    dst[0] = uint8_t(bestA0);
    dst[1] = uint8_t(bestA1);
    for (int i = 0; i < 6; ++i) dst[2 + i] = uint8_t(bits >> (8 * i));
}

void EncodeDXTBlock(const Image& img, int32_t fmt, int quality, uint32_t bx, uint32_t by, uint8_t* dst) {
    // Gather 4x4, replicating edge pixels for images smaller than 4.
    uint8_t block[64];
    for (uint32_t y = 0; y < 4; ++y) {
        const uint32_t sy = std::min(by * 4 + y, img.h - 1);
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = std::min(bx * 4 + x, img.w - 1);
            std::memcpy(&block[(y * 4 + x) * 4], &img.px[(size_t(sy) * img.w + sx) * 4], 4);
        }
    }
    switch (fmt) {
    case FMT_DXT1:
        EncodeColorBlock(block, quality, dst);
        break;
    case FMT_DXT5:
        if (quality >= DXT_QUALITY_BEST) EncodeAlphaBlockSearch(block, dst);
        else {
            uint8_t tmp[16];
            stb_compress_dxt_block(tmp, block, 1, STB_DXT_HIGHQUAL);  // alpha half
            std::memcpy(dst, tmp, 8);
        }
        EncodeColorBlock(block, quality, dst + 8);
        break;
    case FMT_DXT3:
        // Explicit 4-bit alpha, pixel 0 in the low nibble of byte 0.
        for (int i = 0; i < 8; ++i) {
            const uint32_t a0 = Q(block[(i * 2) * 4 + 3], 4);
            const uint32_t a1 = Q(block[(i * 2 + 1) * 4 + 3], 4);
            dst[i] = uint8_t(a0 | (a1 << 4));
        }
        EncodeColorBlock(block, quality, dst + 8);
        break;
    }
}

void EncodeDXT(const Image& img, int32_t fmt, int quality, std::vector<uint8_t>& out) {
    InitEncoders();
    const uint32_t bw = std::max(1u, (img.w + 3) / 4);
    const uint32_t bh = std::max(1u, (img.h + 3) / 4);
    const uint32_t blockBytes = fmt == FMT_DXT1 ? 8 : 16;
    out.resize(size_t(bw) * bh * blockBytes);

    // Rows of blocks are independent: large images are split over the CPU cores.
    // Each block is encoded the same way regardless of the thread count.
    auto rows = [&](uint32_t from, uint32_t to) {
        for (uint32_t by = from; by < to; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx)
                EncodeDXTBlock(img, fmt, quality, bx, by, &out[(size_t(by) * bw + bx) * blockBytes]);
    };
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t threads = std::min<uint32_t>(std::min(hw, 16u), std::max(1u, bh / 16));
    if (threads <= 1 || quality == DXT_QUALITY_FAST) { rows(0, bh); return; }
    std::vector<std::thread> pool;
    for (uint32_t t = 0; t < threads; ++t)
        pool.emplace_back(rows, bh * t / threads, bh * (t + 1) / threads);
    for (auto& th : pool) th.join();
}

// Perceptual weighting is meant for colour: normal maps / SSBump hold vectors.
int DxtQualityFor(const Options& opt) {
    if (opt.dxtQuality == DXT_QUALITY_PERCEPTUAL && (opt.flags & (FLAG_NORMAL | FLAG_SSBUMP))) return DXT_QUALITY_MAX;
    return opt.dxtQuality < 0 || opt.dxtQuality >= DXT_QUALITY_COUNT ? DXT_QUALITY_MAX : opt.dxtQuality;
}

bool Encode(const Image& img, int32_t fmt, int dxtQuality, std::vector<uint8_t>& out) {
    if (IsCompressed(fmt)) { EncodeDXT(img, fmt, dxtQuality, out); return true; }

    const size_t n = size_t(img.w) * img.h;
    out.resize(ImageSize(fmt, img.w, img.h));
    uint8_t* d = out.data();
    const uint8_t* s = img.px.data();

    for (size_t i = 0; i < n; ++i, s += 4) {
        switch (fmt) {
        case FMT_RGBA8888: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; break;
        case FMT_BGRA8888: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; d += 4; break;
        case FMT_RGB888:   d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; break;
        case FMT_BGR888:   d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d += 3; break;
        case FMT_I8:       d[0] = Luminance(s); d += 1; break;
        case FMT_IA88:     d[0] = Luminance(s); d[1] = s[3]; d += 2; break;
        case FMT_A8:       d[0] = s[3]; d += 1; break;
        case FMT_BGR565: {
            const uint32_t v = Q(s[2], 5) | (Q(s[1], 6) << 5) | (Q(s[0], 5) << 11);
            d[0] = uint8_t(v); d[1] = uint8_t(v >> 8); d += 2; break;
        }
        case FMT_BGRA4444: {
            const uint32_t v = Q(s[2], 4) | (Q(s[1], 4) << 4) | (Q(s[0], 4) << 8) | (Q(s[3], 4) << 12);
            d[0] = uint8_t(v); d[1] = uint8_t(v >> 8); d += 2; break;
        }
        case FMT_BGRA5551: {
            const uint32_t v = Q(s[2], 5) | (Q(s[1], 5) << 5) | (Q(s[0], 5) << 10) | ((s[3] >= 128 ? 1u : 0u) << 15);
            d[0] = uint8_t(v); d[1] = uint8_t(v >> 8); d += 2; break;
        }
        default:
            return false;
        }
    }
    return true;
}

// Same thumbnail sizing rule as VTFLib: halve until both sides <= 16.
void ThumbnailSize(uint32_t w, uint32_t h, uint32_t& tw, uint32_t& th) {
    tw = w; th = h;
    while (tw > 16 || th > 16) {
        tw = std::max(1u, tw >> 1);
        th = std::max(1u, th >> 1);
    }
}

} // namespace

bool IsPowerOfTwo(uint32_t v) { return v && !(v & (v - 1)); }

uint32_t ImageSize(int32_t f, uint32_t w, uint32_t h) {
    switch (f) {
    case FMT_DXT1: return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 8;
    case FMT_DXT3:
    case FMT_DXT5: return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 16;
    case FMT_RGBA8888: case FMT_BGRA8888: return w * h * 4;
    case FMT_RGB888: case FMT_BGR888: return w * h * 3;
    case FMT_BGR565: case FMT_BGRA4444: case FMT_BGRA5551: case FMT_IA88: return w * h * 2;
    case FMT_I8: case FMT_A8: return w * h;
    default: return 0;
    }
}

bool IsWritableFormat(int32_t f) { return ImageSize(f, 4, 4) != 0; }

namespace {
uint32_t PowerOfTwo(uint32_t v, int method) {
    uint32_t lo = 1;
    while (lo * 2 <= v && lo < (1u << 15)) lo *= 2;  // largest pow2 <= v (max 32768)
    const uint32_t hi = lo >= v ? lo : std::min(lo * 2, 32768u);
    switch (method) {
    case RESIZE_BIGGEST:  return hi;
    case RESIZE_SMALLEST: return lo;
    default:              return (v - lo) < (hi - v) ? lo : hi;  // ties round up
    }
}
} // namespace

void TargetSize(uint32_t w, uint32_t h, const Options& o, uint32_t& tw, uint32_t& th) {
    tw = w; th = h;
    if (!o.resize) return;
    tw = PowerOfTwo(w, o.resizeMethod);
    th = PowerOfTwo(h, o.resizeMethod);
    if (o.maxWidth && tw > o.maxWidth) tw = o.maxWidth;
    if (o.maxHeight && th > o.maxHeight) th = o.maxHeight;
}

const char* FormatName(int32_t f) {
    switch (f) {
    case FMT_DXT1: return "DXT1";
    case FMT_DXT3: return "DXT3";
    case FMT_DXT5: return "DXT5";
    case FMT_BGR888: return "BGR888";
    case FMT_BGRA8888: return "BGRA8888";
    case FMT_RGB888: return "RGB888";
    case FMT_RGBA8888: return "RGBA8888";
    case FMT_BGR565: return "BGR565";
    case FMT_BGRA4444: return "BGRA4444";
    case FMT_BGRA5551: return "BGRA5551";
    case FMT_I8: return "I8";
    case FMT_IA88: return "IA88";
    case FMT_A8: return "A8";
    default: return "?";
    }
}

namespace {

// Normal maps: filtering averages the vectors, which makes them shorter, so lower
// mips look flatter than they should. Scale every RGB vector (decoded as
// 2*c-1) back to unit length. Alpha (often a specular mask) is left alone.
void RenormalizeNormals(FloatImage& img) {
    float* p = img.px.data();
    for (size_t i = 0, n = size_t(img.w) * img.h; i < n; ++i, p += 4) {
        float x = p[0] * 2.f - 1.f, y = p[1] * 2.f - 1.f, z = p[2] * 2.f - 1.f;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-4f) { x /= len; y /= len; z /= len; }
        else { x = 0.f; y = 0.f; z = 1.f; }
        p[0] = x * .5f + .5f;
        p[1] = y * .5f + .5f;
        p[2] = z * .5f + .5f;
    }
}

// Base level (optionally resized to a power of two) + full mip chain down to 1x1.
// Each level is filtered from the previous one in float, so rounding does not accumulate.
std::vector<Image> BuildChain(const uint8_t* rgba, uint32_t width, uint32_t height, const Options& opt) {
    // Repeat across the edges unless the texture is clamped in that direction:
    // mips/resizes of tiling textures then stay seamless.
    // Edges: clamp unless the user marked the texture as tiling (decals and
    // sprites must not get pixels from the opposite edge in their mipmaps).
    const bool wrapX = opt.tiling && !(opt.flags & FLAG_CLAMPS);
    const bool wrapY = opt.tiling && !(opt.flags & FLAG_CLAMPT);
    // Normal maps (not SSBump: its three vectors are not unit length) hold
    // directions, not colours: no gamma, renormalised after every filter step.
    const bool normal = (opt.flags & FLAG_NORMAL) && !(opt.flags & FLAG_SSBUMP);
    const bool gamma = opt.gammaFilter && !normal;

    uint32_t tw0, th0;
    TargetSize(width, height, opt, tw0, th0);
    std::vector<Image> chain(1);
    FloatImage level;
    const bool resized = tw0 != width || th0 != height;
    if (resized || tw0 > 1 || th0 > 1) level = ToFloat(rgba, width, height, gamma);
    if (resized) {
        level = Resample(level, tw0, th0, Filter(opt.resizeFilter), wrapX, wrapY);
        if (normal) RenormalizeNormals(level);
        chain[0].w = tw0;
        chain[0].h = th0;
        ToBytes(level, gamma, chain[0].px);
    } else {
        chain[0].w = width;
        chain[0].h = height;
        chain[0].px.assign(rgba, rgba + size_t(width) * height * 4);
    }
    while (chain.back().w > 1 || chain.back().h > 1) {
        level = Resample(level, std::max(1u, level.w / 2), std::max(1u, level.h / 2),
                         Filter(opt.mipFilter), wrapX, wrapY);
        if (opt.mipSharpen > 0) Sharpen(level, opt.mipSharpen / 100.0f, wrapX, wrapY, opt.sharpenAlpha);
        if (normal) RenormalizeNormals(level);
        Image m;
        m.w = level.w;
        m.h = level.h;
        ToBytes(level, gamma, m.px);
        chain.push_back(std::move(m));
    }
    return chain;
}

} // namespace

bool BuildPreview(const uint8_t* rgba, uint32_t width, uint32_t height, const Options& opt,
                  std::vector<PreviewLevel>& levels, int32_t& format, int level) {
    if (!rgba || width == 0 || height == 0) return false;
    bool hasAlpha = false;
    for (size_t i = 3; i < size_t(width) * height * 4; i += 4)
        if (rgba[i] != 255) { hasAlpha = true; break; }
    format = ResolveFormat(opt, hasAlpha);
    std::vector<Image> chain = BuildChain(rgba, width, height, opt);
    if (!opt.mipmaps) chain.resize(1);
    levels.assign(chain.size(), PreviewLevel());
    for (size_t m = 0; m < chain.size(); ++m) {
        PreviewLevel& L = levels[m];
        L.w = chain[m].w;
        L.h = chain[m].h;
        L.original = std::move(chain[m].px);
        if (level >= 0 && int(m) != level) continue;
        Image img{L.w, L.h, L.original};
        std::vector<uint8_t> enc;
        if (!Encode(img, format, DxtQualityFor(opt), enc) ||
            !DecodeImage(format, enc.data(), enc.size(), L.w, L.h, L.decoded))
            return false;
        double se = 0;
        for (size_t i = 0; i < L.original.size(); i += 4)
            for (int c = 0; c < 3; ++c) { const double d = double(L.decoded[i + c]) - L.original[i + c]; se += d * d; }
        const double mse = se / (double(L.w) * L.h * 3);
        L.psnr = mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }
    return true;
}

uint64_t FileSize(uint32_t texW, uint32_t texH, int32_t fmt, uint32_t frames, const Options& opt) {
    if (texW == 0 || texH == 0) return 0;
    const int v = opt.versionMinor;
    uint64_t size = v <= 1 ? 64 : v == 2 ? 80 : 80 + (opt.thumbnail ? 2u : 1u) * 8;
    if (opt.thumbnail) {
        uint32_t tw, th;
        ThumbnailSize(texW, texH, tw, th);
        size += ImageSize(FMT_DXT1, tw, th);
    }
    uint64_t mips = 0;
    uint32_t w = texW, h = texH;
    for (;;) {
        mips += ImageSize(fmt, w, h);
        if (!opt.mipmaps || (w == 1 && h == 1)) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return size + mips * frames;
}

bool Write(const uint8_t* rgba, uint32_t width, uint32_t height,
           const Options& opt, std::vector<uint8_t>& out, std::string& error) {
    return Write(std::vector<const uint8_t*>{rgba}, width, height, opt, out, error);
}

bool Write(const std::vector<const uint8_t*>& frames, uint32_t width, uint32_t height,
           const Options& opt, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    if (frames.empty() || frames.size() > 65535) {
        error = "Invalid frame count.";
        return false;
    }
    for (const uint8_t* f : frames)
        if (!f) { error = "Missing frame data."; return false; }
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        error = "Invalid image size.";
        return false;
    }
    if (opt.versionMinor < 1 || opt.versionMinor > 5) {
        error = "Unsupported VTF version (7.1 - 7.5 supported).";
        return false;
    }
    if (opt.mipFilter < 0 || opt.mipFilter >= FILTER_COUNT || opt.resizeFilter < 0 || opt.resizeFilter >= FILTER_COUNT) {
        error = "Unknown filter.";
        return false;
    }

    // One format for the whole texture: with alpha if any frame has alpha.
    bool hasAlpha = false;
    for (const uint8_t* f : frames) {
        for (size_t i = 3, n = size_t(width) * height * 4; i < n && !hasAlpha; i += 4)
            if (f[i] != 255) hasAlpha = true;
        if (hasAlpha) break;
    }
    const int32_t fmt = ResolveFormat(opt, hasAlpha);
    if (!IsWritableFormat(fmt)) {
        error = "Unsupported image format.";
        return false;
    }

    const uint32_t frameCount = uint32_t(frames.size());
    uint32_t texW = 0, texH = 0, mipCount = 1;
    std::vector<std::vector<std::vector<uint8_t>>> encoded(frameCount);  // [frame][mip]
    std::vector<uint8_t> thumb;
    uint32_t tw = 0, th = 0;
    double refl[3] = {0, 0, 0};

    for (uint32_t f = 0; f < frameCount; ++f) {
        std::vector<Image> chain = BuildChain(frames[f], width, height, opt);
        texW = chain[0].w;
        texH = chain[0].h;
        mipCount = opt.mipmaps ? uint32_t(chain.size()) : 1u;

        // Thumbnail from the first frame.
        if (f == 0 && opt.thumbnail) {
            ThumbnailSize(texW, texH, tw, th);
            const Image* src = nullptr;
            for (const Image& m : chain)
                if (m.w == tw && m.h == th) { src = &m; break; }
            Image tmp;
            if (!src) {  // NPOT edge case: nearest sample from the base level
                tmp.w = tw; tmp.h = th; tmp.px.resize(size_t(tw) * th * 4);
                for (uint32_t y = 0; y < th; ++y)
                    for (uint32_t x = 0; x < tw; ++x)
                        std::memcpy(&tmp.px[(size_t(y) * tw + x) * 4],
                                    &chain[0].px[(size_t(y * texH / th) * texW + x * texW / tw) * 4], 4);
                src = &tmp;
            }
            Encode(*src, FMT_DXT1, DxtQualityFor(opt), thumb);
        }

        // Reflectivity (VTFLib formula: mean of (c/255)^2.2), averaged over frames.
        {
            double acc[3] = {0, 0, 0};
            const auto& p = chain[0].px;
            for (size_t i = 0; i < p.size(); i += 4)
                for (int c = 0; c < 3; ++c) acc[c] += std::pow(p[i + c] / 255.0, 2.2);
            const double n = double(texW) * texH;
            for (int c = 0; c < 3; ++c) refl[c] += acc[c] / n / frameCount;
        }

        encoded[f].resize(mipCount);
        for (uint32_t m = 0; m < mipCount; ++m)
            if (!Encode(chain[m], fmt, DxtQualityFor(opt), encoded[f][m])) { error = "Encoding failed."; return false; }
    }

    // High-res data: smallest mip first; inside each mip, frame 0..N-1.
    size_t hiresSize = 0;
    for (const auto& fr : encoded)
        for (const auto& m : fr) hiresSize += m.size();

    // Flags.
    uint32_t flags = opt.flags & ~(FLAG_ONEBITALPHA | FLAG_EIGHTBITALPHA);
    const uint32_t ab = AlphaBits(fmt);
    if (ab == 1) flags |= FLAG_ONEBITALPHA;
    else if (ab > 1) flags |= FLAG_EIGHTBITALPHA;

    // Header.
    const int v = opt.versionMinor;
    const bool resources = v >= 3;
    const uint32_t resourceCount = resources ? (opt.thumbnail ? 2u : 1u) : 0u;
    uint32_t headerSize;
    if (v <= 1)      headerSize = 64;
    else if (v == 2) headerSize = 80;
    else             headerSize = 80 + resourceCount * 8;

    out.reserve(headerSize + thumb.size() + hiresSize);
    Buf b{out};
    b.u8('V'); b.u8('T'); b.u8('F'); b.u8(0);
    b.u32(7); b.u32(uint32_t(v));
    b.u32(headerSize);
    b.u16(uint16_t(texW)); b.u16(uint16_t(texH));
    b.u32(flags);
    b.u16(uint16_t(frameCount));
    b.u16(0);  // start frame
    b.zeros(4);
    b.f32(float(refl[0])); b.f32(float(refl[1])); b.f32(float(refl[2]));
    b.zeros(4);
    b.f32(opt.bumpScale);
    b.i32(fmt);
    b.u8(uint8_t(mipCount));
    b.i32(opt.thumbnail ? FMT_DXT1 : FMT_NONE);
    b.u8(uint8_t(tw)); b.u8(uint8_t(th));
    // offset now 63
    if (v >= 2) b.u16(1); // depth
    if (v >= 3) {
        b.zeros(3);
        b.u32(resourceCount);
        b.zeros(8); // pad to 80
        uint32_t offset = headerSize;
        if (opt.thumbnail) {
            b.u8(0x01); b.u8(0); b.u8(0); b.u8(0); b.u32(offset);
            offset += uint32_t(thumb.size());
        }
        b.u8(0x30); b.u8(0); b.u8(0); b.u8(0); b.u32(offset);
    }
    if (out.size() < headerSize) b.zeros(headerSize - out.size());

    b.bytes(thumb);
    for (int m = int(mipCount) - 1; m >= 0; --m)
        for (uint32_t f = 0; f < frameCount; ++f) b.bytes(encoded[f][size_t(m)]);
    return true;
}

} // namespace vtf
