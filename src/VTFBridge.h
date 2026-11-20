// VTFBridge.h - shared declarations between plug-in entry point and UI.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "VtfWriter.h"

// Module handle of this .8bi (set in DllMain).
extern HINSTANCE g_hInstance;

// Loads last used options from HKCU\Software\VTFBridge (or defaults).
vtf::Options LoadOptions();
// Stores options to HKCU\Software\VTFBridge.
void SaveOptions(const vtf::Options& opt);
// Default options (Auto DXT1 / DXT5, 7.2, Box mipmaps + thumbnail, no resize).
vtf::Options DefaultOptions();
// Replaces out-of-range values (unknown formats, filters, sizes) with defaults.
vtf::Options SanitizeOptions(vtf::Options opt);

// "<folder of the .8bi>\VTFBridge_lang".
std::wstring LanguageFolder();
// Rescans languages and applies the saved language setting.
void InitLanguage();

// Shows the export options dialog. Returns true on OK.
// visibleLayers: number of visible pixel layers (enables "layers -> animation" when >= 2).
// Alpha channels of the document (names, in order) and what "Auto" uses:
// ALPHA_SOURCE_CHANNEL_FIRST (first channel), ALPHA_SOURCE_TRANSPARENCY or ALPHA_SOURCE_NONE.
struct AlphaInfo {
    std::vector<std::wstring> channels;
    int autoSource = 2;
};
// Reads the document as RGBA8 (width x height) with the given alpha source, for
// the preview. Returns false if Photoshop does not provide the pixels.
// Document pixels for the preview: the frames the save would write (animation frames
// with "layers -> animation", else the merged image), alpha per alphaSource.
using FetchPixelsFn = std::function<bool(int alphaSource, bool animate, int frameOrder, std::vector<std::vector<uint8_t>>& frames)>;

bool ShowOptionsDialog(HWND parent, vtf::Options& opt, int width, int height, int visibleLayers,
                       const AlphaInfo& alpha, const FetchPixelsFn& fetch);

// Mipmap preview window. Changes of mip filter, sharpening and DXT quality are
// written to `opt` on OK. Returns true on OK.
// If a label's text does not fit (translations, fonts, DPI), widens the label and
// moves the control after it to the right, keeping the control's right edge.
// Safe to call again after the text changes (works from the original layout).
void FitLabel(HWND dlg, int labelId, int controlId);
void ForgetLabelLayout(HWND dlg);  // call on WM_DESTROY

bool ShowPreviewDialog(HWND parent, vtf::Options& opt, const std::vector<std::vector<uint8_t>>& frames, uint32_t width,
                       uint32_t height);

// Message boxes (title is localised).
void ShowError(HWND parent, const std::wstring& text);
void ShowWarning(HWND parent, const std::wstring& text);
bool AskYesNo(HWND parent, const std::wstring& text);
void ShowAbout(HWND parent);

// "Settings..." (from About): language, how animations open, saving defaults.
void ShowSettingsDialog(HWND parent);

// Asked when an animation with alpha is opened and the setting is "ask":
// returns 0 = hidden alpha layers, 1 = layer transparency, -1 = cancel.
// `remember` stores the answer as the new setting.
int AskAnimAlpha(HWND parent);
