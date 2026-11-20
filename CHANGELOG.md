# Changelog

## 1.2.0

- **DXT quality:** new setting in the *Format* box.
  - **VTFBridge: Accurate** (the new default) and **VTFBridge: Perceptual** — VTFBridge's own encoder:
    stb_dxt's colours refined against the exact palette (so it helps on every GPU alike) and a searched
    DXT5 alpha. Smooth gradients stay smooth: in our gradient test the banding is about half as strong as before.
    *Perceptual* weights errors like the eye; normal maps use *Accurate*.
  - **High (icbc)** / **Best (icbc)** — icbc, the BC1 encoder by Ignacio Castaño (author of NVIDIA
    Texture Tools, MIT); good on detailed textures, but can leave flat 4×4 steps on very smooth gradients.
  - **Fast (stb_dxt)** — the previous encoder.
  - Large images are compressed on all CPU cores.
- **Alpha source:** new *Alpha* list — *Auto* (first alpha channel, else transparency; the list
  shows which), *Layer transparency*, any alpha channel of the document by name, or *None*.
- **Texture preview** (*Preview…* button): every mip level as it will be saved — compressed and
  decoded back, with its accuracy (PSNR and a verbal grade).
  - Compare with the uncompressed mips or **your own image** (PNG, TGA, JPEG, BMP, TIFF, DDS, WebP, VTF):
    reference, split view with a movable line, amplified difference, or hold Space. A VTF reference
    is compared with its own stored mips; the reference never follows the preview's filter settings.
  - Zoom against the full-size texture (switching levels keeps the picture in place), smooth wheel
    zoom around the cursor, *Auto* level by zoom, tiling, and the whole mip chain side by side.
  - Animations: every frame as it will be saved (alpha layers, masks), compared frame by frame with an
    animated VTF reference.
  - Resize filter, mipmap filter, sharpness and DXT quality can be tried before saving.
  - The window can be resized.
- **Mipmap sharpness** (0–100 %) in the *Mipmaps* box: sharpens each smaller mip a little, so
  detail does not wash out in the distance. Colour only by default, *Alpha too* sharpens the alpha as
  well; the full-size image is never changed.
- **Animation alpha as separate layers** (new default): each frame opens as an opaque layer plus a
  hidden grey alpha layer (*Frame 1*, *Frame 1 Alpha*), so the colour under transparent parts stays
  editable; saving pairs them up again. Or layer transparency as before, or ask on every open (with
  *Remember my choice*).
- **File → Automate → VTFBridge: Alpha Layers to Masks:** moves every *Frame N Alpha* layer into the
  layer mask of *Frame N* in one undo step (saving reads masks directly).
- **Settings window** (*Help → About Plug-ins → Valve Texture Format → Settings…*): language, how animations open,
  frame order and the saving defaults (DXT quality, alpha, mipmap filter and sharpness, tiling, .vmt) —
  no need to save a file first.
- The save dialog shows the DXT quality only for DXT formats.

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
