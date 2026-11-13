#!/bin/sh
# R0 post-image: name the DTB as the bootcmd loads it, then build card D's image.
set -e
cp "$BINARIES_DIR/sun8i-t113s-dash-r0.dtb" "$BINARIES_DIR/dash-r0.dtb"
echo "Card D data partition: the backup set (plan b.4) and the images to write go here." > "$BINARIES_DIR/DASHDATA.txt"
exec support/scripts/genimage.sh -c "$BR2_EXTERNAL_DASH_PATH/board/dash/genimage-sd.cfg"
