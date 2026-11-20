// preview_shot.cpp - opens the options dialog with a synthetic 512x256 document
// (channel ports), then presses "Preview..." for screenshots of the preview window.
// Usage: preview_shot VTFBridge.8bi <language code> [steps...]
#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include "PIFormat.h"
#include "../src/resource.h"
typedef void (*PluginMainFn)(int16, FormatRecordPtr, intptr_t*, int16*);
static const int W = 512, H = 256;
static std::vector<uint8_t> g_px(W * H * 4);
// Port = layer * 16 + channel + 1; layer k (animation frame) is shifted k * 40 pixels.
static OSErr ReadPixels(ChannelReadPort port, const PSScaling*, const VRect* r, const PixelMemoryDesc* d, VRect* wrote) {
    const int v = int(intptr_t(port)) - 1, c = v % 16, layer = v / 16;
    for (int y = r->top; y < r->bottom; ++y)
        for (int x = r->left; x < r->right; ++x)
            ((uint8_t*)d->data)[size_t(y) * (d->rowBits / 8) + x] = g_px[(size_t(y) * W + (x + layer * 40) % W) * 4 + c];
    *wrote = *r;
    return 0;
}
// Steps after the language code, applied in order: L=level Z=zoom item V=view M=mode
// R=reference item Q=quality T=1 (tile) A=1 (auto level) W=wheel steps (at the centre)
// S=1 (hold Space) F=path (own reference image through the file dialog) P=frame.
// "N=<layers>" anywhere: a layered document (animation frames, "layers -> animation" on).
static int g_argc; static char** g_argv;
static DWORD WINAPI Clicker(void*) {
    HWND opt = nullptr;
    for (int i = 0; i < 100 && !opt; ++i) { Sleep(100); opt = FindWindowW(L"#32770", nullptr); }
    Sleep(500);
    for (int a = 3; a < g_argc; ++a)
        if (g_argv[a][0] == 'N' && g_argv[a][1] == '=') {
            CheckDlgButton(opt, IDC_ANIMATE, BST_CHECKED);
            SendMessageW(opt, WM_COMMAND, MAKEWPARAM(IDC_ANIMATE, BN_CLICKED), (LPARAM)GetDlgItem(opt, IDC_ANIMATE));
        }
    // Auto DXT1 / DXT5 and High quality... (format item 0), then open the preview.
    SendDlgItemMessageW(opt, IDC_FORMAT, CB_SETCURSEL, 0, 0);
    SendMessageW(opt, WM_COMMAND, MAKEWPARAM(IDC_FORMAT, CBN_SELCHANGE), (LPARAM)GetDlgItem(opt, IDC_FORMAT));
    Sleep(300);
    PostMessageW(opt, WM_COMMAND, MAKEWPARAM(IDC_PREVIEW, BN_CLICKED), 0);
    HWND pv = nullptr;
    for (int i = 0; i < 100; ++i) {
        Sleep(100);
        HWND w = FindWindowW(L"#32770", nullptr);
        if (w && w != opt) { pv = w; break; }
    }
    if (!pv) return 0;
    SetWindowPos(opt, nullptr, -4000, -4000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);  // only the preview on screenshots
    Sleep(2500);
    auto sel = [&](int id, int v) {
        SendDlgItemMessageW(pv, id, CB_SETCURSEL, v, 0);
        PostMessageW(pv, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), (LPARAM)GetDlgItem(pv, id));
    };
    auto click = [&](int id) {
        CheckDlgButton(pv, id, BST_CHECKED);
        PostMessageW(pv, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(pv, id));
    };
    for (int a = 3; a < g_argc; ++a) {
        const char* s = g_argv[a];
        if (std::strlen(s) < 3 || s[1] != '=') continue;
        const int v = atoi(s + 2);
        switch (s[0]) {
        case 'L': sel(IDC_PV_LEVEL, v); break;
        case 'Z': sel(IDC_PV_ZOOM, v); break;
        case 'V': sel(IDC_PV_VIEW, v); break;
        case 'M': sel(IDC_PV_MODE, v); break;
        case 'R': sel(IDC_PV_REF, v); break;
        case 'Q': sel(IDC_PV_QUALITY, v); break;
        case 'P': sel(IDC_PV_FRAME, v); break;
        case 'T': click(IDC_PV_TILE); break;
        case 'A': click(IDC_PV_AUTO); break;
        case 'S': PostMessageW(pv, WM_APP + 42, 1, 0); break;
        case 'W': {
            RECT r; GetWindowRect(GetDlgItem(pv, IDC_PV_AREA), &r);
            PostMessageW(pv, WM_APP + 43, MAKEWPARAM(0, short(v * WHEEL_DELTA)), MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
            break;
        }
        case 'F': {
            const int browse = int(SendDlgItemMessageW(pv, IDC_PV_REF, CB_GETCOUNT, 0, 0)) - 1;
            sel(IDC_PV_REF, browse);
            HWND fd = nullptr;
            for (int i = 0; i < 100 && !fd; ++i) {
                Sleep(100);
                HWND w = FindWindowW(L"#32770", nullptr);
                if (w && w != opt && w != pv) fd = w;
            }
            if (fd) {
                Sleep(1500);
                HWND edit = GetDlgItem(fd, 1148);  // cmb13: file name
                if (!edit) edit = GetDlgItem(fd, 1152);  // edt1
                SetWindowTextA(edit, s + 2);
                SendMessageW(fd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(fd, IDOK));
            }
            break;
        }
        }
        Sleep(700);
    }
    return 0;
}
int main(int argc, char** argv) {
    g_argc = argc; g_argv = argv;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &g_px[(size_t(y) * W + x) * 4];
            const bool text = ((x / 6) % 5 == 0 && y % 40 < 30) || (y % 40 == 15 && x % 120 < 100);  // thin lines
            p[0] = text ? 250 : uint8_t(40 + 150 * x / W);
            p[1] = text ? 240 : uint8_t(60 + 100 * std::sin(y * 0.05));
            p[2] = text ? 120 : uint8_t(160 - 120 * y / H);
            p[3] = uint8_t(x < W - 64 ? 255 : 255 - (x - (W - 64)) * 4);
        }
    HKEY key; RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VTFBridge", 0, 0, 0, KEY_WRITE, 0, &key, 0);
    RegSetValueExA(key, "Language", 0, REG_SZ, (BYTE*)argv[2], DWORD(strlen(argv[2]) + 1));
    RegCloseKey(key);
    PluginMainFn main = (PluginMainFn)GetProcAddress(LoadLibraryA(argv[1]), "PluginMain");
    static ReadChannelDesc ch[4]; static ReadImageDocumentDesc doc; static ChannelPortProcs procs = {1, 1, ReadPixels, 0, 0};
    memset(ch, 0, sizeof(ch)); memset(&doc, 0, sizeof(doc));
    for (int c = 0; c < 4; ++c) { ch[c].port = (ChannelReadPort)(intptr_t)(c + 1); ch[c].depth = 8; ch[c].next = c < 2 ? &ch[c + 1] : nullptr; }
    doc.mergedCompositeChannels = &ch[0];
    doc.mergedTransparency = &ch[3];
    doc.layerCount = 1;
    static ReadChannelDesc lch[16][4]; static ReadLayerDesc layers[16]; static uint16 lname[16][16];
    int nLayers = 0;
    for (int a = 3; a < argc; ++a) if (argv[a][0] == 'N' && argv[a][1] == '=') nLayers = std::min(16, atoi(argv[a] + 2));
    if (nLayers > 1) {
        memset(lch, 0, sizeof(lch)); memset(layers, 0, sizeof(layers));
        for (int l = 0; l < nLayers; ++l) {
            for (int c = 0; c < 4; ++c) {
                lch[l][c].port = (ChannelReadPort)(intptr_t)(l * 16 + c + 1); lch[l][c].depth = 8;
                lch[l][c].next = c < 2 ? &lch[l][c + 1] : nullptr;
            }
            layers[l].compositeChannelsList = &lch[l][0];
            layers[l].transparency = &lch[l][3];
            const char* nm = "Frame "; int k = 0;
            for (; nm[k]; ++k) lname[l][k] = uint16(nm[k]);
            lname[l][k++] = uint16('1' + l); lname[l][k] = 0;
            layers[l].unicodeName = lname[l];
            layers[l].isVisible = 1; layers[l].isPixelBased = 1;
            layers[l].opacity = layers[l].fillOpacity = 255;
            layers[l].next = l + 1 < nLayers ? &layers[l + 1] : nullptr;
        }
        doc.layerCount = nLayers;
        doc.layersDescriptor = &layers[0];
    }
    FormatRecord fr; memset(&fr, 0, sizeof(fr));
    fr.HostSupports32BitCoordinates = 1; fr.imageMode = plugInModeRGBColor; fr.depth = 8; fr.planes = 4;
    fr.imageSize32.h = W; fr.imageSize32.v = H; fr.documentInfo = &doc; fr.channelPortProcs = &procs;
    CreateThread(nullptr, 0, Clicker, nullptr, 0, nullptr);
    int16 r = 0; intptr_t d = 0;
    main(formatSelectorOptionsStart, &fr, &d, &r);
    return 0;
}
