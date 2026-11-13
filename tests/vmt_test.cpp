// vmt_test.cpp - checks the .vmt builder (paths, alpha, animation proxy).
#include "Vmt.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
    using namespace vmt;
    struct P { const char* in; const char* base; const char* vmt; };
    const P paths[] = {
        {"C:\\Steam\\hl2\\materials\\models\\box\\wood.vtf", "models/box/wood", "C:\\Steam\\hl2\\materials\\models\\box\\wood.vmt"},
        {"D:/mod/Materials/Brick.VTF", "Brick", "D:/mod/Materials/Brick.vmt"},
        {"C:\\a\\materials\\x\\materials\\y\\z.vtf", "y/z", "C:\\a\\materials\\x\\materials\\y\\z.vmt"},
        {"C:\\textures\\wood.vtf", "wood", "C:\\textures\\wood.vmt"},
        {"C:\\my.folder\\wood", "wood", "C:\\my.folder\\wood.vmt"},
        {"C:\\materials.vtf", "materials", "C:\\materials.vmt"},
        {"C:\\Игра\\materials\\текстуры\\доска.vtf", "текстуры/доска", "C:\\Игра\\materials\\текстуры\\доска.vmt"},
    };
    for (const P& p : paths) {
        CHECK(BaseTexture(p.in) == p.base, "BaseTexture(%s) = %s", p.in, BaseTexture(p.in).c_str());
        CHECK(VmtPath(p.in) == p.vmt, "VmtPath(%s) = %s", p.in, VmtPath(p.in).c_str());
    }

    const std::string plain = Build("models/box/wood", SHADER_VERTEXLIT, ALPHA_NONE, 1);
    CHECK(plain == "\"VertexLitGeneric\"\r\n{\r\n\t\"$basetexture\" \"models/box/wood\"\r\n}\r\n", "plain:\n%s", plain.c_str());
    const std::string test = Build("a", SHADER_LIGHTMAPPED, ALPHA_TEST, 1);
    CHECK(Has(test, "\"LightmappedGeneric\"") && Has(test, "$alphatest") && !Has(test, "$translucent"), "alphatest");
    const std::string blend = Build("a", SHADER_UNLIT, ALPHA_BLEND, 1);
    CHECK(Has(blend, "\"UnlitGeneric\"") && Has(blend, "$translucent") && !Has(blend, "Proxies"), "translucent");
    const std::string anim = Build("a", SHADER_VERTEXLIT, ALPHA_NONE, 5);
    CHECK(Has(anim, "\"AnimatedTexture\"") && Has(anim, "\"animatedTextureFrameNumVar\" \"$frame\""), "animation proxy");
    int depth = 0;
    for (char c : anim) { depth += c == '{'; depth -= c == '}'; CHECK(depth >= 0, "braces"); }
    CHECK(depth == 0, "unbalanced braces");
    CHECK(std::string(ShaderName(99)) == "VertexLitGeneric", "unknown shader fallback");

    const std::vector<uint8_t> opaque = {1, 2, 3, 255, 4, 5, 6, 255};
    const std::vector<uint8_t> cut = {1, 2, 3, 0, 4, 5, 6, 255};
    const std::vector<uint8_t> soft = {1, 2, 3, 0, 4, 5, 6, 128};
    CHECK(ClassifyAlpha(opaque.data(), 2) == ALPHA_NONE, "opaque");
    CHECK(ClassifyAlpha(cut.data(), 2) == ALPHA_TEST, "cut-out");
    CHECK(ClassifyAlpha(soft.data(), 2) == ALPHA_BLEND, "soft");

    std::printf("failures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
