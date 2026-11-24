#!/bin/sh
# The initramfs' boot picture (board/f133/init/init.c: splash()), run for real in a throwaway container: init.c built
# with a stand-in framebuffer (a file at /dev/fb0, its geometry from a fake ioctl), the pictures where it looks for
# them. Root, with gcc and busybox installed:
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
/* fsck.fat as build.sh links it: here exiting with FSCK_RC, or killed by FSCK_SIG */
int fsck_main(int argc, char **argv) { (void)argc; (void)argv; if (getenv("FSCK_SIG")) raise(atoi(getenv("FSCK_SIG"))); exit(atoi(getenv("FSCK_RC") ? getenv("FSCK_RC") : "0")); }
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "fsck")) { card_fsck("/dev/null"); puts("went-on"); return 0; }
	splash();
	return 0;
}
C
gcc -Wall -o $T/t $T/t.c "$(dirname "$I")/puff.c" 2> $T/cc.log || { cat $T/cc.log; echo "FAIL  the harness does not build"; exit 1; }
pic() { head -c "$2" /dev/urandom > $T/$1.raw; busybox gzip -c $T/$1.raw > $T/$1.gz; }
pic own 48; pic built 48; pic small 40
fresh() { rm -rf /newroot; mkdir -p /newroot/mnt/sd /newroot/usr/share/splash; : > /dev/fb0; }
D=/newroot/mnt/sd

fresh; cp $T/own.gz $D/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t; cmp -s /dev/fb0 $T/own.raw && [ -e /newroot/.splash-shown ] && ok "the owner's own picture (on the card), and the mark for S00splash" || no "own picture"
fresh; cp $T/small.gz $D/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t; cmp -s /dev/fb0 $T/built.raw && ok "a picture of another size passed over for the image's" || no "wrong size"
fresh; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t; cmp -s /dev/fb0 $T/built.raw && ok "no picture of the owner's: the image's" || no "built-in"
fresh
$T/t; [ ! -s /dev/fb0 ] && [ ! -e /newroot/.splash-shown ] && ok "no picture at all: nothing drawn, no mark" || no "none"
fresh; head -c 44 /dev/urandom > $D/splash.bgra.gz; printf '\060\000\000\000' >> $D/splash.bgra.gz
$T/t; [ ! -e /newroot/.splash-shown ] && ok "a picture that does not unpack: no mark (S00splash tries)" || no "corrupt"
cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
$T/t; cmp -s /dev/fb0 $T/built.raw && [ -e /newroot/.splash-shown ] && ok "one that does not unpack, the image's beside it: the image's drawn" || no "corrupt, then built-in"
fresh; { busybox gzip -c $T/own.raw | head -c -8; printf '\001\002\003\004\060\000\000\000'; } > $D/splash.bgra.gz
$T/t; [ ! -s /dev/fb0 ] && [ ! -e /newroot/.splash-shown ] && ok "a picture whose CRC does not match: not drawn" || no "bad crc"
fresh; mkfifo $D/splash.bgra.gz; cp $T/built.gz /newroot/usr/share/splash/splash.bgra.gz
timeout 5 $T/t; [ $? = 0 ] && cmp -s /dev/fb0 $T/built.raw && ok "a FIFO of the picture's name: not waited on, the image's drawn" || no "fifo"
# the card's check (card_fsck): a clean card, a repaired one, one fsck.fat gives up on, and fsck.fat dying: the boot goes on
for c in "FSCK_RC=0" "FSCK_RC=1" "FSCK_RC=4" "FSCK_SIG=9"; do
	[ "$(env $c $T/t fsck)" = went-on ] && ok "the card's check, $c: the boot goes on" || no "card check $c"
done
rm -rf $T /newroot /dev/fb0
[ $bad = 0 ] && echo "init splash: ok" || { echo "init splash: $bad failed"; exit 1; }
