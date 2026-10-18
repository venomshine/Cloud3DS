#!/usr/bin/env python3
"""Genera cia/banner.wav: dos campanadas suaves y ascendentes con un soplo de aire.
Estéreo PCM16 a 32728 Hz (frecuencia del DSP, sin remuestreo) y < 3 s.
Nada de senos puros: en el altavoz de la 3DS suenan a timbre de teléfono."""
import math, os, random, struct, wave

SR = 32728
DUR = 2.6
N = int(SR * DUR)
random.seed(3)
L = [0.0] * N
R = [0.0] * N

def bell(t0, f, amp, pan, decay=1.6):
    # FM de campana: portadora + moduladora inarmónica (1.4) y parciales sueltos
    s0 = int(t0 * SR)
    for i in range(s0, N):
        t = (i - s0) / SR
        env = math.exp(-t * decay) * min(1.0, t / 0.004)
        idx = 2.2 * math.exp(-t * 3.0)
        v = math.sin(2 * math.pi * f * t + idx * math.sin(2 * math.pi * f * 1.4 * t))
        v += 0.35 * math.exp(-t * 3.2) * math.sin(2 * math.pi * f * 2.76 * t)
        v += 0.18 * math.exp(-t * 5.0) * math.sin(2 * math.pi * f * 5.4 * t)
        v *= amp * env
        L[i] += v * (1 - pan)
        R[i] += v * pan

def air(t0, length, amp):
    # soplo de aire filtrado (nube) que entra y sale
    s0, s1 = int(t0 * SR), int((t0 + length) * SR)
    lp = 0.0
    for i in range(s0, min(s1, N)):
        x = (i - s0) / (s1 - s0)
        env = math.sin(math.pi * x) ** 2
        lp += 0.06 * (random.uniform(-1, 1) - lp)
        L[i] += lp * amp * env
        R[i] += lp * amp * env * 0.8

air(0.0, 0.9, 0.9)
bell(0.10, 659.25, 0.42, 0.35)   # Mi5
bell(0.32, 987.77, 0.36, 0.65)   # Si5
bell(0.54, 1318.5, 0.22, 0.5, 2.2)  # Mi6, brillo

# eco cruzado
d = int(0.17 * SR)
for i in range(N - 1, d - 1, -1):
    L[i] += 0.28 * R[i - d]
    R[i] += 0.28 * L[i - d]

# fundido final y normalización
peak = max(max(abs(x) for x in L), max(abs(x) for x in R))
fade = int(0.5 * SR)
out = bytearray()
for i in range(N):
    g = 0.8 / peak
    if i > N - fade:
        g *= (N - i) / fade
    out += struct.pack("<hh", int(L[i] * g * 32767), int(R[i] * g * 32767))
path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "cia", "banner.wav")
w = wave.open(path, "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(bytes(out)); w.close()
print("ok", path, f"{DUR:.1f} s")
