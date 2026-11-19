#!/bin/sh
# Builds out/Frond.app (the desktop with the cores beside it) and out/Frond-VERSION-ARCH.dmg.
# Run from the repository root after out/frond, out/frond-68k and build/desktop/frond-desktop exist.
set -eu
VERSION=$(head -n 1 VERSION)
ARCH=$(uname -m)
APP=out/Frond.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/desktop/frond-desktop "$APP/Contents/MacOS/Frond"
cp out/frond out/frond-68k "$APP/Contents/MacOS/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Frond</string>
<key>CFBundleDisplayName</key><string>Frond</string>
<key>CFBundleIdentifier</key><string>org.frond.Frond</string>
<key>CFBundleExecutable</key><string>Frond</string>
<key>CFBundleIconFile</key><string>frond</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>CFBundleVersion</key><string>$VERSION</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSMicrophoneUsageDescription</key><string>Frond passes the microphone to the emulated device.</string>
</dict></plist>
PLIST
# Icon from the SVG, when rsvg-convert and iconutil are there.
if command -v rsvg-convert >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    SET=$(mktemp -d)/frond.iconset
    mkdir -p "$SET"
    for s in 16 32 128 256 512; do
        rsvg-convert -w $s -h $s packaging/frond.svg -o "$SET/icon_${s}x${s}.png"
        rsvg-convert -w $((s * 2)) -h $((s * 2)) packaging/frond.svg -o "$SET/icon_${s}x${s}@2x.png"
    done
    iconutil -c icns "$SET" -o "$APP/Contents/Resources/frond.icns"
fi
# Qt frameworks and plugins next to the desktop; the cores only use SDL2 and libslirp.
macdeployqt "$APP" -always-overwrite
# The cores' Homebrew libraries (SDL2, libslirp and what they pull in) go into Frameworks.
python3 packaging/macos/bundle-libs.py "$APP" "$APP/Contents/MacOS/frond" "$APP/Contents/MacOS/frond-68k"
codesign --force --deep --sign - "$APP"
DMG="out/Frond-$VERSION-$ARCH.dmg"
rm -f "$DMG"
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Frond $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
echo "$DMG"
