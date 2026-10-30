#!/bin/bash
# Build an isolated Apple Silicon desktop app, or launch it under LLDB.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-macos"
runtime="$build/runtime"
app="$runtime/MeloCafe Debug.app"
mode="${1:-build}"
if [[ $# -gt 0 ]]; then shift; fi

case "$mode" in
build)
    ninja="$(command -v ninja || true)"
    if [[ -z "$ninja" ]]; then
        for candidate in "$root"/dependencies/vcpkg/downloads/tools/ninja-*-osx/ninja; do
            if [[ -x "$candidate" ]]; then ninja="$candidate"; fi
        done
    fi
    if [[ -z "$ninja" ]]; then
        echo "Install Ninja or set it on PATH before building." >&2
        exit 1
    fi
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DVCPKG_TARGET_TRIPLET=arm64-osx -DVCPKG_MANIFEST_DIR="$root" \
        -DVCPKG_MANIFEST_FEATURES=desktop \
        -DENABLE_WXWIDGETS=ON -DENABLE_METAL=ON \
        -DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF \
        -DENABLE_DISCORD_RPC=OFF -DENABLE_LIBUSB=OFF -DENABLE_HIDAPI=OFF \
        -DBUILD_HEADLESS_DYLIB=OFF -DMACOS_BUNDLE=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,-object_path_lto,$build/Cemu.lto.o"
    cmake --build "$build" --parallel "${BUILD_JOBS:-8}"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/SharedSupport" "$runtime/portable"
    cp "$root/bin/Cemu" "$app/Contents/MacOS/Cemu"
    rsync -a "$root/bin/resources" "$root/bin/gameProfiles" "$app/Contents/SharedSupport/"
    xcrun dsymutil "$root/bin/Cemu" -o "$runtime/Cemu.dSYM"
    cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Cemu</string>
<key>CFBundleIdentifier</key><string>cafe.melo.desktop-debug</string>
<key>CFBundleName</key><string>MeloCafe Debug</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
    cat > "$build/debug.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.get-task-allow</key><true/>
<key>com.apple.security.cs.allow-jit</key><true/>
</dict></plist>
PLIST
    codesign --force --sign - --entitlements "$build/debug.entitlements" "$app"
    ;;
debug)
    cd "$root"
    exec xcrun lldb -O 'settings set show-progress false' \
        -O 'settings set show-statusline false' \
        -o "target symbols add \"$runtime/Cemu.dSYM\"" \
        -o 'settings set target.env-vars MTL_CAPTURE_ENABLED=1' \
        -- "$app/Contents/MacOS/Cemu" --verbose "$@"
    ;;
run)
    exec "$app/Contents/MacOS/Cemu" --verbose "$@"
    ;;
*)
    echo "Usage: $0 {build|debug|run} [Cemu launch arguments]" >&2
    exit 2
    ;;
esac
