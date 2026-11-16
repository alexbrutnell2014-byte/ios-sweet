#!/bin/sh
# Build the surya Metal stack for iOS 16 arm64 on a macOS host (GitHub Actions: .github/workflows/metal.yml):
#   1. Turnip (Mesa's Vulkan driver for Adreno) with UniXNU's darwin kernel backend, which talks to the kernel's
#      SweetAccelerator v2 user client through phone/turnip/NativeAdrenoABI.h
#   2. the native Metal bridge (phone/metal2/bridge: UniXNU's bridge over the metal2vulkan runtime) as a static lib
#   3. SweetMetal2.bundle (phone/metal2: Metal frontend + bridge + Turnip), plus the phone tests v2test, vktest, m2vtest
# All from ios-flame (MIT), whose tools/build-turnip-ios.sh and build-m2v-bridge.sh this follows. Upstream sources
# are fetched at their pins, never copied: Mesa 85fda3b3 + UniXNU 6c56ece's patches/mesa (in their order),
# metal2vulkan 720ef1ce + phone/metal2/bridge/patches.
#   tools/build-metal-ios.sh WORKDIR        (output: phone/metal2/out, phone/turnip/out)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$ROOT/work-metal}
mkdir -p "$W"
UNIXNU=6c56ece
MESA=85fda3b36405bcaea8bffc64aa4f2651dc90770d
M2V=720ef1ce
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
IOSCC="$(xcrun --sdk iphoneos -f clang) -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0"

echo "== sources"
if [ ! -d "$W/unixnu" ]; then
	git clone -q --filter=blob:none --no-checkout https://github.com/UniXNU/UniXNU "$W/unixnu"
fi
UC=$(git -C "$W/unixnu" rev-parse "$UNIXNU^{commit}")
if [ ! -d "$W/mesa/.git" ]; then
	mkdir -p "$W/mesa" && git -C "$W/mesa" init -q
	git -C "$W/mesa" fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $MESA ||
		git -C "$W/mesa" fetch -q --depth 1 https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa $MESA
	git -C "$W/mesa" checkout -q FETCH_HEAD
	for p in $(git -C "$W/unixnu" show "$UC:scripts/data/adreno-compiler-inputs.json" | python3 -c 'import json,sys; print("\n".join(json.load(sys.stdin)["patches"]))'); do
		echo "mesa: $p"
		git -C "$W/unixnu" show "$UC:$p" | git -C "$W/mesa" apply --whitespace=nowarn
	done
	for p in "$ROOT"/phone/turnip/patches/*.patch; do
		echo "mesa: $(basename "$p")"
		git -C "$W/mesa" apply --whitespace=nowarn "$p"
	done
fi
if [ ! -d "$W/metal2vulkan/.git" ]; then
	git clone -q https://github.com/steelbrain/metal2vulkan "$W/metal2vulkan"
	git -C "$W/metal2vulkan" checkout -q $M2V
	for p in "$ROOT"/phone/metal2/bridge/patches/*.patch; do
		echo "metal2vulkan: $(basename "$p")"
		git -C "$W/metal2vulkan" apply --whitespace=nowarn "$p"
	done
fi

echo "== turnip"
T=$W/turnip
mkdir -p "$T"
cat > "$T/ios16.cross" <<EOF
[binaries]
c = '$(xcrun --sdk iphoneos --find clang)'
cpp = '$(xcrun --sdk iphoneos --find clang++)'
objc = '$(xcrun --sdk iphoneos --find clang)'
ar = '$(xcrun --sdk iphoneos --find ar)'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
cpp_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
objc_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
c_link_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
cpp_link_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
objc_link_args = ['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']
EOF
opts="-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin -Dtu-darwin-abi-dir=$ROOT/phone/turnip
 -Dplatforms=macos -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
 -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dtools= -Dbuild-tests=false
 -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dexpat=disabled
 -Dzstd=disabled -Dbuildtype=release -Db_ndebug=true"
(
	unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
	export CC="$(xcrun --sdk iphoneos --find clang)" CXX="$(xcrun --sdk iphoneos --find clang++)"
	[ -f "$T/build-ios/build.ninja" ] || meson setup "$T/build-ios" "$W/mesa" --wrap-mode=nodownload --cross-file "$T/ios16.cross" $opts
	ninja -C "$T/build-ios" src/freedreno/vulkan/libvulkan_freedreno.dylib
)
LIB=$T/build-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
nm -gU "$LIB" | grep -q " T _vkGetInstanceProcAddr" || { echo "no vkGetInstanceProcAddr export" >&2; exit 1; }
echo "weak imports (resolve to NULL on iOS 16):"; nm -m "$LIB" | grep "weak external" || true

echo "== metal2vulkan bridge"
B=$W/m2v-bridge
mkdir -p "$B"
LLVM_AS=$(brew --prefix llvm)/bin/llvm-as
"$LLVM_AS" -o "$B/runtime_examples.air" "$W/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
"$LLVM_AS" -o "$B/function_constant.air" "$W/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
cat > "$B/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$ROOT/phone/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$W/metal2vulkan/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$W/metal2vulkan/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF
NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air IPHONEOS_DEPLOYMENT_TARGET=16.0 \
	cargo +1.98.0 build --release --manifest-path "$B/Cargo.toml" --target aarch64-apple-ios --target-dir "$B/target"
BRIDGE=$B/target/aarch64-apple-ios/release/libnative_metal_bridge.a

echo "== SweetMetal2.bundle"
cd "$ROOT/phone/metal2"
O=out/SweetMetal2.bundle
rm -rf out && mkdir -p "$O"
$IOSCC -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-function -bundle \
	-o "$O/SweetMetal2" SweetMetal2.mm NativeBridgeMetalFrontend.mm -x none "$BRIDGE" -Wl,-dead_strip -lc++ \
	-framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation
# clang -g leaves the dSYM inside the bundle: move it out before signing
rm -rf out/SweetMetal2.dSYM; mv "$O/SweetMetal2.dSYM" out/ 2>/dev/null || dsymutil "$O/SweetMetal2" -o out/SweetMetal2.dSYM 2>/dev/null || true
strip -x "$O/SweetMetal2"
cp Info.plist "$O/Info.plist"
cp "$LIB" "$O/libvulkan_freedreno.dylib"
strip -x "$O/libvulkan_freedreno.dylib"
codesign -f -s - "$O/libvulkan_freedreno.dylib"
codesign -f -s - "$O"
$IOSCC -O2 -Wall -o out/m2vtest m2vtest.c "$BRIDGE" -Wl,-dead_strip
strip -x out/m2vtest && codesign -f -s - out/m2vtest
echo "bundle weak imports:"; nm -m "$O/SweetMetal2" | grep "weak external" | grep -v "dynamically looked up" || true

echo "== phone tests"
cd "$ROOT/phone/turnip"
rm -rf out && mkdir -p out
: > out/shaders.h
for s in fill.comp:fill_comp tri.vert:tri_vert tri.frag:tri_frag full.vert:full_vert half.frag:half_frag; do
	glslangValidator -V --target-env vulkan1.2 --vn "${s#*:}" -o out/"${s#*:}".h shaders/"${s%%:*}" >/dev/null
	cat out/"${s#*:}".h >> out/shaders.h
done
$IOSCC -O2 -Wall -I out -I "$W/mesa/include" -o out/vktest vktest.c
$IOSCC -x c++ -std=c++17 -O2 -Wall -o out/v2test v2test.cpp -framework IOKit -framework CoreFoundation
cp "$LIB" out/libvulkan_freedreno.dylib
for f in out/vktest out/v2test out/libvulkan_freedreno.dylib; do codesign -f -s - "$f"; done
ls -la "$ROOT/phone/metal2/out" "$ROOT/phone/metal2/out/SweetMetal2.bundle" "$ROOT/phone/turnip/out"
