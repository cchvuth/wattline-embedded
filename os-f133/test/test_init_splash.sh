#!/bin/sh
# The initramfs' boot picture (board/f133/init/init.c: splash()), run for real in a throwaway container: init.c built
# with a stand-in framebuffer (a file at /dev/fb0, its geometry from a fake ioctl) and no chroot, the pictures where
# it looks for them, the container's busybox as the root's zcat. Root, with gcc and busybox installed:
#   docker run --rm -v "$PWD":/r:ro debian:stable sh -c 'apt-get -qq update && apt-get -qq install -y gcc libc6-dev busybox >/dev/null && sh /r/os-f133/test/test_init_splash.sh'
I=$(cd "$(dirname "$0")/.." && pwd)/board/f133/init/init.c
T=$(mktemp -d)
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
cat > $T/t.c <<C
#include <linux/fb.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int test_ioctl(int fd, unsigned long req, ...)   /* a 4x3 panel, 16 bytes a line: 48 bytes */
{
	va_list ap;
	void *arg;
	(void)fd;
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (req == FBIOGET_VSCREENINFO) { memset(arg, 0, sizeof(struct fb_var_screeninfo)); ((struct fb_var_screeninfo *)arg)->yres = 3; return 0; }
	if (req == FBIOGET_FSCREENINFO) { memset(arg, 0, sizeof(struct fb_fix_screeninfo)); ((struct fb_fix_screeninfo *)arg)->line_length = 16; return 0; }
	return -1;
}
#define ioctl test_ioctl
#define chroot(x) 0
#define main init_main
#include "$I"
#undef main
int main(int argc, char **argv) { splash(argc > 1 && argv[1][0] == 'n'); return 0; }
C
gcc -Wall -o $T/t $T/t.c 2> $T/cc.log || { cat $T/cc.log; echo "FAIL  the harness does not build"; exit 1; }
pic() { head -c "$2" /dev/urandom > $T/$1.raw; busybox gzip -c $T/$1.raw > $T/$1.gz; }
pic own 48; pic built 48; pic small 40
fresh() { rm -rf /newroot; mkdir -p /newroot/data /newroot/mnt/sd/data /newroot/usr/share/splash; : > /dev/fb0; }

fresh; cp $T/own.gz /newroot/data/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t nor; cmp -s /dev/fb0 $T/own.raw && [ -e /newroot/.splash-shown ] && ok "the NOR's own picture, and the mark for S00splash" || no "own picture (NOR)"
fresh; cp $T/own.gz /newroot/mnt/sd/data/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t card; cmp -s /dev/fb0 $T/own.raw && ok "a card install's own picture (the card's /data)" || no "own picture (card)"
fresh; cp $T/small.gz /newroot/data/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t nor; cmp -s /dev/fb0 $T/built.raw && ok "a picture of another size passed over for the image's" || no "wrong size"
fresh; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t nor; cmp -s /dev/fb0 $T/built.raw && ok "no picture of the owner's: the image's" || no "built-in"
fresh
$T/t nor; [ ! -s /dev/fb0 ] && [ ! -e /newroot/.splash-shown ] && ok "no picture at all: nothing drawn, no mark" || no "none"
fresh; head -c 44 /dev/urandom > /newroot/data/splash.bgra.gz; printf '\060\000\000\000' >> /newroot/data/splash.bgra.gz
$T/t nor; [ ! -e /newroot/.splash-shown ] && ok "a picture that does not unpack: no mark (S00splash tries)" || no "corrupt"
fresh; mkfifo /newroot/data/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
timeout 5 $T/t nor; [ $? = 0 ] && cmp -s /dev/fb0 $T/built.raw && ok "a FIFO of the picture's name: not waited on, the image's drawn" || no "fifo"
rm -rf $T /newroot /dev/fb0
[ $bad = 0 ] && echo "init splash: ok" || { echo "init splash: $bad failed"; exit 1; }
