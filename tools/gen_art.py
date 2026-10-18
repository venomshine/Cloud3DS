#!/usr/bin/env python3
"""Genera el icono (48x48) y el banner (256x128) de Cloud3DS."""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = (0x3B, 0x9C, 0xFF)
B = (0x7A, 0x5C, 0xFF)

def grad(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = (x / w * 0.6 + y / h * 0.4)
            px[x, y] = tuple(int(A[i] * (1 - t) + B[i] * t) for i in range(3))
    return im

def cloud(d, cx, cy, s, fill):
    # mismas proporciones que el icono vectorial de la app
    def c(x, y, r):
        d.ellipse([cx + x * s - r * s, cy + y * s - r * s, cx + x * s + r * s, cy + y * s + r * s], fill=fill)
    c(-0.18, 0.02, 0.22)
    c(0.08, -0.08, 0.28)
    c(0.28, 0.08, 0.17)
    d.rounded_rectangle([cx - 0.40 * s, cy + 0.02 * s, cx + 0.45 * s, cy + 0.26 * s], radius=0.12 * s, fill=fill)

def icon():
    S = 4  # supermuestreo
    w = 48 * S
    bg = grad(w, w).convert("RGBA")
    mask = Image.new("L", (w, w), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, w - 1], radius=11 * S, fill=255)
    out = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    out.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(out)
    cloud(d, w / 2 - 1 * S, w / 2 + 1 * S, 34 * S, (255, 255, 255, 255))
    # flecha de subida dentro de la nube
    ax, ay = w / 2 + 1 * S, w / 2 + 2.5 * S
    col = (0x55, 0x80, 0xFF, 255)
    d.polygon([(ax, ay - 7 * S), (ax - 6 * S, ay - 1 * S), (ax + 6 * S, ay - 1 * S)], fill=col)
    d.rectangle([ax - 2 * S, ay - 2 * S, ax + 2 * S, ay + 6 * S], fill=col)
    out = out.resize((48, 48), Image.LANCZOS)
    out.save(os.path.join(ROOT, "cia", "icon.png"))

def font(sz, bold=True):
    for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"]:
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()

def banner():
    S = 2
    W, H = 256 * S, 128 * S
    im = grad(W, H).convert("RGBA")
    d = ImageDraw.Draw(im)
    # nubes suaves de fondo
    soft = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(soft)
    cloud(sd, 40 * S, 30 * S, 70 * S, (255, 255, 255, 40))
    cloud(sd, 220 * S, 100 * S, 90 * S, (255, 255, 255, 30))
    soft = soft.filter(ImageFilter.GaussianBlur(2 * S))
    im.alpha_composite(soft)
    d = ImageDraw.Draw(im)
    f1, f2 = font(30 * S), font(13 * S, bold=False)
    t1, t2 = "Cloud3DS", "tu nube local"
    # icono + título centrados como bloque
    ic = 40 * S
    w1 = d.textlength(t1, font=f1)
    block = ic + 8 * S + w1
    x0 = (W - block) / 2
    cy = 54 * S
    cloud(d, x0 + ic / 2, cy, ic, (255, 255, 255, 255))
    d.text((x0 + ic + 8 * S, cy), t1, font=f1, fill=(255, 255, 255, 255), anchor="lm")
    d.text((W / 2, 88 * S), t2, font=f2, fill=(255, 255, 255, 230), anchor="mm")
    im = im.resize((256, 128), Image.LANCZOS)
    im.save(os.path.join(ROOT, "cia", "banner.png"))

if __name__ == "__main__":
    icon()
    banner()
    print("ok")
