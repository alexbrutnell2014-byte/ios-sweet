#!/bin/sh
# adspd (surya): the audio DSP from userland. Shares /dev/pmem, SMEM and QMI code with wland (../wifi-surya).
set -e
cd "$(dirname "$0")"
mkdir -p out
W=../wifi-surya
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Wno-unused-parameter -I$W \
  -o out/adspd adspd.c $W/pmem.c $W/smem.c $W/qmi.c
codesign -f -s - out/adspd
ls -la out/adspd
