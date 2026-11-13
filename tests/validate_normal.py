"""Mean normal length per mip: ~1 with the Normal Map flag, shorter without (and for SSBump)."""
import io, math, sys
from pathlib import Path
from srctools.vtf import VTF

def lengths(path):
    v = VTF.read(io.BytesIO(Path(path).read_bytes())); v.load()
    out = []
    for m in range(1, v.mipmap_count):
        data = v.get(mipmap=m).to_PIL().convert("RGBA").tobytes()
        ls = [math.sqrt(sum((data[i + c] / 255 * 2 - 1) ** 2 for c in range(3))) for i in range(0, len(data), 4)]
        out.append(sum(ls) / len(ls))
    return out

d = Path(sys.argv[1]); bad = 0
res = {n: lengths(d / f"{n}.vtf") for n in ("plain", "normal", "ssbump")}
for n, ls in res.items():
    print(f"{n:7}", " ".join(f"{x:.3f}" for x in ls))
if not all(abs(x - 1) < 0.02 for x in res["normal"]): bad += 1; print("FAIL: normal-map mips are not unit length")
if not res["plain"][0] < 0.9: bad += 1; print("FAIL: test image is not bumpy enough")
if res["ssbump"] != res["plain"]: bad += 1; print("FAIL: SSBump must not be renormalised")
print("failures:", bad); sys.exit(1 if bad else 0)
