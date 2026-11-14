#!/bin/sh
# Build wland (surya) for iOS 16 arm64 into out/ (ad hoc signed). Runs on macOS with Xcode.
set -e
cd "$(dirname "$0")"
mkdir -p out
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Wno-unused-parameter \
  -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
codesign -f -s - out/wland
ls -la out/wland
