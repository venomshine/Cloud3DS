#!/bin/sh
# Decodifica el QR de la captura y lanza la prueba web
OUT=$1
URL=$(python3 -c "
import cv2,sys
img=cv2.imread('$OUT/qr.png')[0:240,0:400]
print(cv2.QRCodeDetector().detectAndDecode(img)[0])")
echo "URL: $URL"
cd /home/user/tools/tests && node web.js "$URL" "$OUT"
