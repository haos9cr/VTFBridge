// VTFBridgeTools.cpp - automation command in File > Automate (same .8bi):
//   "VTFBridge: Alpha Layers to Masks" - "Frame N Alpha" layers become the masks of "Frame N".
// (Saving reads masks directly, so no command for the other direction is needed.)
// The work is done by a small ExtendScript that the plug-in writes to %TEMP% and runs
// through Photoshop's "AdobeScriptAutomation Scripts" event, as one undo step.
// Registered by the JSON PiPL (VTFBridge.json); built with the Photoshop SDK only.

#include <windows.h>

#include <cstdio>
#include <string>

#include "PIDefines.h"
#include "PIActions.h"
#include "PIActionsPlugin.h"
#include "PIAliasSuite.h"
#include "PIGeneral.h"
#include "SPBasic.h"
#include "SPInterf.h"

#include "Lang.h"
#include "VTFBridge.h"

namespace {

// ASCII-only JavaScript string literal ("..." with \uXXXX for everything else).
std::string JsString(const std::wstring& s) {
    std::string out = "\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += '\\'; out += char(c); }
        else if (c >= 32 && c < 127) out += char(c);
        else { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c)); out += buf; }
    }
    return out + "\"";
}

const char* kScript = R"JS(
var VB_ROOT = this;
function cTID(s) { return charIDToTypeID(s); }
function sTID(s) { return stringIDToTypeID(s); }
function alphaBase(name) {
    var n = name.replace(/\s+$/, "");
    var i = n.lastIndexOf(" ");
    if (i <= 0) return null;
    var s = n.substr(i + 1).toLowerCase();
    if (s != "alpha" && s != "альфа") return null;
    return n.substr(0, i).replace(/\s+$/, "");
}
function collect(container, out) {
    for (var i = 0; i < container.layers.length; i++) {
        var l = container.layers[i];
        if (l.typename == "LayerSet") collect(l, out); else if (l.typename == "ArtLayer") out.push(l);
    }
}
function compositeChannel() {
    return app.activeDocument.mode == DocumentMode.GRAYSCALE ? cTID("Blck") : cTID("Rd  ");
}
function layerChannelRef(layer, channel) {
    var r = new ActionReference();
    r.putEnumerated(cTID("Chnl"), cTID("Chnl"), channel);
    r.putIdentifier(cTID("Lyr "), layer.id);
    return r;
}
function applyImage(srcRef) {
    var d = new ActionDescriptor(), w = new ActionDescriptor();
    w.putReference(cTID("T   "), srcRef);
    d.putObject(cTID("With"), cTID("Clcl"), w);
    executeAction(cTID("AppI"), d, DialogModes.NO);
}
function hasUserMask(layer) {
    var r = new ActionReference();
    r.putIdentifier(cTID("Lyr "), layer.id);
    return executeActionGet(r).getBoolean(sTID("hasUserMask"));
}
function selectMask() {
    var r = new ActionReference();
    r.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("Msk "));
    var d = new ActionDescriptor();
    d.putReference(cTID("null"), r);
    d.putBoolean(cTID("MkVs"), false);
    executeAction(cTID("slct"), d, DialogModes.NO);
}
function addMask() {
    var d = new ActionDescriptor();
    d.putClass(cTID("Nw  "), cTID("Chnl"));
    var r = new ActionReference();
    r.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("Msk "));
    d.putReference(cTID("At  "), r);
    d.putEnumerated(cTID("Usng"), cTID("UsrM"), cTID("RvlA"));
    executeAction(cTID("Mk  "), d, DialogModes.NO);
}

function alphaToMasks() {
    var doc = app.activeDocument, all = [];
    collect(doc, all);
    var frames = [], alphas = [];
    for (var i = 0; i < all.length; i++) {
        var b = alphaBase(all[i].name);
        var isAlpha = false;
        if (b !== null)
            for (var j = 0; j < all.length; j++)
                if (j != i && all[j].name == b) { isAlpha = true; break; }
        if (isAlpha) alphas.push({layer: all[i], base: b, used: false}); else frames.push(all[i]);
    }
    var done = 0, failed = [];
    for (var f = 0; f < frames.length; f++) {
        var frame = frames[f], pair = null;
        for (var k = 0; k < alphas.length && !pair; k++)
            if (!alphas[k].used && alphas[k].base == frame.name) pair = alphas[k];
        if (!pair) continue;
        pair.used = true;
        try {
            if (frame.isBackgroundLayer) frame.isBackgroundLayer = false;
            doc.activeLayer = frame;
            doc.selection.deselect();
            if (hasUserMask(frame)) { selectMask(); } else { addMask(); selectMask(); }
            applyImage(layerChannelRef(pair.layer, compositeChannel()));
            pair.layer.remove();
            done++;
        } catch (e) { failed.push(frame.name + " (" + e.message + ")"); }
    }
    return [done, failed];
}

var VB_RESULT = null;
function VB_main() { VB_RESULT = alphaToMasks(); }

if (!app.documents.length) {
    alert(VB.noDocument);
} else {
    try {
        app.activeDocument.suspendHistory(VB.undo, "VB_main()");
    } catch (e) {
        alert(VB.title + ": " + e.message);
    }
    if (VB_RESULT) {
        var msg = VB_RESULT[0] ? VB.done.replace("{n}", VB_RESULT[0]) : VB.none;
        if (VB_RESULT[1].length) msg += "\n\n" + VB.errors + "\n" + VB_RESULT[1].join("\n");
        alert(msg, VB.title);
    }
}
)JS";

bool WriteScript(const std::wstring& path) {
    std::string js = "// VTFBridge: written by the plug-in, safe to delete.\n#target photoshop\nvar VB = {\n";
    js += "  title: " + JsString(lang::T("tool.to_masks")) + ",\n";
    js += "  undo: " + JsString(lang::T("tool.to_masks")) + ",\n";
    js += "  noDocument: " + JsString(lang::T("tool.no_document")) + ",\n";
    js += "  none: " + JsString(lang::T("tool.to_masks_none")) + ",\n";
    js += "  done: " + JsString(lang::T("tool.to_masks_done")) + ",\n";
    js += "  errors: " + JsString(lang::T("tool.errors")) + "\n};\n";
    js += kScript;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(js.data(), 1, js.size(), f) == js.size();
    std::fclose(f);
    return ok;
}

// Runs the script file through the "AdobeScriptAutomation Scripts" event.
OSErr RunScript(SPBasicSuite* sp, const std::wstring& path) {
    PSActionControlProcs* control = nullptr;
    PSActionDescriptorProcs* desc = nullptr;
    PSAliasSuite* alias = nullptr;
    OSErr err = noErr;
    if (sp->AcquireSuite(kPSActionControlSuite, kPSActionControlSuiteVersion, (const void**)&control) || !control) return -1;
    if (sp->AcquireSuite(kPSActionDescriptorSuite, kPSActionDescriptorSuiteVersion, (const void**)&desc) || !desc) {
        sp->ReleaseSuite(kPSActionControlSuite, kPSActionControlSuiteVersion);
        return -2;
    }
    if (sp->AcquireSuite(kPSAliasSuite, kPSAliasSuiteVersion, (const void**)&alias) || !alias) {
        sp->ReleaseSuite(kPSActionDescriptorSuite, kPSActionDescriptorSuiteVersion);
        sp->ReleaseSuite(kPSActionControlSuite, kPSActionControlSuiteVersion);
        return -3;
    }
    DescriptorEventID event = 0;
    PIActionDescriptor d = nullptr, result = nullptr;
    AliasHandle file = nullptr;
    err = control->StringIDToTypeID("AdobeScriptAutomation Scripts", &event);
    if (!err) err = desc->Make(&d);
    if (!err) err = alias->WinNewAliasFromWidePath(reinterpret_cast<const uint16*>(path.c_str()), &file);
    if (!err) err = desc->PutAlias(d, 'jsCt', reinterpret_cast<Handle>(file));
    if (!err) err = desc->PutString(d, 'jsMs', "undefined");
    if (!err) err = control->Play(&result, event, d, plugInDialogSilent);
    if (result) desc->Free(result);
    if (d) desc->Free(d);
    sp->ReleaseSuite(kPSAliasSuite, kPSAliasSuiteVersion);
    sp->ReleaseSuite(kPSActionDescriptorSuite, kPSActionDescriptorSuiteVersion);
    sp->ReleaseSuite(kPSActionControlSuite, kPSActionControlSuiteVersion);
    return err;
}

SPErr RunTool(const char* caller, const char* selector, void* message) {
    SPMessageData* basic = static_cast<SPMessageData*>(message);
    SPBasicSuite* sp = basic ? basic->basic : nullptr;
    if (!sp) return kSPBadParameterError;
    if (sp->IsEqual(caller, kSPInterfaceCaller) && sp->IsEqual(selector, kSPInterfaceAboutSelector)) {
        ShowAbout(nullptr);
        return kSPNoError;
    }
    if (!sp->IsEqual(caller, kPSPhotoshopCaller) || !sp->IsEqual(selector, kPSDoIt)) return kSPNoError;

    InitLanguage();
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"VTFBridge_alpha_to_masks.jsx";
    OSErr err = WriteScript(path) ? RunScript(sp, path) : OSErr(-4);
    if (err) {
        const std::wstring text = lang::Tf("tool.script_error", {{"code", std::to_wstring(err)}});
        MessageBoxW(GetActiveWindow(), text.c_str(), lang::T("msg.title").c_str(), MB_OK | MB_ICONWARNING);
    }
    return kSPNoError;
}

} // namespace

extern "C" __declspec(dllexport) SPAPI SPErr VTFBridgeAlphaToMasks(const char* caller, const char* selector, void* message) {
    return RunTool(caller, selector, message);
}
