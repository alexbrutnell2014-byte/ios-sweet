# Credits for the surya (Poco X3 NFC) additions

Patch 0083 ports these ios-flame patches (https://github.com/NWylynko/ios-flame):
0088 usb, 0089 tools-usb (sweet_poke.c), 0091 usb-files (sweet_files.c), 0094 pmem (sweet_pmem.c),
0102 rpmh-generic. Changes for surya: no RAM log mirror (flame's 0x1fe000000 is past the end of
surya's RAM), no flame watchdog hook.

ios-flame is under the MIT licence:

```
MIT License

Copyright (c) 2026 dipa-studio

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Phone userland: phone/wifi-surya is a port of ios-flame phone/wifi (MIT, notice above): surya modem
region, MSA region, WLAN SMMU stream, WLAN rails and board-data selection.

## Metal (GPU) stack

- `phone/metal2/` and `phone/turnip/` are ios-flame's (MIT): SweetMetal2.bundle (UniXNU's NativeBridge Metal
  frontend, patched for iOS 16), the native Metal bridge (UniXNU, MIT) with its metal2vulkan patches, Turnip phone
  tests and NativeAdrenoABI.h (UniXNU). `tools/build-metal-ios.sh` follows flame's build scripts.
- Fetched at build time, never copied: Mesa (<https://gitlab.freedesktop.org/mesa/mesa>, MIT) at 85fda3b3 with
  UniXNU's patches/mesa at 6c56ece (<https://github.com/UniXNU/UniXNU>), and metal2vulkan
  (<https://github.com/steelbrain/metal2vulkan>, GPL-2.0+ OR LGPL-3.0+) at 720ef1ce.
