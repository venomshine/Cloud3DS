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

if __name__ == "__main__":
    icon()
    banner()
    print("ok")
