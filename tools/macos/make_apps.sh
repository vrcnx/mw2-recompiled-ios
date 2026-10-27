#!/bin/bash
# Packages the macOS builds as two apps in a player's folder:
#   MW2 Campaign.app     the campaign and Special Ops (build-release-<v>/mw2)
#   MW2 Multiplayer.app  (build-mp-release-<v>/mw2)
# beside the folder's game/, saves/, bots/ and .env, which stay outside the
# apps (runtime/install/install.cpp steps out of a bundle to find them).
#
# The bundle is what macOS keys its treatment of a game on: Game Mode (games
# category, GCSupportsGameMode), Retina drawables (NSHighResolutionCapable,
# without which SDL's Metal view renders at 1x) and no App Nap.
#
#   tools/macos/make_apps.sh <player folder>     VERSION=tu0 (default) or tu6
# The icon is ios/LocalAssets.xcassets' (made from the player's own game,
# git-ignored) or else ios/Assets.xcassets'.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:?usage: tools/macos/make_apps.sh <player folder>}
VERSION=${VERSION:-tu0}
SUFFIX=$([ "$VERSION" = tu6 ] && echo "" || echo "-$VERSION")
PREFIX=${MW2_BUNDLE_PREFIX:-local.mw2recomp}
mkdir -p "$DEST"

icon_png="$ROOT/ios/LocalAssets.xcassets/AppIcon.appiconset/icon-1024.png"
[ -f "$icon_png" ] || icon_png="$ROOT/ios/Assets.xcassets/AppIcon.appiconset/icon-1024.png"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
icns=""
if [ -f "$icon_png" ]; then
    set_dir="$work/AppIcon.iconset"
    mkdir -p "$set_dir"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$icon_png" --out "$set_dir/icon_${s}x${s}.png" > /dev/null
        sips -z $((s * 2)) $((s * 2)) "$icon_png" --out "$set_dir/icon_${s}x${s}@2x.png" > /dev/null
    done
    iconutil -c icns "$set_dir" -o "$work/AppIcon.icns"
    icns="$work/AppIcon.icns"
fi

make_app() {
    local name=$1 exe=$2 binary=$3 id=$4
    [ -x "$binary" ] || { echo "skipping $name: no $binary (build it with build.sh)"; return; }
    local app="$DEST/$name.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$binary" "$app/Contents/MacOS/$exe"
    [ -z "$icns" ] || cp "$icns" "$app/Contents/Resources/AppIcon.icns"
    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$name</string>
	<key>CFBundleDisplayName</key><string>$name</string>
	<key>CFBundleIdentifier</key><string>$PREFIX.$id</string>
	<key>CFBundleExecutable</key><string>$exe</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>LSMinimumSystemVersion</key><string>15.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>GCSupportsGameMode</key><true/>
	<key>LSSupportsGameMode</key><true/>
	<key>GCSupportsControllerUserInteraction</key><true/>
	<key>NSAppSleepDisabled</key><true/>
	<key>NSLocalNetworkUsageDescription</key><string>Finds and joins system link games on your network.</string>
</dict>
</plist>
PLIST
    codesign --force --sign - "$app" > /dev/null 2>&1 || echo "warning: could not sign $app"
    echo "made $app"
}

make_app "MW2 Campaign" mw2-sp "$ROOT/build-release$SUFFIX/mw2" sp
make_app "MW2 Multiplayer" mw2-mp "$ROOT/build-mp-release$SUFFIX/mw2" mp
