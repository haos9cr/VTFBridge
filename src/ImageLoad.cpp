// ImageLoad.cpp - see ImageLoad.h.

#include "ImageLoad.h"

#include <windows.h>
#include <wincodec.h>

#include <cstring>
#include <cwctype>

#include "VtfReader.h"

namespace {

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 31);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
    }
    CloseHandle(f);
    return ok;
}

std::wstring Extension(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.find_first_of(L"\\/", dot) != std::wstring::npos) return L"";
    std::wstring e = path.substr(dot + 1);
    for (wchar_t& c : e) c = wchar_t(std::towlower(c));
    return e;
}

// WIC (Windows Imaging Component), GUIDs spelled out so no extra import library is needed.
const CLSID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const IID kIidWicFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
const GUID kFormatRgba = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool LoadWic(const std::wstring& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, kIidWicFactory,
                                   reinterpret_cast<void**>(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, kFormatRgba, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 32768 && h <= 32768) {
        rgba.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
    }
    Release(conv);
    Release(frame);
    Release(decoder);
    Release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) return false;
    width = w;
    height = h;
    return true;
}

} // namespace

bool DecodeTga(const uint8_t* d, size_t size, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    if (size < 18) return false;
    const int idLen = d[0], cmapType = d[1], type = d[2];
    const int cmapFirst = d[3] | (d[4] << 8), cmapLen = d[5] | (d[6] << 8), cmapBits = d[7];
    const uint32_t w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8);
    const int bpp = d[16], desc = d[17];
    const bool rle = type >= 9;
    const int base = rle ? type - 8 : type;  // 1 colour-mapped, 2 true colour, 3 grey
    if (w == 0 || h == 0 || (base != 1 && base != 2 && base != 3)) return false;
    size_t pos = 18 + size_t(idLen);
    std::vector<uint8_t> palette;
    const int cmapBytes = (cmapBits + 7) / 8;
    if (cmapType == 1) {
        if (pos + size_t(cmapLen) * cmapBytes > size) return false;
        palette.assign(d + pos, d + pos + size_t(cmapLen) * cmapBytes);
        pos += size_t(cmapLen) * cmapBytes;
    }
    if (base == 1 && (cmapType != 1 || (bpp != 8 && bpp != 16))) return false;
    if (base == 2 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    if (base == 3 && bpp != 8 && bpp != 16) return false;
    const int bytes = (bpp + 7) / 8;

    // One source pixel (bytes as in the file) -> RGBA.
    auto toRgba = [&](const uint8_t* p, uint8_t* o) -> bool {
        auto fromBits = [&](const uint8_t* q, int nbytes, int bits, uint8_t* out) {
            if (bits == 15 || bits == 16) {
                const int v = q[0] | (q[1] << 8);
                out[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
                out[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                out[2] = uint8_t((v & 31) * 255 / 31);
                out[3] = (bits == 16 && !(v & 0x8000) && (desc & 15)) ? 0 : 255;
            } else {
                out[0] = q[2]; out[1] = q[1]; out[2] = q[0];
                out[3] = nbytes == 4 ? q[3] : 255;
            }
        };
        if (base == 3) {
            o[0] = o[1] = o[2] = p[0];
            o[3] = bytes == 2 ? p[1] : 255;
        } else if (base == 1) {
            const int idx = (bytes == 2 ? (p[0] | (p[1] << 8)) : p[0]) - cmapFirst;
            if (idx < 0 || idx >= cmapLen) return false;
            fromBits(&palette[size_t(idx) * cmapBytes], cmapBytes, cmapBits, o);
        } else {
            fromBits(p, bytes, bpp, o);
        }
        return true;
    };

    std::vector<uint8_t> px(size_t(w) * h * 4);
    const size_t count = size_t(w) * h;
    size_t i = 0;
    while (i < count) {
        if (!rle) {
            if (pos + bytes > size || !toRgba(d + pos, &px[i * 4])) return false;
            pos += bytes;
            ++i;
            continue;
        }
        if (pos >= size) return false;
        const int head = d[pos++];
        const size_t n = size_t(head & 127) + 1;
        if (i + n > count) return false;
        if (head & 128) {
            uint8_t c[4];
            if (pos + bytes > size || !toRgba(d + pos, c)) return false;
            pos += bytes;
            for (size_t k = 0; k < n; ++k) std::memcpy(&px[(i + k) * 4], c, 4);
        } else {
            for (size_t k = 0; k < n; ++k) {
                if (pos + bytes > size || !toRgba(d + pos, &px[(i + k) * 4])) return false;
                pos += bytes;
            }
        }
        i += n;
    }
    // Origin: bit 5 = top, bit 4 = right.
    const bool top = (desc & 0x20) != 0, right = (desc & 0x10) != 0;
    rgba.resize(px.size());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t sy = top ? y : h - 1 - y, sx = right ? w - 1 - x : x;
            std::memcpy(&rgba[(size_t(y) * w + x) * 4], &px[(size_t(sy) * w + sx) * 4], 4);
        }
    width = w;
    height = h;
    return true;
}

bool LoadImageFile(const std::wstring& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                   std::vector<std::vector<uint8_t>>* mips) {
    if (mips) mips->clear();
    const std::wstring ext = Extension(path);
    if (ext == L"tga" || ext == L"vtf") {
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data)) return false;
        if (ext == L"tga") return DecodeTga(data.data(), data.size(), rgba, width, height);
        vtf::ReadInfo info;
        std::string err;
        std::vector<std::vector<uint8_t>> levels;
        if (!vtf::ReadMips(data.data(), data.size(), levels, info, err)) return false;
        rgba.swap(levels[0]);
        if (mips)
            for (size_t m = 1; m < levels.size() && !levels[m].empty(); ++m) mips->push_back(std::move(levels[m]));
        width = info.width;
        height = info.height;
        return width > 0 && height > 0;
    }
    return LoadWic(path, rgba, width, height);
}

bool LoadImageFrames(const std::wstring& path, std::vector<LoadedFrame>& frames, uint32_t& width, uint32_t& height) {
    frames.clear();
    if (Extension(path) != L"vtf") {
        frames.resize(1);
        return LoadImageFile(path, frames[0].rgba, width, height, &frames[0].mips);
    }
    std::vector<uint8_t> data;
    if (!ReadWholeFile(path, data)) return false;
    vtf::ReadInfo info;
    std::string err;
    for (uint32_t f = 0;; ++f) {
        std::vector<std::vector<uint8_t>> levels;
        if (!vtf::ReadMips(data.data(), data.size(), levels, info, err, f)) return !frames.empty();
        LoadedFrame lf;
        lf.rgba.swap(levels[0]);
        for (size_t m = 1; m < levels.size() && !levels[m].empty(); ++m) lf.mips.push_back(std::move(levels[m]));
        frames.push_back(std::move(lf));
        if (f + 1 >= info.frames || f + 1 >= 256) break;
    }
    width = info.width;
    height = info.height;
    return width > 0 && height > 0;
}
