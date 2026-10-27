#!/usr/bin/env bash
# Builds the iOS/iPadOS app and optionally installs and launches it.
#
#   ios/build.sh device [--run]     a connected iPad, signed with your team
#   ios/build.sh sim    [--run]     the iPad simulator
#   ios/build.sh stage              the game folder to copy into the app (Finder)
#
# One app holds both of the game's executables, the campaign and special ops
# (default.xex) and the multiplayer (default_mp.xex): each is built with its
# own copy of the runtime into a framework, MW2SP and MW2MP, and the app's
# loader (ios/main.m) starts the one the player was last in. Their recompiled
# trees come from the top-level build.sh, run on this Mac for both titles with
# the same VERSION first: TITLE=sp and TITLE=mp, into ppc/ and ppc_mp/ (or
# ppc_tu0/ and ppc_mp_tu0/).
#
#   VERSION=tu6|tu0    the title update's executables or the disc's (tu6)
#   ONLINE=none|lan    system link on this iPad only, or between machines (lan)
#   DIAGNOSTICS=1      build the traces and reports (runtime/diagnostics.h)
#
# Signing settings come from the environment or ios/local.env (git-ignored):
#   MW2_IOS_TEAM=ABCDE12345         Apple development team ID
#   MW2_IOS_BUNDLE_ID=com.you.mw2   bundle identifier
#   MW2_IOS_APP_ID=com.you.mw2.x    the app's own, when not the bundle identifier
#   MW2_IOS_DEVICE=<name|udid>      which iPad, when several are connected
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[ -f ios/local.env ] && . ios/local.env
: "${MW2_IOS_BUNDLE_ID:=io.github.mw2recompiled.ios}"
: "${MW2_IOS_SIMULATOR:=iPad Pro 13-inch (M5)}"
: "${VERSION:=tu6}" "${ONLINE:=lan}" "${DIAGNOSTICS:=0}"
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
export MW2_IOS_TEAM="${MW2_IOS_TEAM:-}" MW2_IOS_BUNDLE_ID

die() { echo "error: $*" >&2; exit 1; }

case $VERSION in
    tu6) mw2=mw2; suffix= ;;
    tu0) mw2=mw2/tu0; suffix=_tu0 ;;
    *)   die "VERSION must be tu6 or tu0" ;;
esac
: "${MW2_IOS_APP_ID:=$MW2_IOS_BUNDLE_ID}"
MW2_IOS_DISPLAY_NAME=MW2
export MW2_IOS_DISPLAY_NAME MW2_IOS_APP_ID

target=${1:-}; shift || true
run=0
for arg in "$@"; do
    case $arg in
    --run) run=1 ;;
    *) die "unknown option $arg" ;;
    esac
done

case $target in
stage)
    # game/ as the app reads it (runtime/install/install.cpp): the two
    # executables this build was made from, beside every fastfile, archive and
    # movie. APFS clones, so staging costs no space. Drag ios-game/game onto
    # the MW2 app in Finder (the iPad's Files tab): devicectl gives up on
    # files this size.
    for xex in default.xex default_mp.xex; do
        [ -f "$mw2/$xex" ] || die "$mw2/$xex is missing: run TITLE=sp and TITLE=mp VERSION=$VERSION ./build.sh first"
    done
    data=$mw2/game
    [ -d "$data" ] || die "$data is missing: build.sh extracts it from the ISO"
    out=ios-game/game
    rm -rf ios-game && mkdir -p "$out"
    cp -c "$mw2/default.xex" "$mw2/default_mp.xex" "$out/"
    # The disc version's folder holds links into mw2/game; -L copies what they name.
    for f in "$data"/*; do cp -cL "$f" "$out/"; done
    echo "Staged $(ls "$out" | wc -l | tr -d ' ') files, $(du -shL "$out" | cut -f1), in $root/$out"
    echo "Copy it in: Finder > your iPad > Files > drag the game folder onto $MW2_IOS_DISPLAY_NAME."
    exit 0
    ;;
sim) sdk=iphonesimulator; prefix=build-iossim; triple=arm64-apple-ios18.0-simulator ;;
device) sdk=iphoneos; prefix=build-ios; triple=arm64-apple-ios18.0 ;;
*) sed -n '2,21p' "$0"; exit 1 ;;
esac

for ppc in ppc$suffix ppc_mp$suffix; do
    [ -f "$ppc/ppc_func_mapping.cpp" ] || die "$ppc/ is missing: run TITLE=sp and TITLE=mp VERSION=$VERSION ./build.sh on this Mac first"
done
[ -x "$LLVM/clang++" ] || die "no clang++ in $LLVM: brew install llvm, or set LLVM"
if [ "$sdk" = iphoneos ] && [ -z "$MW2_IOS_TEAM" ]; then
    die "set MW2_IOS_TEAM (in ios/local.env) to sign for a device"
fi

# The Mac build's fetched sources, when there are any.
fetched=()
for dep in sdl3 ffmpeg_xma imgui; do
    upper=$(echo "$dep" | tr '[:lower:]' '[:upper:]')
    for b in build-mp build build-mp-tu0 build-tu0; do
        if [ -d "$b/_deps/$dep-src" ]; then fetched+=("-DFETCHCONTENT_SOURCE_DIR_$upper=$root/$b/_deps/$dep-src"); break; fi
    done
done
diagnostics=OFF; [ "$DIAGNOSTICS" != 0 ] && diagnostics=ON
sysroot=$sdk
# Every iPad that runs this is an M-series one.
cpu=-mcpu=apple-m1
# What the runtime and its libraries call, linked into each framework.
frameworks=()
for f in AudioToolbox AVFoundation AVFAudio CoreAudio CoreBluetooth CoreFoundation CoreGraphics \
         CoreHaptics CoreMedia CoreMotion CoreVideo Foundation GameController IOSurface Metal \
         QuartzCore UIKit UniformTypeIdentifiers; do
    frameworks+=(-framework "$f")
done
export MW2_IOS_FRAMEWORKS="$root/$prefix-frameworks-$VERSION"
mkdir -p "$MW2_IOS_FRAMEWORKS"
for title in sp mp; do
    build=$prefix-$title-$VERSION
    cmake -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sysroot -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
        -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
        -DCMAKE_AR="$LLVM/llvm-ar" -DCMAKE_RANLIB="$LLVM/llvm-ranlib" \
        -DCMAKE_C_FLAGS=$cpu -DCMAKE_CXX_FLAGS=$cpu -DCMAKE_OBJC_FLAGS=$cpu -DCMAKE_OBJCXX_FLAGS=$cpu \
        -DMW2_TITLE=$title -DMW2_VERSION=$VERSION -DMW2_ONLINE=$ONLINE -DMW2_DIAGNOSTICS=$diagnostics \
        ${fetched[@]+"${fetched[@]}"} > /dev/null
    cmake --build "$build" --target mw2_ios

    # The title as a framework, which exports only its entry point and
    # vkGetInstanceProcAddr, which SDL looks for in the process to find the
    # statically linked MoltenVK: the two runtimes have every other name in
    # common. Xcode signs it as it embeds it.
    name=MW2$(echo "$title" | tr '[:lower:]' '[:upper:]')
    framework="$MW2_IOS_FRAMEWORKS/$name.framework"
    rm -rf "$framework" && mkdir -p "$framework"
    xcrun -sdk $sdk clang++ -target $triple -dynamiclib -o "$framework/$name" \
        -install_name "@rpath/$name.framework/$name" \
        -Wl,-u,_mw2_ios_main -Wl,-exported_symbol,_mw2_ios_main \
        -Wl,-exported_symbol,_vkGetInstanceProcAddr -Wl,-dead_strip \
        "$build/ios/libmw2_ios.a" -liconv "${frameworks[@]}"
    cat > "$framework/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$name</string>
    <key>CFBundleIdentifier</key><string>$MW2_IOS_APP_ID.$title</string>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundlePackageType</key><string>FMWK</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>MinimumOSVersion</key><string>18.0</string>
</dict>
</plist>
PLIST
done

command -v xcodegen > /dev/null || die "xcodegen is not installed (brew install xcodegen)"
# An icon of your own wins when there is one (never committed).
if [ -d ios/LocalAssets.xcassets ]; then
    export MW2_IOS_ASSETS=LocalAssets.xcassets
else
    export MW2_IOS_ASSETS=Assets.xcassets
fi
xcodegen generate --quiet --spec ios/project.yml

derived="$root/build-ios-derived"
xcode_args=(-project ios/MW2.xcodeproj -scheme MW2 -configuration Release
            -sdk "$sdk" -derivedDataPath "$derived" -quiet)
if [ "$sdk" = iphonesimulator ]; then
    xcodebuild "${xcode_args[@]}" -destination "platform=iOS Simulator,name=$MW2_IOS_SIMULATOR" build
    app="$derived/Build/Products/Release-iphonesimulator/MW2.app"
else
    xcodebuild "${xcode_args[@]}" -destination generic/platform=iOS -allowProvisioningUpdates build
    app="$derived/Build/Products/Release-iphoneos/MW2.app"
fi
echo "Built $app"
[ "$run" = 1 ] || exit 0

if [ "$sdk" = iphonesimulator ]; then
    xcrun simctl boot "$MW2_IOS_SIMULATOR" 2> /dev/null || true
    open -a Simulator
    xcrun simctl install "$MW2_IOS_SIMULATOR" "$app"
    exec xcrun simctl launch --console-pty --terminate-running-process "$MW2_IOS_SIMULATOR" "$MW2_IOS_APP_ID"
else
    args=(); [ -n "${MW2_IOS_DEVICE:-}" ] && args=(--device "$MW2_IOS_DEVICE")
    # Installed over a copy that is still running, the app was left refusing
    # to open ("invalid code signature") until installed again: stop it first.
    pid=$(xcrun devicectl device info processes "${args[@]}" 2>/dev/null | awk '/MW2\.app\/MW2/ {print $1; exit}')
    [ -n "$pid" ] && xcrun devicectl device process terminate "${args[@]}" --pid "$pid" > /dev/null 2>&1 || true
    xcrun devicectl device install app "${args[@]}" "$app"
    exec xcrun devicectl device process launch --console --terminate-existing "${args[@]}" "$MW2_IOS_APP_ID"
fi
