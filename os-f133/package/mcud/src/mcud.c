// mcud: the SRNUBI T313D's power MCU on ttyS1 (115200 8N1, raw, no flow control), the one process that owns the line.
//
// Power: the MCU cuts the screen's power unless it is held: the hold frame now and every 60 s. SIGTERM: exit quietly
// (the hold lapses only when nothing sends it again); SIGUSR1: send the release, after which the MCU cuts the power
// ~40 s later and restores it ~3 s after (a real power cycle, the only way the Wi-Fi chip comes back after a warm reset).
//
// Bluetooth: the MCU is also the classic Bluetooth (a Bluetrum car-kit SoC). Frames are 02 | who | cmd | len |
// payload | xor(who..payload); who is 80 for exchanges the SoC starts, 81 for the MCU's. D4 carries AT# text (CR LF
// lines), D6 raw RFCOMM bytes. The MCU firmware owns the SDP records and opens one data channel at a time, telling
// which with a line: SW = Android Auto's, SV = its other one; IA (HFP down) closes it.
//
// /run/mcud.sock (stream): a client's first line names what it wants:
//   aa | sv    the data channel. "open\n" comes when the MCU opens that channel (at once if it is open), then raw bytes
//              both ways (towards the phone: D6 frames of at most 240 bytes, 10 ms apart, as the stock app sends
//              them); the connection is closed when the channel closes.
//   ctl        every MCU text line, one per line; a line it sends that starts with AT# goes to the MCU.
// Android Auto off: a paired phone that holds the AA channel open finds nothing behind it and retries for as long as
// its link stays up (the phone's "connecting to Android Auto", again and again). While the channel is open, once AA has
// been neither on nor starting for $MCUD_AA_GRACE_MS (5 s) in a row, the MCU's auto-connect goes off (AT#MH: it pulled
// a dropped phone back within seconds, seen on the screen) and the phone is disconnected (AT#CD). Every dash-aa start
// turns auto-connect on again (AT#MG: the MCU may keep it off across starts) and, for Android Auto with no phone
// connected ($MCUD_RUN/mcud.phone, written here on the MCU's IB, removed on its IA), calls the phone back (AT#CC, the
// last device). AA on or starting:
// an aa client; $MCUD_RUN/dash-aa (not with dash-aa.extra: another mode, no Android Auto); $MCUD_RUN/dash-aa.starting;
// or $MCUD_DATA/aa-on saying aa (kept on across starts) in mcud's first $MCUD_AA_KEPT_MS (2 min: the dash's restore
// gets its chance; a restore that failed leaves the file). At most 3 drops in any 10 minutes (a phone that comes straight
// back must not loop); an AA start clears the count. Never AT#CB: that is Bluetooth off until the MCU loses power.
// Log on stderr: the MCU's lines (a phone's address and name only as their codes) and its key frames.
//
//   mcud [tty] [socket]      defaults /dev/ttyS1 /run/mcud.sock
//   mcud ctl [secs] [socket] a client: stdin's lines (AT#...) to the MCU, its lines to stdout, for secs (default 5)
// Exit 0 only when asked (SIGTERM, SIGINT, SIGUSR1); any other end is a failure, which S03mcu restarts.
#define _GNU_SOURCE   // accept4
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOC 0x80
#define MCU 0x81
#define F_POWER 0xB0
#define F_KEY 0xB2
#define F_AT 0xD4
#define F_DATA 0xD6
#define F_LOG 0xDE
#define DATA_MAX 240
#define MAXC 8

enum { NONE, AA, SV };
static const char *chname[] = { "none", "aa", "sv" };

static int tty = -1;
static volatile sig_atomic_t quit, release;
static int chan = NONE;   // the data channel the MCU has open
static int64_t aa_off_since = -1;   // the AA channel open and AA off since then (ms), -1 not so
static int64_t drops[3];           // the last three AT#CD (ms), for the cap
static int64_t started;            // mcud's start (ms)

struct client { int fd, kind, opened; char in[256]; size_t n; } cl[MAXC];
static uint8_t outq[65536];   // towards the phone, waiting for its 10 ms slot
static size_t oq_head, oq_len;

static int64_t ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	char b[512];
	va_list a;
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	size_t n = strftime(b, sizeof b, "%H:%M:%S mcud: ", &tm);
	va_start(a, fmt);
	vsnprintf(b + n, sizeof b - n, fmt, a);
	va_end(a);
	fprintf(stderr, "%s\n", b);
}

// one whole frame per write(), so another writer (S03mcu's early hold) never splits one
static int send_frame(uint8_t cmd, const void *p, size_t len)
{
	uint8_t f[DATA_MAX + 16], x;
	if (len > 255 || len + 5 > sizeof f) return -1;
	f[0] = 0x02; f[1] = SOC; f[2] = cmd; f[3] = (uint8_t)len;
	memcpy(f + 4, p, len);
	x = 0;
	for (size_t i = 1; i < len + 4; i++) x ^= f[i];
	f[len + 4] = x;
	size_t want = len + 5;
	for (size_t off = 0; off < want;) {
		ssize_t w = write(tty, f + off, want - off);
		if (w < 0 && errno == EINTR) continue;
		if (w < 0 && errno == EAGAIN) { struct pollfd pf = { tty, POLLOUT, 0 }; poll(&pf, 1, 100); continue; }
		if (w <= 0) return -1;
		off += (size_t)w;
	}
	return 0;
}

static void hold(void) { static const uint8_t p[] = { 0xCE, 1, 0, 0, 0 }; send_frame(F_POWER, p, sizeof p); }
static void send_release(void) { static const uint8_t p[] = { 0xCE, 0, 0, 0, 0 }; send_frame(F_POWER, p, sizeof p); }

static void drop(struct client *c)
{
	if (c->fd >= 0) close(c->fd);
	if (c->opened) oq_len = 0;   // its bytes waiting for the phone go with it
	c->fd = -1; c->kind = NONE; c->opened = 0; c->n = 0;
}

// never waits on a client: one that stops reading (its socket full) is dropped, as one that is gone, else the loop that
// reads the MCU and sends its hold would stall behind it
static void put(struct client *c, const void *p, size_t n)
{
	if (send(c->fd, p, n, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)n) drop(c);
}

static void say(struct client *c, const char *s) { put(c, s, strlen(s)); }

// a data client gets "open" once its channel is open
static void open_waiting(void)
{
	for (int i = 0; i < MAXC; i++)
		if (cl[i].fd >= 0 && cl[i].kind == chan && chan != NONE && !cl[i].opened) {
			cl[i].opened = 1;
			say(&cl[i], "open\n");
		}
}

// the open channel goes: its clients are closed (they see EOF); clients still waiting for a channel keep waiting
static void close_channel(void)
{
	for (int i = 0; i < MAXC; i++)
		if (cl[i].fd >= 0 && cl[i].opened) drop(&cl[i]);
	chan = NONE; oq_len = 0;   // (aa_off_since kept: a phone reopening the channel without IA is still off; aa_watch resets it)
}

static const char *env_or(const char *k, const char *d) { const char *v = getenv(k); return v ? v : d; }
static int there(const char *dir, const char *name)
{
	char p[256];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return access(p, F_OK) == 0;
}

// Android Auto on or starting (see the top)
static int aa_wanted(int64_t now)
{
	const char *run = env_or("MCUD_RUN", "/run"), *data = env_or("MCUD_DATA", "/data");
	char p[256], first[8] = "";
	for (int i = 0; i < MAXC; i++)
		if (cl[i].fd >= 0 && cl[i].kind == AA) return 1;
	if ((there(run, "dash-aa") && !there(run, "dash-aa.extra")) || there(run, "dash-aa.starting")) return 1;
	if (now - started >= atoi(env_or("MCUD_AA_KEPT_MS", "120000"))) return 0;
	snprintf(p, sizeof p, "%s/aa-on", data);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	if (!fgets(first, sizeof first, f)) first[0] = 0;
	fclose(f);
	return strncmp(first, "extra", 5) != 0;   // (an empty line or "aa": Android Auto)
}

// the AA channel open: the phone dropped once AA has been off for the grace in a row (at most 3 in 10 minutes)
static void aa_watch(int64_t now)
{
	if (chan != AA) { aa_off_since = -1; return; }
	if (aa_wanted(now)) { aa_off_since = -1; return; }
	if (aa_off_since < 0) { aa_off_since = now; return; }
	if (now - aa_off_since < atoi(env_or("MCUD_AA_GRACE_MS", "5000"))) return;
	aa_off_since = -1;
	if (drops[0] && now - drops[0] < 600000) {   // the third-last within 10 minutes: no more until AA starts
		static int64_t said;
		if (said != drops[0]) logf_("Android Auto is off, the phone keeps coming back: left connected (3 drops in 10 minutes)"), said = drops[0];
		return;
	}
	drops[0] = drops[1], drops[1] = drops[2], drops[2] = now;
	send_frame(F_AT, "AT#MH\r\n", 7);   // auto-connect off first, or the MCU dials the phone straight back
	send_frame(F_AT, "AT#CD\r\n", 7);
	logf_("Android Auto is off: the phone holding its channel disconnected, auto-connect off (AT#MH, AT#CD)");
}

// a line from the MCU's Bluetooth
static void mcu_line(const char *l)
{
	if (!strncmp(l, "AD", 2) || !strncmp(l, "SA", 2) || !strncmp(l, "MZ", 2) || !strncmp(l, "IX", 2) ||
	    !strncmp(l, "IC", 2) || !strncmp(l, "ID", 2) || !strncmp(l, "IR", 2) || !strncmp(l, "PB", 2) || !strncmp(l, "PD", 2))
		logf_("bt %.2s (%zu bytes)", l, strlen(l) - 2);   // addresses, names, numbers: not in the log
	else
		logf_("bt %s", l);
	if (!strcmp(l, "SW") || !strcmp(l, "SV")) {
		int c = l[1] == 'W' ? AA : SV;
		// the channel announced again with no IA between (the phone closed and reopened it, its link up): a new
		// opening, so the client holding the old one is closed and dials again; the same line twice within 2 s is the
		// MCU repeating itself (it does, for other lines)
		static int64_t opened_at;
		int again = chan == c && ms() - opened_at >= 2000;
		if (chan != c || again) { close_channel(); chan = c; }
		opened_at = ms();
		logf_("data channel %s open%s", chname[chan], again ? " again: its client dials anew" : "");
		open_waiting();
	} else if (!strcmp(l, "IB")) {   // a phone connected (HFP up): dash-aa's start need not call one back
		char p[256];
		snprintf(p, sizeof p, "%s/mcud.phone", env_or("MCUD_RUN", "/run"));
		int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0) close(fd);
	} else if (!strcmp(l, "IA")) {
		char p[256];
		snprintf(p, sizeof p, "%s/mcud.phone", env_or("MCUD_RUN", "/run"));
		unlink(p);
		if (chan != NONE) logf_("data channel %s closed (HFP down)", chname[chan]);
		close_channel();
	}
	char b[300];
	snprintf(b, sizeof b, "%s\n", l);
	for (int i = 0; i < MAXC; i++)
		if (cl[i].fd >= 0 && cl[i].kind == 3) say(&cl[i], b);
}

static uint8_t rx[8192];
static size_t rxn;
static char line[512];
static size_t linen;

static void mcu_frame(uint8_t who, uint8_t cmd, const uint8_t *p, size_t len)
{
	if (cmd == F_AT) {
		for (size_t i = 0; i < len; i++) {
			if (p[i] == '\n' && linen && line[linen - 1] == '\r') {
				line[linen - 1] = 0;
				if (linen - 1 >= 2) mcu_line(line);
				linen = 0;
			} else if (linen < sizeof line - 1) {
				line[linen++] = (char)p[i];
			} else {
				linen = 0;
			}
		}
	} else if (cmd == F_DATA) {
		for (int i = 0; i < MAXC; i++)   // stock too drops data with no channel to take it
			if (cl[i].fd >= 0 && cl[i].kind == chan && cl[i].opened) put(&cl[i], p, len);
	} else if (cmd == F_LOG) {
		logf_("mcu log: %.*s", (int)len, (const char *)p);
	} else if (cmd == F_KEY) {
		char h[3 * 16 + 1] = "";
		for (size_t i = 0; i < len && i < 16; i++) sprintf(h + 3 * i, " %02x", p[i]);
		logf_("key%s", h);
	} else if (who == MCU && cmd != F_POWER) {
		logf_("frame %02x %02x, %zu bytes", who, cmd, len);
	}
}

// split the MCU's bytes into frames; resync on 02, drop a frame whose XOR is wrong (as the stock app does)
static void mcu_bytes(void)
{
	size_t i = 0;
	for (;;) {
		while (i < rxn && rx[i] != 0x02) i++;
		if (rxn - i < 5) break;
		size_t n = (size_t)rx[i + 3] + 5;
		if (rxn - i < n) break;
		uint8_t x = 0;
		for (size_t k = i + 1; k < i + n - 1; k++) x ^= rx[k];
		if (x != rx[i + n - 1]) { i++; continue; }
		mcu_frame(rx[i + 1], rx[i + 2], rx + i + 4, n - 5);
		i += n;
	}
	memmove(rx, rx + i, rxn - i);
	rxn -= i;
}

static int ctl_bytes(const char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1; return 0; }

static void client_in(struct client *c)
{
	uint8_t b[4096];
	ssize_t r = recv(c->fd, b, sizeof b, 0);
	if (r < 0 && errno == EAGAIN) return;
	if (r <= 0) { drop(c); return; }
	if (c->opened) {   // a data client's bytes, towards the phone
		size_t room = sizeof outq - oq_len;
		if ((size_t)r > room) { logf_("data out queue full: client dropped"); drop(c); return; }
		for (ssize_t k = 0; k < r; k++) outq[(oq_head + oq_len + (size_t)k) % sizeof outq] = b[k];
		oq_len += (size_t)r;
		return;
	}
	if (c->kind != NONE && c->kind != 3) return;   // a data client before "open": nothing to send yet
	for (ssize_t k = 0; k < r; k++) {
		if (b[k] != '\n') { if (c->n < sizeof c->in - 1) c->in[c->n++] = (char)b[k]; continue; }
		c->in[c->n] = 0;
		if (c->n && c->in[c->n - 1] == '\r') c->in[--c->n] = 0;
		if (c->kind == NONE) {
			if (!strcmp(c->in, "aa")) c->kind = AA, memset(drops, 0, sizeof drops);   // an AA start: the cap anew
			else if (!strcmp(c->in, "sv")) c->kind = SV;
			else if (!strcmp(c->in, "ctl")) c->kind = 3;
			else { drop(c); return; }
			if (c->kind == chan) open_waiting();
		} else if (!strncasecmp(c->in, "AT#CB", 5) || ctl_bytes(c->in)) {   // never to the MCU, whichever client asks;
			fprintf(stderr, "mcud: refused a ctl line\n");             // nor a control byte (a CR would start another)
		} else if (!strncmp(c->in, "AT#", 3)) {   // ctl: a command for the MCU (KA/KB go without CR LF, as stock)
			char a[260];
			int raw = !strcmp(c->in, "AT#KA") || !strcmp(c->in, "AT#KB");
			int n = snprintf(a, sizeof a, raw ? "%s" : "%s\r\n", c->in);
			if (n > 0 && n < 256) send_frame(F_AT, a, (size_t)n);
		}
		c->n = 0;
		if (c->fd < 0) return;
	}
}

static void on_sig(int s) { if (s == SIGUSR1) release = 1; else quit = 1; }

static int open_tty(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) return -1;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);   // no XON/XOFF, no CR/LF translation: D6 data holds 0x11, 0x13, 0x0d
		t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);
		t.c_cflag |= CS8 | CLOCAL | CREAD;
		cfsetispeed(&t, B115200);
		cfsetospeed(&t, B115200);
		tcsetattr(fd, TCSANOW, &t);
	}
	return fd;
}

static int ctl_client(int secs, const char *sockp)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sockp);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) { perror(sockp); return 1; }
	if (send(s, "ctl\n", 4, MSG_NOSIGNAL) != 4) return 1;
	int in = 0;
	for (int64_t end = ms() + secs * 1000LL; ms() < end;) {
		struct pollfd pf[2] = { { s, POLLIN, 0 }, { in, POLLIN, 0 } };
		if (poll(pf, in >= 0 ? 2 : 1, 100) < 0) break;
		char b[1024];
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			ssize_t r = recv(s, b, sizeof b, 0);
			if (r <= 0) break;
			fwrite(b, 1, (size_t)r, stdout);
			fflush(stdout);
		}
		if (in >= 0 && (pf[1].revents & (POLLIN | POLLHUP))) {
			ssize_t r = read(in, b, sizeof b);
			if (r <= 0) in = -1;
			else if (send(s, b, (size_t)r, MSG_NOSIGNAL) != r) break;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "ctl"))
		return ctl_client(argc > 2 ? atoi(argv[2]) : 5, argc > 3 ? argv[3] : "/run/mcud.sock");
	const char *ttyp = argc > 1 ? argv[1] : "/dev/ttyS1", *sockp = argc > 2 ? argv[2] : "/run/mcud.sock";
	tty = open_tty(ttyp);
	if (tty < 0) { logf_("%s: %s", ttyp, strerror(errno)); return 1; }
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sockp);
	unlink(sockp);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { logf_("%s: %s", sockp, strerror(errno)); return 1; }
	struct sigaction sa = { .sa_handler = on_sig };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	sigset_t block, wake;   // the signals held off except inside ppoll: one between the loop's check and the wait
	sigemptyset(&block), sigaddset(&block, SIGTERM), sigaddset(&block, SIGINT), sigaddset(&block, SIGUSR1);
	sigprocmask(SIG_BLOCK, &block, &wake);   // ends the wait at once instead of after the next minute's hold
	for (int i = 0; i < MAXC; i++) cl[i].fd = -1;

	started = ms();
	hold();
	int64_t next_hold = ms() + 60000, next_data = 0;
	logf_("holding the power on %s; clients on %s", ttyp, sockp);
	while (!quit && !release) {
		struct pollfd pf[MAXC + 2];
		int n = 0;
		pf[n++] = (struct pollfd){ tty, POLLIN, 0 };
		pf[n++] = (struct pollfd){ ls, POLLIN, 0 };
		for (int i = 0; i < MAXC; i++) pf[n++] = (struct pollfd){ cl[i].fd, POLLIN, 0 };
		int64_t now = ms(), wait = next_hold - now;
		if (oq_len && next_data - now < wait) wait = next_data - now;
		if (chan == AA && wait > 500) wait = 500;   // aa_watch looks twice a second while the AA channel is open
		if (wait < 0) wait = 0;
		struct timespec ts = { wait / 1000, (wait % 1000) * 1000000 };
		if (ppoll(pf, (nfds_t)n, &ts, &wake) < 0 && errno != EINTR) break;
		now = ms();
		if (now >= next_hold) { hold(); next_hold = now + 60000; }
		if (pf[0].revents & POLLIN) {
			ssize_t r = read(tty, rx + rxn, sizeof rx - rxn);
			if (r > 0) { rxn += (size_t)r; mcu_bytes(); if (rxn == sizeof rx) rxn = 0; }
		}
		if (pf[1].revents & POLLIN) {
			int fd = accept4(ls, NULL, NULL, SOCK_NONBLOCK);
			int i = 0;
			while (i < MAXC && cl[i].fd >= 0) i++;
			if (fd >= 0 && i < MAXC) { cl[i] = (struct client){ .fd = fd }; } else if (fd >= 0) close(fd);
		}
		for (int i = 0; i < MAXC; i++)
			if (cl[i].fd >= 0 && (pf[i + 2].revents & (POLLIN | POLLHUP | POLLERR)) && pf[i + 2].fd == cl[i].fd) client_in(&cl[i]);
		aa_watch(now);
		if (oq_len && chan != NONE && now >= next_data) {   // one D6 frame per 10 ms
			uint8_t f[DATA_MAX];
			size_t k = oq_len < DATA_MAX ? oq_len : DATA_MAX;
			for (size_t j = 0; j < k; j++) f[j] = outq[(oq_head + j) % sizeof outq];
			oq_head = (oq_head + k) % sizeof outq; oq_len -= k;
			send_frame(F_DATA, f, k);
			next_data = now + 10;
		} else if (oq_len && chan == NONE) {
			oq_len = 0;
		}
	}
	if (release) { send_release(); tcdrain(tty); logf_("released: the MCU cuts the power in ~40 s"); }
	unlink(sockp);
	return quit || release ? 0 : 1;   // else poll failed: S03mcu starts mcud again
}
