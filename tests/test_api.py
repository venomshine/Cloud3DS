#!/usr/bin/env python3
"""Prueba la API de Cloud3DS contra el emulador. Uso: test_api.py captura_qr.png [host]"""
import hashlib, json, os, sys, time, urllib.parse
import http.client
import cv2

shot = sys.argv[1]
host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
img = cv2.imread(shot)[0:240, 0:400]
data, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
print("QR:", data)
assert data.startswith("http://") and "#k=" in data, "QR no válido"
key = data.split("#k=")[1]
port = 80
fails = 0

def req(method, path, body=None, cookie=None, headers=None, raw=False):
    c = http.client.HTTPConnection(host, port, timeout=60)
    h = dict(headers or {})
    if cookie:
        h["Cookie"] = "c3d=" + cookie
    h.setdefault("User-Agent", "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Safari/604.1")
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    d = r.read()
    hd = dict(r.getheaders())
    c.close()
    if raw:
        return r.status, d, hd
    try:
        return r.status, json.loads(d), hd
    except Exception:
        return r.status, d, hd

def check(name, cond, extra=""):
    global fails
    print(("OK   " if cond else "FALLA"), name, extra)
    if not cond:
        fails += 1

q = urllib.parse.quote
st, d, h = req("GET", "/", raw=True)
check("página principal", st == 200 and h.get("Content-Encoding") == "gzip", f"{st} {len(d)} B")
st, d, _ = req("GET", "/api/hello")
check("hello sin sesión", st == 200 and d["authed"] is False, str(d))
st, d, _ = req("GET", "/api/list?p=/")
check("lista sin sesión → 401", st == 401)
st, d, _ = req("POST", "/api/login", body="pin=0000x", headers={"Content-Type": "application/x-www-form-urlencoded"})
check("PIN incorrecto → 403", st == 403, str(d))
st, d, h = req("POST", "/api/login", body="k=" + key, headers={"Content-Type": "application/x-www-form-urlencoded"})
ck = h.get("Set-Cookie", "")
tok = ck.split("c3d=")[1].split(";")[0] if "c3d=" in ck else None
check("login con el QR", st == 200 and tok and len(tok) == 32, str(d))
st, d, _ = req("GET", "/api/info", cookie=tok)
check("info", st == 200 and d["total"] > 0, str(d))
st, d, _ = req("GET", "/api/list?p=/", cookie=tok)
check("lista raíz", st == 200 and isinstance(d["items"], list), f"{len(d.get('items', []))} elementos")
st, d, _ = req("GET", "/api/list?p=" + q("/../etc"), cookie=tok)
check("ruta con .. → 400", st == 400)

req("POST", "/api/delete?p=" + q("/c3d test"), cookie=tok)
st, d, _ = req("POST", "/api/mkdir?p=" + q("/c3d test"), cookie=tok)
check("crear carpeta con espacio", st == 200, str(d))
st, d, _ = req("POST", "/api/mkdir?p=" + q("/c3d test"), cookie=tok)
check("crear carpeta repetida → 409", st == 409)

blob = os.urandom(3 * 1024 * 1024 + 123)
name = "/c3d test/foto ñandú 日本.bin"
t0 = time.time()
st, d, _ = req("PUT", "/api/upload?p=" + q(name), body=blob, cookie=tok)
dt = time.time() - t0
check("subir 3 MB (UTF-8)", st == 200, f"{st} {d} {dt:.2f}s")
st, d, _ = req("PUT", "/api/upload?p=" + q(name), body=b"x", cookie=tok)
check("subir encima sin permiso → 409", st == 409, str(d))
st, d, _ = req("GET", "/api/list?p=" + q("/c3d test"), cookie=tok)
it = [i for i in d.get("items", []) if i["n"] == "foto ñandú 日本.bin"]
check("aparece en la lista con su tamaño", it and it[0]["s"] == len(blob), str(it))
check("fecha de modificación", it and it[0]["t"] > 1600000000, str(it[0]["t"] if it else None))
st, body, h = req("GET", "/api/file?p=" + q(name) + "&dl=1", cookie=tok, raw=True)
check("descargar completo = mismo md5", st == 200 and hashlib.md5(body).hexdigest() == hashlib.md5(blob).hexdigest(),
      f"{st} {len(body)}")
check("Content-Disposition", "attachment" in h.get("Content-Disposition", ""), h.get("Content-Disposition"))
st, body, h = req("GET", "/api/file?p=" + q(name), cookie=tok, headers={"Range": "bytes=1000-1999"}, raw=True)
check("Range 1000-1999", st == 206 and body == blob[1000:2000], f"{st} {h.get('Content-Range')}")
st, body, h = req("GET", "/api/file?p=" + q(name), cookie=tok, headers={"Range": "bytes=-10"}, raw=True)
check("Range sufijo", st == 206 and body == blob[-10:], f"{st}")
st, d, _ = req("PUT", "/api/upload?p=" + q(name) + "&ow=1", body=b"hola", cookie=tok)
st2, body, _ = req("GET", "/api/file?p=" + q(name), cookie=tok, raw=True)
check("sobrescribir con ow=1", st == 200 and body == b"hola", f"{st} {body[:10]}")
st, d, _ = req("PUT", "/api/upload?p=" + q("/c3d test/sub/a/b.txt") + "&mk=1", body=b"deep", cookie=tok)
check("subir creando carpetas (mk=1)", st == 200, str(d))
st, d, _ = req("PUT", "/api/upload?p=" + q("/c3d test/vacío.txt"), body=b"", cookie=tok)
check("subir archivo vacío", st == 200, str(d))
st, d, _ = req("POST", "/api/rename?p=" + q(name) + "&to=" + q("renombrado.txt"), cookie=tok)
check("renombrar", st == 200, str(d))
st, d, _ = req("POST", "/api/rename?p=" + q("/c3d test/renombrado.txt") + "&to=" + q("a/b"), cookie=tok)
check("renombrar con / → 400", st == 400)
st, d, _ = req("POST", "/api/delete?p=" + q("/luma"), cookie=tok)
check("borrar /luma sin force → 403 system", st == 403 and d.get("error") == "system", str(d))
st, d, _ = req("POST", "/api/delete?p=" + q("/"), cookie=tok)
check("borrar raíz → 400", st == 400)
st, d, _ = req("PUT", "/api/upload?p=" + q("/c3d test/x.bin"), body=None, cookie=tok,
               headers={"Content-Length": str(5 * 1024**3)})
check("archivo de 5 GB → 413", st == 413, str(d))

# subida cortada a medias: no debe quedar el archivo ni el temporal
import socket
s = socket.create_connection((host, port))
s.sendall(("PUT /api/upload?p=" + q("/c3d test/cortado.bin") + " HTTP/1.1\r\nHost: x\r\nCookie: c3d=" + tok +
           "\r\nContent-Length: 2000000\r\n\r\n").encode() + os.urandom(300000))
time.sleep(0.5)
s.close()
time.sleep(1.5)
st, d, _ = req("GET", "/api/list?p=" + q("/c3d test"), cookie=tok)
names = [i["n"] for i in d.get("items", [])]
check("subida cortada no deja restos", "cortado.bin" not in names and not any(n.endswith(".part") for n in names), str(names))

# velocidad aproximada (en el emulador no es la real)
big = os.urandom(20 * 1024 * 1024)
t0 = time.time()
st, d, _ = req("PUT", "/api/upload?p=" + q("/c3d test/big.bin"), body=big, cookie=tok)
up = time.time() - t0
t0 = time.time()
st2, body, _ = req("GET", "/api/file?p=" + q("/c3d test/big.bin") + "&dl=1", cookie=tok, raw=True)
down = time.time() - t0
check("20 MB ida y vuelta", st == 200 and body == big, f"subida {20/up:.1f} MB/s, bajada {20/down:.1f} MB/s")

st, d, _ = req("POST", "/api/delete?p=" + q("/c3d test"), cookie=tok)
check("borrar carpeta entera", st == 200, str(d))
st, d, _ = req("GET", "/api/list?p=" + q("/c3d test"), cookie=tok)
check("ya no existe", st == 404)
print("FALLOS:", fails)
sys.exit(1 if fails else 0)
