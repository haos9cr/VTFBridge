# Changelog

## 1.1.1

- **Fixed: pixels from the opposite edge in mipmaps.** Mipmaps and resizing treated every texture
  as tiling unless *Clamp S* / *Clamp T* was set, so e.g. an opaque stripe at the left edge of a
  decal appeared at the right edge of its mipmaps. Edges are now clamped by default (like VTFEdit);
  the new option **Tiling texture** in the *Mipmaps* box turns wrap-around back on for textures
  that repeat. It is remembered, and switched off when re-saving a VTF clamped in both directions.

## 1.1.0

- **Normal maps:** with the *Normal Map* flag the mipmaps are renormalized, so the bumps no longer
  flatten out in the distance (gamma-correct filtering is skipped for normal maps).
- **Optional `.vmt`** next to the saved texture: shader choice, `$basetexture` from the path after
  `materials`, `$alphatest` / `$translucent` from the alpha, `AnimatedTexture` proxy for animations.
  An existing `.vmt` is never overwritten.
- The save dialog shows the estimated file size.
- Tooltips for the texture flags and the material options.
- *About* has a "Check for updates" link (opens the releases page; the plug-in itself does not go online).
- Fixed: line breaks shown as `\n` in the Russian "not a power of two" message;
  `tools/check_lang.py` now reports this mistake.
- Renamed from **VTF Export** to **VTFBridge** (the plug-in opens files too, not only exports them).
  The file is now `VTFBridge.8bi` — delete the old `VTFExport.8bi` from the Plug-ins folder.
- Settings are moved automatically from `HKCU\Software\VTFExport` to `HKCU\Software\VTFBridge`.
- Translations: the folder is now `VTFBridge_lang`; an existing `VTFExport_lang` folder is still
  used while `VTFBridge_lang` does not exist.

## 1.0.0

First public release.

- Open VTF 7.0 – 7.5 in almost every format; alpha as an alpha channel.
- Animated textures open as layers (frame order: bottom-up or top-down).
- Save DXT1/3/5, BGR(A)8888, RGB(A)8888, 565/4444/5551, I8, IA88, A8; *Auto* formats pick
  DXT1/DXT5 (or BGR888/BGRA8888) by alpha. Versions 7.1 – 7.5.
- Visible layers → animation frames.
- Mipmaps with 15 filters, optional gamma-correct filtering; 16×16 DXT1 thumbnail.
- Resize to a power of two with filter and max size.
- Texture flags; one/eight-bit alpha flags set automatically.
- Re-saving an opened VTF keeps its format, version, mipmaps, flags and frame order.
- Interface in English, Russian and German; translations embedded from `lang/` and loaded from
  the `VTFExport_lang` folder next to the plug-in.
