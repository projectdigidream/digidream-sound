#!/usr/bin/env bash
#
# The editor as an Android APK, from the SDK alone.
#
#   ./scripts/package_android.sh            -> build/android/digidreamsound.apk
#
# WHAT IS IN IT. Exactly what scripts/package_web.sh stages -- the page the
# browser loads and the Electron shell ships -- plus android/web/android-shell.js,
# which MainActivity injects into sfx.html to provide window.digidream. One
# payload, three shells.
#
# NO GRADLE, NO CAPACITOR, NO DOWNLOADS. aapt2, javac, d8, zipalign and
# apksigner ship with the SDK and are all this needs; the repository's game APK
# (scripts/build_apk.sh at the top level) is built the same way. The Capacitor
# path in package_mobile.sh remains for iOS, which has no such route.
#
# NO NDK EITHER: the engine and the fitter are WebAssembly, so the APK carries
# no native code and runs on any ABI.
#
# WHAT YOU NEED:
#   ANDROID_HOME or ~/Library/Android/sdk   build-tools and a platform
#   a JDK (17 is what this was written against)
#   ./scripts/build_wasm.sh already run      tool/dist/*.mjs
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
[[ -d "$SDK" ]] || { echo "[apk] no Android SDK at $SDK (set ANDROID_HOME)" >&2; exit 2; }
pick_newest () { ls "$1" 2>/dev/null | sort -V | tail -1; }
BT="$SDK/build-tools/${DSND_BUILD_TOOLS:-$(pick_newest "$SDK/build-tools")}"
PLATFORM_JAR=""
for p in android-34 android-35 android-36 $(pick_newest "$SDK/platforms"); do
  [[ -f "$SDK/platforms/$p/android.jar" ]] && { PLATFORM_JAR="$SDK/platforms/$p/android.jar"; break; }
done
for t in "$BT/aapt2" "$BT/d8" "$BT/apksigner" "$BT/zipalign"; do
  [[ -x "$t" ]] || { echo "[apk] missing $t" >&2; exit 2; }
done
[[ -n "$PLATFORM_JAR" ]] || { echo "[apk] no android.jar under $SDK/platforms" >&2; exit 2; }
command -v javac >/dev/null || { echo "[apk] no javac on PATH" >&2; exit 2; }
[[ -f tool/dist/dsound.mjs && -f tool/dist/dsound-tool.mjs ]] || {
  echo "[apk] tool/dist is not built: run ./scripts/build_wasm.sh" >&2; exit 2; }

OUT="$ROOT/build/android"
STAGE="$OUT/stage"
rm -rf "$STAGE" "$OUT/gen" "$OUT/res"
mkdir -p "$STAGE/assets" "$STAGE/classes" "$OUT/gen"

# --- 1. the page -------------------------------------------------------------
./scripts/package_web.sh "$STAGE/assets/web" > /dev/null
rm -f "$STAGE/assets/web.tar.gz" "$STAGE/assets/web/serve.js"
cp android/web/android-shell.js "$STAGE/assets/web/"
# WHICH BUILD THIS IS, where the error banner and the missing-page report can
# read it: a phone screenshot then says what is installed.
cat > "$STAGE/assets/build.txt" <<EOF
built=$(date '+%Y-%m-%d %H:%M')
source=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)$(git -C "$ROOT" status --porcelain -- . 2>/dev/null | grep -q . && echo '+local' || true)
EOF
echo "[apk] page     $(du -sh "$STAGE/assets/web" | cut -f1)"

# --- 2. resources ------------------------------------------------------------
# The icon is drawn, not shipped as a png: a waveform in a rounded square.
RES="$OUT/res"
mkdir -p "$RES/mipmap-anydpi-v26" "$RES/drawable" "$RES/values" "$RES/mipmap"
cat > "$RES/drawable/ic_fg.xml" <<'XML'
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp" android:height="108dp"
    android:viewportWidth="108" android:viewportHeight="108">
  <path android:strokeColor="#7fd1b9" android:strokeWidth="5" android:strokeLineCap="round"
        android:pathData="M30,54 L30,54 M38,44 L38,64 M46,34 L46,74 M54,40 L54,68 M62,28 L62,80 M70,42 L70,66 M78,50 L78,58" />
</vector>
XML
echo '<resources><color name="ic_bg">#14161a</color></resources>' > "$RES/values/colors.xml"
cat > "$RES/mipmap-anydpi-v26/ic_launcher.xml" <<'XML'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
  <background android:drawable="@color/ic_bg" />
  <foreground android:drawable="@drawable/ic_fg" />
</adaptive-icon>
XML
cp "$RES/drawable/ic_fg.xml" "$RES/mipmap/ic_launcher.xml"
"$BT/aapt2" compile --dir "$RES" -o "$OUT/res.zip" >/dev/null
"$BT/aapt2" link -I "$PLATFORM_JAR" --manifest android/AndroidManifest.xml \
  --java "$OUT/gen" --min-sdk-version 26 --target-sdk-version 34 \
  --version-code "$(date +%s | cut -c1-9)" --version-name "0.1" \
  -o "$OUT/base.apk" "$OUT/res.zip" >/dev/null

# --- 3. java -----------------------------------------------------------------
# Source/target 8 against android.jar, plus the lambda stubs javac needs to see
# and d8 desugars away: the reasons are spelled out in the top-level
# scripts/build_apk.sh, which hit them first.
find android/java "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
javac -source 8 -target 8 -nowarn -Xlint:none \
      -bootclasspath "$PLATFORM_JAR:$BT/core-lambda-stubs.jar" \
      -d "$STAGE/classes" @"$OUT/sources.txt" 2>&1 \
  | grep -vE "^(Note:|warning:|[0-9]+ warning)" || true
[[ -f "$STAGE/classes/com/digidreamsound/sfx/MainActivity.class" ]] || {
  echo "[apk] javac produced no MainActivity" >&2; exit 2; }
"$BT/d8" --min-api 26 --output "$STAGE" $(find "$STAGE/classes" -name '*.class') >/dev/null
rm -rf "$STAGE/classes"

# --- 4. package, align, sign -------------------------------------------------
cp "$OUT/base.apk" "$OUT/unsigned.apk"
(cd "$STAGE" && zip -q -X -r "$OUT/unsigned.apk" classes.dex assets)
# A DEBUG KEY, generated once and kept in build/ (gitignored), so every build
# after the first installs over the previous one. Not a release key.
KEY="$OUT/debug.keystore"
[[ -f "$KEY" ]] || keytool -genkeypair -keystore "$KEY" -storepass android -keypass android \
  -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10950 \
  -dname "CN=DigidreamSound Debug,O=DigidreamSound,C=FR" >/dev/null 2>&1
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
APK="$OUT/digidreamsound.apk"
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android \
  --out "$APK" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk" "$OUT/base.apk" "$OUT/res.zip" "$APK.idsig"
echo "[apk] $APK  ($(du -h "$APK" | cut -f1))"
