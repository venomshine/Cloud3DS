#!/usr/bin/env python3
"""Saca de romfs/certs/cacert.pem (raíces de Mozilla) las raíces RSA más usadas y las guarda en
romfs/certs/httpc_roots.bin para el servicio HTTP del sistema (httpc), que no trae las modernas.
Formato: por cada una, su tamaño (u32, little-endian) y el certificado en DER."""
import base64, os, re, struct, sys

ROOTS = [
    "ISRG Root X1",                       # Let's Encrypt
    "DigiCert Global Root CA",
    "DigiCert Global Root G2",            # GitHub (descargas de releases), Microsoft…
    "DigiCert TLS RSA4096 Root G5",
    "Baltimore CyberTrust Root",
    "GlobalSign Root CA",
    "GlobalSign Root CA - R3",
    "GlobalSign Root CA - R6",
    "Amazon Root CA 1",
    "GTS Root R1",                        # Google
    "Go Daddy Root Certificate Authority - G2",
    "Starfield Root Certificate Authority - G2",
    "Microsoft RSA Root Certificate Authority 2017",
    "Certum Trusted Network CA",
    "Entrust Root Certification Authority - G2",
    "SSL.com Root Certification Authority RSA",
    "Sectigo Public Server Authentication Root R46",
    "USERTrust RSA Certification Authority",
]

here = os.path.dirname(os.path.abspath(__file__))
pem = open(os.path.join(here, "..", "romfs", "certs", "cacert.pem"), encoding="utf-8").read()
blocks = re.findall(r"# (.+?)\n-----BEGIN CERTIFICATE-----\n(.+?)-----END CERTIFICATE-----", pem, re.S)
found = {name.strip(): base64.b64decode(b) for name, b in blocks}
out = b""
for r in ROOTS:
    if r not in found:
        print("no está en el paquete:", r, file=sys.stderr)
        continue
    der = found[r]
    out += struct.pack("<I", len(der)) + der
    print("%-48s %5d bytes" % (r, len(der)))
open(os.path.join(here, "..", "romfs", "certs", "httpc_roots.bin"), "wb").write(out)
print("total", len(out), "bytes")
