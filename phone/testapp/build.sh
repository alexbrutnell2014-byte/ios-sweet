#!/bin/sh
# SweetHello: a tiny UIKit app (Swift) to test installing third-party apps on the port. Ad-hoc signed like a
# re-signed IPA would be. Output: out/SweetHello.ipa
set -e
cd "$(dirname "$0")"
rm -rf out && mkdir -p out/Payload/SweetHello.app
xcrun -sdk iphoneos swiftc -target arm64-apple-ios16.0 -O -o out/Payload/SweetHello.app/SweetHello main.swift
plutil -convert binary1 -o out/Payload/SweetHello.app/Info.plist Info.plist
printf 'APPL????' > out/Payload/SweetHello.app/PkgInfo
codesign -f -s - out/Payload/SweetHello.app
codesign -dvvv out/Payload/SweetHello.app 2>&1 | head -20
(cd out && zip -qry SweetHello.ipa Payload)
ls -la out
