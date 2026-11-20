// fake_host_layers.cpp - emulates Photoshop's layered open and layer access on save.
// Built against the SDK stub (field names as in the real SDK), run under Wine.
// Usage: fake_host_layers plugin.8bi animated.vtf out_dir
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "PIFormat.h"
#include "VtfReader.h"

typedef void (*PluginMainFn)(int16, FormatRecordPtr, intptr_t*, int16*);
static PluginMainFn g_main;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// --- handles -----------------------------------------------------------------
struct H { char* p; int32 size; };
static Handle NewH(int32 size) { return (Handle) new H{(char*)calloc(1, size), size}; }
static void DisposeH(Handle h) { free(((H*)h)->p); delete (H*)h; }
static int32 SizeH(Handle h) { return ((H*)h)->size; }
static OSErr SetSizeH(Handle h, int32 s) { H* x = (H*)h; x->p = (char*)realloc(x->p, s); x->size = s; return 0; }
static Ptr LockH(Handle h, Boolean) { return ((H*)h)->p; }
static void UnlockH(Handle) {}
static HandleProcs g_hp = {1, 6, NewH, DisposeH, SizeH, SetSizeH, LockH, UnlockH};

// --- document model ----------------------------------------------------------------
struct Layer {
    std::string name;
    std::vector<uint8_t> rgba;  // RGBA8, A = transparency
    bool visible = true;
};
static FormatRecord g_fr;
static int g_w, g_h;
static std::vector<Layer> g_layers;  // bottom to top (creation order)
static Layer* g_cur = nullptr;

static OSErr ReadAdvance() {  // plug-in -> document (open)
    const VRect r = g_fr.theRect32;
    const int n = g_fr.hiPlane - g_fr.loPlane + 1;
    if (g_fr.planeBytes != 1 || r.left != 0 || r.right != g_w || r.bottom > g_h || n != g_fr.planes || (n != 3 && n != 4)) return -2;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = 0; x < g_w; ++x)
            for (int p = 0; p < 4; ++p)
                g_cur->rgba[(size_t(y) * g_w + x) * 4 + p] = p < n ?
                    ((uint8_t*)g_fr.data)[size_t(y - r.top) * g_fr.rowBytes + size_t(x) * g_fr.colBytes + p] : 255;
    return 0;
}

// Channel ports: port = pointer to a ChannelSrc.
struct ChannelSrc { const std::vector<uint8_t>* rgba; int component; };
static std::vector<ChannelSrc*> g_ports;
static OSErr ReadPixels(ChannelReadPort port, const PSScaling*, const VRect* rect, const PixelMemoryDesc* d, VRect* wrote) {
    const ChannelSrc* s = (const ChannelSrc*)port;
    if (d->depth != 8) return -1;
    for (int y = rect->top; y < rect->bottom; ++y)
        for (int x = rect->left; x < rect->right; ++x)
            ((uint8_t*)d->data)[size_t(y) * (d->rowBits / 8) + x] = (*s->rgba)[(size_t(y) * g_w + x) * 4 + s->component];
    *wrote = *rect;
    return 0;
}
static ChannelPortProcs g_cpp = {1, 1, ReadPixels, nullptr, nullptr};

static ReadChannelDesc* Chan(const std::vector<uint8_t>* rgba, int component, ReadChannelDesc* next) {
    auto* s = new ChannelSrc{rgba, component};
    g_ports.push_back(s);
    auto* c = new ReadChannelDesc();
    std::memset(c, 0, sizeof(*c));
    c->port = (ChannelReadPort)s;
    c->depth = 8;
    c->next = next;
    return c;
}

static std::vector<uint8_t> g_merged;
// Alpha channels of the document (Channels panel): name + RGBA buffer, value in A.
struct AlphaChan { std::string name; std::vector<uint8_t> rgba; std::vector<uint16> wname; };
static std::vector<AlphaChan> g_alphas;
static ReadImageDocumentDesc g_doc;
static std::vector<ReadLayerDesc> g_descs;
static std::vector<std::vector<uint16>> g_names;

static void BuildDocumentInfo() {
    // Merged image = top visible layer (good enough to see which path is used).
    g_merged.assign(size_t(g_w) * g_h * 4, 77);
    std::memset(&g_doc, 0, sizeof(g_doc));
    g_doc.layerCount = int32(g_layers.size());
    g_doc.mergedCompositeChannels = Chan(&g_merged, 0, Chan(&g_merged, 1, Chan(&g_merged, 2, nullptr)));
    g_doc.mergedTransparency = Chan(&g_merged, 3, nullptr);
    ReadChannelDesc* prev = nullptr;
    for (auto& a : g_alphas) {
        ReadChannelDesc* c = Chan(&a.rgba, 3, nullptr);
        a.wname.assign(a.name.begin(), a.name.end());
        a.wname.push_back(0);
        c->name = a.name.c_str();
        c->unicodeName = a.wname.data();
        c->maxVersion = 3;
        if (prev) prev->next = c; else g_doc.alphaChannels = c;
        prev = c;
    }
    g_descs.assign(g_layers.size(), ReadLayerDesc());
    g_names.assign(g_layers.size(), {});
    for (size_t i = 0; i < g_layers.size(); ++i) {
        ReadLayerDesc& d = g_descs[i];
        std::memset(&d, 0, sizeof(d));
        Layer& L = g_layers[i];
        d.compositeChannelsList = Chan(&L.rgba, 0, Chan(&L.rgba, 1, Chan(&L.rgba, 2, nullptr)));
        d.transparency = Chan(&L.rgba, 3, nullptr);
        d.name = L.name.c_str();
        for (char c : L.name) g_names[i].push_back(uint16(uint8_t(c)));
        g_names[i].push_back(0);
        d.unicodeName = g_names[i].data();
        d.isVisible = L.visible;
        d.isPixelBased = 1;
        d.opacity = d.fillOpacity = 255;
        d.next = i + 1 < g_layers.size() ? &g_descs[i + 1] : nullptr;
    }
    g_doc.layersDescriptor = g_descs.empty() ? nullptr : &g_descs[0];
    g_fr.documentInfo = &g_doc;
    g_fr.channelPortProcs = &g_cpp;
}

static std::string Utf8(const uint16* s) {
    std::wstring w;
    for (; s && *s; ++s) w += wchar_t(*s);
    char buf[256] = {0};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

static void SetReg(const char* name, DWORD v) {
    HKEY key;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VTFBridge", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    RegSetValueExA(key, name, 0, REG_DWORD, (BYTE*)&v, 4);
    RegCloseKey(key);
}

// Opens `path`, returns true if it was opened as layers.
static bool Open(const char* path) {
    std::memset(&g_fr, 0, sizeof(g_fr));
    g_fr.HostSupports32BitCoordinates = 1;
    g_fr.handleProcs = &g_hp;
    g_layers.clear();
    int16 r = 0; intptr_t data = 0;
    HANDLE in = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    g_fr.dataFork = (intptr_t)in;
    g_main(formatSelectorReadPrepare, &g_fr, &data, &r);
    g_main(formatSelectorReadStart, &g_fr, &data, &r);
    CHECK(r == 0, "ReadStart %d", r);
    g_w = g_fr.imageSize32.h; g_h = g_fr.imageSize32.v;
    g_fr.advanceState = ReadAdvance;
    const uint32 n = g_fr.layerData;
    if (n > 0) {
        const bool alphaLayers = g_fr.planes == 3;  // "alpha as separate layers": opaque layers
        CHECK((g_fr.planes == 4 && g_fr.transparencyPlane == 3) || (alphaLayers && g_fr.transparencyPlane == 0),
              "layered planes %d tp %d", g_fr.planes, g_fr.transparencyPlane);
        for (uint32 i = 0; i < n; ++i) {
            g_layers.push_back(Layer());
            g_cur = &g_layers.back();
            g_cur->rgba.assign(size_t(g_w) * g_h * 4, 0);
            g_fr.layerName = nullptr;
            g_fr.isVisible = 1;
            g_main(formatSelectorReadLayerStart, &g_fr, &data, &r);
            g_main(formatSelectorReadLayerContinue, &g_fr, &data, &r);
            g_fr.layerName = nullptr;
            g_main(formatSelectorReadLayerFinish, &g_fr, &data, &r);
            g_cur->name = Utf8(g_fr.layerName);  // the SDK reads it after ReadLayerFinish
            g_cur->visible = g_fr.isVisible != 0;
            CHECK(r == 0, "layer %u result %d", i, r);
        }
    }
    g_main(formatSelectorReadContinue, &g_fr, &data, &r);  // must be a no-op in layered mode
    g_main(formatSelectorReadFinish, &g_fr, &data, &r);
    CloseHandle(in);
    return n > 0;
}

static std::vector<uint8_t> Save(const std::string& path) {
    BuildDocumentInfo();
    g_fr.imageMode = plugInModeRGBColor; g_fr.depth = 8; g_fr.planes = 4;
    int16 r = 0; intptr_t data = 0;
    HANDLE out = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    g_fr.dataFork = (intptr_t)out;
    g_main(formatSelectorWritePrepare, &g_fr, &data, &r);
    g_main(formatSelectorWriteStart, &g_fr, &data, &r);
    g_main(formatSelectorWriteFinish, &g_fr, &data, &r);
    CloseHandle(out);
    CHECK(r == 0, "save result %d", r);
    FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> bytes;
    if (f) { int c; while ((c = std::fgetc(f)) != EOF) bytes.push_back(uint8_t(c)); std::fclose(f); }
    return bytes;
}

static std::vector<std::vector<uint8_t>> Frames(const std::vector<uint8_t>& file) {
    std::vector<std::vector<uint8_t>> fr; vtf::ReadInfo info; std::string err;
    CHECK(vtf::ReadFrames(file.data(), file.size(), fr, info, err), "decode saved file: %s", err.c_str());
    return fr;
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    HMODULE mod = LoadLibraryA(argv[1]);
    g_main = mod ? (PluginMainFn)GetProcAddress(mod, "PluginMain") : nullptr;
    if (!g_main) { std::printf("cannot load plug-in\n"); return 1; }
    const std::string outDir = argv[3];

    // Reference frames straight from the file.
    std::vector<uint8_t> src;
    { FILE* f = std::fopen(argv[2], "rb"); int c; while ((c = std::fgetc(f)) != EOF) src.push_back(uint8_t(c)); std::fclose(f); }
    const auto ref = Frames(src);
    const size_t N = ref.size();
    std::printf("source: %zu frames\n", N);

    for (int order = 0; order < 2; ++order) {
        std::printf("[order %s]\n", order ? "top-down" : "bottom-up");
        RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VTFBridge");
        SetReg("FrameOrder", DWORD(order));
        SetReg("AnimAlpha", 1);  // frames as layers with transparency
        SetReg("ColorFormat", 12); SetReg("AlphaFormat", 12);  // irrelevant: revert decides

        CHECK(Open(argv[2]), "not opened as layers");
        CHECK(g_layers.size() == N, "layer count %zu", g_layers.size());
        for (size_t i = 0; i < g_layers.size(); ++i) {
            const size_t frame = order ? N - 1 - i : i;  // bottom layer = frame 1 (bottom-up)
            CHECK(g_layers[i].rgba == ref[frame], "layer %zu pixels != frame %zu", i, frame);
            const std::string want = "Frame " + std::to_string(frame + 1);
            CHECK(g_layers[i].name == want, "layer %zu name '%s' != '%s'", i, g_layers[i].name.c_str(), want.c_str());
        }
        std::printf("  layers: ");
        for (auto& L : g_layers) std::printf("'%s' ", L.name.c_str());
        std::printf("\n");

        // Change the registry order after opening: the document's own order must win.
        SetReg("FrameOrder", DWORD(1 - order));

        // 1. Re-save as is -> same frames, same order.
        auto saved = Frames(Save(outDir + "\\resave_" + std::to_string(order) + ".vtf"));
        CHECK(saved.size() == N, "resave frame count %zu", saved.size());
        for (size_t f = 0; f < saved.size() && f < N; ++f) CHECK(saved[f] == ref[f], "resave frame %zu differs", f);

        // 2. Hide one layer -> N-1 frames, that frame missing.
        const size_t hide = 1;
        g_layers[hide].visible = false;
        const size_t hiddenFrame = order ? N - 1 - hide : hide;
        saved = Frames(Save(outDir + "\\hidden_" + std::to_string(order) + ".vtf"));
        CHECK(saved.size() == N - 1, "hidden: frame count %zu", saved.size());
        size_t k = 0;
        for (size_t f = 0; f < N; ++f) {
            if (f == hiddenFrame) continue;
            if (k < saved.size()) CHECK(saved[k] == ref[f], "hidden: frame %zu != original %zu", k, f);
            ++k;
        }

        // 3. Only one visible layer -> single frame from the merged image.
        for (auto& L : g_layers) L.visible = false;
        g_layers[0].visible = true;
        saved = Frames(Save(outDir + "\\single_" + std::to_string(order) + ".vtf"));
        CHECK(saved.size() == 1 && saved[0][0] == 77, "single visible layer: %zu frames, first byte %d",
              saved.size(), saved.empty() ? -1 : saved[0][0]);
        std::printf("  resave / hidden layer / single layer checked\n");
    }

    // 4. Document not from an animation (no revertInfo): animate stays off -> merged single frame.
    {
        std::printf("[multi-layer document, not from a VTF]\n");
        RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VTFBridge");
        std::memset(&g_fr, 0, sizeof(g_fr));
        g_fr.HostSupports32BitCoordinates = 1;
        g_fr.handleProcs = &g_hp;
        g_w = 32; g_h = 16;
        g_fr.imageSize32.h = g_w; g_fr.imageSize32.v = g_h;
        g_layers.assign(3, Layer());
        for (size_t i = 0; i < 3; ++i) { g_layers[i].name = "L" + std::to_string(i); g_layers[i].rgba.assign(32 * 16 * 4, uint8_t(50 * i + 10)); }
        auto saved = Frames(Save(outDir + "\\plain_multilayer.vtf"));
        CHECK(saved.size() == 1 && saved[0][0] == 77, "plain multi-layer: %zu frames", saved.size());
        std::printf("  merged single frame checked\n");
    }

    // 5. Alpha source: Auto / transparency / none / a named channel (merged-document path).
    {
        std::printf("[alpha source]\n");
        auto alphaOf = [&](DWORD source, const char* tag) {
            RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VTFBridge");
            SetReg("ColorFormat", 12); SetReg("AlphaFormat", 12);  // BGRA8888: alpha kept exactly
            SetReg("AlphaSource", source);
            std::memset(&g_fr, 0, sizeof(g_fr));
            g_fr.HostSupports32BitCoordinates = 1;
            g_fr.handleProcs = &g_hp;
            g_fr.imageSize32.h = g_w; g_fr.imageSize32.v = g_h;
            auto f = Frames(Save(outDir + "\\alpha_" + tag + ".vtf"));
            return f.empty() ? -1 : int(f[0][3]);
        };
        g_w = 32; g_h = 16;
        g_fr.imageSize32.h = g_w; g_fr.imageSize32.v = g_h;
        g_layers.assign(2, Layer());
        for (auto& L : g_layers) L.rgba.assign(32 * 16 * 4, 120);
        g_alphas.clear();
        CHECK(alphaOf(0, "auto_nochannel") == 77, "auto without channels must use the transparency");
        g_alphas.push_back({"Alpha 1", std::vector<uint8_t>(32 * 16 * 4, 200), {}});
        g_alphas.push_back({"Spec", std::vector<uint8_t>(32 * 16 * 4, 33), {}});
        CHECK(alphaOf(0, "auto") == 200, "auto must use the first alpha channel");
        CHECK(alphaOf(1, "transparency") == 77, "transparency");
        CHECK(alphaOf(2, "none") == 255, "none");
        CHECK(alphaOf(101, "spec") == 33, "channel 'Spec'");
        CHECK(alphaOf(105, "missing") == 200, "missing channel must fall back to auto");
        g_alphas.clear();
        std::printf("  auto / transparency / none / channel / missing channel checked\n");
    }

    // 6. "Alpha as separate layers": opaque colour layer + hidden grey alpha layer per frame;
    //    saving pairs them up again ("Frame N" + "Frame N Alpha").
    {
        std::printf("[alpha as separate layers]\n");
        RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VTFBridge");
        // (no registry value: hidden alpha layers are the default)
        CHECK(Open(argv[2]), "not opened as layers");
        CHECK(g_layers.size() == 2 * N, "layer count %zu (want %zu)", g_layers.size(), 2 * N);
        for (size_t i = 0; i + 1 < g_layers.size(); i += 2) {
            const size_t frame = i / 2;
            const Layer& c = g_layers[i];
            const Layer& a = g_layers[i + 1];
            const std::string want = "Frame " + std::to_string(frame + 1);
            CHECK(c.name == want && c.visible, "layer %zu: '%s' visible %d", i, c.name.c_str(), int(c.visible));
            CHECK(a.name == want + " Alpha" && !a.visible, "layer %zu: '%s' visible %d", i + 1, a.name.c_str(), int(a.visible));
            bool colourOk = true, alphaOk = true;
            for (size_t p = 0; p < ref[frame].size(); p += 4) {
                for (int k = 0; k < 3; ++k) colourOk &= c.rgba[p + k] == ref[frame][p + k];
                colourOk &= c.rgba[p + 3] == 255;
                for (int k = 0; k < 3; ++k) alphaOk &= a.rgba[p + k] == ref[frame][p + 3];
            }
            CHECK(colourOk, "frame %zu: colour layer not the opaque frame colour", frame);
            CHECK(alphaOk, "frame %zu: alpha layer not the frame alpha", frame);
        }
        std::printf("  layers: ");
        for (auto& L : g_layers) std::printf("'%s'%s ", L.name.c_str(), L.visible ? "" : "(hidden)");
        std::printf("\n");

        auto saved = Frames(Save(outDir + "\\alphalayers_resave.vtf"));
        CHECK(saved.size() == N, "alpha layers resave: %zu frames", saved.size());
        for (size_t f = 0; f < saved.size() && f < N; ++f) CHECK(saved[f] == ref[f], "alpha layers resave: frame %zu differs", f);

        // Edit: frame 1's alpha layer painted grey 10, and shown -> still not a frame.
        for (size_t p = 0; p < g_layers[1].rgba.size(); p += 4) g_layers[1].rgba[p] = g_layers[1].rgba[p + 1] = g_layers[1].rgba[p + 2] = 10;
        g_layers[1].visible = true;
        saved = Frames(Save(outDir + "\\alphalayers_edit.vtf"));
        CHECK(saved.size() == N, "edited alpha layer: %zu frames", saved.size());
        if (!saved.empty()) CHECK(saved[0][3] == 10 && saved[0][0] == ref[0][0], "edited alpha: a=%d", saved[0][3]);
        std::printf("  open / resave / edited alpha layer checked\n");
        RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\VTFBridge");
    }

    std::printf("failures: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
