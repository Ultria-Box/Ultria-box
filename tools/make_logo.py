#!/usr/bin/env python3
"""
Ultria logo generator — pure numpy/PIL vectorial render (no AI service needed).
Renders the stylized "U" monogram: neon gradient (cyan -> violet), soft glow,
rounded dark tile, then exports assets/logo.png + assets/icon.ico (multi-size).
"""
import os
import numpy as np
from PIL import Image

S = 1536          # render size (supersampled), downscaled to 1024
OUT = 1024
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")


def make_logo():
    ys, xs = np.mgrid[0:S, 0:S].astype(np.float32)
    scale = S / OUT
    x = xs / scale
    y = ys / scale

    # geometry
    cx, cy = 512.0, 448.0        # arc center
    r = 232.0                    # arc radius (centerline)
    t = 118.0                    # stroke width
    half = t / 2.0
    y_top = 236.0                # top of the legs
    lx, rx = cx - r, cx + r

    # --- U shape SDF (negative inside) ---
    # left leg capsule
    dy_l = np.maximum(y_top - y, 0) + np.maximum(y - cy, 0)
    sdf_l = np.sqrt((x - lx) ** 2 + dy_l ** 2) - half
    # right leg
    dy_r = np.maximum(y_top - y, 0) + np.maximum(y - cy, 0)
    sdf_r = np.sqrt((x - rx) ** 2 + dy_r ** 2) - half
    # bottom arc (lower half; upper half masked far away)
    d_center = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    sdf_arc = np.abs(d_center - r) - half
    sdf_arc = np.where(y >= cy, sdf_arc, 1e6)

    sdf_u = np.minimum(np.minimum(sdf_l, sdf_r), sdf_arc)

    # --- tile SDF (rounded square) ---
    margin = 30.0
    radius = 196.0
    px = np.abs(x - 512.0) - (512.0 - margin)
    py = np.abs(y - 512.0) - (512.0 - margin)
    outside = np.sqrt(np.maximum(px, 0) ** 2 + np.maximum(py, 0) ** 2)
    inside = np.minimum(np.maximum(px, py), 0)
    sdf_tile = outside + inside - radius

    aa = 0.9
    tile_a = np.clip(0.5 - sdf_tile / aa, 0, 1)
    u_a = np.clip(0.5 - sdf_u / aa, 0, 1)

    # --- gradient color field (diagonal cyan -> violet) ---
    g = np.clip(((x - 200) + (y - 200)) / (OUT * 1.15), 0, 1)
    c1 = np.array([0.0, 0.898, 1.0], dtype=np.float32)     # #00e5ff
    c2 = np.array([0.482, 0.184, 1.0], dtype=np.float32)   # #7b2fff
    grad = c1[None, None, :] * (1 - g[..., None]) + c2[None, None, :] * g[..., None]

    # --- background tile ---
    bg_col = np.array([0.071, 0.082, 0.110], dtype=np.float32)  # #12151c
    bg_inner = np.array([0.043, 0.047, 0.063], dtype=np.float32)
    tile_vignette = np.clip(1.0 - sdf_tile / 900.0, 0, 1)[..., None]
    tile_col = bg_col * tile_vignette + bg_inner * (1 - tile_vignette)

    img = tile_col * tile_a[..., None]

    # --- glow behind the U ---
    glow = np.exp(-np.maximum(sdf_u, 0) / 26.0) * 0.42
    img += glow[..., None] * grad * tile_a[..., None]

    # --- core U ---
    img = img * (1 - u_a[..., None]) + grad * u_a[..., None]

    # subtle top highlight line
    hl = np.exp(-np.maximum(sdf_u - 6.0, 0) / 6.0) * np.exp(-((y - 260) / 60.0) ** 2) * 0.35
    img += hl[..., None] * np.array([1.0, 1.0, 1.0], dtype=np.float32) * 0.5 * u_a[..., None]

    arr = np.clip(img, 0, 1)
    im = Image.fromarray((arr * 255).astype(np.uint8), "RGB")

    # downscale with box filter for clean AA
    final = im.resize((OUT, OUT), Image.LANCZOS)
    bg = Image.new("RGB", (OUT, OUT), (10, 12, 16))
    bg.paste(final, (0, 0))
    bg.save(os.path.join(ASSETS, "logo.png"))

    # multi-size ico for the exe
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
    bg.save(os.path.join(ASSETS, "icon.ico"), sizes=sizes)
    print(f"[logo] written {ASSETS}/logo.png and icon.ico")


if __name__ == "__main__":
    os.makedirs(ASSETS, exist_ok=True)
    make_logo()
