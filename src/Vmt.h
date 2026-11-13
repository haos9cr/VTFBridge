// Vmt.h - builds a simple .vmt material for a saved texture.
// Platform-independent (no Windows / SDK), so it is tested on Linux.
#pragma once

#include <cstdint>
#include <string>

namespace vmt {

// Order = order in the UI and value stored in settings. Do not reorder.
enum Shader : int {
    SHADER_VERTEXLIT = 0,    // models
    SHADER_LIGHTMAPPED,      // world brushes
    SHADER_UNLIT,            // sprites, UI, no lighting
    SHADER_COUNT
};

const char* ShaderName(int shader);

enum Alpha : int {
    ALPHA_NONE = 0,   // opaque (or a format without alpha)
    ALPHA_TEST,       // only fully transparent / fully opaque pixels -> $alphatest
    ALPHA_BLEND,      // soft alpha -> $translucent
};

// Value for $basetexture: the part of the .vtf path after the last "materials"
// folder, with forward slashes and without the extension
// ("C:\Game\materials\models\box\wood.vtf" -> "models/box/wood"). Without a
// materials folder: just the file name ("wood"). Input and output are UTF-8.
std::string BaseTexture(const std::string& vtfPath);

// Path of the .vmt next to the .vtf (same name, extension replaced).
std::string VmtPath(const std::string& vtfPath);

// Text of the material (tabs, CRLF line ends like Valve's files).
// frames > 1 adds the AnimatedTexture proxy.
std::string Build(const std::string& baseTexture, int shader, Alpha alpha, uint32_t frames);

// Classifies the alpha of RGBA8 pixels written in a format that keeps alpha.
Alpha ClassifyAlpha(const uint8_t* rgba, size_t pixels);

} // namespace vmt
