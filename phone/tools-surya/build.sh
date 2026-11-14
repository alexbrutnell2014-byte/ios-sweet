#!/bin/sh
# Phone tools from ios-flame phone/tools (MIT): netsvc (publishes wland's utun to configd), nettest.
set -e
cd "$(dirname "$0")"
mkdir -p out
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -fobjc-arc"
$CC -framework Foundation -framework SystemConfiguration -o out/netsvc netsvc.m
codesign -f -s - --entitlements netsvc-ents.plist out/netsvc
if [ -f nettest.m ]; then
  $CC -framework Foundation -framework CFNetwork -o out/nettest nettest.m || echo "nettest: build failed (optional)"
  [ -f out/nettest ] && codesign -f -s - out/nettest
fi
ls -la out
