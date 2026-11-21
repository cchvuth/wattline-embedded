// SPDX-License-Identifier: GPL-2.0
/*
 * /init of the F133's initramfs, built into the kernel (board/f133/initramfs.list). The root filesystem is not held
 * in RAM (64 MiB, and it took 12 of them): it is rootfs.sqfs on the card's F133 partition (p4, FAT), read-only, under
 * a tmpfs overlay that keeps whatever changes. A rootfs.sqfs.new left beside it (an update over SSH, written while
 * the old one is in use) takes its place first, if it is a whole squashfs (else it is removed: a copy cut short). A
 * rootfs.sqfs.try is mounted instead only with its kernel: on the one boot U-Boot tries Image.try ("try=kernel" in
 * the command line). The new root gets /.rootfs-tried, and rootfs.try on the card marks the try for an older
 * initramfs, which a fallback boot may run: it never tries the root on its own then. S98try renames it over
 * rootfs.sqfs once the screen works. The card stays mounted, at /mnt/sd. Messages go to the screen.
 *
 * Booted from the SPI NOR, U-Boot passes root=mtd:rootfs: the root is the squashfs in the NOR's "rootfs" partition
 * (/dev/mtdblock<n>) under the same overlay, /data is the NOR's JFFS2 "data" partition, and the card is not needed
 * (S02sd mounts one if it is there). Updates are tried on the card's own system, then written to the NOR.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void fail(const char *what)
{
	say(what, errno);
	say("rebooting in 10 s", 0);
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
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
	if (nor) {
		/* /data: the NOR's JFFS2 partition, on a directory in place of the rootfs's link to the card */
		unlink("/newroot/data");
		mkdir("/newroot/data", 0755);
		if (mount("mtd:data", "/newroot/data", "jffs2", MS_NOATIME, NULL))
			say("mounting the NOR's data partition (mtd:data)", errno);
	} else if (mount("/sd", "/newroot/mnt/sd", NULL, MS_MOVE, NULL))
		say("moving the card to /mnt/sd", errno);
	if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL))
		say("moving /dev", errno);
	umount("/proc");
	umount("/sys");

	/* switch_root: the initramfs is only this program, freed with it */
	unlink("/init");
	if (chdir("/newroot") || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/"))
		fail("switching to the new root");
	execl("/sbin/init", "/sbin/init", (char *)NULL);
	fail("starting /sbin/init");
	return 1;
}
