# Boot screen: any picture -> the panel's 480x1920 BGRA framebuffer, gzipped (what S00splash writes to /dev/fb0).
# The picture is landscape, as the dash: 1920x480 is used as is, anything else is scaled to fit, centred on black.
# Usage (on the Mac, needs Pillow): python3 -I mksplash.py IN.png|IN.jpg OUT.bgra.gz
# The built-in one: splash.png (the dash's own ring with no data) ->
#   rootfs_overlay_r3/usr/share/splash/splash.bgra.gz. Your own: put OUT.bgra.gz on the card's p2 as splash.bgra.gz.
# The panel is portrait: landscape (x, y) is panel (479 - y, x), i.e. the picture turned a quarter clockwise.
import gzip, sys
from PIL import Image

src, out = sys.argv[1:3]
im = Image.open(src).convert('RGB')
if im.size != (1920, 480):
    im.thumbnail((1920, 480), Image.LANCZOS)
    bg = Image.new('RGB', (1920, 480), 'black')
    bg.paste(im, ((1920 - im.size[0]) // 2, (480 - im.size[1]) // 2))
    im = bg
fb = im.rotate(-90, expand=True)
assert fb.size == (480, 1920)
with gzip.open(out, 'wb', 9) as g:
    g.write(fb.convert('RGBA').tobytes('raw', 'BGRA'))
