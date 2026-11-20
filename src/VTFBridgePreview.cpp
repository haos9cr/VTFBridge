// VTFBridgePreview.cpp - texture preview window: every mip level as it will be stored
// in the file (format, DXT quality, resize / mipmap filter, sharpening), compressed and
// decoded back, compared with a reference (the uncompressed mips or an image file).
//
// Views: result, reference, split (reference | result), amplified difference, and the
// whole mip chain side by side. Zoom is measured against the full-size texture, so
// switching levels keeps the picture in place; the wheel zooms smoothly around the
// cursor, "Auto" picks the level a GPU would use at that zoom, "Tile" repeats the
// texture. Holding Space shows the reference. The work runs on a worker thread.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ImageLoad.h"
#include "Lang.h"
#include "Resample.h"
#include "VTFBridge.h"
#include "VtfWriter.h"
#include "resource.h"

namespace {

constexpr UINT WM_PREVIEW_DONE = WM_APP + 41;
constexpr UINT WM_PV_SPACE = WM_APP + 42;
constexpr UINT WM_PV_WHEEL = WM_APP + 43;
constexpr UINT WM_PV_ZOOMTEXT = WM_APP + 44;
constexpr UINT_PTR kTimerRecompute = 1;

enum Mode { MODE_RESULT = 0, MODE_REFERENCE, MODE_SPLIT, MODE_DIFF, MODE_CHAIN, MODE_COUNT };
enum View { VIEW_RGB = 0, VIEW_ALPHA, VIEW_CHECKER };
constexpr int kDiffGain = 8;

struct RefImage {
    std::vector<LoadedFrame> frames;  // a VTF animation has several; mips = levels stored in the file
    uint32_t w = 0, h = 0;
    std::wstring name;
};

// Output of one worker job.
struct Result {
    bool ok = false;
    int frame = 0;
    int32_t format = 0;
    std::vector<vtf::PreviewLevel> levels;
    std::vector<std::vector<uint8_t>> ref;  // reference file resized to every level (if any)
    std::vector<double> refPsnr;
};

// Shared between the window and the worker threads (outlives either).
struct Shared {
    std::vector<std::vector<uint8_t>> frames;  // RGBA8 per animation frame (one for a still image)
    uint32_t w = 0, h = 0;
    bool anyAlpha = false;  // over all frames: decides Auto DXT1 / DXT5 like the save
    std::mutex mutex;
    int generation = 0;
    std::shared_ptr<const Result> result;
};

// Box-filtered copies of a picture, used when it is shown smaller than 1:1.
struct Pyramid {
    const std::vector<uint8_t>* base = nullptr;
    std::vector<uint8_t> own;                  // base for computed pictures (difference)
    std::vector<std::vector<uint8_t>> levels;  // 1/2, 1/4, ...
    std::vector<uint32_t> w, h;                // [0] = base
};

struct PreviewState {
    vtf::Options opt;      // working copy
    vtf::Options* target;  // written on Apply
    std::shared_ptr<Shared> shared;
    int generation = 0;
    bool busy = false, pending = false, everShown = false;
    std::shared_ptr<const Result> res;
    int frame = 0;
    std::map<int, std::shared_ptr<const Result>> frameCache;  // results for the current settings
    std::string cacheKey;
    std::shared_ptr<const RefImage> ref;
    bool useRef = false;  // reference = file (else the uncompressed mips)
    std::wstring status;  // e.g. file load error

    int level = 0, mode = MODE_RESULT, view = VIEW_RGB;
    bool autoLevel = false, tile = false, fit = true, space = false;
    double scale = 1, cx = 0, cy = 0;  // screen pixels per full-size texel; canvas point at the centre
    double split = 0.5;
    int drag = 0;                      // 1 pan, 2 split line
    POINT dragFrom{};
    double dragCx = 0, dragCy = 0;
    int hover = -1;                    // mip under the mouse (chain view)
    std::map<int, Pyramid> cache;

    // Resizing the window.
    SIZE minTrack{};
    int initW = 0, initH = 0;
    RECT rcArea{}, rcInfo{}, rcHint{}, rcOk{}, rcCancel{};
};

HHOOK g_hook = nullptr;
HWND g_dlg = nullptr;

const char* kQualities[] = {"dxtq.fast", "dxtq.high", "dxtq.best", "dxtq.max", "dxtq.perceptual"};
const int kZoomPresets[] = {10, 25, 50, 100, 200, 400, 800, 1600};

int ComboSel(HWND dlg, int id) { return int(SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0)); }
void AddItem(HWND dlg, int id, const std::wstring& s) { SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, LPARAM(s.c_str())); }
std::wstring Widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }
void Redraw(HWND dlg) { InvalidateRect(GetDlgItem(dlg, IDC_PV_AREA), nullptr, FALSE); }

double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double se = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - b[i + c]; se += d * d; }
    const double mse = se / (double(a.size() / 4) * 3);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

const std::vector<uint8_t>& ResultPixels(const vtf::PreviewLevel& L) { return L.decoded.empty() ? L.original : L.decoded; }

// ---- worker -------------------------------------------------------------------------

// Everything a result depends on except the frame.
std::string SettingsKey(const PreviewState* st) {
    const vtf::Options& o = st->opt;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d/%d/%d/%d/%d/%p/%d", o.mipFilter, o.mipSharpen, int(o.sharpenAlpha), o.dxtQuality,
                  o.resizeFilter, static_cast<const void*>(st->ref.get()), int(st->useRef));
    return buf;
}

void StartJob(HWND dlg, PreviewState* st) {
    const std::string key = SettingsKey(st);
    if (key != st->cacheKey) { st->frameCache.clear(); st->cacheKey = key; }
    if (st->busy) { st->pending = true; return; }
    st->busy = true;
    st->pending = false;
    const int gen = ++st->generation;
    vtf::Options opt = st->opt;
    const int frame = st->frame;
    std::shared_ptr<const RefImage> ref = st->useRef ? st->ref : nullptr;
    std::shared_ptr<Shared> sh = st->shared;
    std::thread([dlg, gen, opt, frame, ref, sh]() mutable {
        auto res = std::make_shared<Result>();
        res->frame = frame;
        // Auto formats look at all frames, like the save.
        opt.colorFormat = opt.alphaFormat = vtf::ResolveFormat(opt, sh->anyAlpha);
        res->ok = vtf::BuildPreview(sh->frames[size_t(frame)].data(), sh->w, sh->h, opt, res->levels, res->format, -1);
        const LoadedFrame* rf = ref && !ref->frames.empty() ? &ref->frames[std::min<size_t>(size_t(frame), ref->frames.size() - 1)] : nullptr;
        if (res->ok && rf && !res->levels.empty()) {
            // The reference does not follow the preview's settings: a level stored in the
            // file (VTF mips) of the same size is used as it is; any other level comes from
            // the reference picture resized to that size (Mitchell for the full size, then
            // plain 2x2 box averaging, like most tools build mips).
            res->ref.resize(res->levels.size());
            res->refPsnr.resize(res->levels.size());
            vtf::FloatImage prev;
            bool havePrev = false;
            for (size_t m = 0; m < res->levels.size(); ++m) {
                const vtf::PreviewLevel& L = res->levels[m];
                const std::vector<uint8_t>* stored = nullptr;
                for (size_t k = 0; k <= rf->mips.size() && !stored; ++k) {
                    const uint32_t kw = std::max(1u, ref->w >> k), kh = std::max(1u, ref->h >> k);
                    if (kw == L.w && kh == L.h) stored = k == 0 ? &rf->rgba : &rf->mips[k - 1];
                }
                if (stored) {
                    res->ref[m] = *stored;
                    prev = vtf::ToFloat(stored->data(), L.w, L.h, false);
                } else {
                    if (!havePrev) prev = vtf::ToFloat(rf->rgba.data(), ref->w, ref->h, false);
                    if (prev.w != L.w || prev.h != L.h)
                        prev = vtf::Resample(prev, L.w, L.h, m == 0 ? vtf::FILTER_MITCHELL : vtf::FILTER_BOX, false, false);
                    vtf::ToBytes(prev, false, res->ref[m]);
                }
                havePrev = true;
                res->refPsnr[m] = Psnr(ResultPixels(L), res->ref[m]);
            }
        }
        {
            std::lock_guard<std::mutex> lock(sh->mutex);
            sh->generation = gen;
            sh->result = res;
        }
        PostMessageW(dlg, WM_PREVIEW_DONE, WPARAM(gen), 0);  // fails harmlessly if the window is gone
    }).detach();
}

// ---- geometry -----------------------------------------------------------------------

SIZE AreaSize(HWND dlg) {
    RECT r;
    GetClientRect(GetDlgItem(dlg, IDC_PV_AREA), &r);
    return {r.right - r.left, r.bottom - r.top};
}

int LevelCount(const PreviewState* st) { return st->res ? int(st->res->levels.size()) : 0; }

// Gap between mips in the chain view (in full-size texels).
double ChainGap(const PreviewState* st) {
    const auto& L = st->res->levels[0];
    return std::max(2.0, std::max(L.w, L.h) / 32.0);
}

// Canvas = what pan / zoom move over, in full-size texels.
void CanvasSize(const PreviewState* st, double& cw, double& ch) {
    cw = ch = 1;
    if (!LevelCount(st)) return;
    const auto& lv = st->res->levels;
    cw = lv[0].w;
    ch = lv[0].h;
    if (st->mode == MODE_CHAIN) {
        cw = 0;
        for (const auto& L : lv) cw += L.w;
        cw += ChainGap(st) * (lv.size() - 1);
    }
}

void ApplyFit(HWND dlg, PreviewState* st) {
    double cw, ch;
    CanvasSize(st, cw, ch);
    const SIZE a = AreaSize(dlg);
    double s = std::min((a.cx - 4) / cw, (a.cy - 4) / ch);
    if (s >= 1) s = std::floor(s);  // whole pixels when enlarging
    st->scale = std::max(s, 1.0 / 256);
    st->cx = cw / 2;
    st->cy = ch / 2;
}

void ClampPan(PreviewState* st) {
    double cw, ch;
    CanvasSize(st, cw, ch);
    if (st->tile && st->mode != MODE_CHAIN) return;
    st->cx = std::min(std::max(st->cx, 0.0), cw);
    st->cy = std::min(std::max(st->cy, 0.0), ch);
}

std::wstring ZoomText(double s) {
    wchar_t buf[32];
    const double p = s * 100;
    std::swprintf(buf, 32, p < 10 ? L"%.1f%%" : L"%.0f%%", p);
    return buf;
}

void UpdateZoomText(HWND dlg, PreviewState* st) {
    SetDlgItemTextW(dlg, IDC_PV_ZOOM, st->fit ? lang::T("preview.fit").c_str() : ZoomText(st->scale).c_str());
}

void UpdateInfo(HWND dlg, PreviewState* st);

// "Auto": the level a GPU would sample at this zoom (nearest mip).
void UpdateAutoLevel(HWND dlg, PreviewState* st) {
    if (!st->autoLevel || st->mode == MODE_CHAIN || !LevelCount(st)) return;
    int l = int(std::floor(std::log2(1.0 / st->scale) + 0.5));
    l = std::min(std::max(l, 0), LevelCount(st) - 1);
    if (l != st->level) {
        st->level = l;
        SendDlgItemMessageW(dlg, IDC_PV_LEVEL, CB_SETCURSEL, l, 0);
        UpdateInfo(dlg, st);
    }
}

// Zooms to `s`, keeping the canvas point under area pixel (ax, ay) in place.
void SetScale(HWND dlg, PreviewState* st, double s, double ax, double ay) {
    const SIZE a = AreaSize(dlg);
    s = std::min(std::max(s, 1.0 / 256), 64.0);
    const double u = st->cx + (ax - a.cx / 2.0) / st->scale, v = st->cy + (ay - a.cy / 2.0) / st->scale;
    st->scale = s;
    st->cx = u - (ax - a.cx / 2.0) / s;
    st->cy = v - (ay - a.cy / 2.0) / s;
    st->fit = false;
    ClampPan(st);
    UpdateZoomText(dlg, st);
    UpdateAutoLevel(dlg, st);
    Redraw(dlg);
}

// ---- pictures -----------------------------------------------------------------------

const std::vector<uint8_t>& RefPixels(const PreviewState* st, int m) {
    if (st->useRef && m < int(st->res->ref.size()) && !st->res->ref[m].empty()) return st->res->ref[m];
    return st->res->levels[m].original;
}

void Heat(int t, uint8_t* o) {  // 0..255 -> black, blue, red, yellow, white
    static const int k[5][3] = {{0, 0, 0}, {30, 60, 255}, {255, 30, 30}, {255, 230, 0}, {255, 255, 255}};
    const int seg = std::min(t * 4 / 256, 3), f = t * 4 - seg * 256;
    for (int c = 0; c < 3; ++c) o[c] = uint8_t(k[seg][c] + (k[seg + 1][c] - k[seg][c]) * f / 256);
    o[3] = 255;
}

// Picture `kind` (0 result, 1 reference, 2 difference) of mip `m`, with its
// box-filtered copies down to level `p`.
Pyramid& GetPyramid(PreviewState* st, int kind, int m, int p) {
    const bool alphaDiff = kind == 2 && st->view == VIEW_ALPHA;
    const int key = (kind * 2 + (alphaDiff ? 1 : 0)) * 64 + m;
    Pyramid& P = st->cache[key];
    const vtf::PreviewLevel& L = st->res->levels[m];
    if (!P.base) {
        if (kind == 0) P.base = &ResultPixels(L);
        else if (kind == 1) P.base = &RefPixels(st, m);
        else {
            const std::vector<uint8_t>& a = ResultPixels(L);
            const std::vector<uint8_t>& b = RefPixels(st, m);
            P.own.resize(a.size());
            for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
                int d;
                if (alphaDiff) d = std::abs(int(a[i + 3]) - b[i + 3]);
                else d = std::max({std::abs(int(a[i]) - b[i]), std::abs(int(a[i + 1]) - b[i + 1]), std::abs(int(a[i + 2]) - b[i + 2])});
                Heat(std::min(255, d * kDiffGain), &P.own[i]);
            }
            P.base = &P.own;
        }
        P.w = {L.w};
        P.h = {L.h};
    }
    while (int(P.w.size()) <= p && (P.w.back() > 1 || P.h.back() > 1)) {
        const uint32_t sw = P.w.back(), sh = P.h.back(), dw = std::max(1u, sw / 2), dh = std::max(1u, sh / 2);
        const std::vector<uint8_t>& s = P.levels.empty() ? *P.base : P.levels.back();
        std::vector<uint8_t> d(size_t(dw) * dh * 4);
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t x0 = std::min(2 * x, sw - 1), x1 = std::min(2 * x + 1, sw - 1);
                const uint32_t y0 = std::min(2 * y, sh - 1), y1 = std::min(2 * y + 1, sh - 1);
                for (int c = 0; c < 4; ++c)
                    d[(size_t(y) * dw + x) * 4 + c] = uint8_t((s[(size_t(y0) * sw + x0) * 4 + c] + s[(size_t(y0) * sw + x1) * 4 + c] +
                                                               s[(size_t(y1) * sw + x0) * 4 + c] + s[(size_t(y1) * sw + x1) * 4 + c] + 2) / 4);
            }
        P.levels.push_back(std::move(d));
        P.w.push_back(dw);
        P.h.push_back(dh);
    }
    return P;
}

const std::vector<uint8_t>& PyrLevel(const Pyramid& P, int p) { return p == 0 ? *P.base : P.levels[p - 1]; }

// Pyramid level for pictures shown at `k` screen pixels per texel.
int PickLevel(double k) {
    if (k >= 1) return 0;
    return std::min(15, std::max(0, int(std::floor(std::log2(1.0 / k) + 0.5))));
}

bool ShowingReference(const PreviewState* st) {
    if (st->mode == MODE_REFERENCE) return !st->space;
    if (st->mode == MODE_RESULT || st->mode == MODE_CHAIN) return st->space;
    return false;
}

// Writes one texel to the BGRA output with the current view.
inline void Shade(const PreviewState* st, const uint8_t* s, int x, int y, uint8_t* d) {
    uint8_t r = s[0], g = s[1], b = s[2];
    if (st->mode != MODE_DIFF) {
        if (st->view == VIEW_ALPHA) {
            r = g = b = s[3];
        } else if (st->view == VIEW_CHECKER) {
            const int c = (((x >> 3) ^ (y >> 3)) & 1) ? 204 : 153;
            r = uint8_t((s[0] * s[3] + c * (255 - s[3]) + 127) / 255);
            g = uint8_t((s[1] * s[3] + c * (255 - s[3]) + 127) / 255);
            b = uint8_t((s[2] * s[3] + c * (255 - s[3]) + 127) / 255);
        }
    }
    d[0] = b; d[1] = g; d[2] = r; d[3] = 255;
}

void Render(PreviewState* st, int aw, int ah, uint8_t* out) {
    for (int i = 0; i < aw * ah; ++i) { out[i * 4] = 44; out[i * 4 + 1] = 40; out[i * 4 + 2] = 40; out[i * 4 + 3] = 255; }
    if (!LevelCount(st)) return;
    const auto& lv = st->res->levels;
    const double s = st->scale, W0 = lv[0].w, H0 = lv[0].h;
    std::vector<int> colA(aw), colB(aw), rowA(ah);

    if (st->mode == MODE_CHAIN) {
        const int p = PickLevel(s);
        const int kind = ShowingReference(st) ? 1 : 0;
        std::vector<const Pyramid*> pyr(lv.size());
        for (size_t m = 0; m < lv.size(); ++m) pyr[m] = &GetPyramid(st, kind, int(m), p);
        std::vector<double> ox(lv.size());
        double x = 0;
        for (size_t m = 0; m < lv.size(); ++m) { ox[m] = x; x += lv[m].w + ChainGap(st); }
        for (int sx = 0; sx < aw; ++sx) {  // colA = mip, colB = texel x
            const double u = st->cx + (sx + 0.5 - aw / 2.0) / s;
            colA[sx] = -1;
            for (size_t m = 0; m < lv.size(); ++m)
                if (u >= ox[m] && u < ox[m] + lv[m].w) { colA[sx] = int(m); colB[sx] = int(u - ox[m]); break; }
        }
        for (int sy = 0; sy < ah; ++sy) {
            const double v = st->cy + (sy + 0.5 - ah / 2.0) / s;
            rowA[sy] = v >= 0 && v < H0 ? int(v) : -1;
        }
        for (int sy = 0; sy < ah; ++sy) {
            if (rowA[sy] < 0) continue;
            for (int sx = 0; sx < aw; ++sx) {
                const int m = colA[sx];
                if (m < 0) continue;
                const int ty = rowA[sy] - int(H0 - lv[m].h);  // mips stand on a common baseline
                if (ty < 0) continue;
                const Pyramid& P = *pyr[m];
                const int pl = std::min<int>(p, int(P.w.size()) - 1);
                const uint32_t pw = P.w[pl], ph = P.h[pl];
                const uint32_t tx = std::min<uint32_t>(uint32_t(colB[sx]) >> pl, pw - 1), tyy = std::min<uint32_t>(uint32_t(ty) >> pl, ph - 1);
                Shade(st, &PyrLevel(P, pl)[(size_t(tyy) * pw + tx) * 4], sx, sy, out + (size_t(sy) * aw + sx) * 4);
            }
        }
        return;
    }

    const int m = std::min(st->level, int(lv.size()) - 1);
    const vtf::PreviewLevel& L = lv[m];
    const double fx = L.w / W0, fy = L.h / H0;  // level texels per canvas unit
    const int p = PickLevel(s / fx);
    const Pyramid *first, *second = nullptr;
    if (st->mode == MODE_DIFF) first = &GetPyramid(st, 2, m, p);
    else if (st->mode == MODE_SPLIT) { first = &GetPyramid(st, 1, m, p); second = &GetPyramid(st, 0, m, p); }
    else first = &GetPyramid(st, ShowingReference(st) ? 1 : 0, m, p);
    const int pl = std::min<int>(p, int(first->w.size()) - 1);
    const uint32_t pw = first->w[pl], ph = first->h[pl];
    for (int sx = 0; sx < aw; ++sx) {
        double u = st->cx + (sx + 0.5 - aw / 2.0) / s;
        if (st->tile) u -= std::floor(u / W0) * W0;
        colA[sx] = u >= 0 && u < W0 ? int(std::min<uint32_t>(uint32_t(std::min(u * fx, L.w - 1.0)) >> pl, pw - 1)) : -1;
    }
    for (int sy = 0; sy < ah; ++sy) {
        double v = st->cy + (sy + 0.5 - ah / 2.0) / s;
        if (st->tile) v -= std::floor(v / H0) * H0;
        rowA[sy] = v >= 0 && v < H0 ? int(std::min<uint32_t>(uint32_t(std::min(v * fy, L.h - 1.0)) >> pl, ph - 1)) : -1;
    }
    const int splitX = int(std::lround(st->split * aw));
    const uint8_t* a = PyrLevel(*first, pl).data();
    const uint8_t* b = second ? PyrLevel(*second, pl).data() : a;
    for (int sy = 0; sy < ah; ++sy) {
        if (rowA[sy] < 0) continue;
        const size_t row = size_t(rowA[sy]) * pw;
        for (int sx = 0; sx < aw; ++sx) {
            if (colA[sx] < 0) continue;
            const uint8_t* src = (second && sx >= splitX ? b : a) + (row + colA[sx]) * 4;
            Shade(st, src, sx, sy, out + (size_t(sy) * aw + sx) * 4);
        }
    }
    if (second && splitX >= 0 && splitX < aw)
        for (int sy = 0; sy < ah; ++sy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = splitX + dx;
                if (x < 0 || x >= aw) continue;
                uint8_t* d = out + (size_t(sy) * aw + x) * 4;
                const uint8_t c = dx == 0 ? 240 : 20;
                d[0] = d[1] = d[2] = c;
            }
}

void Draw(PreviewState* st, const DRAWITEMSTRUCT* di) {
    const RECT rc = di->rcItem;
    const int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    if (aw <= 0 || ah <= 0) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = aw;
    bi.bmiHeader.biHeight = -ah;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(di->hDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    Render(st, aw, ah, static_cast<uint8_t*>(bits));
    HDC mem = CreateCompatibleDC(di->hDC);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(di->hDC, rc.left, rc.top, aw, ah, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

// ---- text ---------------------------------------------------------------------------

std::wstring Grade(double psnr) {
    return lang::T(psnr >= 45 ? "preview.grade_excellent" : psnr >= 38 ? "preview.grade_good"
                 : psnr >= 32 ? "preview.grade_visible" : "preview.grade_heavy");
}

void UpdateInfo(HWND dlg, PreviewState* st) {
    std::wstring text;
    if (!st->status.empty()) {
        text = st->status;
    } else if (LevelCount(st)) {
        const int m = st->mode == MODE_CHAIN ? (st->hover >= 0 ? st->hover : 0) : std::min(st->level, LevelCount(st) - 1);
        const vtf::PreviewLevel& L = st->res->levels[m];
        text = lang::Tf("preview.info", {{"n", std::to_wstring(m)}, {"w", std::to_wstring(L.w)}, {"h", std::to_wstring(L.h)},
                                         {"format", Widen(vtf::FormatName(st->res->format))}});
        wchar_t buf[32];
        if (st->useRef && st->ref && m < int(st->res->refPsnr.size())) {
            const double v = st->res->refPsnr[m];
            std::swprintf(buf, 32, L"%.1f", v);
            text += L"  ·  " + (v >= 99 ? lang::Tf("preview.same_as_file", {{"name", st->ref->name}})
                                            : lang::Tf("preview.accuracy_file", {{"name", st->ref->name}, {"psnr", buf}, {"grade", Grade(v)}}));
        } else if (!L.decoded.empty() && L.psnr < 99) {
            std::swprintf(buf, 32, L"%.1f", L.psnr);
            text += L"  ·  " + lang::Tf("preview.accuracy", {{"psnr", buf}, {"grade", Grade(L.psnr)}});
        } else {
            text += L"  ·  " + lang::T("preview.lossless");
        }
        if (ShowingReference(st)) text += L"  ·  " + lang::T("preview.showing_ref");
    }
    if (st->busy) text += (text.empty() ? L"" : L"  ·  ") + lang::T("preview.computing");
    SetDlgItemTextW(dlg, IDC_PV_INFO, text.c_str());
}

void FillLevels(HWND dlg, PreviewState* st) {
    SendDlgItemMessageW(dlg, IDC_PV_LEVEL, CB_RESETCONTENT, 0, 0);
    for (int m = 0; m < LevelCount(st); ++m)
        AddItem(dlg, IDC_PV_LEVEL, lang::Tf("preview.level_item", {{"n", std::to_wstring(m)},
                                                                   {"w", std::to_wstring(st->res->levels[m].w)},
                                                                   {"h", std::to_wstring(st->res->levels[m].h)}}));
    st->level = std::max(0, std::min(st->level, LevelCount(st) - 1));
    SendDlgItemMessageW(dlg, IDC_PV_LEVEL, CB_SETCURSEL, st->level, 0);
}

void FillReference(HWND dlg, PreviewState* st) {
    SendDlgItemMessageW(dlg, IDC_PV_REF, CB_RESETCONTENT, 0, 0);
    AddItem(dlg, IDC_PV_REF, lang::T("preview.ref_uncompressed"));
    if (st->ref)
    {
        const bool mips = !st->ref->frames.empty() && !st->ref->frames[0].mips.empty();
        std::wstring item = lang::Tf(mips ? "preview.ref_file_mips" : "preview.ref_file_box", {{"name", st->ref->name}});
        if (st->ref->frames.size() > 1) item += L" " + lang::Tf("preview.ref_frames", {{"n", std::to_wstring(st->ref->frames.size())}});
        AddItem(dlg, IDC_PV_REF, item);
    }
    AddItem(dlg, IDC_PV_REF, lang::T("preview.ref_browse"));
    SendDlgItemMessageW(dlg, IDC_PV_REF, CB_SETCURSEL, st->useRef && st->ref ? 1 : 0, 0);
    SendDlgItemMessageW(dlg, IDC_PV_REF, CB_SETDROPPEDWIDTH, 420, 0);
}

void UpdateEnables(HWND dlg, PreviewState* st) {
    const bool chain = st->mode == MODE_CHAIN;
    EnableWindow(GetDlgItem(dlg, IDC_PV_LEVEL), !chain && !st->autoLevel);
    EnableWindow(GetDlgItem(dlg, IDC_PV_AUTO), !chain);
    EnableWindow(GetDlgItem(dlg, IDC_PV_TILE), !chain);
}

void SetSharpLabel(HWND dlg, int v) { SetDlgItemTextW(dlg, IDC_PV_SHARPVAL, (std::to_wstring(v) + L"%").c_str()); }

void InvalidatePictures(PreviewState* st) { st->cache.clear(); }

// Mouse position in area pixels.
POINT ToArea(HWND dlg, LPARAM lParam, bool screen) {
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (screen) ScreenToClient(GetDlgItem(dlg, IDC_PV_AREA), &pt);
    else MapWindowPoints(dlg, GetDlgItem(dlg, IDC_PV_AREA), &pt, 1);
    return pt;
}

bool InArea(HWND dlg, POINT pt) {
    const SIZE a = AreaSize(dlg);
    return pt.x >= 0 && pt.y >= 0 && pt.x < a.cx && pt.y < a.cy;
}

bool NearSplit(HWND dlg, PreviewState* st, POINT pt) {
    if (st->mode != MODE_SPLIT || !InArea(dlg, pt)) return false;
    const SIZE a = AreaSize(dlg);
    return std::abs(pt.x - int(std::lround(st->split * a.cx))) <= 4;
}

int ChainLevelAt(HWND dlg, PreviewState* st, POINT pt) {
    if (st->mode != MODE_CHAIN || !LevelCount(st) || !InArea(dlg, pt)) return -1;
    const SIZE a = AreaSize(dlg);
    const double u = st->cx + (pt.x + 0.5 - a.cx / 2.0) / st->scale, v = st->cy + (pt.y + 0.5 - a.cy / 2.0) / st->scale;
    const auto& lv = st->res->levels;
    double x = 0;
    for (size_t m = 0; m < lv.size(); ++m) {
        if (u >= x && u < x + lv[m].w && v >= double(lv[0].h) - lv[m].h && v < lv[0].h) return int(m);
        x += lv[m].w + ChainGap(st);
    }
    return -1;
}

void ParseZoomText(HWND dlg, PreviewState* st) {
    wchar_t buf[64] = {};
    GetDlgItemTextW(dlg, IDC_PV_ZOOM, buf, 64);
    if (lang::T("preview.fit") == buf) {
        st->fit = true;
        ApplyFit(dlg, st);
        UpdateAutoLevel(dlg, st);
        Redraw(dlg);
        return;
    }
    const double v = std::wcstod(buf, nullptr);
    const SIZE a = AreaSize(dlg);
    if (v > 0) SetScale(dlg, st, v / 100.0, a.cx / 2.0, a.cy / 2.0);
    else UpdateZoomText(dlg, st);
}

void BrowseReference(HWND dlg, PreviewState* st) {
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring filter = lang::T("preview.file_filter");
    filter += L" (*.png, *.tga, *.jpg, *.vtf, ...)";
    filter.push_back(L'\0');
    filter += L"*.png;*.tga;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.dds;*.webp;*.vtf";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(sizeof(file) / sizeof(file[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        auto ref = std::make_shared<RefImage>();
        std::wstring path = file;
        const size_t slash = path.find_last_of(L"\\/");
        ref->name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (LoadImageFrames(path, ref->frames, ref->w, ref->h)) {
            st->ref = ref;
            st->useRef = true;
            st->status.clear();
            InvalidatePictures(st);
            StartJob(dlg, st);
        } else {
            st->status = lang::Tf("preview.load_error", {{"name", ref->name}});
        }
    }
    FillReference(dlg, st);
    UpdateInfo(dlg, st);
    Redraw(dlg);
}

// ---- keyboard / wheel hook ----------------------------------------------------------
// Space would press the focused button and the wheel would scroll the focused combo
// box, so both are taken in the message loop before any control sees them.

LRESULT CALLBACK MsgHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE && g_dlg) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        const bool ours = m->hwnd == g_dlg || IsChild(g_dlg, m->hwnd);
        if (ours && (m->message == WM_KEYDOWN || m->message == WM_KEYUP) && m->wParam == VK_SPACE) {
            wchar_t cls[16] = {};
            GetClassNameW(m->hwnd, cls, 16);
            if (_wcsicmp(cls, L"Edit") != 0) {
                SendMessageW(g_dlg, WM_PV_SPACE, m->message == WM_KEYDOWN ? 1 : 0, 0);
                m->message = WM_NULL;
            }
        } else if (ours && m->message == WM_MOUSEWHEEL) {
            POINT pt = {GET_X_LPARAM(m->lParam), GET_Y_LPARAM(m->lParam)};
            RECT r;
            GetWindowRect(GetDlgItem(g_dlg, IDC_PV_AREA), &r);
            if (PtInRect(&r, pt)) {
                SendMessageW(g_dlg, WM_PV_WHEEL, m->wParam, m->lParam);
                m->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// ---- dialog -------------------------------------------------------------------------

RECT ChildRect(HWND dlg, int id) {
    RECT r;
    GetWindowRect(GetDlgItem(dlg, id), &r);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

void Layout(HWND dlg, PreviewState* st, int cw, int ch) {
    const int dx = cw - st->initW, dy = ch - st->initH;
    auto place = [&](int id, const RECT& r, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(dlg, id), nullptr, r.left + x, r.top + y, r.right - r.left + w, r.bottom - r.top + h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(IDC_PV_AREA, st->rcArea, 0, 0, dx, dy);
    place(IDC_PV_INFO, st->rcInfo, 0, dy, dx, 0);
    place(IDC_PV_HINT, st->rcHint, 0, dy, dx, 0);
    place(IDOK, st->rcOk, dx, dy, 0, 0);
    place(IDCANCEL, st->rcCancel, dx, dy, 0, 0);
    if (st->fit) ApplyFit(dlg, st);
    UpdateAutoLevel(dlg, st);
    InvalidateRect(dlg, nullptr, TRUE);
}

// Shows animation frame `f` (cached result, or computes it).
void SelectFrame(HWND dlg, PreviewState* st, int f) {
    const int n = int(st->shared->frames.size());
    f = std::max(0, std::min(f, n - 1));
    SendDlgItemMessageW(dlg, IDC_PV_FRAME, CB_SETCURSEL, f, 0);
    if (f == st->frame) return;
    st->frame = f;
    auto it = st->cacheKey == SettingsKey(st) ? st->frameCache.find(f) : st->frameCache.end();
    if (it != st->frameCache.end()) {
        st->res = it->second;
        InvalidatePictures(st);
        UpdateAutoLevel(dlg, st);
    } else {
        StartJob(dlg, st);
    }
    UpdateInfo(dlg, st);
    Redraw(dlg);
}

INT_PTR CALLBACK PreviewProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    PreviewState* st = reinterpret_cast<PreviewState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<PreviewState*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, lParam);
        SetWindowTextW(dlg, lang::T("preview.title").c_str());
        const std::pair<int, const char*> labels[] = {
            {IDC_PV_LBL_LEVEL, "preview.level"}, {IDC_PV_AUTO, "preview.auto"}, {IDC_PV_LBL_VIEW, "preview.view"},
            {IDC_PV_LBL_ZOOM, "preview.zoom"}, {IDC_PV_TILE, "preview.tile"}, {IDC_PV_LBL_MODE, "preview.mode"},
            {IDC_PV_LBL_REF, "preview.ref"}, {IDC_PV_LBL_RFILTER, "preview.resize_filter"},
            {IDC_PV_LBL_FILTER, "dlg.mip_filter"}, {IDC_PV_LBL_SHARP, "preview.sharpen"},
            {IDC_PV_LBL_QUALITY, "dlg.dxt_quality"}, {IDC_PV_HINT, "preview.hint"}, {IDOK, "preview.apply"},
            {IDC_PV_SHARP_ALPHA, "dlg.sharpen_alpha"},
            {IDCANCEL, "dlg.cancel"}};
        for (const auto& l : labels) SetDlgItemTextW(dlg, l.first, lang::T(l.second).c_str());
        for (const char* v : {"preview.view_rgb", "preview.view_alpha", "preview.view_checker"}) AddItem(dlg, IDC_PV_VIEW, lang::T(v));
        SendDlgItemMessageW(dlg, IDC_PV_VIEW, CB_SETCURSEL, 0, 0);
        for (const char* v : {"preview.mode_result", "preview.mode_ref", "preview.mode_split", "preview.mode_diff", "preview.mode_chain"})
            AddItem(dlg, IDC_PV_MODE, lang::Tf(v, {{"gain", std::to_wstring(kDiffGain)}}));
        SendDlgItemMessageW(dlg, IDC_PV_MODE, CB_SETCURSEL, 0, 0);
        AddItem(dlg, IDC_PV_ZOOM, lang::T("preview.fit"));
        for (int z : kZoomPresets) AddItem(dlg, IDC_PV_ZOOM, std::to_wstring(z) + L"%");
        for (int f = 0; f < vtf::FILTER_COUNT; ++f) {
            AddItem(dlg, IDC_PV_FILTER, Widen(vtf::FilterName(f)));
            AddItem(dlg, IDC_PV_RFILTER, Widen(vtf::FilterName(f)));
        }
        SendDlgItemMessageW(dlg, IDC_PV_FILTER, CB_SETCURSEL, st->opt.mipFilter, 0);
        SendDlgItemMessageW(dlg, IDC_PV_RFILTER, CB_SETCURSEL, st->opt.resizeFilter, 0);
        for (const char* q : kQualities) AddItem(dlg, IDC_PV_QUALITY, lang::T(q));
        SendDlgItemMessageW(dlg, IDC_PV_QUALITY, CB_SETCURSEL, st->opt.dxtQuality, 0);
        SendDlgItemMessageW(dlg, IDC_PV_SHARP, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(dlg, IDC_PV_SHARP, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(dlg, IDC_PV_SHARP, TBM_SETPOS, TRUE, st->opt.mipSharpen);
        SetSharpLabel(dlg, st->opt.mipSharpen);
        CheckDlgButton(dlg, IDC_PV_SHARP_ALPHA, st->opt.sharpenAlpha ? BST_CHECKED : BST_UNCHECKED);
        FillReference(dlg, st);
        UpdateZoomText(dlg, st);
        // Filters / sharpening only matter when they are used; quality only for DXT.
        uint32_t tw = 0, th = 0;
        vtf::TargetSize(st->shared->w, st->shared->h, st->opt, tw, th);
        const bool resized = st->opt.resize && (tw != st->shared->w || th != st->shared->h);
        EnableWindow(GetDlgItem(dlg, IDC_PV_RFILTER), resized);
        EnableWindow(GetDlgItem(dlg, IDC_PV_LBL_RFILTER), resized);
        EnableWindow(GetDlgItem(dlg, IDC_PV_FILTER), st->opt.mipmaps);
        EnableWindow(GetDlgItem(dlg, IDC_PV_SHARP), st->opt.mipmaps);
        EnableWindow(GetDlgItem(dlg, IDC_PV_SHARP_ALPHA), st->opt.mipmaps);
        const auto isDxt = [](int32_t f) { return f == vtf::FMT_DXT1 || f == vtf::FMT_DXT3 || f == vtf::FMT_DXT5; };
        EnableWindow(GetDlgItem(dlg, IDC_PV_QUALITY), isDxt(st->opt.colorFormat) || isDxt(st->opt.alphaFormat));
        UpdateEnables(dlg, st);
        // Animation: frame list; a still image hides the row and gives the space to the picture.
        {
            const int n = int(st->shared->frames.size());
            SetDlgItemTextW(dlg, IDC_PV_LBL_FRAME, lang::T("preview.frame").c_str());
            for (int f = 0; f < n; ++f)
                AddItem(dlg, IDC_PV_FRAME, lang::Tf("preview.frame_item", {{"n", std::to_wstring(f + 1)}, {"count", std::to_wstring(n)}}));
            SendDlgItemMessageW(dlg, IDC_PV_FRAME, CB_SETCURSEL, 0, 0);
            if (n <= 1) {
                for (int id : {IDC_PV_LBL_FRAME, IDC_PV_FRAME, IDC_PV_PREVFRAME, IDC_PV_NEXTFRAME}) ShowWindow(GetDlgItem(dlg, id), SW_HIDE);
                RECT top = {0, 62, 0, 0}, cur = ChildRect(dlg, IDC_PV_AREA);
                MapDialogRect(dlg, &top);
                SetWindowPos(GetDlgItem(dlg, IDC_PV_AREA), nullptr, cur.left, top.top, cur.right - cur.left, cur.bottom - top.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        const std::pair<int, int> fit[] = {
            {IDC_PV_LBL_LEVEL, IDC_PV_LEVEL}, {IDC_PV_LBL_VIEW, IDC_PV_VIEW}, {IDC_PV_LBL_ZOOM, IDC_PV_ZOOM},
            {IDC_PV_LBL_MODE, IDC_PV_MODE}, {IDC_PV_LBL_REF, IDC_PV_REF}, {IDC_PV_LBL_RFILTER, IDC_PV_RFILTER},
            {IDC_PV_LBL_FILTER, IDC_PV_FILTER}, {IDC_PV_LBL_SHARP, IDC_PV_SHARP}, {IDC_PV_LBL_QUALITY, IDC_PV_QUALITY}};
        for (const auto& f : fit) FitLabel(dlg, f.first, f.second);
        // Remember the layout for resizing.
        RECT rc, rw;
        GetClientRect(dlg, &rc);
        GetWindowRect(dlg, &rw);
        st->initW = rc.right;
        st->initH = rc.bottom;
        st->minTrack = {rw.right - rw.left, rw.bottom - rw.top};
        st->rcArea = ChildRect(dlg, IDC_PV_AREA);
        st->rcInfo = ChildRect(dlg, IDC_PV_INFO);
        st->rcHint = ChildRect(dlg, IDC_PV_HINT);
        st->rcOk = ChildRect(dlg, IDOK);
        st->rcCancel = ChildRect(dlg, IDCANCEL);
        // Centre over the parent.
        HWND parent = GetParent(dlg);
        RECT rp;
        if (parent && GetWindowRect(parent, &rp))
            SetWindowPos(dlg, nullptr, rp.left + ((rp.right - rp.left) - (rw.right - rw.left)) / 2,
                         rp.top + ((rp.bottom - rp.top) - (rw.bottom - rw.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        g_dlg = dlg;
        g_hook = SetWindowsHookExW(WH_GETMESSAGE, MsgHook, nullptr, GetCurrentThreadId());
        StartJob(dlg, st);
        UpdateInfo(dlg, st);
        return TRUE;
    }
    case WM_DESTROY:
        ForgetLabelLayout(dlg);
        if (g_hook) UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_dlg = nullptr;
        KillTimer(dlg, kTimerRecompute);
        break;
    case WM_GETMINMAXINFO:
        if (st && st->minTrack.cx) {
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {st->minTrack.cx, st->minTrack.cy};
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (st && st->initW && wParam != SIZE_MINIMIZED) { Layout(dlg, st, LOWORD(lParam), HIWORD(lParam)); return TRUE; }
        break;
    case WM_PREVIEW_DONE: {
        if (!st) return TRUE;
        std::shared_ptr<const Result> res;
        {
            std::lock_guard<std::mutex> lock(st->shared->mutex);
            if (st->shared->generation == int(wParam)) res = st->shared->result;
        }
        st->busy = false;
        if (res && res->ok) st->frameCache[res->frame] = res;
        if (res && res->frame != st->frame) res = nullptr;  // the frame changed meanwhile
        if (res) {
            if (res->ok) {
                const bool relist = !st->res || st->res->levels.size() != res->levels.size() ||
                                    (!res->levels.empty() && (st->res->levels[0].w != res->levels[0].w || st->res->levels[0].h != res->levels[0].h));
                st->res = res;
                InvalidatePictures(st);
                if (relist) FillLevels(dlg, st);
                if (st->fit || !st->everShown) { st->fit = true; ApplyFit(dlg, st); UpdateZoomText(dlg, st); }
                st->everShown = true;
                UpdateAutoLevel(dlg, st);
            } else {
                st->status = lang::T("preview.unavailable");
            }
        }
        if (st->pending) StartJob(dlg, st);
        UpdateInfo(dlg, st);
        Redraw(dlg);
        return TRUE;
    }
    case WM_DRAWITEM:
        if (st && wParam == IDC_PV_AREA) { Draw(st, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); return TRUE; }
        break;
    case WM_HSCROLL:
        if (st && reinterpret_cast<HWND>(lParam) == GetDlgItem(dlg, IDC_PV_SHARP)) {
            const int v = int(SendDlgItemMessageW(dlg, IDC_PV_SHARP, TBM_GETPOS, 0, 0));
            SetSharpLabel(dlg, v);
            if (v != st->opt.mipSharpen) {
                st->opt.mipSharpen = v;
                SetTimer(dlg, kTimerRecompute, 250, nullptr);  // recompute once the slider rests
            }
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (st && wParam == kTimerRecompute) {
            KillTimer(dlg, kTimerRecompute);
            StartJob(dlg, st);
            UpdateInfo(dlg, st);
            return TRUE;
        }
        break;
    case WM_PV_SPACE:
        if (st && st->space != (wParam != 0)) {
            st->space = wParam != 0;
            UpdateInfo(dlg, st);
            Redraw(dlg);
        }
        return TRUE;
    case WM_ACTIVATE:
        if (st && LOWORD(wParam) == WA_INACTIVE && st->space) { st->space = false; UpdateInfo(dlg, st); Redraw(dlg); }
        break;
    case WM_PV_WHEEL:
        if (st && LevelCount(st)) {
            const POINT pt = ToArea(dlg, lParam, true);
            const double steps = GET_WHEEL_DELTA_WPARAM(wParam) / double(WHEEL_DELTA);
            SetScale(dlg, st, st->scale * std::pow(1.15, steps), pt.x, pt.y);
        }
        return TRUE;
    case WM_PV_ZOOMTEXT:
        if (st) UpdateZoomText(dlg, st);
        return TRUE;
    case WM_SETCURSOR:
        if (st && (reinterpret_cast<HWND>(wParam) == dlg || reinterpret_cast<HWND>(wParam) == GetDlgItem(dlg, IDC_PV_AREA))) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(GetDlgItem(dlg, IDC_PV_AREA), &pt);
            if (st->drag == 2 || NearSplit(dlg, st, pt)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        if (!st) break;
        const POINT pt = ToArea(dlg, lParam, false);
        if (InArea(dlg, pt)) {
            st->drag = NearSplit(dlg, st, pt) ? 2 : 1;
            st->dragFrom = pt;
            st->dragCx = st->cx;
            st->dragCy = st->cy;
            SetCapture(dlg);
            SetFocus(GetDlgItem(dlg, IDC_PV_AREA));
        }
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        if (!st) break;
        const POINT pt = ToArea(dlg, lParam, false);
        if (st->drag == 1) {
            st->cx = st->dragCx - (pt.x - st->dragFrom.x) / st->scale;
            st->cy = st->dragCy - (pt.y - st->dragFrom.y) / st->scale;
            ClampPan(st);
            Redraw(dlg);
        } else if (st->drag == 2) {
            const SIZE a = AreaSize(dlg);
            st->split = std::min(std::max(double(pt.x) / std::max<LONG>(1, a.cx), 0.0), 1.0);
            Redraw(dlg);
        }
        const int h = ChainLevelAt(dlg, st, pt);
        if (h != st->hover) { st->hover = h; UpdateInfo(dlg, st); }
        return TRUE;
    }
    case WM_LBUTTONUP:
        if (st && st->drag) { st->drag = 0; ReleaseCapture(); return TRUE; }
        break;
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wParam)) {
        case IDC_PV_LEVEL:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_LEVEL);
                if (i >= 0) { st->level = i; UpdateInfo(dlg, st); Redraw(dlg); }
            }
            return TRUE;
        case IDC_PV_AUTO:
            st->autoLevel = IsDlgButtonChecked(dlg, IDC_PV_AUTO) == BST_CHECKED;
            UpdateEnables(dlg, st);
            UpdateAutoLevel(dlg, st);
            Redraw(dlg);
            return TRUE;
        case IDC_PV_FRAME:
            if (HIWORD(wParam) == CBN_SELCHANGE) SelectFrame(dlg, st, ComboSel(dlg, IDC_PV_FRAME));
            return TRUE;
        case IDC_PV_PREVFRAME:
            SelectFrame(dlg, st, (st->frame + int(st->shared->frames.size()) - 1) % int(st->shared->frames.size()));
            return TRUE;
        case IDC_PV_NEXTFRAME:
            SelectFrame(dlg, st, (st->frame + 1) % int(st->shared->frames.size()));
            return TRUE;
        case IDC_PV_SHARP_ALPHA:
            st->opt.sharpenAlpha = IsDlgButtonChecked(dlg, IDC_PV_SHARP_ALPHA) == BST_CHECKED;
            StartJob(dlg, st);
            UpdateInfo(dlg, st);
            return TRUE;
        case IDC_PV_TILE:
            st->tile = IsDlgButtonChecked(dlg, IDC_PV_TILE) == BST_CHECKED;
            ClampPan(st);
            Redraw(dlg);
            return TRUE;
        case IDC_PV_VIEW:
            if (HIWORD(wParam) == CBN_SELCHANGE) { st->view = std::max(0, ComboSel(dlg, IDC_PV_VIEW)); Redraw(dlg); }
            return TRUE;
        case IDC_PV_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int prev = st->mode;
                st->mode = std::max(0, ComboSel(dlg, IDC_PV_MODE));
                st->hover = -1;
                if ((prev == MODE_CHAIN) != (st->mode == MODE_CHAIN)) {
                    st->fit = true;
                    ApplyFit(dlg, st);
                    UpdateZoomText(dlg, st);
                }
                UpdateEnables(dlg, st);
                UpdateAutoLevel(dlg, st);
                UpdateInfo(dlg, st);
                Redraw(dlg);
            }
            return TRUE;
        case IDC_PV_REF:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_REF);
                const int browse = st->ref ? 2 : 1;
                if (i == browse) {
                    BrowseReference(dlg, st);
                } else if (i >= 0) {
                    const bool use = i == 1;
                    if (use != st->useRef) {
                        st->useRef = use;
                        st->status.clear();
                        InvalidatePictures(st);
                        if (use && (!st->res || st->res->ref.empty())) StartJob(dlg, st);  // reference not resized yet
                        UpdateInfo(dlg, st);
                        Redraw(dlg);
                    }
                }
            }
            return TRUE;
        case IDC_PV_ZOOM:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_ZOOM);
                const SIZE a = AreaSize(dlg);
                if (i == 0) {
                    st->fit = true;
                    ApplyFit(dlg, st);
                    UpdateAutoLevel(dlg, st);
                    Redraw(dlg);
                } else if (i > 0) {
                    SetScale(dlg, st, kZoomPresets[i - 1] / 100.0, a.cx / 2.0, a.cy / 2.0);
                }
                PostMessageW(dlg, WM_PV_ZOOMTEXT, 0, 0);
            } else if (HIWORD(wParam) == CBN_KILLFOCUS) {
                ParseZoomText(dlg, st);
            }
            return TRUE;
        case IDC_PV_FILTER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_FILTER);
                if (i >= 0 && i != st->opt.mipFilter) { st->opt.mipFilter = i; StartJob(dlg, st); UpdateInfo(dlg, st); }
            }
            return TRUE;
        case IDC_PV_RFILTER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_RFILTER);
                if (i >= 0 && i != st->opt.resizeFilter) { st->opt.resizeFilter = i; StartJob(dlg, st); UpdateInfo(dlg, st); }
            }
            return TRUE;
        case IDC_PV_QUALITY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int i = ComboSel(dlg, IDC_PV_QUALITY);
                if (i >= 0 && i != st->opt.dxtQuality) { st->opt.dxtQuality = i; StartJob(dlg, st); UpdateInfo(dlg, st); }
            }
            return TRUE;
        case IDOK: {
            // Enter in the zoom box applies the zoom instead of closing the window.
            HWND focus = GetFocus(), zoom = GetDlgItem(dlg, IDC_PV_ZOOM);
            if (focus && (focus == zoom || IsChild(zoom, focus))) {
                ParseZoomText(dlg, st);
                return TRUE;
            }
            st->target->mipFilter = st->opt.mipFilter;
            st->target->mipSharpen = st->opt.mipSharpen;
            st->target->sharpenAlpha = st->opt.sharpenAlpha;
            st->target->dxtQuality = st->opt.dxtQuality;
            st->target->resizeFilter = st->opt.resizeFilter;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // namespace

bool ShowPreviewDialog(HWND parent, vtf::Options& opt, const std::vector<std::vector<uint8_t>>& frames, uint32_t width,
                       uint32_t height) {
    if (frames.empty()) return false;
    PreviewState st;
    st.opt = opt;
    st.target = &opt;
    st.shared = std::make_shared<Shared>();
    st.shared->frames = frames;
    st.shared->w = width;
    st.shared->h = height;
    for (const auto& f : frames) {
        for (size_t i = 3; i < f.size() && !st.shared->anyAlpha; i += 4) st.shared->anyAlpha = f[i] != 255;
        if (st.shared->anyAlpha) break;
    }
    const INT_PTR r = DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_VTF_PREVIEW), parent, PreviewProc,
                                      reinterpret_cast<LPARAM>(&st));
    return r == IDOK;
}
