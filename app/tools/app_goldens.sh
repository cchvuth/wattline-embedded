#!/bin/sh
# The Dash UI's reference images: the current wattline app's own dash scenes
# (test/dash_shot_test.dart), rendered at the Dash canvas 1920x480 px = 1536x384 dp at dpr 1.25.
# Works on a scratch copy; the app repo is only read. Output: test/goldens/<app-commit>/*.png
# Rerun whenever the app's UI changes; the native UI is matched against the newest set.
set -e
W=${W:-1536}   # canvas width in dp at 1.25 px/dp; set to the visible top width / 1.25 once C-disp measures the trapezoid (5.2)
R=$(cd "$(dirname "$0")/../.." && pwd); [ -d "$R/wattline" ] || R=$(dirname "$R")   # the Flutter app inside the checkout (CI) or beside it
APP=${APP:-$R/wattline}   # as app.mk has it (WATTLINE)
REV=$(git -C "$APP" describe --always --dirty)
OUT=$(cd "$(dirname "$0")/.." && pwd)/test/goldens/$REV-w$W
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
rsync -a --exclude build --exclude .git --exclude android/.gradle --exclude ios/Pods --exclude t113 "$APP"/ "$TMP"/
sed -e 's/^const _dpr = .*/const _dpr = 1.25; \/\/ the S3XY Dash: 1920 x 480 px/' \
    -e "s/const Size(823\.5, 384)/const Size($W, 384)/g" \
    "$TMP"/test/dash_shot_test.dart > "$TMP"/test/dash_wide_test.dart
grep -q '_dpr = 1.25' "$TMP"/test/dash_wide_test.dart || { echo "shot test changed shape: update this script"; exit 1; }
mkdir -p "$OUT"
(cd "$TMP" && DASH_SHOT="$OUT" flutter test test/dash_wide_test.dart --plain-name 'dash:')
echo "$REV" > "$OUT"/APP_REV; ls "$OUT"
