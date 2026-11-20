// ImageLoad.h - loads a picture from disk as RGBA8 (for the comparison image in the
// preview): PNG, JPEG, BMP, GIF, TIFF, DDS, WebP... through Windows Imaging
// Component, plus TGA and VTF with our own readers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// rgba: width*height*4 bytes, top-down, R,G,B,A. Returns false on failure.
// mips (optional): for a VTF with mipmaps, its smaller levels as stored in the file
// ((*mips)[k] = level k + 1, max(1, width >> (k + 1)) x max(1, height >> (k + 1))).
bool LoadImageFile(const std::wstring& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                   std::vector<std::vector<uint8_t>>* mips = nullptr);

// All frames of a picture (a VTF animation has several; other files one), each with
// the smaller levels stored in the file (VTF mips, may be empty).
struct LoadedFrame {
    std::vector<uint8_t> rgba;
    std::vector<std::vector<uint8_t>> mips;  // level 1, 2, ...
};
bool LoadImageFrames(const std::wstring& path, std::vector<LoadedFrame>& frames, uint32_t& width, uint32_t& height);

// TGA decoder (types 1/2/3 and RLE 9/10/11; 8/15/16/24/32 bit). Exposed for tests.
bool DecodeTga(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
