// Vmt.cpp - see Vmt.h.
#include "Vmt.h"

#include <cctype>

namespace vmt {

const char* ShaderName(int shader) {
    switch (shader) {
    case SHADER_VERTEXLIT:   return "VertexLitGeneric";
    case SHADER_LIGHTMAPPED: return "LightmappedGeneric";
    case SHADER_UNLIT:       return "UnlitGeneric";
    default:                 return "VertexLitGeneric";
    }
}

namespace {

bool IsSep(char c) { return c == '\\' || c == '/'; }

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Path without its extension (only if the dot is in the last component).
std::string StripExtension(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return p;
    for (size_t i = dot; i < p.size(); ++i)
        if (IsSep(p[i])) return p;
    const size_t sep = p.find_last_of("\\/");
    if (sep != std::string::npos && dot < sep) return p;
    return p.substr(0, dot);
}

} // namespace

std::string BaseTexture(const std::string& vtfPath) {
    const std::string noExt = StripExtension(vtfPath);
    // Split into components.
    std::string result;
    size_t start = std::string::npos;  // index after the last "materials" component
    size_t i = 0;
    while (i <= noExt.size()) {
        const size_t j = noExt.find_first_of("\\/", i);
        const size_t end = j == std::string::npos ? noExt.size() : j;
        if (Lower(noExt.substr(i, end - i)) == "materials" && end < noExt.size()) start = end + 1;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (start == std::string::npos) {
        const size_t sep = noExt.find_last_of("\\/");
        return sep == std::string::npos ? noExt : noExt.substr(sep + 1);
    }
    result = noExt.substr(start);
    for (char& c : result)
        if (c == '\\') c = '/';
    return result;
}

std::string VmtPath(const std::string& vtfPath) {
    return StripExtension(vtfPath) + ".vmt";
}

std::string Build(const std::string& baseTexture, int shader, Alpha alpha, uint32_t frames) {
    const char* nl = "\r\n";
    std::string s;
    s += '"'; s += ShaderName(shader); s += '"'; s += nl;
    s += "{"; s += nl;
    s += "\t\"$basetexture\" \""; s += baseTexture; s += '"'; s += nl;
    if (alpha == ALPHA_TEST) { s += "\t\"$alphatest\" \"1\""; s += nl; }
    if (alpha == ALPHA_BLEND) { s += "\t\"$translucent\" \"1\""; s += nl; }
    if (frames > 1) {
        s += nl;
        s += "\t\"Proxies\""; s += nl;
        s += "\t{"; s += nl;
        s += "\t\t\"AnimatedTexture\""; s += nl;
        s += "\t\t{"; s += nl;
        s += "\t\t\t\"animatedTextureVar\" \"$basetexture\""; s += nl;
        s += "\t\t\t\"animatedTextureFrameNumVar\" \"$frame\""; s += nl;
        s += "\t\t\t\"animatedTextureFrameRate\" \"10\""; s += nl;
        s += "\t\t}"; s += nl;
        s += "\t}"; s += nl;
    }
    s += "}"; s += nl;
    return s;
}

Alpha ClassifyAlpha(const uint8_t* rgba, size_t pixels) {
    bool any = false;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t a = rgba[i * 4 + 3];
        if (a == 255) continue;
        if (a != 0) return ALPHA_BLEND;
        any = true;
    }
    return any ? ALPHA_TEST : ALPHA_NONE;
}

} // namespace vmt
