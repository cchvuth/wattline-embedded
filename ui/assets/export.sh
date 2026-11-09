#!/bin/sh
# Renders the app's icons and car-lights layers at the Dash's pixel sizes (Flutter, on a scratch copy of the
# app; the app repo is only read), then turns them and the car pictures into C (ui/gen/). Rerun when the app's
# glyphs, car pictures or lights change. Needs flutter and python3 with Pillow.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HOME/projs/tesla-dash}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude build --exclude .git --exclude android/.gradle --exclude ios/Pods --exclude t113 "$APP"/ "$TMP"/
cp "$HERE/export_parts.dart" "$TMP/lib/"
cp "$HERE/export_test.dart" "$TMP/test/"
sed -i '' "s/^part 'widgets.dart';/part 'widgets.dart';\npart 'export_parts.dart';/" "$TMP/lib/main.dart"
grep -q "part 'export_parts.dart'" "$TMP/lib/main.dart" || { echo "main.dart changed shape: update this script"; exit 1; }
mkdir -p "$TMP/out"
(cd "$TMP" && OUT="$TMP/out" SIZES="$HERE/sizes.json" flutter test test/export_test.dart)
[ -z "$KEEP" ] || cp -R "$TMP/out/." "$KEEP"   # KEEP=<dir>: keep the raw renders to look at
python3 -I "$HERE/gen.py" "$TMP/out" "$APP/assets" "$HERE/../gen"
python3 -I "$HERE/fonts.py"
