#!/bin/sh
OUT=$1
URL=$(python3 -c "
import cv2
print(cv2.QRCodeDetector().detectAndDecode(cv2.imread('$OUT/qr.png')[0:240,0:400])[0])")
cd /home/user/tools/tests && node readme_shots.js "$URL" "$OUT"
