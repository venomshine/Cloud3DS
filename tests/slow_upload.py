#!/usr/bin/env python3
"""Sube un archivo a velocidad limitada (como por Wi-Fi real). Uso: slow_upload.py qr.png nombre MB KBps [ua]"""
import sys, time, socket, urllib.parse, http.client, cv2
img = cv2.imread(sys.argv[1])[0:240, 0:400]
url = cv2.QRCodeDetector().detectAndDecode(img)[0]
key = url.split("#k=")[1]
name, mb, kbps = sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
ua = sys.argv[5] if len(sys.argv) > 5 else "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/130.0 Safari/537.36"
c = http.client.HTTPConnection("127.0.0.1", 80)
c.request("POST", "/api/login", body="k=" + key, headers={"Content-Type": "application/x-www-form-urlencoded", "User-Agent": ua})
r = c.getresponse(); r.read()
tok = r.getheader("Set-Cookie").split("c3d=")[1].split(";")[0]
size = int(mb * 1024 * 1024)
s = socket.create_connection(("127.0.0.1", 80))
s.sendall((f"PUT /api/upload?p={urllib.parse.quote(name)}&ow=1 HTTP/1.1\r\nHost: x\r\nUser-Agent: {ua}\r\n"
           f"Cookie: c3d={tok}\r\nContent-Length: {size}\r\n\r\n").encode())
chunk = bytes(8192)
sent, t0 = 0, time.time()
while sent < size:
    n = min(len(chunk), size - sent)
    s.sendall(chunk[:n]); sent += n
    ahead = sent / (kbps * 1024) - (time.time() - t0)
    if ahead > 0: time.sleep(ahead)
print(s.recv(4096).decode(errors="replace").split("\r\n")[0])
s.close()
