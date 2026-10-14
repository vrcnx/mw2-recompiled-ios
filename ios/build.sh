#!/usr/bin/env bash
# Builds the iOS/iPadOS app and optionally installs and launches it.
#
#   ios/build.sh device [--run]     a connected iPad, signed with your team
#   ios/build.sh sim    [--run]     the iPad simulator
#   ios/build.sh stage              the game folder to copy into the app (Finder)
#
# The recompiled tree comes from the top-level build.sh, run on this Mac with
# the same TITLE and VERSION first: it extracts the disc, applies the title
# update and recompiles the executable into ppc_mp/ (or ppc/, ppc_tu0/...).
#
#   TITLE=mp|sp        which executable (mp, the multiplayer, by default)
#   VERSION=tu6|tu0    the title update's executable or the disc's (tu6)
#   ONLINE=none|lan    system link on this iPad only, or between machines (lan)
#   DIAGNOSTICS=1      build the traces and reports (runtime/diagnostics.h)
#
# Signing settings come from the environment or ios/local.env (git-ignored):
#   MW2_IOS_TEAM=ABCDE12345         Apple development team ID
#   MW2_IOS_BUNDLE_ID=com.you.mw2   bundle identifier
#   MW2_IOS_DEVICE=<name|udid>      which iPad, when several are connected
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[ -f ios/local.env ] && . ios/local.env
: "${MW2_IOS_BUNDLE_ID:=io.github.mw2recompiled.ios}"
: "${MW2_IOS_SIMULATOR:=iPad Pro 13-inch (M5)}"
: "${TITLE:=mp}" "${VERSION:=tu6}" "${ONLINE:=lan}" "${DIAGNOSTICS:=0}"
LLVM=${LLVM:-/opt/homebrew/opt/llvm/bin}
export MW2_IOS_TEAM="${MW2_IOS_TEAM:-}" MW2_IOS_BUNDLE_ID

die() { echo "error: $*" >&2; exit 1; }

case $TITLE in
    mp) ppc=ppc_mp; xex=default_mp.xex ;;
    sp) ppc=ppc;    xex=default.xex ;;
    *)  die "TITLE must be mp or sp" ;;
esac
case $VERSION in
    tu6) mw2=mw2 ;;
    tu0) mw2=mw2/tu0; ppc=${ppc}_tu0 ;;
    *)   die "VERSION must be tu6 or tu0" ;;
esac
# One build per title and version; one app per title, so the multiplayer and
# the campaign sit side by side on the home screen.
export MW2_IOS_TITLE=$TITLE-$VERSION
if [ "$TITLE" = mp ]; then MW2_IOS_DISPLAY_NAME="MW2 Multiplayer"; else MW2_IOS_DISPLAY_NAME="MW2 Campaign"; fi
MW2_IOS_APP_ID=$MW2_IOS_BUNDLE_ID.$TITLE
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
    # game/ as the app reads it (runtime/install/install.cpp): the executable
    # this build was made from, beside every fastfile, archive and movie.
    # APFS clones, so staging costs no space. Drag ios-game/game onto the MW2
    # app in Finder (the iPad's Files tab): devicectl gives up on files this size.
    [ -f "$mw2/$xex" ] || die "$mw2/$xex is missing: run TITLE=$TITLE VERSION=$VERSION ./build.sh first"
    data=$mw2/game
    [ -d "$data" ] || die "$data is missing: build.sh extracts it from the ISO"
    out=ios-game/game
    rm -rf ios-game && mkdir -p "$out"
    cp -c "$mw2/$xex" "$out/"
    # The disc version's folder holds links into mw2/game; -L copies what they
    # name. The multiplayer plays no movies (docs/multiplayer.md), so its
    # folder goes without the 1.3 GB of them.
    for f in "$data"/*; do
        [ "$TITLE" = mp ] && [ "${f##*.}" = bik ] && continue
        cp -cL "$f" "$out/"
    done
    echo "Staged $(ls "$out" | wc -l | tr -d ' ') files, $(du -shL "$out" | cut -f1), in $root/$out"
    echo "Copy it in: Finder > your iPad > Files > drag the game folder onto $MW2_IOS_DISPLAY_NAME."
    exit 0
    ;;
sim) sdk=iphonesimulator; build=build-iossim-$TITLE-$VERSION ;;
device) sdk=iphoneos; build=build-ios-$TITLE-$VERSION ;;
*) sed -n '2,21p' "$0"; exit 1 ;;
esac

[ -f "$ppc/ppc_func_mapping.cpp" ] || die "$ppc/ is missing: run TITLE=$TITLE VERSION=$VERSION ./build.sh on this Mac first"
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
cmake -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sysroot -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
    -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
    -DCMAKE_AR="$LLVM/llvm-ar" -DCMAKE_RANLIB="$LLVM/llvm-ranlib" \
    -DCMAKE_C_FLAGS=$cpu -DCMAKE_CXX_FLAGS=$cpu -DCMAKE_OBJC_FLAGS=$cpu -DCMAKE_OBJCXX_FLAGS=$cpu \
    -DMW2_TITLE=$TITLE -DMW2_VERSION=$VERSION -DMW2_ONLINE=$ONLINE -DMW2_DIAGNOSTICS=$diagnostics \
    ${fetched[@]+"${fetched[@]}"} > /dev/null
cmake --build "$build" --target mw2_ios

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
