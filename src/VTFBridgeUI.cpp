// VTFBridgeUI.cpp - Win32 options dialog, settings persistence, localisation glue.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Themed (common controls v6) look for the dialog inside Photoshop.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "Lang.h"
#include "Resample.h"
#include "VTFBridge.h"
#include "Version.h"
#include "Vmt.h"
#include "resource.h"

void FitMainLabels(HWND dlg);

namespace {

const wchar_t* kRegKey = L"Software\\VTFBridge";
// Settings key of versions before the rename (VTF Export 1.0).
const wchar_t* kLegacyRegKey = L"Software\\VTFExport";

// Moves the settings of VTF Export 1.0 to the new key once, so nothing is reset
// after updating. Does nothing when the new key already exists.
void MigrateLegacySettings() {
    static bool done = false;
    if (done) return;
    done = true;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    HKEY legacy;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyRegKey, 0, KEY_READ, &legacy) != ERROR_SUCCESS) return;
    bool copied = false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        copied = RegCopyTreeW(legacy, nullptr, key) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    RegCloseKey(legacy);
    if (copied) RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyRegKey);
}

// One list: two "Auto" entries (format picked by whether the image has alpha)
// and concrete formats (same format either way).
// label: literal format name, or a localisation key when it starts with "fmt.".
struct FormatItem { int32_t color, alpha; const char* label; };
const FormatItem kFormats[] = {
    {vtf::FMT_DXT1,     vtf::FMT_DXT5,     "fmt.auto_dxt"},
    {vtf::FMT_BGR888,   vtf::FMT_BGRA8888, "fmt.auto_raw"},
    {vtf::FMT_DXT1,     vtf::FMT_DXT1,     "DXT1"},
    {vtf::FMT_DXT3,     vtf::FMT_DXT3,     "DXT3"},
    {vtf::FMT_DXT5,     vtf::FMT_DXT5,     "DXT5"},
    {vtf::FMT_BGR888,   vtf::FMT_BGR888,   "BGR888"},
    {vtf::FMT_BGRA8888, vtf::FMT_BGRA8888, "BGRA8888"},
    {vtf::FMT_RGB888,   vtf::FMT_RGB888,   "RGB888"},
    {vtf::FMT_RGBA8888, vtf::FMT_RGBA8888, "RGBA8888"},
    {vtf::FMT_BGR565,   vtf::FMT_BGR565,   "BGR565"},
    {vtf::FMT_BGRA4444, vtf::FMT_BGRA4444, "BGRA4444"},
    {vtf::FMT_BGRA5551, vtf::FMT_BGRA5551, "BGRA5551"},
    {vtf::FMT_I8,       vtf::FMT_I8,       "fmt.i8"},
    {vtf::FMT_IA88,     vtf::FMT_IA88,     "fmt.ia88"},
    {vtf::FMT_A8,       vtf::FMT_A8,       "fmt.a8"},
};
constexpr int kFormatCount = int(sizeof(kFormats) / sizeof(kFormats[0]));

const char* kFrameOrders[] = {"order.bottom_up", "order.top_down"};

// Max size list for the resize limits (0 = no limit).
const uint32_t kMaxSizes[] = {0, 16384, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32, 16};
constexpr int kMaxSizeCount = int(sizeof(kMaxSizes) / sizeof(kMaxSizes[0]));

const char* kResizeMethods[] = {"resize.nearest", "resize.biggest", "resize.smallest"};

struct FlagItem { uint32_t flag; const char* key; };
// Order must match the checkbox IDs in VTFBridge.rc (IDC_FLAG_FIRST + index).
const FlagItem kFlags[] = {
    {vtf::FLAG_POINTSAMPLE,     "flag.point_sample"},
    {vtf::FLAG_TRILINEAR,       "flag.trilinear"},
    {vtf::FLAG_ANISOTROPIC,     "flag.anisotropic"},
    {vtf::FLAG_CLAMPS,          "flag.clamp_s"},
    {vtf::FLAG_CLAMPT,          "flag.clamp_t"},
    {vtf::FLAG_CLAMPU,          "flag.clamp_u"},
    {vtf::FLAG_NOMIP,           "flag.no_mipmap"},
    {vtf::FLAG_NOLOD,           "flag.no_lod"},
    {vtf::FLAG_NORMAL,          "flag.normal_map"},
    {vtf::FLAG_SSBUMP,          "flag.ssbump"},
    {vtf::FLAG_HINT_DXT5,       "flag.hint_dxt5"},
    {vtf::FLAG_SRGB,            "flag.srgb"},
    {vtf::FLAG_NODEBUGOVERRIDE, "flag.no_debug_override"},
    {vtf::FLAG_SINGLECOPY,      "flag.single_copy"},
    {vtf::FLAG_VERTEXTEXTURE,   "flag.vertex_texture"},
    {vtf::FLAG_BORDER,          "flag.border"},
};
constexpr int kFlagCount = int(sizeof(kFlags) / sizeof(kFlags[0]));

struct DialogState {
    vtf::Options* opt;
    int width, height;
    int visibleLayers;        // frames available for "layers -> animation"
    const AlphaInfo* alpha;   // alpha channels of the document
    const FetchPixelsFn* fetch;  // document pixels for the preview
    std::wstring langBefore;  // restored on Cancel
    HWND tooltip = nullptr;
    std::vector<std::wstring> tipTexts;  // must outlive the tooltip tools
};

// Tooltip key of a flag: "flag.clamp_s" -> "tip.clamp_s".
std::string FlagTipKey(const char* flagKey) { return std::string("tip.") + (flagKey + 5); }

int ComboSel(HWND dlg, int id) { return int(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }

// Alpha list: 0 Auto, 1 Transparency, 2 None, 3 + n = alpha channel n.
int AlphaSourceToIndex(int source, const AlphaInfo& a) {
    if (source >= vtf::ALPHA_SOURCE_CHANNEL_FIRST) {
        const int n = source - vtf::ALPHA_SOURCE_CHANNEL_FIRST;
        return n < int(a.channels.size()) ? 3 + n : 0;
    }
    return source >= 0 && source <= 2 ? source : 0;
}
int AlphaIndexToSource(int index) {
    return index >= 3 ? vtf::ALPHA_SOURCE_CHANNEL_FIRST + (index - 3) : (index >= 0 ? index : 0);
}
const char* kDxtQualities[] = {"dxtq.fast", "dxtq.high", "dxtq.best", "dxtq.max", "dxtq.perceptual"};
bool IsDxt(int32_t f) { return f == vtf::FMT_DXT1 || f == vtf::FMT_DXT3 || f == vtf::FMT_DXT5; }

void FillCombo(HWND dlg, int id, const std::vector<std::wstring>& items) {
    HWND c = GetDlgItem(dlg, id);
    const LRESULT sel = SendMessageW(c, CB_GETCURSEL, 0, 0);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (const auto& it : items) SendMessageW(c, CB_ADDSTRING, 0, LPARAM(it.c_str()));
    SendMessageW(c, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
}

std::wstring FormatLabel(const FormatItem& f) {
    if (std::strncmp(f.label, "fmt.", 4) == 0) return lang::T(f.label);
    return std::wstring(f.label, f.label + std::strlen(f.label));
}

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD def) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value;
    return def;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

std::wstring ReadLanguageSetting() {
    MigrateLegacySettings();
    wchar_t buf[64] = {0};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Language", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"auto";
}

void WriteLanguageSetting(const std::wstring& code) {
    MigrateLegacySettings();
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE*>(code.c_str()),
                   DWORD((code.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// --- dialog -------------------------------------------------------------------

// Fills the language combo: item 0 = Auto, then every available language.
// Item data: -1 for Auto, otherwise index into lang::Available().
void FillLanguages(HWND dlg) {
    HWND combo = GetDlgItem(dlg, IDC_LANG);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const auto& list = lang::Available();
    std::wstring autoText = lang::T("dlg.language_auto");
    for (const auto& l : list)
        if (l.code == lang::Active()) { autoText += L" (" + l.name + L")"; break; }
    SendMessageW(combo, CB_SETITEMDATA, SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(autoText.c_str())), -1);
    int sel = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const LRESULT idx = SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(list[i].name.c_str()));
        SendMessageW(combo, CB_SETITEMDATA, idx, LPARAM(i));
        if (lang::Setting() == list[i].code) sel = int(idx);
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void Collect(HWND dlg, vtf::Options& opt);

// "171 KB", "1.4 MB" (units and decimal separator from the language file).
std::wstring FormatBytes(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) {
        std::swprintf(buf, 64, L"%llu", (unsigned long long)b);
        return std::wstring(buf) + L" " + lang::T("unit.b");
    }
    if (b < 1024 * 1024) {
        std::swprintf(buf, 64, L"%llu", (unsigned long long)((b + 512) / 1024));
        return std::wstring(buf) + L" " + lang::T("unit.kb");
    }
    std::swprintf(buf, 64, L"%.1f", double(b) / (1024.0 * 1024.0));
    std::wstring n = buf;
    const std::wstring sep = lang::T("unit.decimal");
    const size_t dot = n.find(L'.');
    if (dot != std::wstring::npos && !sep.empty()) n.replace(dot, 1, sep);
    return n + L" " + lang::T("unit.mb");
}

void UpdateInfo(HWND dlg, const DialogState* st) {
    vtf::Options tmp = *st->opt;
    Collect(dlg, tmp);
    uint32_t tw, th;
    vtf::TargetSize(uint32_t(st->width), uint32_t(st->height), tmp, tw, th);

    // Estimated file size. Whether the image has alpha is not known yet, so the
    // "Auto" formats show the range between their two formats.
    const uint32_t frames = (tmp.animate && st->visibleLayers >= 2) ? uint32_t(st->visibleLayers) : 1u;
    if (tw <= 65535 && th <= 65535) {
        const uint64_t a = vtf::FileSize(tw, th, tmp.colorFormat, frames, tmp);
        const uint64_t b = vtf::FileSize(tw, th, tmp.alphaFormat, frames, tmp);
        const std::wstring size = a == b ? FormatBytes(a)
                                         : FormatBytes(std::min(a, b)) + L" – " + FormatBytes(std::max(a, b));
        SetDlgItemTextW(dlg, IDC_FILESIZE, lang::Tf("dlg.file_size", {{"size", size}}).c_str());
    } else {
        SetDlgItemTextW(dlg, IDC_FILESIZE, L"");
    }

    std::wstring text;
    if (tw == uint32_t(st->width) && th == uint32_t(st->height))
        text = lang::Tf("dlg.size", {{"w", std::to_wstring(st->width)}, {"h", std::to_wstring(st->height)}});
    else
        text = lang::Tf("dlg.size_resized", {{"w", std::to_wstring(st->width)}, {"h", std::to_wstring(st->height)},
                                             {"tw", std::to_wstring(tw)}, {"th", std::to_wstring(th)}});
    SetDlgItemTextW(dlg, IDC_INFO, text.c_str());
}

void UpdateEnabled(HWND dlg, const DialogState* st) {
    const bool canAnimate = st->visibleLayers >= 2;
    EnableWindow(GetDlgItem(dlg, IDC_ANIMATE), canAnimate);
    if (!canAnimate) CheckDlgButton(dlg, IDC_ANIMATE, BST_UNCHECKED);
    const bool animate = IsDlgButtonChecked(dlg, IDC_ANIMATE) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_ORDER), animate);
    EnableWindow(GetDlgItem(dlg, IDC_LBL_ORDER), animate);
    EnableWindow(GetDlgItem(dlg, IDC_ANIM_HINT), animate);

    const bool resize = IsDlgButtonChecked(dlg, IDC_RESIZE) == BST_CHECKED;
    const bool mips = IsDlgButtonChecked(dlg, IDC_MIPMAPS) == BST_CHECKED;
    for (int id : {IDC_METHOD, IDC_RFILTER, IDC_MAXW, IDC_MAXH, IDC_LBL_METHOD, IDC_LBL_RFILTER, IDC_LBL_MAXW, IDC_LBL_MAXH})
        EnableWindow(GetDlgItem(dlg, id), resize);
    EnableWindow(GetDlgItem(dlg, IDC_MIPFILTER), mips);
    for (int id : {IDC_SHARPEN, IDC_SHARPEN_VAL, IDC_LBL_SHARPEN, IDC_SHARPEN_ALPHA}) EnableWindow(GetDlgItem(dlg, id), mips);
    EnableWindow(GetDlgItem(dlg, IDC_LBL_MIPFILTER), mips);
    EnableWindow(GetDlgItem(dlg, IDC_GAMMA), mips || resize);
    EnableWindow(GetDlgItem(dlg, IDC_TILING), mips || resize);

    // DXT quality only matters for DXT formats.
    const int fi = ComboSel(dlg, IDC_FORMAT);
    const bool dxt = fi >= 0 && fi < kFormatCount && (IsDxt(kFormats[fi].color) || IsDxt(kFormats[fi].alpha));
    EnableWindow(GetDlgItem(dlg, IDC_DXTQ), dxt);
    EnableWindow(GetDlgItem(dlg, IDC_LBL_DXTQ), dxt);

    // A normal map / SSBump is not a material of its own: no .vmt for it.
    bool bump = false;
    for (int i = 0; i < kFlagCount; ++i)
        if ((kFlags[i].flag & (vtf::FLAG_NORMAL | vtf::FLAG_SSBUMP)) && IsDlgButtonChecked(dlg, IDC_FLAG_FIRST + i) == BST_CHECKED)
            bump = true;
    EnableWindow(GetDlgItem(dlg, IDC_VMT), !bump);
    EnableWindow(GetDlgItem(dlg, IDC_SHADER), !bump && IsDlgButtonChecked(dlg, IDC_VMT) == BST_CHECKED);
}

// Tooltips for the flags and the material options (texts follow the language).
void AddTip(DialogState* st, HWND dlg, int id, const std::wstring& text) {
    st->tipTexts.push_back(text);
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = dlg;
    ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dlg, id));
    ti.lpszText = const_cast<wchar_t*>(st->tipTexts.back().c_str());
    SendMessageW(st->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void UpdateTips(DialogState* st, HWND dlg) {
    if (!st->tooltip) {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        st->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      dlg, nullptr, g_hInstance, nullptr);
        if (!st->tooltip) return;
        SendMessageW(st->tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(st->tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    } else {
        // Remove the old tools, then add them again with the new texts.
        for (int i = 0; i < kFlagCount; ++i) {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.hwnd = dlg;
            ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dlg, IDC_FLAG_FIRST + i));
            SendMessageW(st->tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        for (int id : {IDC_VMT, IDC_SHADER, IDC_TILING, IDC_DXTQ, IDC_ALPHA, IDC_SHARPEN, IDC_SHARPEN_ALPHA, IDC_PREVIEW}) {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.hwnd = dlg;
            ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dlg, id));
            SendMessageW(st->tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
    st->tipTexts.clear();
    st->tipTexts.reserve(kFlagCount + 8);  // no reallocation: tools point into the strings
    for (int i = 0; i < kFlagCount; ++i) AddTip(st, dlg, IDC_FLAG_FIRST + i, lang::T(FlagTipKey(kFlags[i].key).c_str()));
    AddTip(st, dlg, IDC_VMT, lang::T("tip.vmt"));
    AddTip(st, dlg, IDC_TILING, lang::T("tip.tiling"));
    AddTip(st, dlg, IDC_DXTQ, lang::T("tip.dxt_quality"));
    AddTip(st, dlg, IDC_ALPHA, lang::T("tip.alpha"));
    AddTip(st, dlg, IDC_SHARPEN, lang::T("tip.sharpen"));
    AddTip(st, dlg, IDC_SHARPEN_ALPHA, lang::T("tip.sharpen_alpha"));
    AddTip(st, dlg, IDC_PREVIEW, lang::T("tip.preview"));
    AddTip(st, dlg, IDC_SHADER, lang::T("tip.shader"));
}

void ApplyTexts(HWND dlg, DialogState* st) {
    SetWindowTextW(dlg, lang::T("dlg.title").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_LANG, lang::T("dlg.language").c_str());
    SetDlgItemTextW(dlg, IDC_GRP_FORMAT, lang::T("dlg.group_format").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_FORMAT, lang::T("dlg.format").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_VERSION, lang::T("dlg.version").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_DXTQ, lang::T("dlg.dxt_quality").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_ALPHA, lang::T("dlg.alpha").c_str());
    SetDlgItemTextW(dlg, IDC_GRP_RESIZE, lang::T("dlg.group_resize").c_str());
    SetDlgItemTextW(dlg, IDC_RESIZE, lang::T("dlg.resize").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_METHOD, lang::T("dlg.resize_method").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_RFILTER, lang::T("dlg.resize_filter").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_MAXW, lang::T("dlg.max_width").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_MAXH, lang::T("dlg.max_height").c_str());
    SetDlgItemTextW(dlg, IDC_GRP_MIPS, lang::T("dlg.group_mips").c_str());
    SetDlgItemTextW(dlg, IDC_MIPMAPS, lang::T("dlg.mipmaps").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_MIPFILTER, lang::T("dlg.mip_filter").c_str());
    SetDlgItemTextW(dlg, IDC_GAMMA, lang::T("dlg.gamma").c_str());
    SetDlgItemTextW(dlg, IDC_LBL_SHARPEN, lang::T("dlg.sharpen").c_str());
    SetDlgItemTextW(dlg, IDC_SHARPEN_ALPHA, lang::T("dlg.sharpen_alpha").c_str());
    SetDlgItemTextW(dlg, IDC_PREVIEW, lang::T("dlg.preview").c_str());
    SetDlgItemTextW(dlg, IDC_TILING, lang::T("dlg.tiling").c_str());
    SetDlgItemTextW(dlg, IDC_THUMB, lang::T("dlg.thumb").c_str());
    SetDlgItemTextW(dlg, IDC_GRP_FLAGS, lang::T("dlg.group_flags").c_str());
    SetDlgItemTextW(dlg, IDC_GRP_ANIM, lang::T("dlg.group_anim").c_str());
    SetDlgItemTextW(dlg, IDC_ANIMATE, lang::Tf("dlg.animate", {{"n", std::to_wstring(st->visibleLayers)}}).c_str());
    SetDlgItemTextW(dlg, IDC_LBL_ORDER, lang::T("dlg.frame_order").c_str());
    SetDlgItemTextW(dlg, IDC_ANIM_HINT, lang::T("dlg.anim_hint").c_str());
    SetDlgItemTextW(dlg, IDC_RESET, lang::T("dlg.reset").c_str());
    SetDlgItemTextW(dlg, IDOK, lang::T("dlg.save").c_str());
    SetDlgItemTextW(dlg, IDCANCEL, lang::T("dlg.cancel").c_str());
    for (int i = 0; i < kFlagCount; ++i) SetDlgItemTextW(dlg, IDC_FLAG_FIRST + i, lang::T(kFlags[i].key).c_str());
    SetDlgItemTextW(dlg, IDC_GRP_VMT, lang::T("dlg.group_vmt").c_str());
    SetDlgItemTextW(dlg, IDC_VMT, lang::T("dlg.vmt").c_str());
    UpdateTips(st, dlg);
    FitMainLabels(dlg);

    // Lists (selection is kept by FillCombo).
    std::vector<std::wstring> formats, filters, methods, sizes, orders, shaders;
    for (int i = 0; i < vmt::SHADER_COUNT; ++i) {
        const char* n = vmt::ShaderName(i);
        shaders.push_back(std::wstring(n, n + std::strlen(n)));
    }
    for (const FormatItem& f : kFormats) formats.push_back(FormatLabel(f));
    for (int f = 0; f < vtf::FILTER_COUNT; ++f) {
        const char* n = vtf::FilterName(f);
        filters.push_back(std::wstring(n, n + std::strlen(n)));
    }
    for (const char* m : kResizeMethods) methods.push_back(lang::T(m));
    for (const char* o : kFrameOrders) orders.push_back(lang::T(o));
    for (uint32_t v : kMaxSizes) sizes.push_back(v ? std::to_wstring(v) : lang::T("dlg.no_limit"));
    std::vector<std::wstring> qualities, alphas;
    for (const char* q : kDxtQualities) qualities.push_back(lang::T(q));
    const AlphaInfo& ai = *st->alpha;
    if (ai.autoSource >= vtf::ALPHA_SOURCE_CHANNEL_FIRST && !ai.channels.empty())
        alphas.push_back(lang::Tf("alpha.auto_channel", {{"name", ai.channels[0]}}));
    else if (ai.autoSource == vtf::ALPHA_SOURCE_TRANSPARENCY)
        alphas.push_back(lang::T("alpha.auto_transparency"));
    else
        alphas.push_back(lang::T("alpha.auto_none"));
    alphas.push_back(lang::T("alpha.transparency"));
    alphas.push_back(lang::T("alpha.none"));
    for (const std::wstring& c : ai.channels) alphas.push_back(lang::Tf("alpha.channel", {{"name", c}}));
    FillCombo(dlg, IDC_DXTQ, qualities);
    FillCombo(dlg, IDC_ALPHA, alphas);
    FillCombo(dlg, IDC_FORMAT, formats);
    FillCombo(dlg, IDC_ORDER, orders);
    FillCombo(dlg, IDC_SHADER, shaders);
    FillCombo(dlg, IDC_RFILTER, filters);
    FillCombo(dlg, IDC_MIPFILTER, filters);
    FillCombo(dlg, IDC_METHOD, methods);
    FillCombo(dlg, IDC_MAXW, sizes);
    FillCombo(dlg, IDC_MAXH, sizes);
    UpdateInfo(dlg, st);
}

// List entry for a colour/alpha format pair (Auto entries first, so DXT1/DXT5
// shows as "Auto"). Unknown combinations fall back to Auto DXT.
int FormatIndex(const vtf::Options& o) {
    for (int i = 0; i < kFormatCount; ++i)
        if (kFormats[i].color == o.colorFormat && kFormats[i].alpha == o.alphaFormat) return i;
    return 0;
}

bool KnownFormat(int32_t f) {
    for (const FormatItem& it : kFormats)
        if (it.color == f || it.alpha == f) return true;
    return false;
}

int MaxSizeIndex(uint32_t v) {
    for (int i = 0; i < kMaxSizeCount; ++i)
        if (kMaxSizes[i] == v) return i;
    return 0;
}

void Fill(HWND dlg, const DialogState* st, const vtf::Options& opt) {
    SendDlgItemMessageW(dlg, IDC_FORMAT, CB_SETCURSEL, FormatIndex(opt), 0);
    CheckDlgButton(dlg, IDC_ANIMATE, opt.animate ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_ORDER, CB_SETCURSEL, opt.frameOrder, 0);
    SendDlgItemMessageW(dlg, IDC_VERSION, CB_SETCURSEL, opt.versionMinor - 1, 0);
    SendDlgItemMessageW(dlg, IDC_DXTQ, CB_SETCURSEL, opt.dxtQuality, 0);
    SendDlgItemMessageW(dlg, IDC_ALPHA, CB_SETCURSEL, AlphaSourceToIndex(opt.alphaSource, *st->alpha), 0);
    CheckDlgButton(dlg, IDC_RESIZE, opt.resize ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_METHOD, CB_SETCURSEL, opt.resizeMethod, 0);
    SendDlgItemMessageW(dlg, IDC_RFILTER, CB_SETCURSEL, opt.resizeFilter, 0);
    SendDlgItemMessageW(dlg, IDC_MAXW, CB_SETCURSEL, MaxSizeIndex(opt.maxWidth), 0);
    SendDlgItemMessageW(dlg, IDC_MAXH, CB_SETCURSEL, MaxSizeIndex(opt.maxHeight), 0);
    CheckDlgButton(dlg, IDC_MIPMAPS, opt.mipmaps ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_MIPFILTER, CB_SETCURSEL, opt.mipFilter, 0);
    CheckDlgButton(dlg, IDC_GAMMA, opt.gammaFilter ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETPOS, TRUE, opt.mipSharpen);
    SetDlgItemTextW(dlg, IDC_SHARPEN_VAL, (std::to_wstring(opt.mipSharpen) + L"%").c_str());
    CheckDlgButton(dlg, IDC_SHARPEN_ALPHA, opt.sharpenAlpha ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TILING, opt.tiling ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_THUMB, opt.thumbnail ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < kFlagCount; ++i)
        CheckDlgButton(dlg, IDC_FLAG_FIRST + i, (opt.flags & kFlags[i].flag) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_VMT, opt.vmt ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_SHADER, CB_SETCURSEL, opt.vmtShader, 0);
    UpdateEnabled(dlg, st);
}

void Collect(HWND dlg, vtf::Options& opt) {
    int i = ComboSel(dlg, IDC_FORMAT);
    if (i >= 0 && i < kFormatCount) { opt.colorFormat = kFormats[i].color; opt.alphaFormat = kFormats[i].alpha; }
    opt.animate = IsDlgButtonChecked(dlg, IDC_ANIMATE) == BST_CHECKED;
    i = ComboSel(dlg, IDC_ORDER);
    if (i >= 0 && i < 2) opt.frameOrder = i;
    i = ComboSel(dlg, IDC_VERSION);
    opt.versionMinor = (i >= 0 && i < 5) ? i + 1 : 2;
    i = ComboSel(dlg, IDC_DXTQ);
    if (i >= 0 && i < vtf::DXT_QUALITY_COUNT) opt.dxtQuality = i;
    i = ComboSel(dlg, IDC_ALPHA);
    if (i >= 0) opt.alphaSource = AlphaIndexToSource(i);

    opt.resize = IsDlgButtonChecked(dlg, IDC_RESIZE) == BST_CHECKED;
    i = ComboSel(dlg, IDC_METHOD);
    if (i >= 0 && i < 3) opt.resizeMethod = i;
    i = ComboSel(dlg, IDC_RFILTER);
    if (i >= 0 && i < vtf::FILTER_COUNT) opt.resizeFilter = i;
    i = ComboSel(dlg, IDC_MAXW);
    if (i >= 0 && i < kMaxSizeCount) opt.maxWidth = kMaxSizes[i];
    i = ComboSel(dlg, IDC_MAXH);
    if (i >= 0 && i < kMaxSizeCount) opt.maxHeight = kMaxSizes[i];

    opt.mipmaps = IsDlgButtonChecked(dlg, IDC_MIPMAPS) == BST_CHECKED;
    i = ComboSel(dlg, IDC_MIPFILTER);
    if (i >= 0 && i < vtf::FILTER_COUNT) opt.mipFilter = i;
    opt.gammaFilter = IsDlgButtonChecked(dlg, IDC_GAMMA) == BST_CHECKED;
    opt.mipSharpen = int(SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_GETPOS, 0, 0));
    opt.sharpenAlpha = IsDlgButtonChecked(dlg, IDC_SHARPEN_ALPHA) == BST_CHECKED;
    opt.tiling = IsDlgButtonChecked(dlg, IDC_TILING) == BST_CHECKED;
    opt.thumbnail = IsDlgButtonChecked(dlg, IDC_THUMB) == BST_CHECKED;
    opt.flags = 0;
    for (int k = 0; k < kFlagCount; ++k)
        if (IsDlgButtonChecked(dlg, IDC_FLAG_FIRST + k) == BST_CHECKED) opt.flags |= kFlags[k].flag;
    opt.vmt = IsDlgButtonChecked(dlg, IDC_VMT) == BST_CHECKED;
    i = ComboSel(dlg, IDC_SHADER);
    if (i >= 0 && i < vmt::SHADER_COUNT) opt.vmtShader = i;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        ForgetLabelLayout(dlg);
        break;
    case WM_INITDIALOG: {
        DialogState* st = reinterpret_cast<DialogState*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);

        HWND ver = GetDlgItem(dlg, IDC_VERSION);
        for (const wchar_t* v : {L"7.1", L"7.2", L"7.3", L"7.4", L"7.5"})
            SendMessageW(ver, CB_ADDSTRING, 0, LPARAM(v));

        SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETPAGESIZE, 0, 10);
        FillLanguages(dlg);
        ApplyTexts(dlg, st);
        Fill(dlg, st, *st->opt);
        UpdateInfo(dlg, st);

        // Centre over the parent.
        HWND parent = GetParent(dlg);
        RECT rp, rd;
        if (!parent || !GetWindowRect(parent, &rp)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &rp, 0);
        GetWindowRect(dlg, &rd);
        SetWindowPos(dlg, nullptr,
                     rp.left + ((rp.right - rp.left) - (rd.right - rd.left)) / 2,
                     rp.top + ((rp.bottom - rp.top) - (rd.bottom - rd.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_SHARPEN)) {
            const int v = int(SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_GETPOS, 0, 0));
            SetDlgItemTextW(dlg, IDC_SHARPEN_VAL, (std::to_wstring(v) + L"%").c_str());
            return TRUE;
        }
        break;
    case WM_COMMAND: {
        DialogState* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
        switch (LOWORD(wParam)) {
        case IDOK:
            Collect(dlg, *st->opt);
            WriteLanguageSetting(lang::Setting());
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            lang::Select(st->langBefore);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_RESET: {
            vtf::Options d = DefaultOptions();
            d.frameOrder = st->opt->frameOrder;  // "reset" keeps the animation choice
            d.animAlpha = st->opt->animAlpha;
            d.animate = IsDlgButtonChecked(dlg, IDC_ANIMATE) == BST_CHECKED;
            Fill(dlg, st, d);
            UpdateInfo(dlg, st);
            return TRUE;
        }
        case IDC_PREVIEW: {
            vtf::Options tmp = *st->opt;
            Collect(dlg, tmp);
            std::vector<std::vector<uint8_t>> frames;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            const bool ok = st->fetch && *st->fetch && (*st->fetch)(tmp.alphaSource, tmp.animate, tmp.frameOrder, frames);
            SetCursor(old);
            if (!ok) {
                MessageBoxW(dlg, lang::T("preview.unavailable").c_str(), lang::T("msg.title").c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            // The preview shows the size after resizing; the source is the document.
            if (ShowPreviewDialog(dlg, tmp, frames, uint32_t(st->width), uint32_t(st->height))) {
                Fill(dlg, st, tmp);  // filter, sharpening and DXT quality chosen in the preview
                UpdateInfo(dlg, st);
            }
            return TRUE;
        }
        case IDC_MIPMAPS:
        case IDC_RESIZE:
        case IDC_ANIMATE:
        case IDC_THUMB:
        case IDC_VMT:
            UpdateEnabled(dlg, st);
            UpdateInfo(dlg, st);
            return TRUE;
        case IDC_METHOD:
        case IDC_MAXW:
        case IDC_MAXH:
        case IDC_FORMAT:
        case IDC_VERSION:
            if (HIWORD(wParam) == CBN_SELCHANGE) { UpdateEnabled(dlg, st); UpdateInfo(dlg, st); }
            return TRUE;
        case IDC_LANG:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND combo = GetDlgItem(dlg, IDC_LANG);
                const LRESULT idx = SendMessageW(combo, CB_GETCURSEL, 0, 0);
                const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, idx, 0);
                const auto& list = lang::Available();
                lang::Select(data < 0 || size_t(data) >= list.size() ? std::wstring(L"auto") : list[size_t(data)].code);
                vtf::Options current = *st->opt;
                Collect(dlg, current);  // keep what the user already changed
                ApplyTexts(dlg, st);
                Fill(dlg, st, current);
                FillLanguages(dlg);  // "Auto (...)" text may change
            }
            return TRUE;
        default:
            if (LOWORD(wParam) >= IDC_FLAG_FIRST && LOWORD(wParam) < IDC_FLAG_FIRST + kFlagCount)
                UpdateEnabled(dlg, st);  // Normal Map / SSBump disable the .vmt option
            break;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

// --- settings --------------------------------------------------------------------

vtf::Options DefaultOptions() {
    return vtf::Options();  // DXT1 / DXT5, 7.2, no resize, Box mipmaps, thumbnail
}

vtf::Options LoadOptions() {
    MigrateLegacySettings();
    vtf::Options o = DefaultOptions();
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return o;

    const DWORD kMissing = 0xFFFFFFFFu;
    const DWORD color = ReadDword(key, L"ColorFormat", kMissing);
    if (color != kMissing) {
        o.colorFormat = int32_t(color);
        o.alphaFormat = int32_t(ReadDword(key, L"AlphaFormat", DWORD(o.alphaFormat)));
    } else {  // settings of plug-in v1/v2: a single "Format" value
        const int32_t legacy = int32_t(ReadDword(key, L"Format", DWORD(vtf::FMT_AUTO_DXT)));
        if (legacy == vtf::FMT_AUTO_UNCOMPRESSED) { o.colorFormat = vtf::FMT_BGR888; o.alphaFormat = vtf::FMT_BGRA8888; }
        else if (legacy != vtf::FMT_AUTO_DXT) { o.colorFormat = o.alphaFormat = legacy; }
    }
    o.versionMinor = int(ReadDword(key, L"Version", DWORD(o.versionMinor)));
    o.resize = ReadDword(key, L"Resize", o.resize) != 0;
    o.resizeMethod = int(ReadDword(key, L"ResizeMethod", DWORD(o.resizeMethod)));
    o.resizeFilter = int(ReadDword(key, L"ResizeFilter", DWORD(o.resizeFilter)));
    o.maxWidth = ReadDword(key, L"MaxWidth", o.maxWidth);
    o.maxHeight = ReadDword(key, L"MaxHeight", o.maxHeight);
    o.mipmaps = ReadDword(key, L"Mipmaps", o.mipmaps) != 0;
    o.mipFilter = int(ReadDword(key, L"MipFilter", DWORD(o.mipFilter)));
    o.gammaFilter = ReadDword(key, L"GammaMipmaps", o.gammaFilter) != 0;
    o.tiling = ReadDword(key, L"Tiling", o.tiling) != 0;
    o.thumbnail = ReadDword(key, L"Thumbnail", o.thumbnail) != 0;
    o.flags = ReadDword(key, L"Flags", o.flags) & vtf::USER_FLAGS_MASK;
    o.frameOrder = int(ReadDword(key, L"FrameOrder", DWORD(o.frameOrder)));
    o.animAlpha = int(ReadDword(key, L"AnimAlpha", DWORD(o.animAlpha)));
    o.vmt = ReadDword(key, L"Vmt", o.vmt) != 0;
    o.vmtShader = int(ReadDword(key, L"VmtShader", DWORD(o.vmtShader)));
    o.dxtQuality = int(ReadDword(key, L"DxtQuality", DWORD(o.dxtQuality)));
    o.mipSharpen = int(ReadDword(key, L"MipSharpen", DWORD(o.mipSharpen)));
    o.sharpenAlpha = ReadDword(key, L"MipSharpenAlpha", o.sharpenAlpha ? 1 : 0) != 0;
    o.alphaSource = int(ReadDword(key, L"AlphaSource", DWORD(o.alphaSource)));
    o.animate = false;  // never remembered: depends on the document
    RegCloseKey(key);
    return SanitizeOptions(o);
}

vtf::Options SanitizeOptions(vtf::Options o) {
    const vtf::Options d;
    if (!KnownFormat(o.colorFormat) || !KnownFormat(o.alphaFormat)) {
        o.colorFormat = d.colorFormat;
        o.alphaFormat = d.alphaFormat;
    }
    // A pair that is not in the list (e.g. DXT1 / DXT3 from plug-in v3) becomes Auto DXT.
    const int idx = FormatIndex(o);
    o.colorFormat = kFormats[idx].color;
    o.alphaFormat = kFormats[idx].alpha;
    if (o.frameOrder < 0 || o.frameOrder > 1) o.frameOrder = d.frameOrder;
    if (o.animAlpha < 0 || o.animAlpha > 2) o.animAlpha = d.animAlpha;
    if (o.versionMinor < 1 || o.versionMinor > 5) o.versionMinor = d.versionMinor;
    if (o.resizeMethod < 0 || o.resizeMethod > 2) o.resizeMethod = d.resizeMethod;
    if (o.resizeFilter < 0 || o.resizeFilter >= vtf::FILTER_COUNT) o.resizeFilter = d.resizeFilter;
    if (o.mipFilter < 0 || o.mipFilter >= vtf::FILTER_COUNT) o.mipFilter = d.mipFilter;
    if (MaxSizeIndex(o.maxWidth) == 0) o.maxWidth = 0;
    if (MaxSizeIndex(o.maxHeight) == 0) o.maxHeight = 0;
    o.flags &= vtf::USER_FLAGS_MASK;
    if (o.vmtShader < 0 || o.vmtShader >= vmt::SHADER_COUNT) o.vmtShader = d.vmtShader;
    if (o.dxtQuality < 0 || o.dxtQuality >= vtf::DXT_QUALITY_COUNT) o.dxtQuality = d.dxtQuality;
    if (o.mipSharpen < 0 || o.mipSharpen > 100) o.mipSharpen = d.mipSharpen;
    if (o.alphaSource < 0 || (o.alphaSource > 2 && o.alphaSource < vtf::ALPHA_SOURCE_CHANNEL_FIRST) ||
        o.alphaSource > vtf::ALPHA_SOURCE_CHANNEL_FIRST + 250)
        o.alphaSource = d.alphaSource;
    return o;
}

void SaveOptions(const vtf::Options& o) {
    MigrateLegacySettings();
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    WriteDword(key, L"ColorFormat", DWORD(o.colorFormat));
    WriteDword(key, L"AlphaFormat", DWORD(o.alphaFormat));
    WriteDword(key, L"Version", DWORD(o.versionMinor));
    WriteDword(key, L"Resize", o.resize);
    WriteDword(key, L"ResizeMethod", DWORD(o.resizeMethod));
    WriteDword(key, L"ResizeFilter", DWORD(o.resizeFilter));
    WriteDword(key, L"MaxWidth", o.maxWidth);
    WriteDword(key, L"MaxHeight", o.maxHeight);
    WriteDword(key, L"Mipmaps", o.mipmaps);
    WriteDword(key, L"MipFilter", DWORD(o.mipFilter));
    WriteDword(key, L"GammaMipmaps", o.gammaFilter);
    WriteDword(key, L"Tiling", o.tiling);
    WriteDword(key, L"Thumbnail", o.thumbnail);
    WriteDword(key, L"Flags", o.flags);
    WriteDword(key, L"FrameOrder", DWORD(o.frameOrder));
    WriteDword(key, L"AnimAlpha", DWORD(o.animAlpha));
    WriteDword(key, L"Vmt", o.vmt);
    WriteDword(key, L"VmtShader", DWORD(o.vmtShader));
    WriteDword(key, L"DxtQuality", DWORD(o.dxtQuality));
    WriteDword(key, L"MipSharpen", DWORD(o.mipSharpen));
    WriteDword(key, L"MipSharpenAlpha", o.sharpenAlpha ? 1 : 0);
    WriteDword(key, L"AlphaSource", DWORD(o.alphaSource));
    RegCloseKey(key);
}

std::wstring LanguageFolder() {
    wchar_t path[MAX_PATH * 2] = {0};
    const DWORD n = GetModuleFileNameW(g_hInstance, path, DWORD(sizeof(path) / sizeof(path[0])));
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    p = slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
    const std::wstring folder = p + L"\\VTFBridge_lang";
    // Folder name of versions before the rename, used while the new one does not exist.
    const std::wstring legacy = p + L"\\VTFExport_lang";
    const auto isDir = [](const std::wstring& d) {
        const DWORD a = GetFileAttributesW(d.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    return !isDir(folder) && isDir(legacy) ? legacy : folder;
}

void InitLanguage() {
    lang::Select(ReadLanguageSetting());  // remembered before Init so it is applied after the scan
    lang::Init(LanguageFolder());
}

// --- UI entry points ---------------------------------------------------------------

void FitLabel(HWND dlg, int labelId, int controlId) {
    HWND label = GetDlgItem(dlg, labelId), ctrl = GetDlgItem(dlg, controlId);
    if (!label || !ctrl) return;
    auto rectOf = [&](HWND w) {
        RECT r;
        GetWindowRect(w, &r);
        MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
        return r;
    };
    // Original layout, remembered on the first call: label left/right, control left/right.
    const wchar_t* kProp = L"VTFBridge.FitLabel";
    uintptr_t packed = reinterpret_cast<uintptr_t>(GetPropW(label, kProp));
    RECT rl = rectOf(label), rc = rectOf(ctrl);
    if (!packed) {
        packed = (uintptr_t(uint16_t(rl.left)) << 48) | (uintptr_t(uint16_t(rl.right)) << 32) |
                 (uintptr_t(uint16_t(rc.left)) << 16) | uintptr_t(uint16_t(rc.right));
        SetPropW(label, kProp, reinterpret_cast<HANDLE>(packed));
    }
    const int l0 = int(uint16_t(packed >> 48)), r0 = int(uint16_t(packed >> 32));
    const int c0 = int(uint16_t(packed >> 16)), c1 = int(uint16_t(packed));
    wchar_t text[256] = {};
    const int len = GetWindowTextW(label, text, 256);
    HDC dc = GetDC(label);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0)));
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text, len, &sz);
    SelectObject(dc, old);
    ReleaseDC(label, dc);
    const int gap = c0 - r0;
    const int need = sz.cx + 2;
    int grow = std::max(0, need - (r0 - l0));
    grow = std::min(grow, (c1 - c0) / 2);  // the control keeps at least half its width
    SetWindowPos(label, nullptr, l0, rl.top, r0 - l0 + grow, rl.bottom - rl.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(ctrl, nullptr, c0 + grow, rc.top, c1 - c0 - grow, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    (void)gap;
}

void ForgetLabelLayout(HWND dlg) {
    EnumChildWindows(dlg, [](HWND w, LPARAM) -> BOOL { RemovePropW(w, L"VTFBridge.FitLabel"); return TRUE; }, 0);
}

void FitMainLabels(HWND dlg) {
    const std::pair<int, int> pairs[] = {
        {IDC_LBL_FORMAT, IDC_FORMAT}, {IDC_LBL_VERSION, IDC_VERSION}, {IDC_LBL_ALPHA, IDC_ALPHA},
        {IDC_LBL_METHOD, IDC_METHOD}, {IDC_LBL_RFILTER, IDC_RFILTER}, {IDC_LBL_MAXW, IDC_MAXW},
        {IDC_LBL_MAXH, IDC_MAXH}, {IDC_LBL_MIPFILTER, IDC_MIPFILTER}, {IDC_LBL_SHARPEN, IDC_SHARPEN},
        {IDC_LBL_ORDER, IDC_ORDER}};
    for (const auto& p : pairs) FitLabel(dlg, p.first, p.second);
}

bool ShowOptionsDialog(HWND parent, vtf::Options& opt, int width, int height, int visibleLayers,
                       const AlphaInfo& alpha, const FetchPixelsFn& fetch) {
    DialogState st{&opt, width, height, visibleLayers, &alpha, &fetch, lang::Setting(), nullptr, {}};
    const INT_PTR r = DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_VTF_OPTIONS), parent,
                                      DialogProc, reinterpret_cast<LPARAM>(&st));
    return r == IDOK;
}

void ShowError(HWND parent, const std::wstring& text) {
    MessageBoxW(parent, text.c_str(), lang::T("msg.title").c_str(), MB_OK | MB_ICONERROR);
}

void ShowWarning(HWND parent, const std::wstring& text) {
    MessageBoxW(parent, text.c_str(), lang::T("msg.title").c_str(), MB_OK | MB_ICONWARNING);
}

bool AskYesNo(HWND parent, const std::wstring& text) {
    return MessageBoxW(parent, text.c_str(), lang::T("msg.title").c_str(), MB_YESNO | MB_ICONWARNING) == IDYES;
}

namespace {

constexpr int kAboutSettings = 1000;

HRESULT CALLBACK AboutCallback(HWND, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

} // namespace

void ShowAbout(HWND parent) {
    InitLanguage();
    const std::wstring url = L"" VTFBRIDGE_URL;
    const std::wstring releases = url + L"/releases/latest";  // "check for updates": just opens the page
    const std::wstring text = lang::Tf("about.text", {{"folder", LanguageFolder()}, {"version", L"" VTFBRIDGE_VERSION_STR}});
    const std::wstring title = lang::T("about.title");

    // Task dialog with a clickable link (comctl32 v6, which Photoshop loads);
    // plain message box as a fallback.
    typedef HRESULT(WINAPI * TaskDialogIndirectFn)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    if (!comctl) comctl = LoadLibraryW(L"comctl32.dll");
    auto taskDialog = comctl ? reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(comctl, "TaskDialogIndirect")) : nullptr;
    if (taskDialog) {
        const std::wstring content = text + L"\n\n<a href=\"" + url + L"\">" + url + L"</a>\n<a href=\"" + releases +
                                     L"\">" + lang::T("about.updates") + L"</a>";
        TASKDIALOGCONFIG cfg = {};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = parent;
        cfg.hInstance = g_hInstance;
        cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_OK_BUTTON;
        const std::wstring settings = lang::T("about.settings");
        const TASKDIALOG_BUTTON buttons[] = {{kAboutSettings, settings.c_str()}};
        cfg.pButtons = buttons;
        cfg.cButtons = 1;
        cfg.nDefaultButton = IDOK;
        cfg.pszWindowTitle = title.c_str();
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszContent = content.c_str();
        cfg.pfCallback = AboutCallback;
        int pressed = 0;
        if (SUCCEEDED(taskDialog(&cfg, &pressed, nullptr, nullptr))) {
            if (pressed == kAboutSettings) ShowSettingsDialog(parent);
            return;
        }
    }
    if (MessageBoxW(parent, (text + L"\n\n" + url + L"\n" + lang::T("about.updates") + L": " + releases + L"\n\n" +
                             lang::T("about.open_settings")).c_str(),
                    title.c_str(), MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShowSettingsDialog(parent);
}

// --- Settings ------------------------------------------------------------------------

namespace {

struct SettingsState {
    vtf::Options opt;
    int alphaShown = 0;  // index shown in the alpha list (stored channel choices show as Auto)
};

void SettingsTexts(HWND dlg, SettingsState* st) {
    SetWindowTextW(dlg, lang::T("settings.title").c_str());
    const std::pair<int, const char*> labels[] = {
        {IDC_LBL_LANG, "dlg.language"}, {IDC_GRP_ST_OPEN, "settings.open_group"}, {IDC_LBL_ST_ANIMALPHA, "settings.anim_alpha"},
        {IDC_LBL_ORDER, "dlg.frame_order"}, {IDC_GRP_ST_SAVE, "settings.save_group"}, {IDC_ST_SAVE_HINT, "settings.save_hint"},
        {IDC_LBL_DXTQ, "dlg.dxt_quality"}, {IDC_LBL_ALPHA, "dlg.alpha"}, {IDC_LBL_MIPFILTER, "dlg.mip_filter"},
        {IDC_LBL_SHARPEN, "dlg.sharpen"}, {IDC_SHARPEN_ALPHA, "dlg.sharpen_alpha"}, {IDC_TILING, "dlg.tiling"},
        {IDC_VMT, "dlg.vmt"}, {IDC_LBL_ST_SHADER, "settings.shader"}, {IDC_RESET, "settings.reset"},
        {IDOK, "settings.ok"}, {IDCANCEL, "dlg.cancel"}};
    for (const auto& l : labels) SetDlgItemTextW(dlg, l.first, lang::T(l.second).c_str());
    std::vector<std::wstring> items;
    for (const char* k : {"animalpha.layers", "animalpha.transparency", "animalpha.ask"}) items.push_back(lang::T(k));
    FillCombo(dlg, IDC_ST_ANIMALPHA, items);
    items.clear();
    for (const char* o : kFrameOrders) items.push_back(lang::T(o));
    FillCombo(dlg, IDC_ORDER, items);
    items.clear();
    for (const char* q : kDxtQualities) items.push_back(lang::T(q));
    FillCombo(dlg, IDC_DXTQ, items);
    items.clear();
    for (const char* k : {"alpha.auto", "alpha.transparency", "alpha.none"}) items.push_back(lang::T(k));
    FillCombo(dlg, IDC_ALPHA, items);
    items.clear();
    for (int f = 0; f < vtf::FILTER_COUNT; ++f) { const char* n = vtf::FilterName(f); items.push_back(std::wstring(n, n + std::strlen(n))); }
    FillCombo(dlg, IDC_MIPFILTER, items);
    items.clear();
    for (int i = 0; i < vmt::SHADER_COUNT; ++i) { const char* n = vmt::ShaderName(i); items.push_back(std::wstring(n, n + std::strlen(n))); }
    FillCombo(dlg, IDC_SHADER, items);
    for (int id : {IDC_LBL_ST_ANIMALPHA, IDC_LBL_ORDER, IDC_LBL_DXTQ, IDC_LBL_ALPHA, IDC_LBL_MIPFILTER, IDC_LBL_SHARPEN, IDC_LBL_LANG}) (void)id;
    (void)st;
}

void SettingsFill(HWND dlg, SettingsState* st) {
    const vtf::Options& o = st->opt;
    SendDlgItemMessageW(dlg, IDC_ST_ANIMALPHA, CB_SETCURSEL, o.animAlpha, 0);
    SendDlgItemMessageW(dlg, IDC_ORDER, CB_SETCURSEL, o.frameOrder, 0);
    SendDlgItemMessageW(dlg, IDC_DXTQ, CB_SETCURSEL, o.dxtQuality, 0);
    st->alphaShown = o.alphaSource >= 0 && o.alphaSource <= vtf::ALPHA_SOURCE_NONE ? o.alphaSource : 0;
    SendDlgItemMessageW(dlg, IDC_ALPHA, CB_SETCURSEL, st->alphaShown, 0);
    SendDlgItemMessageW(dlg, IDC_MIPFILTER, CB_SETCURSEL, o.mipFilter, 0);
    SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_SETPOS, TRUE, o.mipSharpen);
    SetDlgItemTextW(dlg, IDC_SHARPEN_VAL, (std::to_wstring(o.mipSharpen) + L"%").c_str());
    CheckDlgButton(dlg, IDC_SHARPEN_ALPHA, o.sharpenAlpha ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TILING, o.tiling ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_VMT, o.vmt ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_SHADER, CB_SETCURSEL, o.vmtShader, 0);
    EnableWindow(GetDlgItem(dlg, IDC_SHADER), o.vmt);
    EnableWindow(GetDlgItem(dlg, IDC_LBL_ST_SHADER), o.vmt);
}

void SettingsCollect(HWND dlg, SettingsState* st) {
    vtf::Options& o = st->opt;
    auto sel = [&](int id) { return int(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); };
    if (sel(IDC_ST_ANIMALPHA) >= 0) o.animAlpha = sel(IDC_ST_ANIMALPHA);
    if (sel(IDC_ORDER) >= 0) o.frameOrder = sel(IDC_ORDER);
    if (sel(IDC_DXTQ) >= 0) o.dxtQuality = sel(IDC_DXTQ);
    if (sel(IDC_ALPHA) >= 0 && sel(IDC_ALPHA) != st->alphaShown) o.alphaSource = sel(IDC_ALPHA);  // keep a channel choice unless changed
    if (sel(IDC_MIPFILTER) >= 0) o.mipFilter = sel(IDC_MIPFILTER);
    o.mipSharpen = int(SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_GETPOS, 0, 0));
    o.sharpenAlpha = IsDlgButtonChecked(dlg, IDC_SHARPEN_ALPHA) == BST_CHECKED;
    o.tiling = IsDlgButtonChecked(dlg, IDC_TILING) == BST_CHECKED;
    o.vmt = IsDlgButtonChecked(dlg, IDC_VMT) == BST_CHECKED;
    if (sel(IDC_SHADER) >= 0) o.vmtShader = sel(IDC_SHADER);
}

void SettingsFit(HWND dlg) {
    const std::pair<int, int> pairs[] = {{IDC_LBL_LANG, IDC_LANG}, {IDC_LBL_ST_ANIMALPHA, IDC_ST_ANIMALPHA}, {IDC_LBL_ORDER, IDC_ORDER},
                                         {IDC_LBL_DXTQ, IDC_DXTQ}, {IDC_LBL_ALPHA, IDC_ALPHA}, {IDC_LBL_MIPFILTER, IDC_MIPFILTER},
                                         {IDC_LBL_SHARPEN, IDC_SHARPEN}, {IDC_LBL_ST_SHADER, IDC_SHADER}};
    for (const auto& p : pairs) FitLabel(dlg, p.first, p.second);
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsState* st = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        st = reinterpret_cast<SettingsState*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        SettingsTexts(dlg, st);
        SettingsFill(dlg, st);
        FillLanguages(dlg);
        SettingsFit(dlg);
        return TRUE;
    case WM_DESTROY:
        ForgetLabelLayout(dlg);
        break;
    case WM_HSCROLL:
        if (st && reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_SHARPEN)) {
            const int v = int(SendDlgItemMessageW(dlg, IDC_SHARPEN, TBM_GETPOS, 0, 0));
            SetDlgItemTextW(dlg, IDC_SHARPEN_VAL, (std::to_wstring(v) + L"%").c_str());
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wParam)) {
        case IDC_VMT: {
            const bool on = IsDlgButtonChecked(dlg, IDC_VMT) == BST_CHECKED;
            EnableWindow(GetDlgItem(dlg, IDC_SHADER), on);
            EnableWindow(GetDlgItem(dlg, IDC_LBL_ST_SHADER), on);
            return TRUE;
        }
        case IDC_LANG:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND combo = GetDlgItem(dlg, IDC_LANG);
                const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, SendMessageW(combo, CB_GETCURSEL, 0, 0), 0);
                const auto& list = lang::Available();
                lang::Select(data < 0 || size_t(data) >= list.size() ? std::wstring(L"auto") : list[size_t(data)].code);
                SettingsCollect(dlg, st);
                SettingsTexts(dlg, st);
                SettingsFill(dlg, st);
                FillLanguages(dlg);
                SettingsFit(dlg);
            }
            return TRUE;
        case IDC_RESET: {
            vtf::Options d;
            st->opt = d;
            SettingsFill(dlg, st);
            return TRUE;
        }
        case IDOK:
            SettingsCollect(dlg, st);
            SaveOptions(st->opt);
            WriteLanguageSetting(lang::Setting());
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // namespace

void ShowSettingsDialog(HWND parent) {
    InitLanguage();
    SettingsState st;
    st.opt = LoadOptions();
    const std::wstring before = lang::Setting();
    if (DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_VTF_SETTINGS), parent, SettingsProc, reinterpret_cast<LPARAM>(&st)) != IDOK)
        lang::Select(before);  // cancelled: back to the previous language
}

namespace {

// Choice buttons drawn by hand: bold title, details below, the same left margin
// for every line (a multi-line push button does not indent wrapped lines).
struct AskState {
    HFONT bold = nullptr;
};

void DrawChoice(const DRAWITEMSTRUCT* di, HFONT bold, const std::wstring& text) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    const bool pressed = (di->itemState & ODS_SELECTED) != 0, focus = (di->itemState & ODS_FOCUS) != 0;
    // Themed push-button background (uxtheme loaded on demand), classic frame as fallback.
    typedef HANDLE(WINAPI * OpenThemeFn)(HWND, LPCWSTR);
    typedef HRESULT(WINAPI * DrawBgFn)(HANDLE, HDC, int, int, const RECT*, const RECT*);
    typedef HRESULT(WINAPI * CloseThemeFn)(HANDLE);
    static HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    static auto openTheme = ux ? reinterpret_cast<OpenThemeFn>(GetProcAddress(ux, "OpenThemeData")) : nullptr;
    static auto drawBg = ux ? reinterpret_cast<DrawBgFn>(GetProcAddress(ux, "DrawThemeBackground")) : nullptr;
    static auto closeTheme = ux ? reinterpret_cast<CloseThemeFn>(GetProcAddress(ux, "CloseThemeData")) : nullptr;
    HANDLE theme = openTheme && drawBg && closeTheme ? openTheme(di->hwndItem, L"BUTTON") : nullptr;
    if (theme) {
        const int state = pressed ? 3 /*PBS_PRESSED*/ : focus ? 5 /*PBS_DEFAULTED*/ : 1 /*PBS_NORMAL*/;
        drawBg(theme, dc, 1 /*BP_PUSHBUTTON*/, state, &rc, nullptr);
        closeTheme(theme);
    } else {
        DrawFrameControl(dc, &rc, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0));
    }
    const size_t nl = text.find(L'\n');
    const std::wstring title = text.substr(0, nl), details = nl == std::wstring::npos ? L"" : text.substr(nl + 1);
    const int pad = MulDiv(10, GetDeviceCaps(dc, LOGPIXELSX), 96);
    RECT tr = {rc.left + pad, rc.top + pad / 2 + 2, rc.right - pad, rc.bottom - pad / 2};
    if (pressed) OffsetRect(&tr, 1, 1);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    HFONT normal = reinterpret_cast<HFONT>(SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old = SelectObject(dc, bold ? bold : normal);
    RECT measure = tr;
    DrawTextW(dc, title.c_str(), -1, &measure, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    DrawTextW(dc, title.c_str(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    tr.top = measure.bottom + 1;
    SelectObject(dc, normal);
    DrawTextW(dc, details.c_str(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    if (focus && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = rc;
        InflateRect(&f, -3, -3);
        DrawFocusRect(dc, &f);
    }
}

INT_PTR CALLBACK AskProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    AskState* st = reinterpret_cast<AskState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<AskState*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        SetWindowTextW(dlg, lang::T("msg.title").c_str());
        LOGFONTW lf = {};
        if (GetObjectW(reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0)), sizeof(lf), &lf)) {
            lf.lfWeight = FW_BOLD;
            st->bold = CreateFontIndirectW(&lf);
        }
        for (int id : {IDC_ASK_LAYERS, IDC_ASK_TRANSPARENCY}) {
            HWND b = GetDlgItem(dlg, id);
            SetWindowLongPtrW(b, GWL_STYLE, (GetWindowLongPtrW(b, GWL_STYLE) & ~LONG_PTR(0xF)) | BS_OWNERDRAW);
        }
        SetDlgItemTextW(dlg, IDC_ASK_TEXT, lang::T("ask.anim_alpha").c_str());
        SetDlgItemTextW(dlg, IDC_ASK_LAYERS, lang::T("ask.layers").c_str());
        SetDlgItemTextW(dlg, IDC_ASK_TRANSPARENCY, lang::T("ask.transparency").c_str());
        SetDlgItemTextW(dlg, IDC_ASK_REMEMBER, lang::T("ask.remember").c_str());
        SetDlgItemTextW(dlg, IDC_ASK_HINT, lang::T("ask.remember_hint").c_str());
        SetDlgItemTextW(dlg, IDCANCEL, lang::T("dlg.cancel").c_str());
        SetFocus(GetDlgItem(dlg, IDC_ASK_LAYERS));
        return FALSE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (st && (di->CtlID == IDC_ASK_LAYERS || di->CtlID == IDC_ASK_TRANSPARENCY)) {
            DrawChoice(di, st->bold, lang::T(di->CtlID == IDC_ASK_LAYERS ? "ask.layers" : "ask.transparency"));
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        if (st && st->bold) { DeleteObject(st->bold); st->bold = nullptr; }
        break;
    case WM_COMMAND: {
        const bool remember = IsDlgButtonChecked(dlg, IDC_ASK_REMEMBER) == BST_CHECKED;
        switch (LOWORD(wParam)) {
        case IDC_ASK_LAYERS: EndDialog(dlg, remember ? 10 : 0); return TRUE;
        case IDC_ASK_TRANSPARENCY: EndDialog(dlg, remember ? 11 : 1); return TRUE;
        case IDCANCEL: EndDialog(dlg, -1); return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

int AskAnimAlpha(HWND parent) {
    InitLanguage();
    AskState st;
    const INT_PTR r = DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_VTF_ASK), parent, AskProc, reinterpret_cast<LPARAM>(&st));
    if (r < 0) return -1;
    const int choice = int(r % 10);
    if (r >= 10) {  // "Remember my choice"
        vtf::Options o = LoadOptions();
        o.animAlpha = choice;
        SaveOptions(o);
    }
    return choice;
}
