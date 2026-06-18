#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="Open Plaud"
EXECUTABLE_NAME="OpenPlaudApp"
BUNDLE_ID="com.guzus.openplaud.mac"
VERSION="0.1.0"
INSTALL_DIR="${INSTALL_DIR:-/Applications}"
BUILD_CONFIGURATION="${BUILD_CONFIGURATION:-release}"

cd "$ROOT"

swift build -c "$BUILD_CONFIGURATION" --product "$EXECUTABLE_NAME"
BIN_DIR="$(swift build -c "$BUILD_CONFIGURATION" --show-bin-path)"
EXECUTABLE="$BIN_DIR/$EXECUTABLE_NAME"
ICON="$ROOT/Resources/OpenPlaud.icns"

if [[ ! -x "$EXECUTABLE" ]]; then
  echo "missing executable: $EXECUTABLE" >&2
  exit 1
fi

if [[ ! -f "$ICON" ]]; then
  echo "missing app icon: $ICON" >&2
  exit 1
fi

APP_DIR="$ROOT/build/$APP_NAME.app"
CONTENTS="$APP_DIR/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP_DIR"
mkdir -p "$MACOS" "$RESOURCES"

cp "$EXECUTABLE" "$MACOS/$EXECUTABLE_NAME"
cp "$ICON" "$RESOURCES/OpenPlaud.icns"

cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleDisplayName</key>
  <string>$APP_NAME</string>
  <key>CFBundleExecutable</key>
  <string>$EXECUTABLE_NAME</string>
  <key>CFBundleIconFile</key>
  <string>OpenPlaud</string>
  <key>CFBundleIdentifier</key>
  <string>$BUNDLE_ID</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>$APP_NAME</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>$VERSION</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>NSHumanReadableCopyright</key>
  <string>Copyright © 2026 Open Plaud contributors.</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
PLIST

chmod +x "$MACOS/$EXECUTABLE_NAME"
codesign --force --deep --sign - --timestamp=none "$APP_DIR" >/dev/null

mkdir -p "$INSTALL_DIR"
osascript -e "tell application \"$APP_NAME\" to quit" >/dev/null 2>&1 || true
pkill -x "$EXECUTABLE_NAME" >/dev/null 2>&1 || true
rm -rf "$INSTALL_DIR/$APP_NAME.app"
cp -R "$APP_DIR" "$INSTALL_DIR/$APP_NAME.app"
xattr -dr com.apple.quarantine "$INSTALL_DIR/$APP_NAME.app" >/dev/null 2>&1 || true

echo "$INSTALL_DIR/$APP_NAME.app"
