#!/usr/bin/env bash
#
# The mobile app, through Capacitor.
#
#   ./scripts/package_mobile.sh android [--release]
#   ./scripts/package_mobile.sh ios
#   ./scripts/package_mobile.sh <platform> --stage-only
#
# Android and iOS differ only by the platform argument and the final builder,
# so they are one script. scripts/package_ios.sh is a two-line wrapper, kept so
# that `ls scripts/` tells the truth about what this project can build.
#
# iOS IS WRITTEN, NOT SCAFFOLDED. `npx cap add ios` needs Xcode and CocoaPods,
# and this machine has Command Line Tools and system Ruby 2.6. The config and
# the steps are here and are believed correct; nobody has run them. Saying so
# is better than shipping a green checkmark for a platform nobody has opened.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PLATFORM="${1:-}"
MODE="${2:-debug}"
case "$PLATFORM" in
  # ANDROID DOES NOT GO THROUGH CAPACITOR. scripts/package_android.sh builds the
  # APK from the SDK alone -- no npm, no Gradle, no downloads -- and was
  # verified on the emulator; this path never was. Kept as the entry point so
  # `package_mobile.sh android` still means "the Android app".
  android) exec "$ROOT/scripts/package_android.sh" ;;
  ios) ;;
  *) echo "usage: $0 android|ios [--release|--stage-only]" >&2; exit 2 ;;
esac

mkdir -p mobile
./scripts/package_web.sh mobile/www > /dev/null
rm -f mobile/www.tar.gz mobile/www/serve.js

# A redirect rather than a copy: sfx.html's URLs are relative to tool/, so
# moving it up a level would break every one of them.
cat > mobile/www/index.html <<'HTML'
<!doctype html>
<meta charset="utf-8">
<meta http-equiv="refresh" content="0; url=tool/sfx.html">
<title>DigidreamSound</title>
HTML
echo "staged mobile/www ($(du -sh mobile/www | cut -f1))"

if [ ! -f mobile/capacitor.config.json ]; then
  cat > mobile/capacitor.config.json <<'JSON'
{
  "appId": "com.digidreamsound.sfx",
  "appName": "DigidreamSound",
  "webDir": "www",
  "server": { "androidScheme": "https" },
  "android": { "allowMixedContent": false },
  "ios": { "contentInset": "always" }
}
JSON
fi
if [ ! -f mobile/package.json ]; then
  cat > mobile/package.json <<'JSON'
{
  "name": "digidreamsound-mobile",
  "private": true,
  "devDependencies": {
    "@capacitor/cli": "^6.0.0",
    "@capacitor/core": "^6.0.0",
    "@capacitor/android": "^6.0.0",
    "@capacitor/ios": "^6.0.0",
    "@capacitor/filesystem": "^6.0.0"
  }
}
JSON
fi

[ "$MODE" = "--stage-only" ] && exit 0

cd mobile
[ -d node_modules ] || npm install --silent --no-audit --no-fund
[ -d "$PLATFORM" ] || npx cap add "$PLATFORM"
npx cap sync "$PLATFORM"

if [ "$PLATFORM" = "android" ]; then
  cd android
  if [ "$MODE" = "--release" ]; then ./gradlew assembleRelease; else ./gradlew assembleDebug; fi
  echo "package_mobile: see mobile/android/app/build/outputs/apk/"
else
  echo "package_mobile: iOS project synced. Open mobile/ios/App/App.xcworkspace in Xcode."
  echo "  This path has never been run here -- no Xcode, no CocoaPods. See docs/SOUND.md."
fi
