"""Without "tiling", the left-edge stripe must not leak into the right quarter of any
mipmap (8 pixels wide and larger; on smaller mips every filter legitimately spreads
across the whole image). With "tiling", smooth filters must reach the right edge."""
import io, sys
from pathlib import Path
from srctools.vtf import VTF

def right_alpha(path):
    v = VTF.read(io.BytesIO(Path(path).read_bytes())); v.load()
    res = []
    for m in range(1, v.mipmap_count):
        img = v.get(mipmap=m).to_PIL().convert("RGBA")
        w, h = img.size
        if w < 8: break
        a = img.getchannel("A")
        res.append(max(a.getpixel((x, y)) for y in range(h) for x in range(w * 3 // 4, w)))
    return res

d = Path(sys.argv[1]); bad = 0; tiled_leaks = 0
for f in sorted(d.glob("*_clamp.vtf")):
    name = f.stem[:-6]
    clamp = right_alpha(f)
    tile = right_alpha(d / f"{name}_tile.vtf")
    print(f"{name:12} clamp max right-quarter alpha per mip: {clamp}   tiling: {tile}")
    if max(clamp) > 0:
        bad += 1; print(f"FAIL: {name}: stripe leaked to the right edge without tiling")
    if max(tile) > 0: tiled_leaks += 1
if tiled_leaks == 0:
    bad += 1; print("FAIL: tiling has no effect")
print("failures:", bad); sys.exit(1 if bad else 0)
