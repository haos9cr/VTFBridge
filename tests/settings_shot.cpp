// settings_shot.cpp - screenshots: "about" -> About, then Settings; "ask" -> the question shown when
// an animation with alpha is opened. Usage: settings_shot VTFBridge.8bi <lang> about|ask [file.vtf x]
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include "PIFormat.h"
typedef void (*PluginMainFn)(int16, FormatRecordPtr, intptr_t*, int16*);
static bool g_about;
static void Activate() {  // bring the dialog to the front (active title bar)
    HWND w = nullptr;
    for (int i = 0; i < 100 && !w; ++i) { Sleep(100); w = FindWindowW(L"#32770", nullptr); }
    Sleep(1000);
    if (w) { SetForegroundWindow(w); SendMessageW(w, WM_NCACTIVATE, TRUE, 0); }
}
static DWORD WINAPI Clicker(void*) {
    if (!g_about) { Activate(); return 0; }
    HWND td = nullptr;
    for (int i = 0; i < 100 && !td; ++i) { Sleep(100); td = FindWindowW(L"#32770", nullptr); }
    Sleep(1500);
    SendMessageW(td, TDM_CLICK_BUTTON, 1000, 0);
    Sleep(1500);
    Activate();
    return 0;
}
static FormatRecord g_fr;
int main(int argc, char** argv) {
    HMODULE m = LoadLibraryA(argv[1]);
    PluginMainFn f = (PluginMainFn)GetProcAddress(m, "PluginMain");
    HKEY key; RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\VTFBridge", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    RegSetValueExA(key, "Language", 0, REG_SZ, (const BYTE*)argv[2], DWORD(strlen(argv[2]) + 1));
    DWORD mode = argc > 4 ? 2 : 0; RegSetValueExA(key, "AnimAlpha", 0, REG_DWORD, (BYTE*)&mode, 4);
    RegCloseKey(key);
    g_about = std::strcmp(argv[3], "about") == 0;
    CreateThread(nullptr, 0, Clicker, nullptr, 0, nullptr);
    int16 r = 0; intptr_t d = 0;
    if (g_about) { f(formatSelectorAbout, nullptr, &d, &r); return 0; }
    std::memset(&g_fr, 0, sizeof(g_fr));
    g_fr.HostSupports32BitCoordinates = 1;
    g_fr.dataFork = (intptr_t)CreateFileA(argv[4], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    f(formatSelectorReadPrepare, &g_fr, &d, &r);
    f(formatSelectorReadStart, &g_fr, &d, &r);
    return 0;
}
