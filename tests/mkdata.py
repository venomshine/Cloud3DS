#!/usr/bin/env python3
"""Rellena la SD emulada con datos de prueba realistas. Uso: mkdata.py ruta_sdmc"""
import os, random, sys, math
from PIL import Image, ImageDraw, ImageFilter

sd = sys.argv[1]
random.seed(7)
def mk(p): os.makedirs(os.path.join(sd, p.lstrip("/")), exist_ok=True)
def put(p, data):
    full = os.path.join(sd, p.lstrip("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    open(full, "wb").write(data)

PAL = [((40, 120, 255), (140, 70, 255)), ((255, 120, 60), (255, 200, 80)), ((30, 180, 120), (20, 90, 160)),
       ((230, 60, 110), (90, 40, 160)), ((60, 200, 230), (40, 80, 200)), ((250, 210, 60), (230, 90, 40))]
def scene(w, h, k):
    a, b = PAL[k % len(PAL)]
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = y / h
            px[x, y] = tuple(int(a[i] * (1 - t) + b[i] * t) for i in range(3))
    d = ImageDraw.Draw(im)
    for i in range(7):
        x, y, r = random.randint(0, w), random.randint(h // 3, h), random.randint(20, 70)
        d.ellipse([x - r, y - r, x + r, y + r], fill=tuple(min(255, c + 40) for c in b))
    d.rectangle([0, int(h * .78), w, h], fill=(40, 30, 50))
    for i in range(5):
        x = random.randint(10, w - 40)
        d.rectangle([x, int(h * .78) - 30, x + 26, int(h * .78)], fill=(250, 250, 250))
    return im.filter(ImageFilter.SMOOTH)

import io
def png(im):
    b = io.BytesIO(); im.save(b, "PNG"); return b.getvalue()
def jpg(im):
    b = io.BytesIO(); im.save(b, "JPEG", quality=85); return b.getvalue()

for i in range(6):
    put(f"/luma/screenshots/2026-10-1{i}_1{i}-2{i}-0{i}.000_top.png", png(scene(400, 240, i)))
    put(f"/luma/screenshots/2026-10-1{i}_1{i}-2{i}-0{i}.000_bot.png", png(scene(320, 240, i + 3)))
for i in range(4):
    put(f"/DCIM/100NIN03/HNI_00{i+1:02d}.JPG", jpg(scene(640, 480, i + 1)))
for n in ["Checkpoint", "FBI", "Anemone3DS", "Universal-Updater", "GodMode9"]:
    put(f"/3ds/{n}/{n}.3dsx", os.urandom(random.randint(300, 900) * 1024))
put("/3ds/Checkpoint/saves/0x0055D Pokemon Y/2026-10-02 1915/main", os.urandom(450 * 1024))
for n, s in [("Pokemon Mystery Dungeon.cia", 2400), ("Super Mario 3D Land.cia", 3100), ("Theme Shop Pack.cia", 900)]:
    put(f"/cias/{n}", os.urandom(s * 1024))
put("/luma/config.ini", b"[boot]\nautoboot_enabled = 0\n")
put("/boot.firm", os.urandom(200 * 1024))
put("/boot.3dsx", os.urandom(150 * 1024))
put("/Nintendo 3DS/README.txt", b"datos del sistema\n")
put("/notes.txt", "Lista de cosas por hacer:\n- Terminar el capítulo 7\n- Sacar capturas del jefe final\n".encode())
print("datos listos en", sd)
