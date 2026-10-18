#!/usr/bin/env python3
"""Genera el icono (48x48) y el banner (256x128) de Cloud3DS con el mismo logo que la app:
la nube con el degradado azul cielo -> violeta sobre el azul marino de la interfaz."""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A, B = (0x3B, 0x9C, 0xFF), (0x7A, 0x5C, 0xFF)
NAVY, NAVY2 = (0x0D, 0x12, 0x1C), (0x1B, 0x24, 0x36)

def grad_h(w, h, c0, c1):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for x in range(w):
        t = x / max(1, w - 1)
        c = tuple(int(c0[i] * (1 - t) + c1[i] * t) for i in range(3))
        for y in range(h):
            px[x, y] = c
    return im

def cloud_mask(size, cx, cy, r):
    # misma geometría que draw_cloud() en source/gfx.c
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    def c(x, y, rr):
        d.ellipse([cx + x * r - rr * r, cy + y * r - rr * r, cx + x * r + rr * r, cy + y * r + rr * r], fill=255)
    c(-0.36, 0.04, 0.44)
    c(0.56, 0.16, 0.34)
    c(0.12, -0.18, 0.56)
    d.rounded_rectangle([cx - 0.80 * r, cy + 0.04 * r, cx + 0.90 * r, cy + 0.50 * r], radius=0.23 * r, fill=255)
    return m

def paste_cloud(im, cx, cy, r, glow=True):
    w, h = im.size
    m = cloud_mask((w, h), cx, cy, r)
    if glow:
        g = m.filter(ImageFilter.GaussianBlur(r * 0.35))
        layer = Image.new("RGBA", (w, h), (0x5B, 0x6E, 0xFF, 0))
        layer.putalpha(g.point(lambda v: int(v * 0.45)))
        im.alpha_composite(layer)
    col = grad_h(w, h, A, B).convert("RGBA")
    # el degradado va de un extremo a otro de la nube, no de la imagen
    x0, x1 = int(cx - 0.8 * r), int(cx + 0.9 * r)
    col = grad_h(max(1, x1 - x0), h, A, B).convert("RGBA")
    full = Image.new("RGBA", (w, h))
    full.paste(col, (x0, 0))
    full.paste(Image.new("RGBA", (max(0, x0), h), A + (255,)), (0, 0))
    full.paste(Image.new("RGBA", (max(0, w - x1), h), B + (255,)), (x1, 0))
    full.putalpha(m)
    im.alpha_composite(full)

def bg(w, h):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = (y / h) * 0.7 + (x / w) * 0.3
            px[x, y] = tuple(int(NAVY[i] * (1 - t) + NAVY2[i] * t) for i in range(3)) + (255,)
    return im

def icon():
    S = 4
    w = 48 * S
    im = bg(w, w)
    paste_cloud(im, w / 2 - 1.5 * S, w / 2 + 1 * S, 17 * S)
    mask = Image.new("L", (w, w), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, w - 1], radius=11 * S, fill=255)
    out = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    out.resize((48, 48), Image.LANCZOS).save(os.path.join(ROOT, "cia", "icon.png"))

def font(sz, bold=True):
    for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"]:
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()

def banner():
    S = 2
    W, H = 256 * S, 128 * S
    im = bg(W, H)
    # halo suave detrás del logo
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([W / 2 - 110 * S, 10 * S, W / 2 + 110 * S, 120 * S], fill=(0x4A, 0x6C, 0xFF, 40))
    im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(24 * S)))
    d = ImageDraw.Draw(im)
    f1, f2 = font(30 * S), font(13 * S, bold=False)
    t1, t2 = "Cloud3DS", "tu nube local"
    r = 22 * S
    w1 = d.textlength(t1, font=f1)
    block = 1.7 * r + 10 * S + w1
    x0 = (W - block) / 2
    cy = 54 * S
    paste_cloud(im, x0 + 0.8 * r, cy, r)
    d = ImageDraw.Draw(im)
    d.text((x0 + 1.7 * r + 10 * S, cy + 4 * S), t1, font=f1, fill=(0xEC, 0xF0, 0xF7, 255), anchor="lm")
    d.text((W / 2, 92 * S), t2, font=f2, fill=(0x8B, 0x96, 0xAB, 255), anchor="mm")
    im.resize((256, 128), Image.LANCZOS).convert("RGB").save(os.path.join(ROOT, "cia", "banner.png"))

def logo_png(size, path):
    """Logo suavizado (8x de supermuestreo) para la consola: la nube centrada en su caja real
    (ancho 1.7r, alto 1.24r) con un degradado continuo y un brillo suave arriba."""
    S = 8
    W = size * S
    r = W / 1.86
    cx = W / 2 - 0.05 * r            # centro de la caja de la nube, no del lóbulo
    cy = W / 2 + 0.12 * r
    m = cloud_mask((W, W), cx, cy, r)
    col = Image.new("RGBA", (W, W))
    px = col.load()
    x0, x1 = cx - 0.8 * r, cx + 0.9 * r
    y0, y1 = cy - 0.74 * r, cy + 0.5 * r
    for y in range(W):
        ty = (y - y0) / (y1 - y0)
        hl = max(0.0, 0.22 * (1 - ty * 1.6))   # brillo arriba, se apaga hacia la mitad
        for x in range(W):
            t = min(1, max(0, (x - x0) / (x1 - x0)))
            c = [A[i] * (1 - t) + B[i] * t for i in range(3)]
            c = [int(v + (255 - v) * hl) for v in c]
            px[x, y] = tuple(c) + (255,)
    col.putalpha(m)
    col.resize((size, size), Image.LANCZOS).save(path)

def glow_png(size, path):
    """Resplandor suave con la forma de la nube (para la presentación)."""
    W = size
    r = W / 3.2
    m = cloud_mask((W, W), W / 2 - 0.05 * r, W / 2 + 0.12 * r, r).filter(ImageFilter.GaussianBlur(W / 10))
    g = Image.new("RGBA", (W, W), (0x5B, 0x6E, 0xFF, 0))
    g.putalpha(m.point(lambda v: int(v * 0.9)))
    g.save(path)

def halo_png(size, path):
    """Halo circular: degradado radial que se desvanece hasta transparente."""
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = im.load()
    c = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5 / c
            a = max(0.0, 1 - d) ** 2.2
            px[x, y] = (0x4A, 0x7C, 0xFF, int(255 * a))
    im.save(path)

def sprites():
    g = os.path.join(ROOT, "gfx")
    os.makedirs(g, exist_ok=True)
    for s in (256, 128, 64, 32):
        logo_png(s, os.path.join(g, f"logo{s}.png"))
    glow_png(256, os.path.join(g, "glow.png"))
    halo_png(128, os.path.join(g, "halo.png"))

if __name__ == "__main__":
    icon()
    banner()
    sprites()
    print("ok")
