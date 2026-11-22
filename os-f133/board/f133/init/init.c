// SPDX-License-Identifier: GPL-2.0
/*
 * /init of the F133's initramfs, built into the kernel (board/f133/initramfs.list). The root filesystem is not held
 * in RAM (64 MiB, and it took 12 of them): it is rootfs.sqfs on the card's F133 partition (p4, FAT), read-only, under
 * a tmpfs overlay that keeps whatever changes. A rootfs.sqfs.new left beside it (an update over SSH, written while
 * the old one is in use) takes its place first, if it is a whole squashfs (else it is removed: a copy cut short). A
 * rootfs.sqfs.try is mounted instead only with its kernel: on the one boot U-Boot tries Image.try ("try=kernel" in
 * the command line). The new root gets /.rootfs-tried, and rootfs.try on the card marks the try for an older
 * initramfs, which a fallback boot may run: it never tries the root on its own then. S98try renames it over
 * rootfs.sqfs once the screen works. The card stays mounted, at /mnt/sd. Messages go to the kernel log;
 * a failure that ends the boot goes on the screen too.
 *
 * Booted from the SPI NOR, U-Boot passes root=mtd:rootfs: the root is the squashfs in the NOR's "rootfs" partition
 * (/dev/mtdblock<n>) under the same overlay, /data is the NOR's JFFS2 "data" partition, and the card is not needed
 * (S02sd mounts one if it is there). Updates are tried on the card's own system, then written to the NOR.
 *
 * The boot picture goes on the screen from here, as soon as the root and /data are there, and only then the backlight
 * comes on (the kernel leaves it off: the device tree): the first light shows the picture, not an empty panel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "puff.h"

static void say(const char *what, int err)
{
	char b[200];
	int fd, n;

	n = snprintf(b, sizeof(b), "<%d>init: %s%s%s\n", err ? 3 : 5, what, err ? ": " : "", err ? strerror(err) : "");
	fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, b, n);
		close(fd);
	}
}

/* The backlight on (15 % of its 2000 steps, as the kernel had it; the dash sets its own level when it starts). */
static void light(void)
{
	int fd = open("/sys/class/backlight/backlight/brightness", O_WRONLY);

	if (fd >= 0) {
		write(fd, "300", 3);
		close(fd);
	}
}

/* The text console on the framebuffer (1), or off it (0: nothing of it lands on a picture drawn there). */
static void fbcon(int on)
{
	char p[64], name[32];
	int i, n, fd;

	for (i = 0; i < 8; i++) {
		snprintf(p, sizeof(p), "/sys/class/vtconsole/vtcon%d/name", i);
		fd = open(p, O_RDONLY);
		if (fd < 0)
			break;
		n = read(fd, name, sizeof(name) - 1);
		close(fd);
		name[n > 0 ? n : 0] = 0;
		if (!strstr(name, "frame buffer"))
			continue;
		snprintf(p, sizeof(p), "/sys/class/vtconsole/vtcon%d/bind", i);
		fd = open(p, O_WRONLY);
		if (fd >= 0) {
			write(fd, on ? "1" : "0", 1);
			close(fd);
		}
	}
}

/*
 * A step the boot cannot go on without: said on the screen, then a restart. The boot is quiet (loglevel=1 in the
 * command line keeps these lines off the console) and the text console may have let go of the framebuffer for the
 * boot picture: both undone first, with /dev and /sys there for it whichever step failed.
 */
static void fail(const char *what)
{
	int e = errno;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);	/* (the last steps have moved /dev away and unmounted /sys; */
	mount("sysfs", "/sys", "sysfs", 0, NULL);	/* over one still there, a second does no harm) */
	klogctl(8, NULL, 8);	/* SYSLOG_ACTION_CONSOLE_LEVEL: everything */
	fbcon(1);
	light();
	say(what, e);
	say("rebooting in 10 s", 0);
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}

/*
 * A gzip file's contents, want bytes of them, in memory (the caller's to free), or NULL: not gzip, another length, or
 * a CRC that does not match. Its deflate stream goes through zlib's puff (puff.c): a picture is long runs, which
 * puff copies as fast as anything, and the root's zcat took 0.2 s for one (and a process of the cold root).
 */
static unsigned char *unpack(int fd, unsigned long want)
{
	static unsigned long crc_of[256];
	unsigned long n, p = 10, inl, outl = want, c = 0xffffffff, i, k;
	unsigned char *in = NULL, *out = NULL;
	struct stat sb;

	if (fstat(fd, &sb) || sb.st_size < 18 || sb.st_size > (8 << 20) || lseek(fd, 0, SEEK_SET))
		return NULL;
	n = sb.st_size;
	in = malloc(n);
	if (!in || read(fd, in, n) != (ssize_t)n || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8 || in[3] & 0xe0)
		goto out;
	if (in[3] & 4)			/* the header's optional parts: an extra field, a name, a comment, its CRC */
		p += 2 + (in[10] | in[11] << 8);
	for (i = 0; i < 2; i++)
		if (in[3] & (8 << i))
			while (p < n && in[p++])
				;
	if (in[3] & 2)
		p += 2;
	if (p + 8 > n)
		goto out;
	inl = n - p - 8;
	out = malloc(want);
	if (out && !puff(out, &outl, in + p, &inl) && outl == want) {
		for (i = 0; i < 256; i++) {
			for (crc_of[i] = i, k = 0; k < 8; k++)
				crc_of[i] = crc_of[i] & 1 ? 0xedb88320 ^ crc_of[i] >> 1 : crc_of[i] >> 1;
		}
		for (i = 0; i < want; i++)
			c = crc_of[(c ^ out[i]) & 0xff] ^ c >> 8;
		p += inl;		/* the trailer: the contents' CRC, then their length */
		if ((c ^ 0xffffffff) == (in[p] | in[p + 1] << 8 | in[p + 2] << 16 | (unsigned long)in[p + 3] << 24)) {
			free(in);
			return out;
		}
	}
	free(out);
out:
	free(in);
	return NULL;
}

/*
 * The boot picture, then the backlight: the owner's own (/data/splash.bgra.gz: the NOR's data partition, or the
 * card's /data) if there is one, else the image's. Each is the panel's framebuffer, gzipped; one of another size (its
 * last 4 bytes say it) or that does not unpack is passed over. The text console lets go of the framebuffer first, so
 * no cursor lands on the picture. /.splash-shown tells S00splash. Whatever fails here, the boot goes on and the
 * backlight comes on: nothing of this is needed to start.
 */
static void splash(int nor)
{
	const char *pic[2] = { nor ? "/newroot/data/splash.bgra.gz" : "/newroot/mnt/sd/data/splash.bgra.gz",
			       "/newroot/usr/share/splash/splash.bgra.gz" };
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	struct stat sb;
	unsigned char t[4], *px = NULL;
	unsigned long len;
	int i, fd, s, fb = open("/dev/fb0", O_WRONLY);

	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &f))
		goto out;
	len = (unsigned long)f.line_length * v.yres;
	for (i = 0; i < 2 && !px; i++) {
		s = open(pic[i], O_RDONLY | O_NONBLOCK);	/* (a FIFO of that name on /data would hold the boot for good) */
		if (s < 0)
			continue;
		if (!fstat(s, &sb) && S_ISREG(sb.st_mode) && lseek(s, -4, SEEK_END) >= 0 && read(s, t, 4) == 4 &&
		    (t[0] | t[1] << 8 | t[2] << 16 | (unsigned long)t[3] << 24) == len)
			px = unpack(s, len);
		close(s);
	}
	if (!px)
		goto out;
	fbcon(0);
	if (write(fb, px, len) == (ssize_t)len) {
		fd = open("/newroot/.splash-shown", O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd >= 0)
			close(fd);
		say("the boot picture is up", 0);	/* (with the kernel's time: where a boot's seconds go) */
	} else {
		say("the boot picture was not drawn", 0);
	}
	free(px);
out:
	if (fb >= 0)
		close(fb);
	light();
}

/*
 * rootfs.sqfs on a read-only loop device: its fd (kept open until the mount holds the device: with autoclear, the
 * last close of an unmounted loop device detaches it), or -1
 */
static int loop_attach(const char *file, char *dev, size_t len)
{
	struct loop_info64 li;
	int ctl, n, lfd, ffd;

	ctl = open("/dev/loop-control", O_RDWR);
	if (ctl < 0)
		return -1;
	n = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (n < 0)
		return -1;
	snprintf(dev, len, "/dev/loop%d", n);
	lfd = open(dev, O_RDWR);
	ffd = open(file, O_RDONLY);
	if (lfd < 0 || ffd < 0 || ioctl(lfd, LOOP_SET_FD, ffd))
		return -1;
	memset(&li, 0, sizeof(li));
	li.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
	strncpy((char *)li.lo_file_name, "rootfs.sqfs", LO_NAME_SIZE - 1);
	ioctl(lfd, LOOP_SET_STATUS64, &li);
	close(ffd);
	return lfd;
}

/*
 * A whole squashfs: its magic, and no more bytes in use (the superblock's bytes_used, at 40) than the file has. A copy
 * cut short has the superblock, written first, but not all it counts.
 */
static int sqfs_whole(const char *file, off_t size)
{
	unsigned char sb[48];
	unsigned long long used = 0;
	int fd, i, n = -1;

	fd = open(file, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, sb, sizeof(sb));
		close(fd);
	}
	if (n != (int)sizeof(sb) || memcmp(sb, "hsqs", 4))
		return 0;
	for (i = 47; i >= 40; i--)
		used = used << 8 | sb[i];
	return used <= (unsigned long long)size;
}

/*
 * root=mtd:<name> in the command line: the block device of that NOR partition into dev (waiting up to 10 s for the
 * flash and its node) and 1; 0 without one, or if the partition never shows up.
 */
static int nor_root(const char *b, char *dev, size_t len)
{
	char want[48], line[128];
	const char *p;
	struct stat st;
	FILE *f;
	int i, n;

	p = strstr(b, "root=mtd:");
	if (!p)
		return 0;
	p += 9;
	snprintf(want, sizeof(want), "\"%.*s\"", (int)strcspn(p, " \n"), p);	/* /proc/mtd quotes the names */
	for (i = 0; i < 100; i++) {
		f = fopen("/proc/mtd", "r");
		while (f && fgets(line, sizeof(line), f))
			if (strstr(line, want) && sscanf(line, "mtd%d:", &n) == 1) {
				fclose(f);
				snprintf(dev, len, "/dev/mtdblock%d", n);
				for (; i < 100 && stat(dev, &st); i++)
					usleep(100000);
				return 1;
			}
		if (f)
			fclose(f);
		usleep(100000);
	}
	say("root=mtd: names no partition in /proc/mtd", ENODEV);
	return 0;
}

int main(void)
{
	struct stat st;
	char dev[32], cmd[1024];
	const char *root = "/sd/rootfs.sqfs";
	int i, lfd, tried = 0, nor;
	FILE *f;

	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);

	i = 0;
	f = fopen("/proc/cmdline", "r");
	if (f) {
		i = fread(cmd, 1, sizeof(cmd) - 1, f);
		fclose(f);
	}
	cmd[i] = 0;

	/* the NOR's root, else (none asked for, or it fails) the card's as before */
	nor = nor_root(cmd, dev, sizeof(dev));
	if (nor && !mount(dev, "/ro", "squashfs", MS_RDONLY, NULL))
		goto overlay;
	if (nor) {
		say("mounting the NOR's rootfs; the card's instead", errno);
		nor = 0;
	}

	for (i = 0; i < 100 && stat("/dev/mmcblk0p4", &st); i++)
		usleep(100000);
	if (mount("/dev/mmcblk0p4", "/sd", "vfat", MS_NOATIME, "utf8,fmask=0077,dmask=0077"))
		fail("mounting the card's F133 partition (/dev/mmcblk0p4)");

	if (!stat("/sd/rootfs.sqfs.new", &st)) {
		if (!sqfs_whole("/sd/rootfs.sqfs.new", st.st_size)) {
			unlink("/sd/rootfs.sqfs.new");
			say("rootfs.sqfs.new is not a whole squashfs (a copy cut short?): removed, rootfs.sqfs kept", 0);
		} else if (rename("/sd/rootfs.sqfs.new", "/sd/rootfs.sqfs"))
			say("putting rootfs.sqfs.new in place", errno);
		else
			say("rootfs.sqfs.new in place", 0);
		sync();
	}

	if (strstr(cmd, "try=kernel")) {
		if (!stat("/sd/rootfs.sqfs.try", &st)) {
			int fd = open("/sd/rootfs.try", O_CREAT | O_WRONLY | O_TRUNC, 0644);

			if (fd >= 0)
				close(fd);
			sync();
			root = "/sd/rootfs.sqfs.try", tried = 1;
			say("trying rootfs.sqfs.try with its kernel", 0);
		} else {
			say("a tried kernel with no rootfs.sqfs.try: rootfs.sqfs (not kept)", 0);
		}
	}

	lfd = loop_attach(root, dev, sizeof(dev));
	if (lfd < 0)
		fail("rootfs.sqfs on a loop device");
	if (mount(dev, "/ro", "squashfs", MS_RDONLY, NULL))
		fail("mounting rootfs.sqfs");
	close(lfd);	/* the mount holds it now; autoclear detaches it when that goes */
overlay:
	if (mount("tmpfs", "/rw", "tmpfs", 0, "mode=0755"))
		fail("the overlay's tmpfs");
	mkdir("/rw/upper", 0755);
	mkdir("/rw/work", 0755);
	if (mount("overlay", "/newroot", "overlay", 0, "lowerdir=/ro,upperdir=/rw/upper,workdir=/rw/work"))
		fail("the root overlay");
	if (tried) {	/* for S98try: this boot is the try */
		int fd = open("/newroot/.rootfs-tried", O_CREAT | O_WRONLY | O_TRUNC, 0644);

		if (fd >= 0)
			close(fd);
	}

	mkdir("/newroot/mnt", 0755);
	mkdir("/newroot/mnt/sd", 0755);
	if (!nor && mount("/sd", "/newroot/mnt/sd", NULL, MS_MOVE, NULL))
		say("moving the card to /mnt/sd", errno);
	/*
	 * /data: the NOR's JFFS2 partition, on a directory in place of the rootfs's link to the card. On a NOR boot, and
	 * on a card boot U-Boot made for a screen that runs from the NOR (data=nor: an update on trial, or kept and on
	 * its way to the NOR), which so has the screen's own settings. There the flash may still be on its way (5 s for
	 * it), and the card's /data stays if the NOR's does not mount.
	 */
	if (nor || strstr(cmd, "data=nor")) {
		int e;

		unlink("/newroot/data");
		mkdir("/newroot/data", 0755);
		for (i = 0; (e = mount("mtd:data", "/newroot/data", "jffs2", MS_NOATIME, NULL)) && !nor && i < 50; i++)
			usleep(100000);
		if (e) {
			say("mounting the NOR's data partition (mtd:data)", errno);
			if (!nor) {
				rmdir("/newroot/data");
				if (symlink("/mnt/sd/data", "/newroot/data"))
					say("the card's /data again", errno);
			}
		} else
			nor = 1;	/* (for the boot picture: the NOR's /data) */
	}
	splash(nor);
	if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL))
		say("moving /dev", errno);
	umount("/proc");
	umount("/sys");

	/* switch_root: the initramfs is only this program, freed with it */
	unlink("/init");
	if (chdir("/newroot") || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/"))
		fail("switching to the new root");
	say("starting /sbin/init", 0);
	execl("/sbin/init", "/sbin/init", (char *)NULL);
	fail("starting /sbin/init");
	return 1;
}
