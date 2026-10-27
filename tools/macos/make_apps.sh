#!/bin/bash
# Packages the macOS builds as one app in a player's folder, MW2.app, holding
# both of the game's programs in Contents/MacOS:
#   mw2-sp   the campaign and Special Ops (build-release-<v>/mw2), what opens
#   mw2-mp   the multiplayer (build-mp-release-<v>/mw2)
# The app opens on the campaign's main menu, and the game's own menus switch
# between them (runtime/install/install.cpp). The folder's game/, saves/,
# bots/ and .env stay outside the app, which the runtime steps out of to find
# them.
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

sp="$ROOT/build-release$SUFFIX/mw2"
mp="$ROOT/build-mp-release$SUFFIX/mw2"
for binary in "$sp" "$mp"; do
    [ -x "$binary" ] || { echo "error: no $binary (build it with build.sh)" >&2; exit 1; }
done
app="$DEST/MW2.app"
# The two apps of earlier versions of this script.
rm -rf "$app" "$DEST/MW2 Campaign.app" "$DEST/MW2 Multiplayer.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$sp" "$app/Contents/MacOS/mw2-sp"
cp "$mp" "$app/Contents/MacOS/mw2-mp"
[ -z "$icns" ] || cp "$icns" "$app/Contents/Resources/AppIcon.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>MW2</string>
	<key>CFBundleDisplayName</key><string>MW2</string>
	<key>CFBundleIdentifier</key><string>$PREFIX</string>
	<key>CFBundleExecutable</key><string>mw2-sp</string>
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
# The second program first: the app's signature seals it.
codesign --force --sign - "$app/Contents/MacOS/mw2-mp" > /dev/null 2>&1 &&
    codesign --force --sign - "$app" > /dev/null 2>&1 || echo "warning: could not sign $app"
echo "made $app"
