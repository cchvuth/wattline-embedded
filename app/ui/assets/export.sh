#!/bin/sh
# Renders the app's icons and car-lights layers at the Dash's pixel sizes (Flutter, on a scratch copy of the
# app; the app repo is only read), then turns them and the car pictures into C (ui/gen/). Rerun when the app's
# glyphs, car pictures or lights change. Needs flutter and python3 with Pillow.
# Another device: PX (px per dp), SIZES (its sizes file) and GEN (its icon set's folder), as its profile says
# (device/*.mk), e.g. the F133 screen's PX=1.875 SIZES=ui/assets/sizes-f133.json GEN=ui/gen/f133 (aa_icon.py alike).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$(cd "$HERE/../../.." && pwd)/wattline}   # the Flutter app beside this checkout (app.mk: ../wattline)
SIZES=$(cd "$(dirname "${SIZES:-$HERE/sizes.json}")" && pwd)/$(basename "${SIZES:-$HERE/sizes.json}")   # read from $TMP
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude build --exclude .git --exclude android/.gradle --exclude ios/Pods --exclude t113 "$APP"/ "$TMP"/
cp "$HERE/export_parts.dart" "$TMP/lib/"
cp "$HERE/export_test.dart" "$TMP/test/"
sed -i '' "s/^part 'widgets.dart';/part 'widgets.dart';\npart 'export_parts.dart';/" "$TMP/lib/main.dart"
grep -q "part 'export_parts.dart'" "$TMP/lib/main.dart" || { echo "main.dart changed shape: update this script"; exit 1; }
mkdir -p "$TMP/out"
(cd "$TMP" && OUT="$TMP/out" SIZES="$SIZES" flutter test test/export_test.dart)
[ -z "$KEEP" ] || cp -R "$TMP/out/." "$KEEP"   # KEEP=<dir>: keep the raw renders to look at
python3 -I "$HERE/gen.py" "$TMP/out" "$APP/assets" "${GEN:-$HERE/../gen}"
python3 -I "$HERE/fonts.py"
