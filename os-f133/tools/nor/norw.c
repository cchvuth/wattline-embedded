// SPDX-License-Identifier: MIT
// norw [-c|-n] IMAGE /dev/mtdN: writes IMAGE over the start of a NOR partition that the running system may be reading
// from (the NOR's rootfs, while booted from it). Only a whole image, to its own partition and no bigger than it: a
// squashfs ("hsqs", its bytes_used within the file) to the partition named "rootfs", a FIT (0xd00dfeed, its totalsize
// within the file) to "kernel" (the name and size from /sys/class/mtd/mtdN). Anything else is refused, the whole chip
// and U-Boot's partition included. -n: those checks only, nothing written. Link it static and run it from a tmpfs
// (/run): nothing it needs then pages in from the flash it overwrites. Block by block (the erase size): left alone when
// the flash holds it already, else erased, written and read back, three tries. SIGPIPE and SIGHUP are ignored, so the
// SSH session that started it can die (its shell paging in from a half-written root) without stopping it.
// -c, once all blocks match: /data read-only, then the MCU's power cycle (a warm reset loses the Wi-Fi chip), as
// S03mcu's let_go: its supervisor stopped, SIGUSR1 to mcud (it sends the release whole and exits), else the release
// straight to ttyS1; a reboot if the power is still on two minutes later. Without -c it only writes (the kernel's
// partition).
// NORW_SYS: another /sys/class/mtd (the tests).
#include <endian.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static void power_cycle(void)
{
	char b[32] = {0}, c[16] = {0};
	int p = 0, f = open("/var/run/mcud-supervise.pid", O_RDONLY);

	if (f >= 0) {	/* mcud's supervisor (S03mcu) first: no mcud starts again to hold the power after the release */
		if (read(f, b, sizeof(b) - 1) > 0 && atoi(b) > 0)
			kill(atoi(b), SIGTERM);
		close(f);
		unlink("/var/run/mcud-supervise.pid");
	}
	memset(b, 0, sizeof(b));
	f = open("/var/run/mcud.pid", O_RDONLY);
	if (f >= 0) {
		if (read(f, b, sizeof(b) - 1) > 0)
			p = atoi(b);
		close(f);
	}
	snprintf(b, sizeof(b), "/proc/%d/comm", p);
	f = p > 0 ? open(b, O_RDONLY) : -1;
	if (f >= 0) {
		if (read(f, c, sizeof(c) - 1) < 0)
			c[0] = 0;
		close(f);
	}
	if (!strcmp(c, "mcud\n") && !kill(p, SIGUSR1)) {
		for (int i = 0; i < 20 && !kill(p, 0); i++)
			usleep(100000);
		if (kill(p, 0))
			return;	/* mcud sent the release and went */
		kill(p, SIGKILL);
	}
	f = open("/dev/ttyS1", O_WRONLY | O_NOCTTY);
	if (f >= 0) {
		if (write(f, "\002\200\260\005\316\000\000\000\000\373", 10) != 10)
			perror("ttyS1");
		close(f);
	}
}

// mtdN's sysfs attribute into b, its newline dropped; -1 if there is none
static int mtd_attr(char *b, int len, unsigned n, const char *attr)
{
	const char *sys = getenv("NORW_SYS");
	char p[256];
	int f, r = -1;

	snprintf(p, sizeof(p), "%s/mtd%u/%s", sys ? sys : "/sys/class/mtd", n, attr);
	f = open(p, O_RDONLY);
	if (f >= 0) {
		r = read(f, b, len - 1);
		close(f);
	}
	if (r <= 0)
		return -1;
	b[r] = 0;
	b[strcspn(b, "\n")] = 0;
	return 0;
}

// the partition IMAGE (in, size bytes) is for, if DEV is that partition and IMAGE fits it; else NULL, why on stderr
static const char *check(int in, long long size, const char *img, const char *dev)
{
	unsigned char h[48];
	uint32_t fdt[2];
	uint64_t used;
	const char *want = NULL, *base = strrchr(dev, '/');
	char name[64], psize[32], c;
	unsigned n;

	if (pread(in, h, sizeof(h), 0) == sizeof(h)) {
		memcpy(&used, h + 40, sizeof(used));	/* squashfs superblock: bytes_used */
		memcpy(fdt, h, sizeof(fdt));		/* FDT header: magic, totalsize */
		if (!memcmp(h, "hsqs", 4) && le64toh(used) <= (uint64_t)size)
			want = "rootfs";
		else if (be32toh(fdt[0]) == 0xd00dfeed && be32toh(fdt[1]) <= size)
			want = "kernel";
	}
	if (!want) {
		fprintf(stderr, "norw: %s is neither a whole squashfs (the root) nor a whole FIT (the kernel)\n", img);
		return NULL;
	}
	base = base ? base + 1 : dev;
	if (sscanf(base, "mtd%u%c", &n, &c) != 1 || mtd_attr(name, sizeof(name), n, "name") ||
	    mtd_attr(psize, sizeof(psize), n, "size")) {
		fprintf(stderr, "norw: %s is not an MTD partition (/dev/mtdN)\n", dev);
		return NULL;
	}
	if (strcmp(name, want)) {
		fprintf(stderr, "norw: %s goes to the \"%s\" partition, %s is \"%s\"\n", img, want, dev, name);
		return NULL;
	}
	if (size > atoll(psize)) {
		fprintf(stderr, "norw: %s (%lld bytes) is bigger than the \"%s\" partition (%s)\n", img, size, name, psize);
		return NULL;
	}
	return want;
}

int main(int argc, char **argv)
{
	int opt = argc == 4 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-n"));
	int cycle = opt && argv[1][1] == 'c';
	struct mtd_info_user mi;
	struct stat st;
	const char *part;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 3 + opt) {
		fprintf(stderr, "usage: norw [-c|-n] IMAGE /dev/mtdN\n");
		return 2;
	}
	const char *img = argv[1 + opt], *dev = argv[2 + opt];
	int in = open(img, O_RDONLY);
	if (in < 0 || fstat(in, &st)) {
		perror(img);
		return 1;
	}
	if (!(part = check(in, st.st_size, img, dev)))
		return 1;
	if (opt && !cycle) {
		printf("ok: %s fits %s, the \"%s\" partition\n", img, dev, part);
		return 0;
	}
	int fd = open(dev, O_RDWR | O_SYNC);
	if (fd < 0 || ioctl(fd, MEMGETINFO, &mi)) {
		perror(dev);
		return 1;
	}
	// and the device opened is that partition's own: not a link named like it to another (the whole chip's)
	char nd[32];
	unsigned pn, ma, mn;
	struct stat ds;
	const char *b = strrchr(dev, '/');
	if (sscanf(b ? b + 1 : dev, "mtd%u", &pn) != 1 || mtd_attr(nd, sizeof(nd), pn, "dev") || fstat(fd, &ds) ||
	    sscanf(nd, "%u:%u", &ma, &mn) != 2 || !S_ISCHR(ds.st_mode) || major(ds.st_rdev) != ma ||
	    minor(ds.st_rdev) != mn) {
		fprintf(stderr, "norw: %s is not mtd%u's own device\n", dev, pn);
		return 1;
	}
	if ((unsigned long long)st.st_size > mi.size) {
		fprintf(stderr, "image %lld bytes, partition %u\n", (long long)st.st_size, mi.size);
		return 1;
	}
	size_t bs = mi.erasesize, n = (st.st_size + bs - 1) / bs, same = 0;
	unsigned char *want = malloc(bs), *got = malloc(bs);
	if (!want || !got)
		return 1;
	for (size_t b = 0; b < n; b++) {
		off_t o = (off_t)b * bs;
		memset(want, 0xff, bs);	/* past the image's end: erased */
		if (pread(in, want, bs, o) < 0) {
			perror("read image");
			return 1;
		}
		for (int t = 0;; t++) {
			if (pread(fd, got, bs, o) == (ssize_t)bs && !memcmp(got, want, bs)) {
				same += !t;
				break;
			}
			if (t == 3) {
				fprintf(stderr, "block %zu (0x%llx): still differs after 3 writes\n", b, (unsigned long long)o);
				return 1;
			}
			struct erase_info_user ei = {(__u32)o, (__u32)bs};
			if (ioctl(fd, MEMERASE, &ei) || pwrite(fd, want, bs, o) != (ssize_t)bs)
				perror("erase/write");
		}
		if (b % 16 == 15 || b == n - 1)
			printf("%zu/%zu blocks\n", b + 1, n);
	}
	printf("ok: %zu blocks match (%zu were already right)\n", n, same);
	close(fd);
	if (!cycle)
		return 0;
	sync();
	if (mount(NULL, "/data", NULL, MS_REMOUNT | MS_RDONLY, NULL))
		perror("/data read-only");
	sync();
	printf("power cycle: the MCU cuts the power in ~40 s\n");
	power_cycle();
	sleep(120);
	printf("power still on: reboot\n");
	sync();
	reboot(RB_AUTOBOOT);
	return 1;
}
