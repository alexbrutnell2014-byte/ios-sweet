#!/bin/sh
# Phone tools. From ios-flame phone/tools (MIT): netsvc (publishes wland's utun to configd), nettest, lsreg
# (LaunchServices registration, -r rebuilds), psx (processes; -k NAME kills), oslog (unified log), prefs
# (CFPreferences), imgtest (ImageIO decode check), openapp (launch an app), exq (extension records).
# Ours: cmdd (runs commands the PC drops in /var/local/cmd; tools/surya-run.py).
set -e
cd "$(dirname "$0")"
mkdir -p out
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2"
sign() {	# NAME: ad hoc, with NAME-ents.plist when there is one
	if [ -f "$1-ents.plist" ]; then codesign -f -s - --entitlements "$1-ents.plist" "out/$1"; else codesign -f -s - "out/$1"; fi
}
$CC -fobjc-arc -framework Foundation -framework SystemConfiguration -o out/netsvc netsvc.m && sign netsvc
$CC -fobjc-arc -framework Foundation -framework CFNetwork -o out/nettest nettest.m && sign nettest
$CC -Wall -o out/cmdd cmdd.c && sign cmdd
$CC -Wall -o out/psx psx.c && sign psx
$CC -Wall -o out/mvf mvf.c && sign mvf
for m in oslog prefs openapp exq; do $CC -fobjc-arc -framework Foundation -o out/$m $m.m && sign $m; done
$CC -fobjc-arc -framework Foundation -framework MobileCoreServices -o out/lsreg lsreg.m && sign lsreg
$CC -fobjc-arc -framework Foundation -framework ImageIO -framework CoreGraphics -o out/imgtest imgtest.m && sign imgtest
ls -la out
