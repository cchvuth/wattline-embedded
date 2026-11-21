// SPDX-License-Identifier: MIT
// aa.c — Android Auto wireless receiver for the Dash (native C: OpenSSL + core/pb.c, no aasdk).
//
// Transport: Android Auto frames the phone and head unit over TCP 5288, and tunnels a TLS
// session INSIDE those frames (not a raw TLS socket): the head unit is the TLS server and
// presents the user-supplied head-unit certificate. We drive OpenSSL through a pair of memory
// BIOs — hand it the ciphertext that arrives in frames, take the ciphertext it wants sent and
// wrap it back into frames. This file is that transport: the frame codec + the TLS engine, plus
// a loopback self-test (aa --selftest) that handshakes a client and server through the frame
// codec and passes a message each way, so the mechanism is proven without a phone.
//
// Built on top (needs an Android phone + the user's cert to test): the plaintext version
// exchange, auth-complete, service discovery (advertise 1280x480 H.264 video + touch), channel
// open, and media -> the decode/rotate/blit sink (already proven; spike-vdec --fb-stream).
//
// One source for every screen; what differs is chosen at build time (-D; defined = on):
//   AA_BT                 aa bt: the Bluetooth start over BlueZ (RFCOMM profiles over GDBus; gio-2.0 and gio-unix-2.0)
//   AA_MCU                aa mcu: the Bluetooth start through the screen's power MCU, which is its classic Bluetooth:
//                         mcud hands aa the phone's RFCOMM channel over its socket (BlueZ has no part in it)
//   AA_USB                aa usb: wired, Android Open Accessory over usbfs (link -lpthread)
//   AA_SYSTEM_AUDIO_ONLY  service discovery offers the system audio sink and the mic only, for a screen that must
//                         never play the phone's audio: music and the navigation voice stay on the phone's Bluetooth
//   PJ_PANEL_H, PJ_RGN_X/Y/W/H  the canvas and the picture's place on it (ui/project_region.h; defaults: the Dash's)
// The Dash (the T113, the default): -DAA_BT on the device (with gio's cflags and libs), nothing on a build host.
// The F133 screen (SRNUBI T313D: its 720x1920 portrait panel shown as a 1920x720 canvas, the picture in the left
// half's lower 3/4; its AP is the OS's, so aa ap goes unused there):
//   -DAA_MCU=1 -DAA_USB=1 -DAA_SYSTEM_AUDIO_ONLY=1 -DPJ_PANEL_H=720 -DPJ_RGN_X=0 -DPJ_RGN_Y=180 -DPJ_RGN_W=960
//   -DPJ_RGN_H=540, linked with -lssl -lcrypto -lpthread
// Found at run time rather than built in: a touchscreen that reports a portrait panel (taller than wide, the F133's)
// is turned as the dash's display is (mt_canvas).
#include "../core/pb.h"
#include "aa_proto.h"
#include "../ui/project_region.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <linux/input.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fcntl.h>
#include <sys/time.h>
#include <poll.h>
#include <time.h>
#ifdef AA_USB   // usbfs, and a thread for the accessory's bulk IN
#include <dirent.h>
#include <pthread.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#endif
#ifdef AA_BT   // target build: BlueZ RFCOMM advertise/accept over GDBus (host selftest build omits glib)
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#endif

#define die(...) do { fprintf(stderr, "aa: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

// ---- AA frame header (aasdk-compatible) ----
// byte0 = channel id; byte1 = flags; then a 2-byte big-endian payload length.
// flags: FIRST=1, LAST=2 (both set = a whole message in one frame = BULK); CONTROL=4; ENCRYPTED=8.
// A FIRST-but-not-LAST frame is followed by a 4-byte big-endian total message size (not used here:
// the control/handshake messages fit one frame).
enum { FRAME_FIRST = 1, FRAME_LAST = 2, FRAME_BULK = 3, FRAME_CONTROL = 4, FRAME_ENCRYPTED = 8 };

// write one single-frame (BULK) message; returns bytes written into out (>= len+4) or -1 if it won't fit.
static int frame_put(uint8_t *out, size_t cap, uint8_t chan, uint8_t flags, const uint8_t *p, size_t len)
{
	if (len > 0xffff || cap < len + 4) return -1;
	out[0] = chan;
	out[1] = (uint8_t)(flags | FRAME_BULK);
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)(len & 0xff);
	memcpy(out + 4, p, len);
	return (int)(len + 4);
}

// parse one frame header at p[0..n); on success sets *chan/*flags/*payload/*plen and returns the
// whole frame length; returns 0 if the frame is not fully present yet, -1 on a malformed header.
static int frame_get(const uint8_t *p, size_t n, uint8_t *chan, uint8_t *flags, const uint8_t **payload, size_t *plen)
{
	if (n < 4) return 0;
	size_t len = ((size_t)p[2] << 8) | p[3];
	size_t off = 4;
	if ((p[1] & FRAME_BULK) == FRAME_FIRST) { if (n < 8) return 0; off = 8; }  // FIRST only: skip the 4-byte total
	if (n < off + len) return 0;
	*chan = p[0]; *flags = p[1]; *payload = p + off; *plen = len;
	return (int)(off + len);
}

// ---- TLS engine over memory BIOs ----
typedef struct { SSL_CTX *ctx; SSL *ssl; BIO *rbio, *wbio; } aa_tls_t;

static SSL_CTX *tls_ctx(const char *cert, const char *key, int server)
{
	SSL_CTX *c = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
	if (!c) die("SSL_CTX_new");
	// the head unit's side takes any certificate strength, set before its own is loaded: a SHA-1 or short-key head-unit
	// certificate (which certcheck lets through) must not be refused at load by the default level
	if (!server) SSL_CTX_set_security_level(c, 0);
	if (cert && SSL_CTX_use_certificate_file(c, cert, SSL_FILETYPE_PEM) != 1) die("load cert %s", cert);
	SSL_CTX_set_default_passwd_cb_userdata(c, (void *)"");   // a passphrase-protected key fails at once, never prompts
	if (key && SSL_CTX_use_PrivateKey_file(c, key, SSL_FILETYPE_PEM) != 1) die("load key %s", key);
	SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);  // AA pins at the app layer, not via the CA store
	if (!server) {   // the head unit's side, as aasdk's: TLS 1.2, and the phone's certificate taken as it comes
		SSL_CTX_set_max_proto_version(c, TLS1_2_VERSION);
	}
	return c;
}

static void tls_init(aa_tls_t *t, SSL_CTX *ctx, int server)
{
	t->ctx = ctx;
	t->ssl = SSL_new(ctx);
	t->rbio = BIO_new(BIO_s_mem());   // ciphertext IN (from frames)
	t->wbio = BIO_new(BIO_s_mem());   // ciphertext OUT (to frames)
	if (!t->ssl || !t->rbio || !t->wbio) die("SSL_new/BIO");
	SSL_set_bio(t->ssl, t->rbio, t->wbio);  // SSL owns the BIOs after this
	if (server) SSL_set_accept_state(t->ssl); else SSL_set_connect_state(t->ssl);
}

// feed ciphertext that arrived in a frame into the engine
static void tls_feed(aa_tls_t *t, const uint8_t *p, size_t n) { if (n) BIO_write(t->rbio, p, (int)n); }
// take ciphertext the engine wants to send (to wrap into frames); returns bytes into out
static int tls_take(aa_tls_t *t, uint8_t *out, size_t cap) { int r = BIO_read(t->wbio, out, (int)cap); return r > 0 ? r : 0; }
// drive the handshake one step: returns 1 done, 0 wants more ciphertext, -1 error
static int tls_handshake(aa_tls_t *t)
{
	int r = SSL_do_handshake(t->ssl);
	if (r == 1) return 1;
	int e = SSL_get_error(t->ssl, r);
	if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
	return -1;
}
static void tls_free(aa_tls_t *t) { if (t->ssl) SSL_free(t->ssl); }  // frees the BIOs too

// ---- self-test: client <-> server handshake through the frame codec, then a message each way ----
// pump: move all pending ciphertext from `from` into `to` as ENCRYPTED frames and feed them in.
static void pump(aa_tls_t *from, aa_tls_t *to)
{
	uint8_t cipher[16384], frame[16384 + 8];
	int n;
	while ((n = tls_take(from, cipher, sizeof cipher)) > 0) {
		int fl = frame_put(frame, sizeof frame, 0, FRAME_ENCRYPTED, cipher, n);
		if (fl < 0) die("selftest: frame too big");
		uint8_t chan, flags; const uint8_t *pl; size_t plen;
		if (frame_get(frame, fl, &chan, &flags, &pl, &plen) != fl) die("selftest: frame roundtrip");
		tls_feed(to, pl, plen);
	}
}

// A head-unit certificate a phone can accept: readable, not self-signed (phones pin Google's Android Auto CA, so a
// self-signed one fails the handshake), and, given its key, that key loads and matches it (aa serve dies on a bad key).
// 0 usable; 1 self-signed; 2 the key missing, not this certificate's, or passphrase-protected (aa serve cannot use
// it); -1 no PEM certificate.
static int cert_check(const char *path, const char *keypath)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	X509 *x = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	if (!x) return -1;
	int r = X509_check_issued(x, x) == X509_V_OK;
	if (!r && keypath) {
		EVP_PKEY *k = NULL;
		// "" as the passphrase: a protected key is refused at once (no prompt on the console S06wattline runs on)
		if ((f = fopen(keypath, "r"))) k = PEM_read_PrivateKey(f, NULL, NULL, (void *)""), fclose(f);
		r = k && X509_check_private_key(x, k) == 1 ? 0 : 2;
		EVP_PKEY_free(k);
	}
	X509_free(x);
	return r;
}

// generate a self-signed cert/key to PEM files via the OpenSSL API (no openssl CLI needed; runs on-device)
static int gen_cert(const char *crt, const char *key, long valid_secs)
{
	EVP_PKEY *pk = EVP_RSA_gen(2048);
	if (!pk) return -1;
	X509 *x = X509_new();
	if (!x) { EVP_PKEY_free(pk); return -1; }
	X509_set_version(x, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
	X509_gmtime_adj(X509_getm_notBefore(x), 0);
	X509_gmtime_adj(X509_getm_notAfter(x), valid_secs);
	X509_set_pubkey(x, pk);
	X509_NAME *n = X509_get_subject_name(x);
	X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"dash-aa", -1, -1, 0);
	X509_set_issuer_name(x, n);
	int ok = X509_sign(x, pk, EVP_sha256()) != 0;
	FILE *f = fopen(key, "w"); if (f) { ok &= PEM_write_PrivateKey(f, pk, NULL, NULL, 0, NULL, NULL); fclose(f); } else ok = 0;
	f = fopen(crt, "w"); if (f) { ok &= PEM_write_X509(f, x); fclose(f); } else ok = 0;
	X509_free(x); EVP_PKEY_free(pk);
	return ok ? 0 : -1;
}

// Extra RFCOMM profiles, for helpers kept outside this repository: one per line of BT_EXTRA (absent on a plain image),
//   UUID SOCKET [dial] [NAME...]
// aa bt registers each, in both roles, while its helper runs (SOCKET exists when aa bt starts). A connection goes to the
// helper as the connected socket itself (SCM_RIGHTS over its UNIX socket SOCKET) and is closed here. With "dial", aa bt
// also opens the profile, once per Bluetooth connection, to each connected device that offers the UUID (some phones wait
// for the head unit to open it).
#define BT_EXTRA     "/etc/wattline/bt-profiles"
#define BT_EXTRA_MAX 4
typedef struct { char uuid[37], sock[108], name[32]; int dial; } bt_extra_t;
// one line -> *e: 1 a profile, 0 blank or a comment, -1 malformed
static int bt_extra_line(const char *line, bt_extra_t *e)
{
	char b[256], *sv, *t;
	memset(e, 0, sizeof *e);
	snprintf(b, sizeof b, "%s", line);
	b[strcspn(b, "\r\n")] = 0;
	for (char *h = b; (h = strchr(h, '#')); h++)   /* a comment: '#' at the start or after a blank (a path may hold one) */
		if (h == b || h[-1] == ' ' || h[-1] == '\t') { *h = 0; break; }
	if (!(t = strtok_r(b, " \t", &sv))) return 0;
	if (strlen(t) != 36) return -1;
	for (int i = 0; i < 36; i++)   /* 8-4-4-4-12 hex digits */
		if ((i == 8 || i == 13 || i == 18 || i == 23) ? t[i] != '-' : !isxdigit((unsigned char)t[i])) return -1;
	memcpy(e->uuid, t, 37);
	if (!(t = strtok_r(NULL, " \t", &sv)) || t[0] != '/' || strlen(t) >= sizeof e->sock) return -1;
	memcpy(e->sock, t, strlen(t) + 1);
	t = strtok_r(NULL, "", &sv);   /* the rest: [dial] [NAME...] */
	while (t && (*t == ' ' || *t == '\t')) t++;
	if (t && !strncmp(t, "dial", 4) && (!t[4] || t[4] == ' ' || t[4] == '\t')) { e->dial = 1; t += 4; while (*t == ' ' || *t == '\t') t++; }
	snprintf(e->name, sizeof e->name, "%s", t && *t ? t : "External");
	for (size_t k = strlen(e->name); k && (e->name[k - 1] == ' ' || e->name[k - 1] == '\t'); ) e->name[--k] = 0;
	for (char *c = e->name; *c; c++) if (*c < 0x20 || *c > 0x7e) *c = '?';   /* ASCII: a cut UTF-8 name is no valid string */
	return 1;
}

// $TMPDIR/name (/tmp without it): where the self-tests put their throwaway certificates
static const char *tmp_path(char *b, size_t n, const char *name)
{
	const char *d = getenv("TMPDIR");
	snprintf(b, n, "%s/%s", d && *d ? d : "/tmp", name);
	return b;
}

static int selftest(void)
{
	// an ephemeral self-signed cert/key for the server side (stands in for the user's head-unit cert)
	char crt[256], key[256];
	tmp_path(crt, sizeof crt, "aa-st.crt"), tmp_path(key, sizeof key, "aa-st.key");
	if (gen_cert(crt, key, 86400)) die("selftest: cert generation failed");
	SSL_CTX *sc = tls_ctx(crt, key, 1), *cc = tls_ctx(crt, key, 0);
	aa_tls_t s, c; tls_init(&s, sc, 1); tls_init(&c, cc, 0);
	for (int i = 0; i < 20; i++) {       // drive both sides to a completed handshake
		int rc = tls_handshake(&c), rs = tls_handshake(&s);
		if (rc < 0 || rs < 0) die("selftest: handshake error");
		pump(&c, &s); pump(&s, &c);
		if (rc == 1 && rs == 1) break;
	}
	if (!SSL_is_init_finished(c.ssl) || !SSL_is_init_finished(s.ssl)) die("selftest: handshake did not finish");
	// a message each way over the established session, through frames
	const char *m1 = "hello-from-phone", *m2 = "video-config-ack";
	uint8_t buf[256];
	SSL_write(c.ssl, m1, (int)strlen(m1)); pump(&c, &s);
	int n = SSL_read(s.ssl, buf, sizeof buf); if (n != (int)strlen(m1) || memcmp(buf, m1, n)) die("selftest: c->s mismatch");
	SSL_write(s.ssl, m2, (int)strlen(m2)); pump(&s, &c);
	n = SSL_read(c.ssl, buf, sizeof buf); if (n != (int)strlen(m2) || memcmp(buf, m2, n)) die("selftest: s->c mismatch");
	// the protobuf path (core/pb.c) the control messages use
	uint8_t pbbuf[64]; pb_out o = { pbbuf, 0, sizeof pbbuf, 0 };
	pb_u(&o, 1, 1); pb_b(&o, 2, (const uint8_t *)"dash", 4);   // e.g. a version/name message
	if (o.bad || !pb_ok(pbbuf, o.n)) die("selftest: pb build/parse");
	uint64_t v; const uint8_t *sp; size_t sl;
	if (!pb_get_u(pbbuf, o.n, 1, &v) || v != 1 || !pb_get_b(pbbuf, o.n, 2, &sp, &sl) || sl != 4 || memcmp(sp, "dash", 4))
		die("selftest: pb fields");
	// the extra Bluetooth profiles' lines (bt_extra_line)
	bt_extra_t e;
	if (bt_extra_line("12345678-1234-5678-9abc-DEF012345678 /run/x.sock dial  A helper  # note\n", &e) != 1 || !e.dial
	    || strcmp(e.sock, "/run/x.sock") || strcmp(e.name, "A helper")) die("selftest: bt-profiles line");
	if (bt_extra_line("12345678-1234-5678-9abc-def012345678 /run/y", &e) != 1 || e.dial || strcmp(e.name, "External")
	    || bt_extra_line("12345678-1234-5678-9abc-def012345678 /run/y dialer", &e) != 1 || e.dial || strcmp(e.name, "dialer"))
		die("selftest: bt-profiles defaults");
	if (bt_extra_line("# a comment", &e) || bt_extra_line("  \t\n", &e) || bt_extra_line("", &e)) die("selftest: bt-profiles blanks");
	if (bt_extra_line("12345678-1234-5678-9abc-def012345678 /run/a#b dial N\xc3\xa9 #c", &e) != 1 || strcmp(e.sock, "/run/a#b")
	    || !e.dial || strcmp(e.name, "N??")) die("selftest: bt-profiles '#' in a path, a non-ASCII name");
	if (bt_extra_line("12345678-1234-5678-9abc-def01234567 /run/x", &e) != -1             /* 35 characters */
	    || bt_extra_line("12345678-1234-5678-9abc-def0123456789 /run/x", &e) != -1         /* 37 */
	    || bt_extra_line("12345678x1234-5678-9abc-def012345678 /run/x", &e) != -1
	    || bt_extra_line("12345678-1234-5678-9abc-def01234567g /run/x", &e) != -1
	    || bt_extra_line("12345678-1234-5678-9abc-def012345678 run/x", &e) != -1
	    || bt_extra_line("12345678-1234-5678-9abc-def012345678", &e) != -1) die("selftest: bt-profiles malformed");
	tls_free(&c); tls_free(&s); SSL_CTX_free(sc); SSL_CTX_free(cc);
	printf("AA-SELFTEST OK: TLS-in-frames handshake + a message each way + protobuf roundtrip + bt-profiles lines\n");
	return 0;
}

// ---- AA messages: one message = [uint16 BE message id][protobuf], wrapped as a frame payload ----
static int msg_wrap(uint8_t *out, size_t cap, uint16_t id, const uint8_t *pb, size_t n)
{
	if (cap < n + 2) return -1;
	out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id; memcpy(out + 2, pb, n);
	return (int)(n + 2);
}
static uint16_t msg_id(const uint8_t *p, size_t n) { return n >= 2 ? (uint16_t)((p[0] << 8) | p[1]) : 0; }

// What the phone is asked to render: a 720p frame with margins, so the UI it lays out in the frame's middle is
// exactly the panel's picture rectangle (PJ_RGN_W x PJ_RGN_H), pixel for pixel: no scaling, so no stretch or blur.
// The decoder shows that middle window. Touches are NOT in frame coordinates: a current phone takes them in its UI
// area (the frame minus the margins; a phone's AA displays there are e.g. an 80x480 rail + a 1080x480 app = 1160x480). Frame
// coordinates (headunit's 2018 way, +half the margins) landed every tap that offset low-right on the unit.
// The lower setting (the dash's settings.conf `aa_low_res = 1`, Settings > Display): a 480p frame whose UI area has
// the picture's shape (800x332), which the decoder scales up 1.45x to fill the picture: less for the phone and the
// WiFi, a softer picture. Read at each phone's service discovery, so a change counts from the next connection.
// ponytail: the phone scales its UI by the density; 160 is a first value for this panel (110 keeps the 480p layout
// alike), tune both on the unit.
typedef struct { int res, fw, fh, uw, uh, dpi, fps; } aa_video_t;   // frame w x h, the UI area in it, density, AA_FPS_*
static const aa_video_t AA_VIDEO_720 = {AA_RES_720p, 1280, 720, PJ_RGN_W, PJ_RGN_H, 160, AA_FPS_30};
// the 480p UI area's height: the picture's shape at 800 wide, rounded up to 4s (the phone centres it, so each margin is
// even, as NV12 needs) as the decoder takes its window: 332 on the Dash, 452 on the F133
#define AA_LOW_UH (((800 * PJ_RGN_H + PJ_RGN_W - 1) / PJ_RGN_W + 3) & ~3)
static const aa_video_t AA_VIDEO_480 = {AA_RES_480p, 800, 480, 800, AA_LOW_UH, 110, AA_FPS_30};
_Static_assert((1280 - PJ_RGN_W) % 4 == 0 && (720 - PJ_RGN_H) % 4 == 0 && PJ_RGN_W <= 1280 && PJ_RGN_H <= 720 &&
	       AA_LOW_UH <= 480 && (480 - AA_LOW_UH) % 4 == 0, "the UI area must fit its frame, centred on even pixels");
static aa_video_t vid = {AA_RES_720p, 1280, 720, PJ_RGN_W, PJ_RGN_H, 160, AA_FPS_30};   // this session's (touches: its UI area)
#define AA_SETTINGS "/data/settings.conf"   // the dash's (app/settings.c)
static int setting_on(const char *key)   // a switch of the dash's settings ("key = 1"), any non-zero as the app reads it
{
	FILE *f = fopen(AA_SETTINGS, "r");
	char l[128], k[32];
	int on = 0, v;
	if (!f) return 0;
	while (fgets(l, sizeof l, f)) if (sscanf(l, " %31[a-z0-9_] = %d", k, &v) == 2 && !strcmp(k, key)) on = v != 0;
	fclose(f);
	return on;
}
static aa_video_t video_wanted(void)   // the settings' lower resolution, read at each connection; 30 fps (the panel's most)
{
	return setting_on("aa_low_res") ? AA_VIDEO_480 : AA_VIDEO_720;
}

// one audio-sink ChannelDescriptor (PCM 16-bit at the given rate/channels)
static size_t audio_chd(uint8_t *buf, size_t cap, int ch, int type, int rate, int chans)
{
	uint8_t ac[16]; pb_out c = { ac, 0, sizeof ac, 0 };
	pb_u(&c, AA_ACFG_SAMPLE_RATE, rate); pb_u(&c, AA_ACFG_BIT_DEPTH, 16); pb_u(&c, AA_ACFG_CHANNELS, chans);
	uint8_t av[32]; pb_out a = { av, 0, sizeof av, 0 };
	pb_u(&a, AA_AVCH_STREAM_TYPE, AA_AVSTREAM_AUDIO); pb_u(&a, AA_AVCH_AUDIO_TYPE, type);
	pb_b(&a, AA_AVCH_AUDIO_CONFIGS, ac, c.n);
	pb_out o = { buf, 0, cap, 0 };
	pb_u(&o, AA_CHD_CHANNEL_ID, ch); pb_b(&o, AA_CHD_AV_CHANNEL, av, a.n);
	return o.bad ? 0 : o.n;
}

// ServiceDiscoveryResponse: video (H.264 at vm's frame and frame rate), sensors, touch, the three audio sinks and the mic —
// the set OpenAuto advertises; AA refuses to start without sensors, audio playback or a mic ("No audio/mic").
// AA_SYSTEM_AUDIO_ONLY leaves out the media and speech sinks, for a screen the phone's audio must never play through
// (the F133): the system sink is the playback AA needs, and the phone keeps music and the navigation voice on its own
// Bluetooth to the car.
static size_t build_service_discovery_response(uint8_t *buf, size_t cap, const aa_video_t *vm)
{
	uint8_t vc[16]; pb_out v = { vc, 0, sizeof vc, 0 };
	pb_u(&v, AA_VC_RESOLUTION, vm->res); pb_u(&v, AA_VC_FPS, vm->fps);
	// margins + density are required too ("density missing" -> PROTOCOL_WRONG_CONFIGURATION)
	pb_u(&v, AA_VC_MARGIN_W, vm->fw - vm->uw); pb_u(&v, AA_VC_MARGIN_H, vm->fh - vm->uh); pb_u(&v, AA_VC_DPI, vm->dpi);
	uint8_t av[48]; pb_out a = { av, 0, sizeof av, 0 };
	pb_u(&a, AA_AVCH_STREAM_TYPE, AA_AVSTREAM_VIDEO);
	pb_b(&a, AA_AVCH_VIDEO_CONFIGS, vc, v.n);
	pb_u(&a, AA_AVCH_AVAILABLE_IN_CALL, 1);
	uint8_t chd[64]; pb_out c = { chd, 0, sizeof chd, 0 };
	pb_u(&c, AA_CHD_CHANNEL_ID, AA_CH_VIDEO);
	pb_b(&c, AA_CHD_AV_CHANNEL, av, a.n);
	// sensor service: a driving-status + night-mode source. AA tears down with "No sensors" if the HU advertises none.
	uint8_t sens[32]; pb_out s = { sens, 0, sizeof sens, 0 };
	uint8_t one[8]; pb_out d = { one, 0, sizeof one, 0 };
	pb_u(&d, AA_SEN_TYPE, AA_SENSOR_DRIVING_STATUS); pb_b(&s, AA_SENCH_SENSORS, one, d.n);
	d.n = d.bad = 0; pb_u(&d, AA_SEN_TYPE, AA_SENSOR_NIGHT_DATA); pb_b(&s, AA_SENCH_SENSORS, one, d.n);
	uint8_t schd[48]; pb_out sc = { schd, 0, sizeof schd, 0 };
	pb_u(&sc, AA_CHD_CHANNEL_ID, AA_CH_SENSOR);
	pb_b(&sc, AA_CHD_SENSOR_CHANNEL, sens, s.n);
	// the touch screen is the phone's UI area, in its own pixels (see aa_video_t)
	uint8_t tc[16]; pb_out t = { tc, 0, sizeof tc, 0 }; pb_u(&t, AA_TC_WIDTH, vm->uw); pb_u(&t, AA_TC_HEIGHT, vm->uh);
	uint8_t inp[24]; pb_out ip = { inp, 0, sizeof inp, 0 }; pb_b(&ip, AA_INCH_TOUCH_SCREEN, tc, t.n);
	uint8_t ichd[40]; pb_out ic = { ichd, 0, sizeof ichd, 0 };
	pb_u(&ic, AA_CHD_CHANNEL_ID, AA_CH_INPUT); pb_b(&ic, AA_CHD_INPUT_CHANNEL, inp, ip.n);
	uint8_t media[48], speech[48], sys[48];
	size_t mn = audio_chd(media, sizeof media, AA_CH_MEDIA_AUDIO, AA_AUDIO_MEDIA, 48000, 2);
	size_t spn = audio_chd(speech, sizeof speech, AA_CH_SPEECH_AUDIO, AA_AUDIO_SPEECH, 16000, 1);
	size_t syn = audio_chd(sys, sizeof sys, AA_CH_SYSTEM_AUDIO, AA_AUDIO_SYSTEM, 16000, 1);
	if (!mn || !spn || !syn) return 0;
	// microphone (AV input, 16 kHz mono): advertised so AA starts; no capture yet, so voice won't hear us
	uint8_t mc[16]; pb_out mcf = { mc, 0, sizeof mc, 0 };
	pb_u(&mcf, AA_ACFG_SAMPLE_RATE, 16000); pb_u(&mcf, AA_ACFG_BIT_DEPTH, 16); pb_u(&mcf, AA_ACFG_CHANNELS, 1);
	uint8_t avi[32]; pb_out ai = { avi, 0, sizeof avi, 0 };
	pb_u(&ai, AA_AVIN_STREAM_TYPE, AA_AVSTREAM_AUDIO); pb_b(&ai, AA_AVIN_AUDIO_CONFIG, mc, mcf.n);
	uint8_t michd[48]; pb_out mi = { michd, 0, sizeof michd, 0 };
	pb_u(&mi, AA_CHD_CHANNEL_ID, AA_CH_AV_INPUT); pb_b(&mi, AA_CHD_AV_INPUT_CHANNEL, avi, ai.n);
	pb_out o = { buf, 0, cap, 0 };
	pb_b(&o, AA_SDRESP_CHANNELS, chd, c.n);
	pb_b(&o, AA_SDRESP_CHANNELS, schd, sc.n);
	pb_b(&o, AA_SDRESP_CHANNELS, ichd, ic.n);
#ifndef AA_SYSTEM_AUDIO_ONLY
	pb_b(&o, AA_SDRESP_CHANNELS, media, mn);
	pb_b(&o, AA_SDRESP_CHANNELS, speech, spn);
#endif
	pb_b(&o, AA_SDRESP_CHANNELS, sys, syn);
	pb_b(&o, AA_SDRESP_CHANNELS, michd, mi.n);
	pb_b(&o, AA_SDRESP_HEAD_UNIT_NAME, (const uint8_t *)"Wattline", 8);
	pb_b(&o, AA_SDRESP_HU_MANUFACTURER, (const uint8_t *)"Wattline", 8);
	pb_b(&o, AA_SDRESP_HU_MODEL, (const uint8_t *)"Dash", 4);
	pb_u(&o, AA_SDRESP_LHD, 1);
	return o.bad ? 0 : o.n;
}
static size_t build_channel_open_response(uint8_t *buf, size_t cap, int status)
{
	pb_out o = { buf, 0, cap, 0 }; pb_u(&o, AA_CORESP_STATUS, status); return o.bad ? 0 : o.n;
}
static size_t build_av_setup_response(uint8_t *buf, size_t cap)
{
	pb_out o = { buf, 0, cap, 0 };
	pb_u(&o, AA_AVSETUPRESP_STATUS, AA_AVSTATUS_OK);
	pb_u(&o, AA_AVSETUPRESP_MAX_UNACKED, 2);   /* a frame on its way while the last one's ACK crosses the WiFi: at 1, each
	                                            * round trip paced the phone (48-55 of 60 fps, unevenly) */
	pb_u(&o, AA_AVSETUPRESP_CONFIGS, 0);   /* repeated: we offered one config, index 0 */
	return o.bad ? 0 : o.n;
}
// InputEventIndication carrying one touch point (panel pixel x,y; action PRESS/RELEASE/DRAG)
static size_t build_touch(uint8_t *buf, size_t cap, uint64_t ts, uint32_t x, uint32_t y, int action)
{
	uint8_t loc[24]; pb_out l = { loc, 0, sizeof loc, 0 };
	pb_u(&l, AA_TL_X, x); pb_u(&l, AA_TL_Y, y); pb_u(&l, AA_TL_POINTER_ID, 0);
	uint8_t te[48]; pb_out t = { te, 0, sizeof te, 0 };
	pb_b(&t, AA_TE_TOUCH_LOCATION, loc, l.n);
	pb_u(&t, AA_TE_ACTION_INDEX, 0);
	pb_u(&t, AA_TE_TOUCH_ACTION, action);
	pb_out o = { buf, 0, cap, 0 };
	pb_u(&o, AA_IEI_TIMESTAMP, ts);
	pb_u(&o, AA_IEI_DISP_CHANNEL, 0);
	pb_b(&o, AA_IEI_TOUCH_EVENT, te, t.n);
	return o.bad ? 0 : o.n;
}

// build/parse roundtrip over the real field numbers (no phone needed)
static int msg_selftest(void)
{
	for (int mode = 0; mode < 3; mode++) {   // 720p, the lower setting's 480p, 720p at 60 frames a second
	aa_video_t v60 = AA_VIDEO_720; v60.fps = AA_FPS_60;
	const aa_video_t *vm = mode == 1 ? &AA_VIDEO_480 : mode == 2 ? &v60 : &AA_VIDEO_720;
	uint8_t sdr[512]; size_t sn = build_service_discovery_response(sdr, sizeof sdr, vm);
	if (!sn || !pb_ok(sdr, sn)) die("msg: build SDR");
	pb_in in = { sdr, sn, 0 }; pb_val f; int found = 0, nch = 0;
	while (pb_next(&in, &f) == 1) {
		if (f.field != AA_SDRESP_CHANNELS || f.wt != 2) continue;
		nch++;
		uint64_t chid; const uint8_t *avp; size_t avl;
		if (pb_get_u(f.p, f.len, AA_CHD_CHANNEL_ID, &chid) && chid == AA_CH_VIDEO &&
		    pb_get_b(f.p, f.len, AA_CHD_AV_CHANNEL, &avp, &avl)) {
			const uint8_t *vcp; size_t vcl; uint64_t st, res;
			if (pb_get_u(avp, avl, AA_AVCH_STREAM_TYPE, &st) && st == AA_AVSTREAM_VIDEO &&
			    pb_get_b(avp, avl, AA_AVCH_VIDEO_CONFIGS, &vcp, &vcl) &&
			    pb_get_u(vcp, vcl, AA_VC_RESOLUTION, &res) && res == (uint64_t)vm->res) {
				uint64_t mw = 99, mh = 99, fps = 99;
				pb_get_u(vcp, vcl, AA_VC_MARGIN_W, &mw), pb_get_u(vcp, vcl, AA_VC_MARGIN_H, &mh), pb_get_u(vcp, vcl, AA_VC_FPS, &fps);
				found = mw == (uint64_t)(vm->fw - vm->uw) && mh == (uint64_t)(vm->fh - vm->uh) && fps == (uint64_t)vm->fps;
			}
		}
	}
	if (!found) die("msg: SDR video config roundtrip");
#ifdef AA_SYSTEM_AUDIO_ONLY
	if (nch != 5) die("msg: SDR advertises %d channels, want 5 (video, sensor, input, system audio, mic)", nch);
#else
	if (nch != 7) die("msg: SDR advertises %d channels, want 7 (video, sensor, input, 3 audio, mic)", nch);
#endif
	{   // the touch screen advertised = the picture (the phone's UI area): touches are sent in its pixels
		pb_in in2 = { sdr, sn, 0 }; pb_val f2; uint64_t tw = 0, th = 0;
		while (pb_next(&in2, &f2) == 1) {
			const uint8_t *ic, *tcp; size_t icl, tcl; uint64_t cid;
			if (f2.field == AA_SDRESP_CHANNELS && f2.wt == 2 && pb_get_u(f2.p, f2.len, AA_CHD_CHANNEL_ID, &cid) && cid == AA_CH_INPUT &&
			    pb_get_b(f2.p, f2.len, AA_CHD_INPUT_CHANNEL, &ic, &icl) && pb_get_b(ic, icl, AA_INCH_TOUCH_SCREEN, &tcp, &tcl))
				pb_get_u(tcp, tcl, AA_TC_WIDTH, &tw), pb_get_u(tcp, tcl, AA_TC_HEIGHT, &th);
		}
		if (tw != (uint64_t)vm->uw || th != (uint64_t)vm->uh) die("msg: touch screen %llux%llu, want the UI area %dx%d", (unsigned long long)tw, (unsigned long long)th, vm->uw, vm->uh);
	}
	}

	uint8_t co[16]; size_t con = build_channel_open_response(co, sizeof co, AA_STATUS_OK);
	uint64_t cs; if (!con || !pb_get_u(co, con, AA_CORESP_STATUS, &cs) || cs != AA_STATUS_OK) die("msg: channel-open");
	uint8_t as[24]; size_t asn = build_av_setup_response(as, sizeof as);
	uint64_t ast; if (!asn || !pb_get_u(as, asn, AA_AVSETUPRESP_STATUS, &ast) || ast != AA_AVSTATUS_OK) die("msg: av-setup");

	uint8_t tb[64]; size_t tn = build_touch(tb, sizeof tb, 123, 640, 240, AA_TOUCH_PRESS);
	const uint8_t *tep, *locp; size_t tel, locl; uint64_t tsv, ta, tx, ty;
	if (!tn || !pb_get_u(tb, tn, AA_IEI_TIMESTAMP, &tsv) || tsv != 123 ||
	    !pb_get_b(tb, tn, AA_IEI_TOUCH_EVENT, &tep, &tel) ||
	    !pb_get_u(tep, tel, AA_TE_TOUCH_ACTION, &ta) || ta != AA_TOUCH_PRESS ||
	    !pb_get_b(tep, tel, AA_TE_TOUCH_LOCATION, &locp, &locl) ||
	    !pb_get_u(locp, locl, AA_TL_X, &tx) || tx != 640 || !pb_get_u(locp, locl, AA_TL_Y, &ty) || ty != 240)
		die("msg: touch build/parse");

	// frame-wrap a message and read the id + body back
	uint8_t payload[600], frame[620];
	uint8_t sdr[512]; size_t sn = build_service_discovery_response(sdr, sizeof sdr, &AA_VIDEO_720);
	int pn = msg_wrap(payload, sizeof payload, AA_CTL_SERVICE_DISCOVERY_RESPONSE, sdr, sn);
	int fn = frame_put(frame, sizeof frame, AA_CH_CONTROL, AA_ENC_ENCRYPTED, payload, pn);
	uint8_t chan, flags; const uint8_t *fp; size_t fl;
	if (pn < 0 || fn < 0 || frame_get(frame, fn, &chan, &flags, &fp, &fl) != fn ||
	    chan != AA_CH_CONTROL || msg_id(fp, fl) != AA_CTL_SERVICE_DISCOVERY_RESPONSE)
		die("msg: frame+id wrap");

	printf("AA-MSG-SELFTEST OK: service-discovery(video) + channel-open + av-setup + touch + frame/id roundtrip\n");
	return 0;
}

// ---- server: TCP 5288 + the AA handshake sequence, media -> the decoder sink ----
// NOTE: compiled and built on the self-tested transport+message layer, but the live exchange is
// only validated with an Android phone + the user's head-unit cert (none in the repo/image).
static int64_t now_us(void);
// before authentication the whole version + TLS exchange must be done by this time (0: no deadline), so a client that
// trickles bytes cannot hold the single accept slot; each read also has its own timeout (SO_RCVTIMEO)
static int64_t rd_deadline;
static int readn(int fd, uint8_t *b, size_t n)
{
	size_t g = 0;
	while (g < n) {
		if (rd_deadline && now_us() > rd_deadline) return -1;
		ssize_t r = read(fd, b + g, n - g);
		if (r < 0 && errno == EINTR) continue;   /* a stop's signal (SA_RESTART does not restart a read with SO_RCVTIMEO) */
		if (r <= 0) return -1;
		g += (size_t)r;
	}
	return 0;
}
static int writen(int fd, const uint8_t *b, size_t n) { size_t w = 0; while (w < n) { ssize_t r = write(fd, b + w, n - w); if (r <= 0) return -1; w += (size_t)r; } return 0; }

// read one frame: header(2) + size(SHORT 2 / EXTENDED 6 on a FIRST frame) + payload
static int read_frame(int fd, uint8_t *chan, uint8_t *flags, uint8_t *buf, size_t cap)
{
	uint8_t h[8];
	if (readn(fd, h, 4)) return -1;
	*chan = h[0]; *flags = h[1];
	size_t len = ((size_t)h[2] << 8) | h[3];
	if ((h[1] & AA_FRAME_BULK) == AA_FRAME_FIRST && readn(fd, h + 4, 4)) return -1;  // skip the 4-byte total
	if (len > cap) return -1;
	return readn(fd, buf, len) ? -1 : (int)len;
}

// Receive one logical message. A message larger than a frame arrives split (FIRST, MIDDLE.., LAST), and frames of
// different channels interleave on the wire, so every channel reassembles its own. Each frame is encrypted on its
// own (its payload is whole TLS records), so it is decrypted as it comes and its plaintext appended (aasdk
// MessageInStream + Cryptor::decrypt). *msg points into the channel's buffer: valid until the next call.
#define AA_NCHAN 16
#define AA_MSG_MAX (4u << 20)   /* a 720p key frame is a few hundred KB; refuse nonsense */
typedef struct { uint8_t *p; size_t n, cap; } aa_asm_t;
static aa_asm_t rx_asm[AA_NCHAN];
static int asm_add(aa_asm_t *a, const uint8_t *d, size_t n, uint8_t ch)
{
	if (!n) return 0;
	if (a->n + n > (ch == AA_CH_VIDEO ? AA_MSG_MAX : (1u << 20))) return -1;   /* only video is big (the discovery request's phone icons: tens of KB) */
	if (a->n + n > a->cap) {
		size_t c = a->cap ? a->cap : 32768; while (c < a->n + n) c *= 2;
		uint8_t *q = realloc(a->p, c); if (!q) return -1;
		a->p = q; a->cap = c;
	}
	memcpy(a->p + a->n, d, n); a->n += n;
	return 0;
}
static int recv_msg(int fd, aa_tls_t *tls, uint8_t *chan, const uint8_t **msg, size_t *mlen)
{
	static uint8_t fr[AA_MAX_FRAME_PAYLOAD + 512], plain[AA_MAX_FRAME_PAYLOAD + 512];
	for (;;) {
		uint8_t ch, flags; int pn = read_frame(fd, &ch, &flags, fr, sizeof fr);
		if (pn < 0) return -1;
		// before TLS (version + handshake) only whole single frames on the control channel: no reassembly buffers
		// (up to 16 x 4 MB) for a peer that has not authenticated
		if (!tls && (ch != AA_CH_CONTROL || (flags & AA_FRAME_BULK) != AA_FRAME_BULK)) return -1;
		aa_asm_t *a = &rx_asm[ch % AA_NCHAN];
		if (flags & AA_FRAME_FIRST) a->n = 0;   // FIRST or BULK: a new message starts on this channel
		if (flags & AA_ENC_ENCRYPTED) {
			if (!tls) return -1;
			tls_feed(tls, fr, (size_t)pn);
			for (;;) {   // everything this frame's records hold
				int r = SSL_read(tls->ssl, plain, sizeof plain);
				if (r > 0) { if (asm_add(a, plain, (size_t)r, ch)) return -1; continue; }
				if (SSL_get_error(tls->ssl, r) != SSL_ERROR_WANT_READ) return -1;
				break;
			}
		} else if (tls) {   // once TLS is up the phone sends only encrypted frames: a plaintext one is another device on the AP
			static int said;
			if (!said) said = 1, fprintf(stderr, "aa: plaintext frame after TLS on channel %u: ignored\n", ch);
			continue;
		} else if (asm_add(a, fr, (size_t)pn, ch)) return -1;
		if (flags & AA_FRAME_LAST) { *chan = ch; *msg = a->p; *mlen = a->n; a->n = 0; return 0; }   // (n = 0: no frame without FIRST replays it)   // LAST or BULK: complete
	}
}

// the transport's write: the socket's (TCP), or aa usb's usb_write (a frame a bulk transfer, over the accessory's OUT
// endpoint)
static ssize_t (*xwrite)(int, const void *, size_t) = write;

// a write to the phone failed or was cut short (SO_SNDTIMEO: the phone stopped reading): the frame stream is broken,
// so every send fails at once from then on (callers that ack, ping or send touches don't check each send, and each would
// wait out the timeout again) and the session ends at its loop's next turn. Cleared per phone, at its dial.
static int tx_dead;

// send a framed message; with tls, the [msgid][pb] is encrypted and the frame flagged ENCRYPTED.
/* A stop (dash-aa stop: the AA button, the split's Exit) reaches aa serve as SIGTERM: a session up tells the
 * phone first (ShutdownRequest, reason QUIT), so it ends Android Auto and leaves the AP itself, rather than finding the
 * connection dead mid-session; its answer (or 2 s) ends the session, and aa serve then exits. 1 = asked, 2 = told. */
static volatile sig_atomic_t aa_quit;
static void aa_on_term(int s) { (void)s; if (!aa_quit) aa_quit = 1; }   /* a second one changes nothing */

static int send_msg(int fd, aa_tls_t *tls, uint8_t chan, uint8_t msgtype, uint16_t msgid, const uint8_t *pb, size_t pblen)
{
	if (tx_dead) return -1;
	uint8_t msg[AA_MAX_FRAME_PAYLOAD]; int mn = msg_wrap(msg, sizeof msg, msgid, pb, pblen);
	if (mn < 0) return -1;
	uint8_t cipher[AA_MAX_FRAME_PAYLOAD + 512], frame[AA_MAX_FRAME_PAYLOAD + 520];
	const uint8_t *payload = msg; size_t plen = (size_t)mn; uint8_t enc = AA_ENC_PLAIN;
	if (tls) { SSL_write(tls->ssl, msg, mn); int cn = tls_take(tls, cipher, sizeof cipher); if (cn <= 0) return -1; payload = cipher; plen = (size_t)cn; enc = AA_ENC_ENCRYPTED; }
	int fn = frame_put(frame, sizeof frame, chan, (uint8_t)(enc | msgtype), payload, plen);
	if (fn < 0) return -1;
	// one write: on this blocking socket a short count comes from SO_SNDTIMEO, and writing the rest would wait the
	// timeout out a second time. A stop's signal interrupts a write too (SA_RESTART does not restart one with
	// SO_SNDTIMEO): then the rest goes on, so the frame stays whole for the goodbye that follows
	size_t w = 0;
	while (w < (size_t)fn) {
		ssize_t r = xwrite(fd, frame + w, (size_t)fn - w);
		if (r > 0) { w += (size_t)r; if (w < (size_t)fn && !aa_quit) break; continue; }
		if (r < 0 && errno == EINTR) continue;
		break;
	}
	if (w != (size_t)fn) { tx_dead = 1; return -1; }
	return 0;
}

// drive the TLS handshake over SSL_HANDSHAKE (msgid 3) PLAIN control messages; we are the server
static int do_ssl_handshake(int fd, aa_tls_t *tls)
{
	for (int i = 0; i < 40; i++) {
		int r = tls_handshake(tls);
		if (r < 0) { fprintf(stderr, "aa: tls step %d: error in %s\n", i, SSL_state_string_long(tls->ssl)); return -1; }
		uint8_t c[AA_MAX_FRAME_PAYLOAD]; int cn, out = 0;
		while ((cn = tls_take(tls, c, sizeof c)) > 0) {
			out += cn;
			if (send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SSL_HANDSHAKE, c, (size_t)cn)) return -1;
		}
		fprintf(stderr, "aa: tls step %d: sent %d bytes, now %s\n", i, out, SSL_state_string_long(tls->ssl));
		X509 *pc = SSL_get_peer_certificate(tls->ssl);   // the phone's own certificate, once it has come
		if (pc && i < 4) {
			char sub[160]; X509_NAME_oneline(X509_get_subject_name(pc), sub, sizeof sub);
			fprintf(stderr, "aa: phone cert: %s\n", sub);
		}
		if (pc) X509_free(pc);
		if (r == 1) return 0;
		uint8_t chan; const uint8_t *m; size_t mn;
		if (recv_msg(fd, NULL, &chan, &m, &mn)) { fprintf(stderr, "aa: tls step %d: phone closed\n", i); return -1; }
		uint16_t id = msg_id(m, mn);
		if (id != AA_CTL_SSL_HANDSHAKE || mn < 2) { fprintf(stderr, "aa: tls step %d: got msg %u (%zu bytes), not a handshake\n", i, id, mn); return -1; }
		fprintf(stderr, "aa: tls step %d: got %zu bytes\n", i, mn - 2);
		tls_feed(tls, m + 2, mn - 2);
	}
	return -1;
}

// fork the decoder sink: spike-vdec --fb-stream reading H.264 on its stdin, drawing the frame's middle
// PJ_RGN_W x PJ_RGN_H window at the picture rectangle: --region X W for a picture of the panel's full height (the Dash:
// its decoder takes only that form), else --region X Y W H (the F133's)
#define STR_(x) #x
#define STR(x) STR_(x)
static pid_t sink_pid = -1;
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031   /* Linux's, without _GNU_SOURCE */
#endif
static int spawn_sink(void)
{
	int p[2]; if (pipe(p)) return -1;
	if (fcntl(p[1], F_SETPIPE_SZ, 512 << 10) < 0)   /* a frame's write never waits on the decoder (its ACK, and so the phone,
	                                                * would): room for a key frame and a few more, not seconds of them */
		fprintf(stderr, "aa: decoder pipe at its default size: %s\n", strerror(errno));
	pid_t c = fork();
	sink_pid = c;
	if (c == 0) {
		dup2(p[0], 0); close(p[0]); close(p[1]);
#if PJ_RGN_Y == 0 && PJ_RGN_H == PJ_PANEL_H
		execlp("spike-vdec", "spike-vdec", "--fb-stream", "--region", STR(PJ_RGN_X), STR(PJ_RGN_W), (char *)NULL);
#else
		execlp("spike-vdec", "spike-vdec", "--fb-stream", "--region", STR(PJ_RGN_X), STR(PJ_RGN_Y), STR(PJ_RGN_W), STR(PJ_RGN_H),
		       (char *)NULL);
#endif
		_exit(127);
	}
	close(p[0]);
	if (c < 0) { close(p[1]); return -1; }
	fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);   /* sink_write waits, but not for ever */
	return p[1];
}

// all of it into the decoder's pipe, waiting at most 2 s in all for room: a decoder stuck (in the driver, or stopped)
// counts as dead, rather than holding the session, the phone's link, the markers and the lock for good
static int sink_write(int fd, const uint8_t *b, size_t n)
{
	int64_t end = now_us() + 2000000;
	for (size_t w = 0; w < n; ) {
		ssize_t r = write(fd, b + w, n - w);
		if (r > 0) { w += (size_t)r; continue; }
		int64_t left = end - now_us();
		if (r == 0 || (errno != EAGAIN && errno != EINTR) || left <= 0) return -1;
		struct pollfd pf = { .fd = fd, .events = POLLOUT };
		poll(&pf, 1, (int)(left / 1000) + 1);
	}
	return 0;
}

static uint32_t av_sess[16];   // per-AV-channel session id from AV_START, echoed in media ACKs
static int is_av(uint8_t ch) { return ch == AA_CH_VIDEO || ch == AA_CH_MEDIA_AUDIO || ch == AA_CH_SPEECH_AUDIO || ch == AA_CH_SYSTEM_AUDIO || ch == AA_CH_AV_INPUT; }
static int is_media(uint16_t id) { return id == AA_AV_MEDIA_WITH_TIMESTAMP || id == AA_AV_MEDIA; }

// ACK one media frame on its channel: the AV_START session, value 1 per frame (as OpenAuto); with max_unacked 2 the
// phone holds back its next frame while two are unacknowledged.
static void ack(int fd, aa_tls_t *tls, uint8_t ch)
{
	uint8_t a[16]; pb_out o = { a, 0, sizeof a, 0 }; pb_u(&o, AA_ACK_SESSION, av_sess[ch & 15]); pb_u(&o, AA_ACK_VALUE, 1);
	send_msg(fd, tls, ch, AA_MSG_SPECIFIC, AA_AV_MEDIA_ACK, a, o.n);
}

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

// The head unit pings the phone. Measured against a current phone, not the 2018 references: it logs "Missing HU
// ping requests for 2950 ms" when they come slower than ~3 s, and rejects a PLAIN ping (aasdk's way) with
// FRAMER_READ_UNEXPECTED_UNENCRYPTED_MESSAGE. So: encrypted, every 1.5 s.
#define AA_PING_EVERY_US 1500000
static void send_ping(int fd, aa_tls_t *tls)
{
	uint8_t r[16]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_PING_TS, (uint64_t)now_us());
	send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_PING_REQUEST, r, o.n);
}

// ---- touch: the touchscreen read directly (evdev multi-touch, protocol B), every finger ----
// Sent only while the UI shows the picture (PJ_VISIBLE) and the phone has its input channel open. A gesture whose
// first finger lands outside the picture is not sent at all; fingers that wander off it are held at its edge.
// Positions go in picture pixels (the phone's UI area, the touch screen advertised in service discovery), with the
// touch's own kernel timestamp in us (as headunit): the phone measures fling speed from those, so send-time stamps
// (jittery) make swipes overshoot.
#define MT_MAX 10
typedef struct { int active, on, moved, x, y; } mt_slot_t;   /* active: finger down (kernel); on: in the phone's list */
static mt_slot_t mt[MT_MAX];
static int mt_cur, mt_gesture, mt_prev_any;   /* mt_gesture: 0 none, 1 ours, -1 ignored until every finger is up */
static int mt_dropped;   /* SYN_DROPPED seen: events ignored until the frame's SYN_REPORT, then the kernel's state read */
static int mt_max_x = PJ_PANEL_W - 1, mt_max_y = PJ_PANEL_H - 1;
static int mt_portrait;   /* the touchscreen reports the portrait panel (taller than wide: the F133's JD9366T, 720x1920) */
static int input_open;
static int touch_open(void)
{
	int f = open(PJ_TOUCH_DEV, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (f < 0) { fprintf(stderr, "aa: touchscreen %s: %s\n", PJ_TOUCH_DEV, strerror(errno)); return -1; }
	int clk = CLOCK_MONOTONIC;   /* the events' times on now_us's clock (see phone_off) */
	if (ioctl(f, EVIOCSCLOCKID, &clk)) fprintf(stderr, "aa: touchscreen clock: %s (touch times would be wrong)\n", strerror(errno));
	struct input_absinfo a;
	if (!ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &a) && a.maximum > 0) mt_max_x = a.maximum;
	if (!ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &a) && a.maximum > 0) mt_max_y = a.maximum;
	mt_portrait = mt_max_y > mt_max_x;
	return f;
}
/* finger k on the landscape canvas (PJ_PANEL_W x PJ_PANEL_H), where the dash draws. A portrait touchscreen is turned as
 * the dash's display is (rotate = 270 in dash.conf, LVGL's turn): panel (px, py) is landscape (py, PJ_PANEL_H - 1 - px) */
static void mt_canvas(int k, int *x, int *y)
{
	if (mt_portrait) {
		*x = (int)((int64_t)mt[k].y * PJ_PANEL_W / (mt_max_y + 1));
		*y = PJ_PANEL_H - 1 - (int)((int64_t)mt[k].x * PJ_PANEL_H / (mt_max_x + 1));
	} else {
		*x = (int)((int64_t)mt[k].x * PJ_PANEL_W / (mt_max_x + 1));
		*y = (int)((int64_t)mt[k].y * PJ_PANEL_H / (mt_max_y + 1));
	}
}
/* finger k in the phone's UI-area pixels: its place on the picture, scaled to this session's UI area */
static void mt_ui(int k, int *x, int *y)
{
	int px, py;
	mt_canvas(k, &px, &py), px -= PJ_RGN_X, py -= PJ_RGN_Y;
	px = px < 0 ? 0 : px >= PJ_RGN_W ? PJ_RGN_W - 1 : px;
	py = py < 0 ? 0 : py >= PJ_RGN_H ? PJ_RGN_H - 1 : py;
	*x = px * vid.uw / PJ_RGN_W, *y = py * vid.uh / PJ_RGN_H;
}
static size_t build_mt(uint8_t *buf, size_t cap, uint64_t ts, const int *idx, int n, int action_index, int action)
{
	uint8_t te[320]; pb_out t = { te, 0, sizeof te, 0 };
	for (int k = 0; k < n; k++) {
		int px, py;
		mt_ui(idx[k], &px, &py);
		uint8_t loc[24]; pb_out l = { loc, 0, sizeof loc, 0 };
		pb_u(&l, AA_TL_X, (uint64_t)px); pb_u(&l, AA_TL_Y, (uint64_t)py);
		pb_u(&l, AA_TL_POINTER_ID, (uint64_t)idx[k]);   /* the slot: the same id for a finger's whole stroke */
		pb_b(&t, AA_TE_TOUCH_LOCATION, loc, l.n);
	}
	pb_u(&t, AA_TE_ACTION_INDEX, (uint64_t)action_index);
	pb_u(&t, AA_TE_TOUCH_ACTION, (uint64_t)action);
	pb_out o = { buf, 0, cap, 0 };
	pb_u(&o, AA_IEI_TIMESTAMP, ts);
	pb_b(&o, AA_IEI_TOUCH_EVENT, te, t.n);
	return t.bad || o.bad ? 0 : o.n;
}
// The phone's clock minus ours (CLOCK_MONOTONIC), in nanoseconds: its pings carry its time in nanoseconds (its video, in
// microseconds, the same clock: its uptime), and touch events must go out on it. On ours or since 1970 the phone read
// them as a thousand times faster (microseconds) or as from the future (nanoseconds): a quick flick flew the map ten
// screens, or for ever. Each ping updates it; a gesture keeps the one it started with (mt_frame).
static int64_t phone_off;
static int phone_off_seen;   // from a ping, which supersedes the video's estimate
static int phone_off_ok;     // known (a ping or a timestamped frame): no gesture goes to the phone before it is
// a ping's sample of it, less the ping's way here: the least-delayed (the highest) is the closest, kept and let sink
// 1 ms a ping (the two clocks may drift); a ping held up behind a video burst does not drag it down
static void phone_clock(int64_t smp)
{
	phone_off = !phone_off_seen || smp > phone_off - 1000000 ? smp : phone_off - 1000000;
	phone_off_seen = phone_off_ok = 1;
}
static int mt_force_visible, mt_log[16][3], mt_nlog;   /* the self-test's view: no screen, events recorded */
static uint64_t mt_log_ts[16];
static void mt_send(int fd, aa_tls_t *tls, uint64_t ts, int ai, int action, int with)
{
	int idx[MT_MAX], n = 0, at = 0;   /* the list: every finger on, plus `with` (one going up) */
	for (int k = 0; k < MT_MAX; k++) if (mt[k].on || k == with) { if (k == with) at = n; idx[n++] = k; }
	if (fd >= 0 && !tls) return;   /* nothing goes to a phone in plain text */
	if (fd < 0) {   /* the self-test records; --touch-test prints what would go to the phone */
		if (mt_nlog < 16) { mt_log[mt_nlog][0] = action; mt_log[mt_nlog][1] = n; mt_log[mt_nlog][2] = with >= 0 ? at : ai; mt_log_ts[mt_nlog++] = ts; }
		if (fd == -2) {
			printf("t=%llu.%06llu action %d index %d:", (unsigned long long)(ts / 1000000000), (unsigned long long)(ts % 1000000000 / 1000), action, with >= 0 ? at : ai);
			for (int k = 0; k < n; k++) {
				int px, py;
				mt_ui(idx[k], &px, &py);
				printf(" [id %d at %d,%d]", idx[k], px, py);
			}
			printf("\n"); fflush(stdout);
		}
		return;
	}
	uint8_t b[400]; size_t len = build_mt(b, sizeof b, ts, idx, n, with >= 0 ? at : ai, action);
	if (len) send_msg(fd, tls, AA_CH_INPUT, AA_MSG_SPECIFIC, AA_IN_EVENT, b, len);
}
// one SYN_REPORT: turn the slots' changes into Android Auto touch events
static void mt_frame(int fd, aa_tls_t *tls, uint64_t ts)
{
	static int64_t g_off;   /* phone_off for this gesture: a ping's update mid-stroke would bend its speed */
	if (!mt_prev_any) g_off = phone_off;
	int64_t st = (int64_t)ts + g_off;   /* never below 0 (a sample off the phone's uptime would wrap it to the far future) */
	ts = st > 0 ? (uint64_t)st : 0;
	if (!mt_prev_any) {   /* a gesture starts: ours only if its first finger is on the picture, now */
		int first = -1;
		for (int i = 0; i < MT_MAX && first < 0; i++) if (mt[i].active) first = i;
		if (first >= 0) {
			int x, y;
			mt_canvas(first, &x, &y);
			mt_gesture = tls && input_open && phone_off_ok && (mt_force_visible || access(PJ_VISIBLE, F_OK) == 0) &&
				     x >= PJ_RGN_X && x < PJ_RGN_X + PJ_RGN_W && y >= PJ_RGN_Y && y < PJ_RGN_Y + PJ_RGN_H ? 1 : -1;
		}   /* (and not before the phone's clock is known) */
	}
	if (mt_gesture == 1) {
		for (int i = 0; i < MT_MAX; i++) if (mt[i].active && !mt[i].on) {   /* fingers down */
			int n = 0; for (int k = 0; k < MT_MAX; k++) n += mt[k].on;
			mt[i].on = 1; mt[i].moved = 0;
			mt_send(fd, tls, ts, 0, n ? AA_TOUCH_POINTER_DOWN : AA_TOUCH_PRESS, i);
		}
		int moved = 0;                                                     /* the fingers that stay on, moving */
		for (int k = 0; k < MT_MAX; k++) moved |= mt[k].on && mt[k].active && mt[k].moved;
		if (moved) mt_send(fd, tls, ts, 0, AA_TOUCH_DRAG, -1);
		for (int i = 0; i < MT_MAX; i++) if (mt[i].on && !mt[i].active) {   /* fingers up: the last one is the UP */
			mt[i].on = 0;
			int n = 0; for (int k = 0; k < MT_MAX; k++) n += mt[k].on;
			mt_send(fd, tls, ts, 0, n ? AA_TOUCH_POINTER_UP : AA_TOUCH_RELEASE, i);
		}
	} else for (int i = 0; i < MT_MAX; i++) mt[i].on = 0;
	mt_prev_any = 0;
	for (int i = 0; i < MT_MAX; i++) { mt[i].moved = 0; mt_prev_any |= mt[i].active; }
	if (!mt_prev_any) mt_gesture = 0;
}
// the touchscreen's slots as the kernel holds them now: after events were dropped (between sessions the queue
// overflows) the stream alone would leave a stale slot or position (it reports only what changes)
static void mt_resync(int tf)
{
	struct input_absinfo a;
	if (ioctl(tf, EVIOCGABS(ABS_MT_SLOT), &a)) return;   /* no slots to read back */
	mt_cur = a.value >= 0 && a.value < MT_MAX ? a.value : -1;
	/* only the slots the device has (a GT911: 5): the kernel fills no more, and what it leaves is never read */
	int n = a.maximum + 1 < MT_MAX ? a.maximum + 1 : MT_MAX;
	static const uint32_t codes[3] = { ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
	struct { uint32_t code; int32_t v[MT_MAX]; } q;
	for (int c = 0; c < 3; c++) {
		q.code = codes[c];
		for (int k = 0; k < MT_MAX; k++) q.v[k] = c == 0 ? -1 : c == 1 ? mt[k].x : mt[k].y;   /* unchanged if unfilled */
		if (ioctl(tf, EVIOCGMTSLOTS(sizeof q), &q)) continue;
		for (int k = 0; k < n; k++)
			if (c == 0) mt[k].active = q.v[k] >= 0; else if (c == 1) mt[k].x = q.v[k]; else mt[k].y = q.v[k];
	}
	for (int k = n; k < MT_MAX; k++) mt[k].active = 0;   /* slots the device lacks: never a finger */
}
// read what the touchscreen has; with tls NULL (between sessions) just keep the slots in step, sending nothing
static void touch_read(int tf, int fd, aa_tls_t *tls)
{
	struct input_event ev[64]; ssize_t r;
	while ((r = read(tf, ev, sizeof ev)) > 0)
		for (int k = 0; k < (int)(r / (ssize_t)sizeof *ev); k++) {
			const struct input_event *e = &ev[k];
			if (mt_dropped && !(e->type == EV_SYN && e->code == SYN_REPORT)) continue;
			if (e->type == EV_ABS) {
				if (e->code == ABS_MT_SLOT) { mt_cur = e->value >= 0 && e->value < MT_MAX ? e->value : -1; continue; }
				if (mt_cur < 0) continue;
				mt_slot_t *s = &mt[mt_cur];
				if (e->code == ABS_MT_TRACKING_ID) s->active = e->value >= 0;
				else if (e->code == ABS_MT_POSITION_X) s->x = e->value, s->moved = 1;
				else if (e->code == ABS_MT_POSITION_Y) s->y = e->value, s->moved = 1;
			} else if (e->type == EV_SYN && e->code == SYN_DROPPED) {   /* the kernel lost events: its state, read back at */
				mt_dropped = 1;                                              /* the frame's end (what follows is partial) */
			} else if (e->type == EV_SYN && e->code == SYN_REPORT) {
				if (mt_dropped) mt_resync(tf), mt_dropped = 0;
				mt_frame(fd, tls, ((uint64_t)e->input_event_sec * 1000000u + (uint64_t)e->input_event_usec) * 1000u);   /* ns */
			}
		}
}

// the session after SSL + auth: answer the phone's requests, ping it, route video to the sink, ACK (and drop)
// audio, send the touchscreen's touches
#define AA_SILENT_US 6000000   /* the phone answers our 1.5 s pings: this long without a byte, it is gone */
// the night-mode sensor: the dash's day/night (PJ_NIGHT), sent when the phone starts it and again on every change
static int night_now(void)
{
	int c = '0';
	FILE *f = fopen(PJ_NIGHT, "r");
	if (f) c = fgetc(f), fclose(f);
	return c == '1';
}
static void send_night(int fd, aa_tls_t *tls, int night)
{
	uint8_t ev[24]; pb_out e = { ev, 0, sizeof ev, 0 }; uint8_t sub[8]; pb_out b = { sub, 0, sizeof sub, 0 };
	pb_u(&b, AA_NIGHT_IS_NIGHT, night); pb_b(&e, AA_SENEV_NIGHT_MODE, sub, b.n);
	send_msg(fd, tls, AA_CH_SENSOR, AA_MSG_SPECIFIC, AA_SEN_EVENT, ev, e.n);
}
static void aa_session(int fd, aa_tls_t *tls, int sink, int *touchp)
{
	int night_on = 0, night_sent = -1;   /* the phone started the night sensor; what it was last told */
	int touch = *touchp;
	const uint8_t *m; size_t mn; uint8_t chan; uint32_t frames = 0, st_frames = 0; uint64_t st_bytes = 0;
	static uint32_t nlog; static int64_t nlog_at;   /* the log is on /data: 300 messages logged per 10 min, across sessions */
	if (now_us() - nlog_at > 600000000LL) nlog = 0, nlog_at = now_us();
	memset(av_sess, 0, sizeof av_sess); input_open = 0;
	for (int k = 0; k < MT_MAX; k++) mt[k].active = mt[k].on = mt[k].moved = 0;   // a finger left down at the last
	mt_prev_any = 0; mt_gesture = 0;   // session's end: forgotten (positions kept: the kernel sends only changes)
	if (touch >= 0) {   // catch up on what happened before this session, sending nothing, then the kernel's own state
		touch_read(touch, fd, NULL);
		mt_resync(touch), mt_dropped = 0;
		mt_gesture = 0;
		for (int k = 0; k < MT_MAX; k++) mt[k].on = mt[k].moved = 0, mt_prev_any |= mt[k].active;   // a finger down
	}                                                                    // now: ignored until all are up
	fprintf(stderr, "aa: session loop: waiting for the phone's first message\n");
	int64_t next_ping = now_us() + AA_PING_EVERY_US, last_rx = now_us(), next_stat = now_us() + 5000000, quit_until = 0;
	for (;;) {
		if (aa_quit == 1) {   // stopped: the phone told first (dash-aa stops the decoder at the same time: no matter now)
			uint8_t q[4]; pb_out o = { q, 0, sizeof q, 0 }; pb_u(&o, AA_SHUTDOWN_REASON, AA_SHUTDOWN_QUIT);
			if (!tx_dead) send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SHUTDOWN_REQUEST, q, o.n);
			fprintf(stderr, "aa: stopped: telling the phone the session ends\n");
			aa_quit = 2, quit_until = now_us() + 2000000;
		}
		if (aa_quit == 2) {
			if (tx_dead || now_us() >= quit_until) { fprintf(stderr, "aa: ending the session: stopped (%s)\n", tx_dead ? "the phone gone" : "no answer in 2 s"); break; }
		} else {
			// the decoder died (bad or unsupported stream): end the session rather than leave its last picture up as if
			// live; the phone reconnects and a fresh decoder starts
			if (sink == -2) { fprintf(stderr, "aa: ending the session: no video decoder\n"); break; }
			if (tx_dead) { fprintf(stderr, "aa: ending the session: a write to the phone failed (it stopped reading)\n"); break; }
		}
		int64_t t = now_us(), left = next_ping - t;
		if (t - last_rx > AA_SILENT_US) { fprintf(stderr, "aa: phone silent for %d s, ending the session\n", AA_SILENT_US / 1000000); break; }
		if (t >= next_stat) {   // what the video path did: frames and data in from the phone
			fprintf(stderr, "aa: 5 s: %u video frames, %llu KB\n", st_frames, (unsigned long long)(st_bytes >> 10));
			st_frames = 0; st_bytes = 0; next_stat = t + 5000000;
		}
		if (left <= 0) {
			send_ping(fd, tls); next_ping = t + AA_PING_EVERY_US; left = AA_PING_EVERY_US;
			if (night_on) { int n = night_now(); if (n != night_sent) send_night(fd, tls, n), night_sent = n; }   /* dusk, dawn */
			if (access(PJ_AA_FOCUS, F_OK) == 0) {   /* the dash's AA button after AA's own Exit: the map back (unrequested) */
				unlink(PJ_AA_FOCUS);
				uint8_t vf[8]; pb_out o = { vf, 0, sizeof vf, 0 }; pb_u(&o, AA_VFI_FOCUS_MODE, AA_FOCUS_FOCUSED); pb_u(&o, AA_VFI_UNREQUESTED, 1);
				if (!send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_VIDEO_FOCUS_INDICATION, vf, o.n)) {   /* (not for a phone gone) */
					close(open("/run/dash-project", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
					fprintf(stderr, "aa: video focus taken back (the dash's AA button)\n");
				}
			}
			if (tx_dead) continue;   /* to the loop's top: the session ends now */
		}
		struct pollfd pf[2] = { { fd, POLLIN, 0 }, { touch, POLLIN, 0 } };
		if (aa_quit == 2) { int64_t q = quit_until - now_us(); if (q < left) left = q > 0 ? q : 0; }   // woken for its deadline
		int pr = poll(pf, touch >= 0 ? 2 : 1, (int)(left / 1000) + 1);
		if (pr < 0) { if (errno == EINTR) continue; break; }
		if (pr == 0) continue;
		if (touch >= 0 && (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {   // the touchscreen went away (its queue
			fprintf(stderr, "aa: touch device gone\n"); close(touch); *touchp = touch = -1;   // may still read POLLIN): for
		} else if (touch >= 0 && (pf[1].revents & POLLIN)) touch_read(touch, fd, tls);     // the next sessions too
		if (!(pf[0].revents & POLLIN)) { if (pf[0].revents) break; continue; }   // hung up / error with nothing to read
		if (recv_msg(fd, tls, &chan, &m, &mn) || mn < 2) break;
		last_rx = now_us();
		uint16_t id = msg_id(m, mn); const uint8_t *body = m + 2; size_t blen = mn - 2;
		if (!(is_av(chan) && is_media(id)) && !(chan == AA_CH_CONTROL && (id == AA_CTL_PING_REQUEST || id == AA_CTL_PING_RESPONSE))) {
			if (nlog < 300) fprintf(stderr, "aa: rx chan %u msg 0x%04x (%zu bytes)\n", chan, id, blen);   // the log is on /data:
			else if (nlog == 300) fprintf(stderr, "aa: further messages not logged (300 per 10 min)\n");   // never flooded
			if (nlog <= 300) nlog++;
		}
		if (chan == AA_CH_CONTROL && id == AA_CTL_SERVICE_DISCOVERY_REQUEST) {
			vid = video_wanted();
			if (nlog <= 300) fprintf(stderr, "aa: video %dx%d at %d fps, the phone's UI %dx%d\n", vid.fw, vid.fh, vid.fps == AA_FPS_60 ? 60 : 30, vid.uw, vid.uh);
			uint8_t sd[512]; size_t sn = build_service_discovery_response(sd, sizeof sd, &vid);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SERVICE_DISCOVERY_RESPONSE, sd, sn);
		} else if (id == AA_CTL_CHANNEL_OPEN_REQUEST) {
			// The request comes ON the channel being opened (a control-type message there), and the answer goes back
			// on that same channel (aasdk *ServiceChannel::sendChannelOpenResponse, headunit hu_handle_ChannelOpenRequest).
			uint8_t co[16]; size_t cn = build_channel_open_response(co, sizeof co, AA_STATUS_OK);
			send_msg(fd, tls, chan, AA_MSG_CONTROL, AA_CTL_CHANNEL_OPEN_RESPONSE, co, cn);
			if (chan == AA_CH_INPUT) input_open = 1;
			if (nlog <= 300) fprintf(stderr, "aa: channel %u opened\n", chan);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_AUDIO_FOCUS_REQUEST) {
			uint64_t ty = 0; pb_get_u(body, blen, AA_AFREQ_TYPE, &ty);
			uint8_t r[8]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_AFRESP_STATE, ty == AA_AF_RELEASE ? AA_AF_LOSS : AA_AF_GAIN);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_AUDIO_FOCUS_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_NAV_FOCUS_REQUEST) {
			uint8_t r[8]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_NAVF_TYPE, 2);   // 2 = projected nav
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_NAV_FOCUS_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_PING_REQUEST) {
			uint64_t ts = 0;
			if (pb_get_u(body, blen, AA_PING_TS, &ts) && ts && ts < (uint64_t)INT64_MAX / 2) {   /* its clock now */
				if (!phone_off_seen) fprintf(stderr, "aa: the phone's clock: %llu ns (its pings)\n", (unsigned long long)ts);
				phone_clock((int64_t)ts - now_us() * 1000);
			}
			uint8_t r[16]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_PING_TS, ts);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_PING_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_SHUTDOWN_REQUEST) {
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SHUTDOWN_RESPONSE, (const uint8_t *)"", 0);
			fprintf(stderr, "aa: phone asked to shut down\n");
			break;
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_SHUTDOWN_RESPONSE) {
			fprintf(stderr, "aa: the phone acknowledged the end of the session\n");
			break;
		} else if (chan == AA_CH_INPUT && id == AA_IN_BINDING_REQUEST) {
			uint8_t r[8]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_BINDRESP_STATUS, AA_STATUS_OK);
			send_msg(fd, tls, AA_CH_INPUT, AA_MSG_SPECIFIC, AA_IN_BINDING_RESPONSE, r, o.n);
		} else if (chan == AA_CH_SENSOR && id == AA_SEN_START_REQUEST) {
			uint64_t st = 0; pb_get_u(body, blen, AA_SENREQ_TYPE, &st);
			uint8_t sr[8]; pb_out o = { sr, 0, sizeof sr, 0 }; pb_u(&o, AA_SENRESP_STATUS, AA_STATUS_OK);
			send_msg(fd, tls, AA_CH_SENSOR, AA_MSG_SPECIFIC, AA_SEN_START_RESPONSE, sr, o.n);
			// seed one reading so the phone has a value. ponytail: driving status hard-wired UNRESTRICTED;
			// wire it to the car's speed/gear (parked vs moving) from the CAN bridge when that's plumbed in.
			uint8_t ev[24]; pb_out e = { ev, 0, sizeof ev, 0 }; uint8_t sub[8]; pb_out b = { sub, 0, sizeof sub, 0 };
			if (st == AA_SENSOR_DRIVING_STATUS)   { pb_u(&b, AA_DRV_STATUS, 0);     pb_b(&e, AA_SENEV_DRIVING_STATUS, sub, b.n); }
			else if (st == AA_SENSOR_NIGHT_DATA)  { night_on = 1; night_sent = night_now(); pb_u(&b, AA_NIGHT_IS_NIGHT, (uint64_t)night_sent); pb_b(&e, AA_SENEV_NIGHT_MODE, sub, b.n); }
			if (e.n) send_msg(fd, tls, AA_CH_SENSOR, AA_MSG_SPECIFIC, AA_SEN_EVENT, ev, e.n);
		} else if (is_av(chan) && id == AA_AV_SETUP_REQUEST) {
			uint8_t r[24]; size_t rn = build_av_setup_response(r, sizeof r);
			send_msg(fd, tls, chan, AA_MSG_SPECIFIC, AA_AV_SETUP_RESPONSE, r, rn);
			if (chan == AA_CH_VIDEO) {   // the HU takes video focus once set up (headunit MediaSetupComplete): unrequested
				uint8_t vf[8]; pb_out o = { vf, 0, sizeof vf, 0 }; pb_u(&o, AA_VFI_FOCUS_MODE, AA_FOCUS_FOCUSED); pb_u(&o, AA_VFI_UNREQUESTED, 1);
				send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_VIDEO_FOCUS_INDICATION, vf, o.n);
				if (nlog <= 300) fprintf(stderr, "aa: video setup done, focus taken\n");
			}
		} else if (chan == AA_CH_VIDEO && id == AA_AV_VIDEO_FOCUS_REQUEST) {
			// as asked, not unrequested (OpenAuto VideoService::onVideoFocusRequest). UNFOCUSED is AA's own Exit (its
			// launcher's head-unit tile): the dash comes back (the UI leaves the split as the marker goes); a later
			// FOCUSED brings the map back
			uint64_t mode = AA_FOCUS_FOCUSED;
			pb_get_u(body, blen, AA_VFR_FOCUS_MODE, &mode);
			int off = mode == AA_FOCUS_UNFOCUSED;
			uint8_t vf[8]; pb_out o = { vf, 0, sizeof vf, 0 }; pb_u(&o, AA_VFI_FOCUS_MODE, off ? AA_FOCUS_UNFOCUSED : AA_FOCUS_FOCUSED); pb_u(&o, AA_VFI_UNREQUESTED, 0);
			send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_VIDEO_FOCUS_INDICATION, vf, o.n);
			if (off) unlink("/run/dash-project");
			else close(open("/run/dash-project", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
			if (nlog <= 300) fprintf(stderr, off ? "aa: video focus released (AA's Exit): the dash back\n" : "aa: video focus granted\n");
		} else if (chan == AA_CH_AV_INPUT && id == AA_AV_INPUT_OPEN_REQUEST) {
			uint8_t r[16]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_AVINRESP_SESSION, av_sess[AA_CH_AV_INPUT]); pb_u(&o, AA_AVINRESP_VALUE, 0);
			send_msg(fd, tls, AA_CH_AV_INPUT, AA_MSG_SPECIFIC, AA_AV_INPUT_OPEN_RESPONSE, r, o.n);
		} else if (is_av(chan) && id == AA_AV_START) {
			uint64_t ss = 0; pb_get_u(body, blen, AA_START_SESSION, &ss); av_sess[chan & 15] = (uint32_t)ss;
			if (nlog <= 300) fprintf(stderr, "aa: av start chan %u session %u\n", chan, av_sess[chan & 15]);
		} else if (is_av(chan) && is_media(id)) {
			if (chan == AA_CH_VIDEO) {
				if (!frames++) fprintf(stderr, "aa: first video frame (%zu bytes)\n", blen);
				st_frames++; st_bytes += blen;
				const uint8_t *nal = body; size_t nl = blen;
				if (id == AA_AV_MEDIA_WITH_TIMESTAMP) {   // 8-byte timestamp (big-endian)
					if (nl <= 8) goto acked;
					if (!phone_off_seen && frames <= 3) {   /* the phone's clock until its first ping: microseconds */
						uint64_t vt = 0; for (int i = 0; i < 8; i++) vt = vt << 8 | nal[i];
						if (vt && vt < (uint64_t)INT64_MAX / 2 / 1000) phone_off = (int64_t)vt * 1000 - now_us() * 1000, phone_off_ok = 1;
					}
					nal += 8; nl -= 8;
				}
				// then an access-unit delimiter: the decoder takes a NAL as complete when the next start code
				// comes, so without it every picture would wait for the following one (a still screen: forever)
				static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xf0 };
				if (sink >= 0 && (sink_write(sink, nal, nl) || sink_write(sink, aud, sizeof aud))) { fprintf(stderr, "aa: video sink died or stuck\n"); sink = -2; }   /* aa_serve, its owner, closes it */
			}   // ponytail: audio is ACKed and dropped — no sound out yet; pipe to ALSA when the Dash gets a speaker path
		acked:
			ack(fd, tls, chan);
		}
	}
	fprintf(stderr, "aa: %u video frames this session\n", frames);
}

// touch self-test: a two-finger pinch, then a stroke that starts off the picture; checks the Android actions
static int mt_selftest(void)
{
	aa_tls_t *fake = (aa_tls_t *)&mt_nlog;   /* any non-NULL: mt_send records instead of sending when fd < 0 */
	input_open = 1; mt_force_visible = 1; phone_off_ok = 1; memset(mt, 0, sizeof mt); mt_prev_any = 0; mt_gesture = 0;
	#define F(slot, act, X, Y) (mt[slot].active = (act), mt[slot].x = (X), mt[slot].y = (Y), mt[slot].moved = 1)
	F(0, 1, 500, 200); mt_frame(-1, fake, 1);                       /* finger 0 down on the picture */
	F(1, 1, 700, 250); mt_frame(-1, fake, 2);                       /* finger 1 down */
	F(0, 1, 480, 190); F(1, 1, 720, 260); mt_frame(-1, fake, 3);    /* both move */
	mt[0].active = 0; mt_frame(-1, fake, 4);                        /* finger 0 up */
	mt[1].active = 0; mt_frame(-1, fake, 5);                        /* finger 1 up */
	F(0, 1, 1600, 200); mt_frame(-1, fake, 6);                      /* a stroke from the dash strip (x 1600) */
	F(0, 1, 600, 200); mt_frame(-1, fake, 7); mt[0].active = 0; mt_frame(-1, fake, 8);   /* ...even onto the picture */
	static const int want[][3] = { { AA_TOUCH_PRESS, 1, 0 }, { AA_TOUCH_POINTER_DOWN, 2, 1 }, { AA_TOUCH_DRAG, 2, 0 },
	                               { AA_TOUCH_POINTER_UP, 2, 0 }, { AA_TOUCH_RELEASE, 1, 0 } };
	int ok = mt_nlog == 5;
	for (int i = 0; ok && i < 5; i++) ok = !memcmp(mt_log[i], want[i], sizeof want[i]);
	if (!ok) {
		for (int i = 0; i < mt_nlog; i++) fprintf(stderr, "  got action %d, %d fingers, index %d\n", mt_log[i][0], mt_log[i][1], mt_log[i][2]);
		die("touch selftest: wrong event sequence (%d events)", mt_nlog);
	}
	/* the phone's clock (an hour behind ours): touches stamped on it, a gesture keeping its offset through a ping, the
	 * least-delayed ping kept */
	phone_off_seen = 0; phone_clock(-3600000000000LL); mt_nlog = 0;
	F(0, 1, 500, 200); mt_frame(-1, fake, 5000000000000ULL);
	phone_clock(-3500000000000LL);
	F(0, 1, 520, 200); mt_frame(-1, fake, 5000020000000ULL);
	mt[0].active = 0; mt_frame(-1, fake, 5000040000000ULL);
	ok = mt_nlog == 3 && mt_log_ts[0] == 1400000000000ULL && mt_log_ts[1] == 1400020000000ULL && mt_log_ts[2] == 1400040000000ULL;
	phone_off_seen = 0; phone_clock(7000000); phone_clock(2000000);   /* a ping 5 ms more delayed: 1 ms down only */
	if (!ok || phone_off != 6000000) die("touch selftest: the phone's clock (%d events, offset %lld)", mt_nlog, (long long)phone_off);
	phone_off_seen = 0; phone_clock(-10000); mt_nlog = 0; F(0, 1, 500, 200); mt_frame(-1, fake, 3000);   /* clamped at 0 */
	mt[0].active = 0; mt_frame(-1, fake, 4000);
	if (mt_nlog != 2 || mt_log_ts[0] != 0) die("touch selftest: a time below zero not clamped");
	phone_off_ok = 0; mt_nlog = 0; F(0, 1, 500, 200); mt_frame(-1, fake, 5000); mt[0].active = 0; mt_frame(-1, fake, 6000);
	phone_off_ok = 1; F(0, 1, 500, 200); mt_frame(-1, fake, 7000); mt[0].active = 0; mt_frame(-1, fake, 8000);
	if (mt_nlog != 2) die("touch selftest: a gesture before the phone's clock was known went out (%d events)", mt_nlog);
	phone_off = 0, phone_off_seen = phone_off_ok = 0;
	/* a portrait touchscreen (the F133's), turned 270 as the dash's display: 20, 10 into the picture lands at 20, 10 */
	mt_portrait = 1, mt_max_x = PJ_PANEL_H - 1, mt_max_y = PJ_PANEL_W - 1;
	mt[0].x = PJ_PANEL_H - 1 - (PJ_RGN_Y + 10), mt[0].y = PJ_RGN_X + 20;
	int ux, uy; vid = AA_VIDEO_720; mt_ui(0, &ux, &uy);
	if (ux != 20 * vid.uw / PJ_RGN_W || uy != 10 * vid.uh / PJ_RGN_H) die("touch selftest: portrait touch at %d,%d, want 20,10", ux, uy);
	mt_portrait = 0, mt_max_x = PJ_PANEL_W - 1, mt_max_y = PJ_PANEL_H - 1;
	#undef F
	uint8_t b[400]; int idx[2] = { 0, 1 }; mt[0].x = 120; mt[1].x = 1279; mt[0].y = 0; mt[1].y = 479;   /* picture corners */
	size_t n = build_mt(b, sizeof b, 9, idx, 2, 1, AA_TOUCH_POINTER_DOWN);
	const uint8_t *te; size_t tl; uint64_t act, ai;
	if (!n || !pb_get_b(b, n, AA_IEI_TOUCH_EVENT, &te, &tl) || !pb_get_u(te, tl, AA_TE_TOUCH_ACTION, &act) || act != AA_TOUCH_POINTER_DOWN ||
	    !pb_get_u(te, tl, AA_TE_ACTION_INDEX, &ai) || ai != 1) die("touch selftest: event encoding");
	input_open = 0; mt_force_visible = 0; memset(mt, 0, sizeof mt); mt_prev_any = 0; mt_gesture = 0;
	printf("AA-TOUCH-SELFTEST OK: pinch = PRESS, POINTER_DOWN, DRAG, POINTER_UP, RELEASE; off-picture stroke ignored; on the phone's clock\n");
	return 0;
}

// receive-path self-test: a 40 KB message split over FIRST/MIDDLE/LAST frames, each frame its own TLS records, with
// a whole message on another channel interleaved between them; recv_msg must return both, intact, in order
static void put_enc(int fd, aa_tls_t *t, uint8_t ch, uint8_t ft, const uint8_t *p, size_t n, uint32_t total)
{
	uint8_t c[AA_MAX_FRAME_PAYLOAD + 512], h[8]; int cn = 0, k;
	SSL_write(t->ssl, p, (int)n);
	while ((k = tls_take(t, c + cn, sizeof c - (size_t)cn)) > 0) cn += k;
	h[0] = ch; h[1] = (uint8_t)(ft | AA_ENC_ENCRYPTED); h[2] = (uint8_t)(cn >> 8); h[3] = (uint8_t)cn;
	h[4] = (uint8_t)(total >> 24); h[5] = (uint8_t)(total >> 16); h[6] = (uint8_t)(total >> 8); h[7] = (uint8_t)total;
	if (writen(fd, h, ft == AA_FRAME_FIRST ? 8 : 4) || writen(fd, c, (size_t)cn)) die("rx selftest: write");
}
static int rx_selftest(void)
{
	char crt[256], key[256];
	tmp_path(crt, sizeof crt, "aa-rx.crt"), tmp_path(key, sizeof key, "aa-rx.key");
	if (gen_cert(crt, key, 86400)) die("rx selftest: cert");
	SSL_CTX *sc = tls_ctx(crt, key, 1), *cc = tls_ctx(crt, key, 0);
	aa_tls_t ph, hu; tls_init(&ph, sc, 1); tls_init(&hu, cc, 0);   // the phone is the TLS server, the HU the client
	for (int i = 0; i < 20 && !(SSL_is_init_finished(ph.ssl) && SSL_is_init_finished(hu.ssl)); i++) {
		if (tls_handshake(&hu) < 0 || tls_handshake(&ph) < 0) die("rx selftest: handshake");
		pump(&hu, &ph); pump(&ph, &hu);
	}
	int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) die("rx selftest: socketpair");
	enum { BIG = 40002 };
	static uint8_t big[BIG]; big[0] = 0; big[1] = AA_AV_MEDIA; for (int i = 2; i < BIG; i++) big[i] = (uint8_t)(i * 7 + 3);
	uint8_t ping[4] = { 0, AA_CTL_PING_REQUEST, 0x08, 0x2a };
	put_enc(sv[0], &ph, AA_CH_VIDEO, AA_FRAME_FIRST, big, 16000, BIG);
	put_enc(sv[0], &ph, AA_CH_CONTROL, AA_FRAME_BULK, ping, sizeof ping, 0);
	put_enc(sv[0], &ph, AA_CH_VIDEO, AA_FRAME_MIDDLE, big + 16000, 16000, 0);
	put_enc(sv[0], &ph, AA_CH_VIDEO, AA_FRAME_LAST, big + 32000, BIG - 32000, 0);
	uint8_t ch; const uint8_t *m; size_t n;
	if (recv_msg(sv[1], &hu, &ch, &m, &n) || ch != AA_CH_CONTROL || n != sizeof ping || memcmp(m, ping, n)) die("rx selftest: interleaved message");
	if (recv_msg(sv[1], &hu, &ch, &m, &n) || ch != AA_CH_VIDEO || n != BIG || memcmp(m, big, n)) die("rx selftest: split message (got %zu bytes on %u)", n, ch);
	close(sv[0]); close(sv[1]); tls_free(&ph); tls_free(&hu); SSL_CTX_free(sc); SSL_CTX_free(cc);
	printf("AA-RX-SELFTEST OK: 40 KB message over 3 encrypted frames + an interleaved channel, reassembled\n");
	return 0;
}

/* one phone's connection, from the version to its session's end, TCP or USB (fd: the socket; for aa usb and
 * --conn-test, a socketpair); closes fd; true: a session ran */
static int aa_conn(int fd, SSL_CTX *ctx, int *touchp)
{
	while (waitpid(-1, NULL, WNOHANG) > 0) {}   /* a decoder left behind (it would not die) that has died since */
	tx_dead = 0;   /* this phone's: its version and handshake go through send_msg too */
	struct timeval tmo = { 5, 0 }, live = { AA_SILENT_US / 1000000, 0 };   // per read: before auth, and in the session
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
	rd_deadline = now_us() + 20000000;   // version + TLS within 20 s, however the bytes trickle in
	aa_tls_t t; tls_init(&t, ctx, 0);
	uint8_t vr[4] = { AA_VERSION_MAJOR >> 8, AA_VERSION_MAJOR & 0xff, AA_VERSION_MINOR >> 8, AA_VERSION_MINOR & 0xff };
	send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_VERSION_REQUEST, vr, sizeof vr);
	uint8_t chan; const uint8_t *m; size_t mn;
	int ok = recv_msg(fd, NULL, &chan, &m, &mn) == 0 && msg_id(m, mn) == AA_CTL_VERSION_RESPONSE;
	fprintf(stderr, "aa: version %s\n", ok ? "agreed" : "refused");
	int up = ok && do_ssl_handshake(fd, &t) == 0, lk = -1;
	if (up) {   // one phone at a time (PJ_SESSION): taken once TLS is up (a dial that fails before holds nothing), held to
		    // the session's end. aa opens the file nowhere else (closing another descriptor of it would drop this POSIX
		    // lock), and the decoder child does not inherit it
		struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
		if ((lk = open(PJ_SESSION, O_RDWR | O_CREAT | O_CLOEXEC, 0644)) >= 0 && fcntl(lk, F_SETLK, &fl))
			fprintf(stderr, "aa: another phone's session is up: this phone refused\n"), close(lk), lk = -1, up = -1;
	}
	if (up == 1) {
		fprintf(stderr, "aa: TLS up (%s, %s), session starting\n", SSL_get_version(t.ssl), SSL_get_cipher(t.ssl));
		phone_off = 0, phone_off_seen = phone_off_ok = 0;   /* this phone's clock: learned from its video and pings */
		rd_deadline = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &live, sizeof live);   // a frame stalled half-way ends the session
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &live, sizeof live);   // and so does a phone that stops reading
		// AUTH_COMPLETE is the PLAINTEXT handshake-done signal: the phone activates its SSL context on
		// receiving it, and only frames AFTER it are encrypted. Sent encrypted, the phone logs "encrypted
		// frame ... with no ssl context, waiting" and times out. So NULL (plain), like VERSION/SSL_HANDSHAKE.
		uint8_t ac[8]; pb_out o = { ac, 0, sizeof ac, 0 }; pb_u(&o, AA_AC_STATUS, AA_STATUS_OK);
		if (send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_AUTH_COMPLETE, ac, o.n)) {
			fprintf(stderr, "aa: the phone left before its session (AUTH_COMPLETE not sent)\n");
			goto done;   /* no decoder and no split for a session that never starts */
		}
		if (aa_quit) { fprintf(stderr, "aa: stopped during the handshake: no session\n"); goto done; }   /* no split */
		char c; struct pollfd pp = { fd, POLLIN, 0 };   /* nor for one that leaves at once: its first byte first */
		if (poll(&pp, 1, AA_SILENT_US / 1000) != 1 || recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) { fprintf(stderr, "aa: the phone left before its session\n"); goto done; }
		int sink = spawn_sink();
		if (sink < 0) { fprintf(stderr, "aa: no video decoder (%s): no session\n", strerror(errno)); goto done; }
		close(open("/run/dash-project", O_CREAT | O_WRONLY, 0644));   // the dash UI yields the video region
		close(open(PJ_AA_SESSION, O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
		unlink(PJ_AA_FOCUS);   // one left from before
		aa_session(fd, &t, sink, touchp);
		// stop the decoder and wait it out BEFORE dropping the marker: the UI repaints the dash when the marker
		// goes, and a decoder still drawing would leave its last picture over it
		if (sink >= 0) close(sink);
		// reaped only here (SIGCHLD left as it is): a decoder that died mid-session stays a zombie until this waitpid,
		// so its pid cannot go to another process before the kill
		if (sink_pid > 0) {   /* 2 s to go, then killed (a stopped decoder outlasts SIGTERM); one stuck in the kernel even
			                 * then is left behind, never to run again (no picture drawn), and reaped at a later dial */
			kill(sink_pid, SIGTERM);
			int r = 0;
			for (int i = 0; i < 40 && (r = waitpid(sink_pid, NULL, WNOHANG)) == 0; i++) {
				if (i == 20) kill(sink_pid, SIGKILL);
				usleep(100000);
			}
			if (r == 0) fprintf(stderr, "aa: the decoder (pid %d) does not die: left behind\n", (int)sink_pid);
			sink_pid = -1;
		}
		unlink("/run/dash-project");
		unlink(PJ_AA_SESSION), unlink(PJ_AA_FOCUS), unlink(PJ_FPS);   // the decoder, killed, leaves its fps behind
		fprintf(stderr, "aa: session ended\n");
	} else if (ok && !up) {
		unsigned long err = ERR_get_error();
		fprintf(stderr, "aa: TLS handshake failed: %s\n", err ? ERR_error_string(err, NULL) : "peer closed or timed out");
	}
done:
	for (int k = 0; k < AA_NCHAN; k++) free(rx_asm[k].p), rx_asm[k] = (aa_asm_t){0};   /* the next phone starts small */
	tls_free(&t); close(fd);
	if (lk >= 0) close(lk);   // the next phone's turn
	return up == 1;
}

static int aa_serve(const char *cert, const char *key)
{
	signal(SIGPIPE, SIG_IGN);
	// SIGTERM: aa_quit. SA_RESTART keeps reads and writes going; poll() still returns EINTR, so the loops notice
	struct sigaction st = { 0 }; st.sa_handler = aa_on_term; st.sa_flags = SA_RESTART; sigaction(SIGTERM, &st, NULL);
	setvbuf(stderr, NULL, _IOLBF, 0);   // line-buffered: the serve log survives a hard kill (dash-aa stop)
	int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if (ls < 0) die("socket: %s", strerror(errno));   /* not the decoder's */
	int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(AA_PORT);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) die("bind/listen %d: %s", AA_PORT, strerror(errno));
	int touch = touch_open();   // after the bind: a second instance dies above, before touching anything
	SSL_CTX *ctx = tls_ctx(cert, key, 0);   // the head unit is the TLS client: it sends the ClientHello
	fprintf(stderr, "aa: listening on %d (phone must be on the AP; cert %s)\n", AA_PORT, cert);
	for (;;) {
		if (aa_quit) { fprintf(stderr, "aa: stopped\n"); return 0; }
		struct pollfd lp = { ls, POLLIN, 0 };   // not a bare accept: SA_RESTART would restart it past a stop
		if (poll(&lp, 1, 1000) != 1) continue;
		int fd = accept(ls, NULL, NULL); if (fd < 0) continue;
		fcntl(fd, F_SETFD, FD_CLOEXEC);   /* nor the phone's socket */
		// as aasdk (TCPWrapper) and headunit (hu_tcp): small writes (media ACKs, touches, pings) go out at once,
		// not held until the phone acknowledges the previous one -- with one frame in flight, that stalls video
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		fprintf(stderr, "aa: phone dialled in on %d\n", AA_PORT);
		aa_conn(fd, ctx, &touch);
	}   // (a stop during a session: the loop's top, once aa_conn has said goodbye and closed it)
}

#ifdef AA_USB
// ---- wired: Android Open Accessory over usbfs (no libusb needed) ----
// A phone in its normal mode answers AOA's vendor requests: GET_PROTOCOL (51), the identity strings (SEND_STRING, 52:
// the Android Auto app's accessory filter takes manufacturer "Android", model "Android Auto"), START (53). It comes back
// as Google's accessory (18d1:2d00, 2d01 with adb), whose interface 0 has a bulk IN and a bulk OUT endpoint carrying the
// same frames as the TCP socket. A thread copies bulk IN into a socketpair, so aa_conn reads and polls a socket as over
// Wi-Fi; its writes go out one frame a bulk transfer (usb_write). aa usb serves one phone's connection and exits: its
// wrapper cuts the port's power (VBUS) and runs it again, as the stock firmware does before every start, so the phone
// always comes back out of accessory mode for a fresh session.
#define AOA_VID 0x18d1
enum { AOA_GET_PROTOCOL = 51, AOA_SEND_STRING = 52, AOA_START = 53 };
static const char *const aoa_ident[6] = {   /* manufacturer, model, description, version, URI, serial (a placeholder) */
	"Android", "Android Auto", "Android Auto", "2.0.1", "https://www.android.com/auto/", "HU-AAAAAA001" };
typedef struct { uint16_t vid, pid; uint8_t cls; unsigned ep_in, ep_out, mps; } usb_dev_t;
static int usb_fd = -1;
static unsigned usb_ep_out, usb_mps = 512;

// a bulk write's timeout is the session socket's own send timeout (aa_conn sets it once the session starts); before
// that, 20 s: the phone's app may still be starting
static ssize_t usb_write(int fd, const void *b, size_t n)
{
	struct timeval tv = { 0, 0 }; socklen_t tl = sizeof tv;
	getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, &tl);
	unsigned ms = tv.tv_sec || tv.tv_usec ? (unsigned)(tv.tv_sec * 1000 + tv.tv_usec / 1000) : 20000;
	struct usbdevfs_bulktransfer t = { .ep = usb_ep_out, .len = (unsigned)n, .timeout = ms, .data = (void *)b };
	int r = ioctl(usb_fd, USBDEVFS_BULK, &t);
	if (r == (int)n && n % usb_mps == 0) {   /* whole packets end with a zero-length one, else the phone waits for more */
		t.len = 0;
		if (ioctl(usb_fd, USBDEVFS_BULK, &t) < 0) return -1;
	}
	return r;
}

// a usbfs node's descriptors (the device's, then its configurations', as read() gives them) -> ids, class, and the first
// configuration's interface 0 bulk endpoints (the accessory's); 0, or -1 if they are not a device's
static int usb_parse(const uint8_t *b, size_t n, usb_dev_t *d)
{
	if (n < 18 || b[0] < 18 || b[1] != USB_DT_DEVICE) return -1;
	*d = (usb_dev_t){ .vid = (uint16_t)(b[8] | b[9] << 8), .pid = (uint16_t)(b[10] | b[11] << 8), .cls = b[4], .mps = 512 };
	int if0 = 0, configs = 0;
	for (size_t i = b[0]; i + 2 <= n && b[i] >= 2 && i + b[i] <= n; i += b[i]) {
		if (b[i + 1] == USB_DT_CONFIG && ++configs > 1) break;
		if (b[i + 1] == USB_DT_INTERFACE && b[i] >= 9) if0 = b[i + 2] == 0 && b[i + 3] == 0;   /* interface 0, setting 0 */
		else if (b[i + 1] == USB_DT_ENDPOINT && b[i] >= 7 && if0 && (b[i + 3] & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
			if (b[i + 2] & USB_DIR_IN) d->ep_in = b[i + 2];
			else d->ep_out = b[i + 2], d->mps = (b[i + 4] | b[i + 5] << 8) & 0x7ff;
		}
	}
	if (!d->mps) d->mps = 512;
	return 0;
}

// a phone's accessory descriptors (device; configuration; interface 0, the accessory: bulk 0x81 in, 0x02 out; interface
// 1, adb) and a second configuration that must not count -> interface 0's endpoints
static int usb_selftest(void)
{
	static const uint8_t d[] = {
		18, USB_DT_DEVICE, 0x00, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x01, 0x2d, 0, 1, 1, 2, 3, 1,
		9, USB_DT_CONFIG, 55, 0, 2, 1, 0, 0x80, 250,
		9, USB_DT_INTERFACE, 0, 0, 2, 0xff, 0xff, 0, 0,
		7, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0,
		7, USB_DT_ENDPOINT, 0x02, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0,
		9, USB_DT_INTERFACE, 1, 0, 2, 0xff, 0x42, 1, 0,
		7, USB_DT_ENDPOINT, 0x83, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0,
		7, USB_DT_ENDPOINT, 0x04, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0,
		9, USB_DT_CONFIG, 32, 0, 1, 2, 0, 0x80, 250,
		9, USB_DT_INTERFACE, 0, 0, 2, 0xff, 0xff, 0, 0,
		7, USB_DT_ENDPOINT, 0x85, USB_ENDPOINT_XFER_BULK, 0x40, 0x00, 0,
		7, USB_DT_ENDPOINT, 0x06, USB_ENDPOINT_XFER_BULK, 0x40, 0x00, 0,
	};
	usb_dev_t u;
	if (usb_parse(d, sizeof d, &u) || u.vid != AOA_VID || u.pid != 0x2d01 || u.ep_in != 0x81 || u.ep_out != 0x02 || u.mps != 512)
		die("usb selftest: the accessory's endpoints (in 0x%02x out 0x%02x, %u-byte packets)", u.ep_in, u.ep_out, u.mps);
	if (!usb_parse(d, 17, &u) || !usb_parse(d + 18, sizeof d - 18, &u)) die("usb selftest: not a device's descriptors, taken");
	printf("AA-USB-SELFTEST OK: the accessory's bulk endpoints from its descriptors\n");
	return 0;
}

static int usb_ctrl(int fd, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	struct usbdevfs_ctrltransfer c = { .bRequestType = type, .bRequest = req, .wValue = val, .wIndex = idx, .wLength = len,
					   .timeout = 1000, .data = data };
	return ioctl(fd, USBDEVFS_CONTROL, &c);
}

// The phone, opened as an accessory with interface 0 claimed: every second, each USB device (/dev/bus/usb/BBB/DDD, from
// devtmpfs) is looked at: an accessory is opened (one left from an earlier run is used as it is, as AOA says; its power
// cut by the wrapper first is the sure start); any other device but a hub is asked for AOA once (a refusal: not again).
// Returns its usbfs fd, *d its endpoints.
static int usb_accessory(usb_dev_t *d)
{
	static char asked[16][64]; static unsigned nasked;   /* nodes asked already (devnums only grow: no clash soon) */
	for (int said = 0;; sleep(1)) {
		DIR *bus = opendir("/dev/bus/usb");
		if (!bus) { if (!said++) fprintf(stderr, "aa: no /dev/bus/usb (no USB host, or no mdev): waiting\n"); continue; }
		struct dirent *be, *de;
		while ((be = readdir(bus))) {
			char dir[48]; DIR *dv;
			if (be->d_name[0] == '.' || snprintf(dir, sizeof dir, "/dev/bus/usb/%.8s", be->d_name) >= (int)sizeof dir || !(dv = opendir(dir))) continue;
			while ((de = readdir(dv))) {
				char path[64]; uint8_t desc[4096]; ssize_t n; usb_dev_t u; int fd, seen = 0;
				if (de->d_name[0] == '.' || snprintf(path, sizeof path, "%s/%.8s", dir, de->d_name) >= (int)sizeof path) continue;
				if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0) continue;
				if ((n = read(fd, desc, sizeof desc)) < 18 || usb_parse(desc, (size_t)n, &u) || u.cls == USB_CLASS_HUB) { close(fd); continue; }
				if (u.vid == AOA_VID && u.pid >= 0x2d00 && u.pid <= 0x2d05) {   /* an accessory */
					unsigned ifc = 0;
					int bad = !u.ep_in || !u.ep_out ? -1 : ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) ? errno : 0;
					if (!bad) { closedir(dv), closedir(bus); *d = u; return fd; }
					for (unsigned k = 0; k < nasked && k < 16; k++) seen |= !strcmp(asked[k], path);
					if (!seen) {   /* said once a node: it is looked at again every second, the log is on /data */
						snprintf(asked[nasked++ % 16], sizeof asked[0], "%s", path);
						if (bad < 0) fprintf(stderr, "aa: accessory %s without its two bulk endpoints\n", path);
						else fprintf(stderr, "aa: accessory %s: interface 0: %s\n", path, strerror(bad));
					}
					close(fd); continue;
				}
				for (unsigned k = 0; k < nasked && k < 16; k++) seen |= !strcmp(asked[k], path);
				if (seen) { close(fd); continue; }
				snprintf(asked[nasked++ % 16], sizeof asked[0], "%s", path);
				uint8_t ver[2] = { 0, 0 };
				if (usb_ctrl(fd, USB_DIR_IN | USB_TYPE_VENDOR, AOA_GET_PROTOCOL, 0, 0, ver, 2) == 2 && (ver[0] | ver[1] << 8) >= 1) {
					int ok = 1;
					for (int i = 0; i < 6 && ok; i++)
						ok = usb_ctrl(fd, USB_DIR_OUT | USB_TYPE_VENDOR, AOA_SEND_STRING, 0, (uint16_t)i, (void *)aoa_ident[i],
							      (uint16_t)(strlen(aoa_ident[i]) + 1)) >= 0;
					ok = ok && usb_ctrl(fd, USB_DIR_OUT | USB_TYPE_VENDOR, AOA_START, 0, 0, NULL, 0) >= 0;
					fprintf(stderr, "aa: %s (%04x:%04x) speaks AOA %u: %s\n", path, u.vid, u.pid, ver[0] | ver[1] << 8,
						ok ? "asked to start Android Auto (accessory mode)" : "its switch failed");
				}
				close(fd);
			}
			closedir(dv);
		}
		closedir(bus);
	}
}

typedef struct { int sock, ufd; unsigned ep; } usb_rx_t;
static void *usb_rx(void *arg)   /* bulk IN -> the session's socket, until the phone goes (or aa exits: usbfs waits are killable) */
{
	const usb_rx_t *u = arg;
	static uint8_t b[16384];   /* a multiple of the packet size: a transfer never overflows it */
	for (;;) {
		struct usbdevfs_bulktransfer t = { .ep = u->ep, .len = sizeof b, .timeout = 0, .data = b };   /* no timeout: one would */
		int n = ioctl(u->ufd, USBDEVFS_BULK, &t);                                                   /* drop the bytes it got */
		if (n < 0 || (n > 0 && writen(u->sock, b, (size_t)n))) break;
	}
	shutdown(u->sock, SHUT_RDWR);   /* the session reads the end */
	return NULL;
}

// aa usb: one phone's connection over USB; 0 if its session ran. The reader thread dies with the process.
static int aa_usb_serve(const char *cert, const char *key)
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	int touch = touch_open();
	SSL_CTX *ctx = tls_ctx(cert, key, 0);
	fprintf(stderr, "aa: waiting for a phone on USB (cert %s)\n", cert);
	usb_dev_t d; pthread_t th; int sv[2];
	int ufd = usb_accessory(&d);
	fprintf(stderr, "aa: the phone is on as an accessory (%04x:%04x, bulk in 0x%02x, out 0x%02x, %u-byte packets)\n",
		d.vid, d.pid, d.ep_in, d.ep_out, d.mps);
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) die("socketpair: %s", strerror(errno));
	static usb_rx_t rx;   /* the reader thread reads it until the process ends: not on this function's stack */
	rx = (usb_rx_t){ sv[1], ufd, d.ep_in };
	usb_fd = ufd, usb_ep_out = d.ep_out, usb_mps = d.mps, xwrite = usb_write;
	if (pthread_create(&th, NULL, usb_rx, &rx)) die("no USB reader thread");
	return aa_conn(sv[0], ctx, &touch) ? 0 : 1;
}
#endif // AA_USB

// ---- the connection self-test (aa --conn-test): a phone over a socketpair, through version, TLS, discovery, a channel,
// a ping, its shutdown; then a stop (SIGTERM): the head unit tells the phone ----
// A session raises the dash's markers (/run/dash-project: its split view), so not where a dash runs: the build host.
static int conn_want(int fd, aa_tls_t *t, uint8_t ch, uint16_t id, const uint8_t **m, size_t *mn)
{
	uint8_t c;
	for (int i = 0; i < 50; i++)   /* the head unit's own pings come between */
		if (recv_msg(fd, t, &c, m, mn) || *mn < 2) return -1;
		else if (c == ch && msg_id(*m, *mn) == id) return 0;
	return -1;
}
static void conn_hello(int fd, aa_tls_t *ph)   /* the phone's side, from the version to its service discovery request */
{
	const uint8_t *m; size_t mn; uint8_t cb[AA_MAX_FRAME_PAYLOAD]; int cn;
	if (conn_want(fd, NULL, AA_CH_CONTROL, AA_CTL_VERSION_REQUEST, &m, &mn)) die("conn-test: no version request");
	uint8_t vr[6] = { 0, AA_VERSION_MAJOR, 0, AA_VERSION_MINOR, 0, AA_VER_MATCH };
	send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_VERSION_RESPONSE, vr, sizeof vr);
	while (!SSL_is_init_finished(ph->ssl)) {   /* the head unit's records in, the phone's out */
		if (conn_want(fd, NULL, AA_CH_CONTROL, AA_CTL_SSL_HANDSHAKE, &m, &mn)) die("conn-test: the handshake stopped");
		tls_feed(ph, m + 2, mn - 2);
		if (tls_handshake(ph) < 0) die("conn-test: the phone's handshake failed");
		while ((cn = tls_take(ph, cb, sizeof cb)) > 0) send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SSL_HANDSHAKE, cb, (size_t)cn);
	}
	if (conn_want(fd, NULL, AA_CH_CONTROL, AA_CTL_AUTH_COMPLETE, &m, &mn)) die("conn-test: no auth complete (plain)");
	send_msg(fd, ph, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SERVICE_DISCOVERY_REQUEST, (const uint8_t *)"", 0);
}
static int conn_selftest(void)
{
	if (access("/run/dash-ready", F_OK) == 0) die("conn-test: a dash runs here (its split view would open): on the build host");
	char crt[256], key[256];   /* as selftest's */
	tmp_path(crt, sizeof crt, "aa-cn.crt"), tmp_path(key, sizeof key, "aa-cn.key");
	if (gen_cert(crt, key, 86400)) die("conn-test: cert");
	SSL_CTX *hc = tls_ctx(crt, key, 0), *pc = tls_ctx(crt, key, 1);   /* the head unit is the client, the phone the server */
	int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) die("conn-test: socketpair");
	signal(SIGPIPE, SIG_IGN);
	pid_t hu = fork();
	if (hu == 0) { close(sv[1]); int touch = -1; _exit(aa_conn(sv[0], hc, &touch) ? 0 : 3); }
	close(sv[0]);
	int fd = sv[1]; struct timeval tmo = { 10, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
	aa_tls_t ph; tls_init(&ph, pc, 1);
	const uint8_t *m; size_t mn;
	conn_hello(fd, &ph);
	if (conn_want(fd, &ph, AA_CH_CONTROL, AA_CTL_SERVICE_DISCOVERY_RESPONSE, &m, &mn)) die("conn-test: no service discovery");
	uint64_t mw = 0, mh = 0, tw = 0, th = 0; pb_in in = { m + 2, mn - 2, 0 }; pb_val f;
	while (pb_next(&in, &f) == 1) {
		const uint8_t *av, *vc, *ic, *tc; size_t avl, vcl, icl, tcl;
		if (f.field != AA_SDRESP_CHANNELS || f.wt != 2) continue;
		if (pb_get_b(f.p, f.len, AA_CHD_AV_CHANNEL, &av, &avl) && pb_get_b(av, avl, AA_AVCH_VIDEO_CONFIGS, &vc, &vcl))
			pb_get_u(vc, vcl, AA_VC_MARGIN_W, &mw), pb_get_u(vc, vcl, AA_VC_MARGIN_H, &mh);
		if (pb_get_b(f.p, f.len, AA_CHD_INPUT_CHANNEL, &ic, &icl) && pb_get_b(ic, icl, AA_INCH_TOUCH_SCREEN, &tc, &tcl))
			pb_get_u(tc, tcl, AA_TC_WIDTH, &tw), pb_get_u(tc, tcl, AA_TC_HEIGHT, &th);
	}
	aa_video_t want = video_wanted();   /* (no settings file on the build host: this build's own choice) */
	if (mw != (uint64_t)(want.fw - want.uw) || mh != (uint64_t)(want.fh - want.uh) || tw != (uint64_t)want.uw || th != (uint64_t)want.uh)
		die("conn-test: video margins %llux%llu, touch %llux%llu", (unsigned long long)mw, (unsigned long long)mh, (unsigned long long)tw, (unsigned long long)th);
	uint8_t co[8]; pb_out o = { co, 0, sizeof co, 0 }; pb_u(&o, AA_COREQ_CHANNEL_ID, AA_CH_INPUT);
	send_msg(fd, &ph, AA_CH_INPUT, AA_MSG_CONTROL, AA_CTL_CHANNEL_OPEN_REQUEST, co, o.n);
	if (conn_want(fd, &ph, AA_CH_INPUT, AA_CTL_CHANNEL_OPEN_RESPONSE, &m, &mn)) die("conn-test: the input channel not opened");
	uint8_t pg[16]; pb_out po = { pg, 0, sizeof pg, 0 }; pb_u(&po, AA_PING_TS, 123456789);
	send_msg(fd, &ph, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_PING_REQUEST, pg, po.n);
	uint64_t ts = 0;
	if (conn_want(fd, &ph, AA_CH_CONTROL, AA_CTL_PING_RESPONSE, &m, &mn) || !pb_get_u(m + 2, mn - 2, AA_PING_TS, &ts) || ts != 123456789)
		die("conn-test: no ping answer");
	send_msg(fd, &ph, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SHUTDOWN_REQUEST, (const uint8_t *)"", 0);
	if (conn_want(fd, &ph, AA_CH_CONTROL, AA_CTL_SHUTDOWN_RESPONSE, &m, &mn)) die("conn-test: no shutdown answer");
	int st; waitpid(hu, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) die("conn-test: the head unit's connection ended %d, not as a session", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	close(fd); tls_free(&ph);
	/* a stop (SIGTERM, as dash-aa stop sends it, with aa serve's handler): the head unit tells the phone
	 * (ShutdownRequest, reason QUIT) and ends the session once the phone answers */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) die("conn-test: socketpair");
	hu = fork();
	if (hu == 0) {
		close(sv[1]);
		struct sigaction sa = { 0 }; sa.sa_handler = aa_on_term; sa.sa_flags = SA_RESTART; sigaction(SIGTERM, &sa, NULL);
		int touch = -1; _exit(aa_conn(sv[0], hc, &touch) ? 0 : 3);
	}
	close(sv[0]);
	fd = sv[1]; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
	tls_init(&ph, pc, 1);
	conn_hello(fd, &ph);
	if (conn_want(fd, &ph, AA_CH_CONTROL, AA_CTL_SERVICE_DISCOVERY_RESPONSE, &m, &mn)) die("conn-test: stop: no service discovery");
	usleep(200000);   /* its session loop running */
	kill(hu, SIGTERM);
	uint64_t why = 0;
	if (conn_want(fd, &ph, AA_CH_CONTROL, AA_CTL_SHUTDOWN_REQUEST, &m, &mn) || !pb_get_u(m + 2, mn - 2, AA_SHUTDOWN_REASON, &why) || why != AA_SHUTDOWN_QUIT)
		die("conn-test: stopped, the head unit did not tell the phone (reason %llu)", (unsigned long long)why);
	send_msg(fd, &ph, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SHUTDOWN_RESPONSE, (const uint8_t *)"", 0);
	waitpid(hu, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) die("conn-test: stopped, the head unit's connection ended %d, not as a session", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	close(fd); tls_free(&ph); SSL_CTX_free(hc); SSL_CTX_free(pc);
	printf("AA-CONN-TEST OK: version, TLS, auth, discovery (margins %llux%llu, touch %llux%llu), channel, ping, shutdown; a stop says goodbye (quit) over a socketpair\n",
	       (unsigned long long)mw, (unsigned long long)mh, (unsigned long long)tw, (unsigned long long)th);
	return 0;
}

// ---- wireless BT handshake: [uint16 BE len][uint16 BE id][protobuf] over an RFCOMM fd ----
static int bt_send(int fd, uint16_t id, const uint8_t *pb, size_t n)
{
	uint8_t h[4] = { (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)(id >> 8), (uint8_t)id };
	return (writen(fd, h, 4) || writen(fd, pb, n)) ? -1 : 0;
}
static int bt_recv(int fd, uint16_t *id, uint8_t *buf, size_t cap, size_t *n)
{
	uint8_t h[4]; if (readn(fd, h, 4)) return -1;
	size_t len = ((size_t)h[0] << 8) | h[1]; *id = (uint16_t)((h[2] << 8) | h[3]);
	if (len > cap || readn(fd, buf, len)) return -1;
	*n = len; return 0;
}
// the AP's credentials, the answer to the phone's WifiInfoRequest
static int bt_send_info(int fd, const char *ssid, const char *key, const char *bssid)
{
	uint8_t pb[256]; pb_out w = { pb, 0, sizeof pb, 0 };
	pb_b(&w, AA_BT_WIR_SSID, (const uint8_t *)ssid, strlen(ssid));
	pb_b(&w, AA_BT_WIR_KEY, (const uint8_t *)key, strlen(key));
	pb_b(&w, AA_BT_WIR_BSSID, (const uint8_t *)bssid, strlen(bssid));
	pb_u(&w, AA_BT_WIR_SECURITY, AA_SEC_WPA2_PERSONAL);
	pb_u(&w, AA_BT_WIR_AP_TYPE, AA_AP_DYNAMIC);
	return bt_send(fd, AA_BT_WIFI_INFO_RESPONSE, pb, w.n);
}
// hand the phone the AP credentials over the connected RFCOMM fd; after this it joins the AP and dials TCP 5288
static int aa_bt_handshake(int fd, const char *ip, int port, const char *ssid, const char *key, const char *bssid)
{
	uint8_t pb[256]; pb_out o = { pb, 0, sizeof pb, 0 };
	pb_b(&o, AA_BT_WSR_IP, (const uint8_t *)ip, strlen(ip));
	pb_u(&o, AA_BT_WSR_PORT, (uint64_t)port);
	if (bt_send(fd, AA_BT_WIFI_START_REQUEST, pb, o.n)) return -1;
	uint16_t id = 0; uint8_t m[256]; size_t mn;
	for (int i = 0; i < 8 && id != AA_BT_WIFI_INFO_REQUEST; i++)   // the phone also sends WifiStartResponse (7); skip to the request
		if (bt_recv(fd, &id, m, sizeof m, &mn)) return -1;
	if (id != AA_BT_WIFI_INFO_REQUEST) return -1;
	return bt_send_info(fd, ssid, key, bssid);
}

// BT message-layer self-test over a socketpair (no Bluetooth stack needed)
static int bt_selftest(void)
{
	int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) die("socketpair");
	pid_t c = fork();
	if (c == 0) { close(sv[0]); // the phone side: expect WifiStartRequest, send WifiInfoRequest, read WifiInfoResponse
		uint16_t id; uint8_t m[256]; size_t mn;
		if (bt_recv(sv[1], &id, m, sizeof m, &mn) || id != AA_BT_WIFI_START_REQUEST) _exit(1);
		uint64_t port; const uint8_t *ip; size_t ipl;
		if (!pb_get_b(m, mn, AA_BT_WSR_IP, &ip, &ipl) || ipl != 8 || memcmp(ip, "10.0.0.1", 8) ||
		    !pb_get_u(m, mn, AA_BT_WSR_PORT, &port) || port != AA_PORT) _exit(2);
		if (bt_send(sv[1], AA_BT_WIFI_INFO_REQUEST, (const uint8_t *)"", 0)) _exit(3);
		if (bt_recv(sv[1], &id, m, sizeof m, &mn) || id != AA_BT_WIFI_INFO_RESPONSE) _exit(4);
		const uint8_t *ss; size_t sl; uint64_t sec;
		if (!pb_get_b(m, mn, AA_BT_WIR_SSID, &ss, &sl) || sl != 4 || memcmp(ss, "dash", 4) ||
		    !pb_get_u(m, mn, AA_BT_WIR_SECURITY, &sec) || sec != AA_SEC_WPA2_PERSONAL) _exit(5);
		_exit(0);
	}
	close(sv[1]);
	int rc = aa_bt_handshake(sv[0], "10.0.0.1", AA_PORT, "dash", "dashpass1234", "aa:bb:cc:dd:ee:ff");
	int st; waitpid(c, &st, 0); close(sv[0]);
	if (rc || !WIFEXITED(st) || WEXITSTATUS(st)) die("bt handshake selftest (child exit %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	printf("AA-BT-SELFTEST OK: WifiStartRequest + WifiInfoResponse handshake roundtrip\n");
	return 0;
}

#ifdef AA_MCU
// ---- the Bluetooth start through the screen's MCU (the F133): the phone's RFCOMM channel from mcud ----
// The SRNUBI T313D's power MCU is also its classic Bluetooth, and its firmware owns the Android Auto RFCOMM service (the
// SDP record): this kernel has no RFCOMM, and the AIC8800's own controller is LE only. mcud, in the screen's OS, owns
// the MCU's line and gives the channel to a client of its socket: "aa\n" asks for it, "open\n" comes when a phone opens
// it (at once if one has), then the RFCOMM bytes both ways until it closes (EOF: the phone's HFP link gone, or another
// channel opened). On it, as over BlueZ: the AP's address and credentials, then the link held until it closes (the
// phone's "car still here"), a repeated WifiInfoRequest answered; then the next opening.
static int mcu_channel(const char *sock)   /* connected, and the channel open: its fd; -1 if not */
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	char l[8]; size_t n = 0;   /* "open\n" a byte at a time: the phone's bytes follow it */
	if (!connect(fd, (struct sockaddr *)&a, sizeof a) && !writen(fd, (const uint8_t *)"aa\n", 3))
		while (n < sizeof l && read(fd, l + n, 1) == 1 && l[n++] != '\n') {}
	if (n == 5 && !memcmp(l, "open\n", 5)) return fd;
	close(fd);
	return -1;
}
static _Noreturn void aa_mcu_serve(const char *ip, const char *ssid, const char *key, const char *bssid)
{
	const char *sock = getenv("MCUD_SOCK") ? getenv("MCUD_SOCK") : "/run/mcud.sock";   /* another: the host test */
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	fprintf(stderr, "aa: waiting for a phone on the MCU's Bluetooth (%s)\n", sock);
	for (int quiet = 0;;) {
		int fd = mcu_channel(sock);
		if (fd < 0) {   /* mcud not there (yet), or gone */
			if (!quiet) fprintf(stderr, "aa: no channel from mcud: trying again every 2 s\n"), quiet = 1;
			sleep(2);
			continue;
		}
		quiet = 0;
		fprintf(stderr, "aa: a phone opened the Android Auto channel; handing AP creds\n");
		sleep(1);   /* as the stock firmware, a second after the MCU's SW before the first message */
		struct timeval tv = { 10, 0 }, none = { 0, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		int rc = -1;
		for (int i = 0; i < 3 && rc; i++) {   /* the request again if the phone said nothing in 10 s (stock: 3 tries) */
			errno = 0;
			if (!(rc = aa_bt_handshake(fd, ip, AA_PORT, ssid, key, bssid))) break;
			if (errno != EAGAIN && errno != EWOULDBLOCK) break;
		}
		if (rc) fprintf(stderr, "aa: BT creds handshake failed\n");
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);   // block: the link held to its end
		uint16_t id; uint8_t m[1024]; size_t mn; int nb = 0;
		while (bt_recv(fd, &id, m, sizeof m, &mn) == 0) {   // logged a few times only: a phone could repeat these for hours
			if (nb < 20) fprintf(stderr, "aa: BT rx id %u (%zu bytes): link held\n", id, mn);
			else if (nb == 20) fprintf(stderr, "aa: further BT messages not logged\n");
			if (nb <= 20) nb++;
			if (id == AA_BT_WIFI_INFO_REQUEST && bt_send_info(fd, ssid, key, bssid)) break;   /* asked again */
		}
		while (read(fd, m, sizeof m) > 0) {}   /* a message bigger than m: the rest drained, the link held to its end */
		close(fd);
		fprintf(stderr, "aa: the channel closed\n");
	}
}
#endif // AA_MCU

#ifdef AA_BT
// ---- BlueZ glue: advertise the AA RFCOMM service (SDP) and accept the phone's connection ----
// The phone discovers us by the AA UUID over an already-paired BT link, opens RFCOMM channel 8, and
// BlueZ hands us the connected fd via org.bluez.Profile1.NewConnection. We run the creds handshake on
// it, then the phone leaves BT and joins the 5 GHz AP. We stay registered to serve reconnects.
#define BLUEZ_BUS      "org.bluez"
#define PROFILE_MGR_IF "org.bluez.ProfileManager1"
#define PROFILE_PATH   "/wattline/aa/profile"
// HFP Hands-Free: a real car exposes this, so the phone auto-connects it and keeps the link up
// (that connection is what gives Android Auto its window to start wireless projection).
#define HFP_HF_UUID    "0000111e-0000-1000-8000-00805f9b34fb"
#define HFP_PATH       "/wattline/aa/hfp"
static bt_extra_t extras[BT_EXTRA_MAX];   /* BT_EXTRA's profiles whose helper runs (registered) */
static int nextras;
static char extra_path[BT_EXTRA_MAX][32];
static GHashTable *extra_dialed[BT_EXTRA_MAX];   /* device paths dialed during their current connection */

// a connected profile socket to its helper (SCM_RIGHTS over the helper's UNIX socket); closed here either way. Never
// blocks aa bt's main loop (a helper that does not accept: the link is dropped); 0 when the helper has it
static int hand_fd(int fd, const char *sock)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), ok = 0;
	char b = 0; struct iovec io = { &b, 1 };
	union { struct cmsghdr h; char c[CMSG_SPACE(sizeof(int))]; } u;
	memset(&u, 0, sizeof u);
	struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.c, .msg_controllen = sizeof u.c };
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof fd);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) || sendmsg(s, &m, MSG_NOSIGNAL) != 1)
		fprintf(stderr, "aa: no helper at %s (%s): the link is closed\n", sock, strerror(errno));
	else ok = 1;
	if (s >= 0) close(s);
	close(fd);
	return ok ? 0 : -1;
}
// a dial or hand-off that failed: tried again at the next tick, up to 3 times per connection (a dead helper does not
// make it redial every 5 s); the device's next connection starts afresh
static GHashTable *extra_fails[BT_EXTRA_MAX];   /* device path -> failures during its current connection */
static void extra_retry(int x, const char *dev)
{
	int n = GPOINTER_TO_INT(g_hash_table_lookup(extra_fails[x], dev)) + 1;
	g_hash_table_insert(extra_fails[x], g_strdup(dev), GINT_TO_POINTER(n));
	if (n < 3) g_hash_table_remove(extra_dialed[x], dev);
}
// in a forked child: every inherited fd but stdio and its own link (the D-Bus socket, other links that arrived)
static void close_inherited(int keep)
{
	for (int f = 3; f < 1024; f++) if (f != keep) close(f);
}

typedef struct { const char *ip, *ssid, *key, *bssid; int port; } bt_creds;

// drive the HFP service-level connection from the HF side: we send the AT commands, the phone (AG) answers.
// A minimal SLC (BRSF -> CIND=? -> CIND? -> CMER) is enough for the AG to mark us connected; then we hold
// the link open and drain whatever the AG sends. Runs in a forked child (it blocks for the link's lifetime).
static void hfp_handle(int fd)
{
	static const char *seq[] = { "AT+BRSF=0\r", "AT+CIND=?\r", "AT+CIND?\r", "AT+CMER=3,0,0,1\r" };
	char buf[512];
	for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
		if (writen(fd, (const uint8_t *)seq[i], strlen(seq[i]))) return;
		for (;;) {   // read the AG's reply up to its terminating OK/ERROR
			ssize_t n = read(fd, buf, sizeof buf - 1);
			if (n <= 0) return;
			buf[n] = 0;
			if (strstr(buf, "OK") || strstr(buf, "ERROR")) break;
		}
	}
	while (read(fd, buf, sizeof buf) > 0) { }   // SLC up: keep the link alive until the phone drops it
}

// A bonded phone only: RequireAuthentication lets an unbonded "Just Works" link through (the kernel allows a
// no-bonding pairing even while the adapter is not pairable), and the agent says yes to it. Bonding needs the pairable
// window dash-aa opens, so this keeps the AP's passphrase to phones paired in that window.
static int dev_bonded(GDBusConnection *conn, const char *dev)
{
	GVariant *r = g_dbus_connection_call_sync(conn, BLUEZ_BUS, dev, "org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", "org.bluez.Device1", "Bonded"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (!r) return 0;
	GVariant *v = NULL;
	g_variant_get(r, "(v)", &v);
	int b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
	if (v) g_variant_unref(v);
	g_variant_unref(r);
	return b;
}

static void profile_method(GDBusConnection *conn, const char *sender, const char *path,
			   const char *iface, const char *method, GVariant *params,
			   GDBusMethodInvocation *inv, gpointer user)
{
	(void)sender; (void)iface;
	bt_creds *cr = user;
	if (!strcmp(method, "NewConnection")) {
		const char *dev; gint32 idx; GVariant *props = NULL;
		g_variant_get(params, "(&oh@a{sv})", &dev, &idx, &props);
		if (props) g_variant_unref(props);
		GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
		GUnixFDList *fdl = g_dbus_message_get_unix_fd_list(msg);
		int fd = fdl ? g_unix_fd_list_get(fdl, idx, NULL) : -1;
		g_dbus_method_invocation_return_value(inv, NULL);   // ack: we own the fd now
		if (fd < 0) return;
		if (!dev_bonded(conn, dev)) { fprintf(stderr, "aa: %s is not bonded: refused\n", dev); close(fd); return; }
		// BlueZ hands the socket over non-blocking; our readn/writen expect blocking I/O
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		int x = -1;
		for (int i = 0; i < nextras; i++) if (!strcmp(path, extra_path[i])) x = i;
		if (x >= 0) {
			fprintf(stderr, "aa: %s on %s: to %s\n", dev, extras[x].name, extras[x].sock);
			if (hand_fd(fd, extras[x].sock) && extras[x].dial) extra_retry(x, dev);
		} else if (!strcmp(path, HFP_PATH)) {
			// HFP link lives for the whole BT session -> handle in a child so the main loop keeps serving
			fprintf(stderr, "aa: phone %s connected HFP; establishing SLC\n", dev);
			pid_t c = fork();
			if (c == 0) { close_inherited(fd); hfp_handle(fd); _exit(0); }
			close(fd);
		} else if (!strcmp(path, PROFILE_PATH)) {
			// RFCOMM lives for the whole wireless session: after the creds, the phone sends WifiConnectStatus here
			// and keeps this link as its "car present" signal while projecting. Closing it right after the creds
			// makes the phone see CAR_BLUETOOTH_DISAPPEAR and tear the session down. So hold it open (forked, like
			// HFP) and drain until the phone drops it, instead of closing.
			fprintf(stderr, "aa: phone %s connected over RFCOMM; handing AP creds\n", dev);
			pid_t c = fork();
			if (c == 0) {
				close_inherited(fd);
				struct timeval tv = { 10, 0 };
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
				if (aa_bt_handshake(fd, cr->ip, cr->port, cr->ssid, cr->key, cr->bssid)) {
					fprintf(stderr, "aa: BT creds handshake failed\n"); _exit(1);
				}
				struct timeval none = { 0, 0 };   // creds done: block and keep the link alive for the session
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
				uint16_t id; uint8_t m[256]; size_t mn; int nb = 0;
				while (bt_recv(fd, &id, m, sizeof m, &mn) == 0) {   // logged a few times only: a phone could repeat these for hours
					if (nb < 20) fprintf(stderr, "aa: BT rx id %u (%zu bytes) — link held\n", id, mn);
					else if (nb == 20) fprintf(stderr, "aa: further BT messages not logged\n");
					if (nb <= 20) nb++;
				}
				fprintf(stderr, "aa: RFCOMM closed by phone\n");
				_exit(0);
			}
			close(fd);
		} else {   /* a path of ours that is no profile (one whose RegisterProfile failed): nothing is sent there */
			fprintf(stderr, "aa: %s on %s: no such profile, closed\n", dev, path);
			close(fd);
		}
	} else { // Release / RequestDisconnection: just ack
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static const char PROFILE_XML[] =
	"<node><interface name='org.bluez.Profile1'>"
	"<method name='Release'/>"
	"<method name='NewConnection'>"
	"<arg type='o' direction='in'/><arg type='h' direction='in'/><arg type='a{sv}' direction='in'/>"
	"</method>"
	"<method name='RequestDisconnection'><arg type='o' direction='in'/></method>"
	"</interface></node>";

// pairing agent: auto-accept every request so a phone pairs with no touch on the Dash.
// It accepts anyone while the Dash is pairable: dash-aa keeps that to 2 minutes after AA is turned on.
#define AGENT_PATH "/wattline/aa/agent"
static const char AGENT_XML[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

// The adapter of a device object (/org/bluez/hci0/dev_..): pairable now (dash-aa's 2-minute window)?
static int adapter_pairable(GDBusConnection *conn, const char *dev)
{
	char ad[64];
	const char *p = strstr(dev, "/dev_");
	size_t n = p ? (size_t)(p - dev) : 0;
	if (!n || n >= sizeof ad) return 0;
	memcpy(ad, dev, n), ad[n] = 0;
	GVariant *r = g_dbus_connection_call_sync(conn, BLUEZ_BUS, ad, "org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", "org.bluez.Adapter1", "Pairable"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (!r) return 0;
	GVariant *v = NULL;
	g_variant_get(r, "(v)", &v);
	int on = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
	if (v) g_variant_unref(v);
	g_variant_unref(r);
	return on;
}

static void agent_method(GDBusConnection *conn, const char *sender, const char *path,
			 const char *iface, const char *method, GVariant *params,
			 GDBusMethodInvocation *inv, gpointer user)
{
	(void)sender; (void)path; (void)iface; (void)user;
	// a pairing only inside the window: the kernel lets a no-bonding pairing through at any time, and a device that
	// copies a bonded phone's address would otherwise be confirmed here and pass the Bonded check (AuthorizeService:
	// a bonded phone's later connections, still yes)
	if (!strcmp(method, "RequestPinCode") || !strcmp(method, "RequestPasskey") || !strcmp(method, "RequestConfirmation") ||
	    !strcmp(method, "RequestAuthorization")) {
		const char *dev = "";
		g_variant_get_child(params, 0, "&o", &dev);
		if (!adapter_pairable(conn, dev)) {
			fprintf(stderr, "aa: pairing from %s refused: not in the pairing window\n", dev);
			g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "not pairable now");
			return;
		}
	}
	if (!strcmp(method, "RequestPinCode"))
		g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
	else if (!strcmp(method, "RequestPasskey"))
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
	else {
		if (!strcmp(method, "RequestConfirmation")) fprintf(stderr, "aa: auto-confirming pairing\n");
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static int reg_agent(GDBusConnection *bus)
{
	GError *e = NULL;
	GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(AGENT_XML, &e);
	if (!ni) { fprintf(stderr, "aa: agent xml: %s\n", e->message); return -1; }
	GDBusInterfaceVTable vt = { agent_method, NULL, NULL, { 0 } };
	if (!g_dbus_connection_register_object(bus, AGENT_PATH, ni->interfaces[0], &vt, NULL, NULL, &e)) {
		fprintf(stderr, "aa: agent object: %s\n", e->message); return -1;
	}
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ_BUS, "/org/bluez", "org.bluez.AgentManager1",
		"RegisterAgent", g_variant_new("(os)", AGENT_PATH, "DisplayYesNo"),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
	if (!r) { fprintf(stderr, "aa: RegisterAgent: %s\n", e->message); return -1; }
	g_variant_unref(r);
	r = g_dbus_connection_call_sync(bus, BLUEZ_BUS, "/org/bluez", "org.bluez.AgentManager1",
		"RequestDefaultAgent", g_variant_new("(o)", AGENT_PATH),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
	if (!r) { fprintf(stderr, "aa: RequestDefaultAgent: %s\n", e->message); return -1; }
	g_variant_unref(r);
	return 0;
}

// register one org.bluez.Profile1 object + RegisterProfile for it; opts is an a{sv} the caller built
static int reg_profile(GDBusConnection *bus, GDBusNodeInfo *ni, const char *obj_path,
		       const char *uuid, GVariant *opts, bt_creds *cr)
{
	GError *e = NULL;
	GDBusInterfaceVTable vt = { profile_method, NULL, NULL, { 0 } };
	guint id = g_dbus_connection_register_object(bus, obj_path, ni->interfaces[0], &vt, cr, NULL, &e);
	if (!id) { fprintf(stderr, "aa: register_object %s: %s\n", obj_path, e->message); return -1; }
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ_BUS, "/org/bluez", PROFILE_MGR_IF, "RegisterProfile",
		g_variant_new("(os@a{sv})", obj_path, uuid, opts),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
	if (!r) { fprintf(stderr, "aa: RegisterProfile %s: %s\n", uuid, e->message); g_dbus_connection_unregister_object(bus, id); return -1; }
	g_variant_unref(r);
	return 0;
}

// aa bt <ip> <ssid> <key> <bssid>: register the AA + HFP profiles and serve connections until killed
// every 5 s, for each "dial" profile: open it once per Bluetooth connection of each connected device that offers it.
// Once only: the device may close it itself and reopen it from its side; a redial would start it over. A dial that
// fails (other than AlreadyConnected/InProgress) is tried again at the next tick.
typedef struct { int x; char *dev; } dial_t;
static void extra_dial_done(GObject *o, GAsyncResult *res, gpointer user)
{
	dial_t *d = user; GError *e = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(o), res, &e);
	if (r) { fprintf(stderr, "aa: %s open to %s\n", extras[d->x].name, d->dev); g_variant_unref(r); }
	else {
		if (!strstr(e->message, "AlreadyConnected") && !strstr(e->message, "InProgress")) {
			fprintf(stderr, "aa: %s to %s: %s\n", extras[d->x].name, d->dev, e->message);
			extra_retry(d->x, d->dev);
		}
		g_error_free(e);
	}
	g_free(d->dev); g_free(d);
}
static ino_t extra_ino[BT_EXTRA_MAX];   /* each helper's socket as last seen: a new one (the helper restarted) dials afresh */
static gboolean extra_dial(gpointer user)
{
	GDBusConnection *bus = user;
	int up[BT_EXTRA_MAX] = { 0 };   /* the helper runs: its socket is there (a link handed to none would only be closed) */
	for (int x = 0; x < nextras; x++) {
		struct stat st;
		if (!(up[x] = stat(extras[x].sock, &st) == 0)) continue;
		if (st.st_ino != extra_ino[x])   /* restarted, e.g. after another phone's session: its devices are dialed again */
			extra_ino[x] = st.st_ino, g_hash_table_remove_all(extra_dialed[x]), g_hash_table_remove_all(extra_fails[x]);
	}
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ_BUS, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
		NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (!r) return G_SOURCE_CONTINUE;
	GVariantIter *objs; const char *path; GVariant *ifs;
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);   /* connected now */
	g_variant_get(r, "(a{oa{sa{sv}}})", &objs);
	while (g_variant_iter_loop(objs, "{&o@a{sa{sv}}}", &path, &ifs)) {
		GVariant *dv = g_variant_lookup_value(ifs, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
		if (!dv) continue;
		gboolean conn = FALSE, bonded = FALSE;
		g_variant_lookup(dv, "Connected", "b", &conn);
		g_variant_lookup(dv, "Bonded", "b", &bonded);   /* only a bonded device's links reach a helper anyway */
		if (conn) g_hash_table_add(seen, g_strdup(path));
		conn = conn && bonded;
		GVariant *uu = g_variant_lookup_value(dv, "UUIDs", G_VARIANT_TYPE("as"));
		const char **v = uu ? g_variant_get_strv(uu, NULL) : NULL;
		for (int x = 0; x < nextras; x++) {
			if (!extras[x].dial || !up[x]) continue;
			if (!conn) continue;
			int has = 0;
			for (int i = 0; v && v[i]; i++) has |= !g_ascii_strcasecmp(v[i], extras[x].uuid);
			if (!has || g_hash_table_contains(extra_dialed[x], path)) continue;
			g_hash_table_add(extra_dialed[x], g_strdup(path));
			dial_t *d = g_new(dial_t, 1); d->x = x; d->dev = g_strdup(path);
			g_dbus_connection_call(bus, BLUEZ_BUS, path, "org.bluez.Device1", "ConnectProfile", g_variant_new("(s)", extras[x].uuid),
				NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, extra_dial_done, d);
		}
		g_free(v); if (uu) g_variant_unref(uu);
		g_variant_unref(dv);
	}
	g_variant_iter_free(objs);
	g_variant_unref(r);
	/* a device not connected now (gone or removed): its next connection starts afresh. A drop and reconnect between two
	 * ticks is caught by dev_changed, which clears it at the drop */
	GHashTableIter it; gpointer k;
	for (int x = 0; x < nextras; x++)
		for (GHashTable *t = extra_dialed[x]; t; t = t == extra_dialed[x] ? extra_fails[x] : NULL) {
			g_hash_table_iter_init(&it, t);
			while (g_hash_table_iter_next(&it, &k, NULL)) if (!g_hash_table_contains(seen, k)) g_hash_table_iter_remove(&it);
		}
	g_hash_table_destroy(seen);
	return G_SOURCE_CONTINUE;
}

// a device's Connected going false (BlueZ's PropertiesChanged): its next connection is dialed afresh, even when it
// comes back before the next tick
static void dev_changed(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig,
			GVariant *params, gpointer user)
{
	(void)c; (void)sender; (void)iface; (void)sig; (void)user;
	const char *ifn; GVariant *changed = NULL; gboolean conn;
	g_variant_get(params, "(&s@a{sv}@as)", &ifn, &changed, NULL);
	if (!strcmp(ifn, "org.bluez.Device1") && g_variant_lookup(changed, "Connected", "b", &conn) && !conn)
		for (int x = 0; x < nextras; x++) { g_hash_table_remove(extra_dialed[x], path); g_hash_table_remove(extra_fails[x], path); }
	g_variant_unref(changed);
}

// BT_EXTRA's profiles whose helper runs: registered; returns how many
static int extras_register(GDBusConnection *bus, GDBusNodeInfo *ni, bt_creds *cr)
{
	FILE *f = fopen(BT_EXTRA, "r");
	if (!f) return 0;
	char l[256]; int dial = 0, ln = 0;
	while (fgets(l, sizeof l, f)) {
		ln++;
		if (!strchr(l, '\n') && !feof(f)) {   /* longer than the buffer: skipped whole (its tail is no line of its own) */
			int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
			fprintf(stderr, "aa: %s line %d: too long, skipped\n", BT_EXTRA, ln);
			continue;
		}
		bt_extra_t e; int r = bt_extra_line(l, &e);
		if (r < 0) { fprintf(stderr, "aa: %s line %d: not \"UUID /SOCKET [dial] [NAME]\": skipped\n", BT_EXTRA, ln); continue; }
		if (!r) continue;
		if (nextras == BT_EXTRA_MAX) { fprintf(stderr, "aa: %s line %d: more than %d profiles, not offered\n", BT_EXTRA, ln, BT_EXTRA_MAX); continue; }
		if (access(e.sock, F_OK)) { fprintf(stderr, "aa: %s: no helper at %s: not offered\n", e.name, e.sock); continue; }
		int x = nextras;
		snprintf(extra_path[x], sizeof extra_path[x], "/wattline/aa/x%d", ln);   /* by line: a failed one leaves no clash */
		GVariantBuilder o; g_variant_builder_init(&o, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&o, "{sv}", "Name", g_variant_new_string(e.name));
		g_variant_builder_add(&o, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
		g_variant_builder_add(&o, "{sv}", "RequireAuthorization", g_variant_new_boolean(FALSE));
		extras[x] = e;   /* before registering: a connection may come at once */
		nextras++;
		if (reg_profile(bus, ni, extra_path[x], e.uuid, g_variant_builder_end(&o), cr)) { nextras--; continue; }
		extra_dialed[x] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		extra_fails[x] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		dial |= e.dial;
	}
	fclose(f);
	if (dial) {
		g_dbus_connection_signal_subscribe(bus, BLUEZ_BUS, "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL,
			"org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE, dev_changed, NULL, NULL);
		g_timeout_add_seconds(5, extra_dial, bus);
	}
	return nextras;
}

static int aa_bt_serve(const char *ip, const char *ssid, const char *key, const char *bssid)
{
	signal(SIGCHLD, SIG_IGN);   // auto-reap the forked HFP handlers
	static bt_creds cr; cr = (bt_creds){ ip, ssid, key, bssid, AA_PORT };
	GError *e = NULL;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
	if (!bus) { fprintf(stderr, "aa: dbus connect: %s\n", e->message); return -1; }
	GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(PROFILE_XML, &e);
	if (!ni) { fprintf(stderr, "aa: node info: %s\n", e->message); return -1; }
	if (reg_agent(bus)) return -1;

	// the AA wireless profile (we drive the creds handshake on connect)
	GVariantBuilder ao; g_variant_builder_init(&ao, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&ao, "{sv}", "Name", g_variant_new_string("Android Auto"));
	g_variant_builder_add(&ao, "{sv}", "Role", g_variant_new_string("server"));
	g_variant_builder_add(&ao, "{sv}", "Channel", g_variant_new_uint16(AA_BT_RFCOMM_CHANNEL));
	// a paired link only: this channel hands out the AP's passphrase, so an unpaired device must not get it
	g_variant_builder_add(&ao, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&ao, "{sv}", "RequireAuthorization", g_variant_new_boolean(FALSE));
	if (reg_profile(bus, ni, PROFILE_PATH, AA_BT_UUID, g_variant_builder_end(&ao), &cr)) return -1;

	// the HFP Hands-Free profile so the phone treats us as a car and keeps the connection
	GVariantBuilder ho; g_variant_builder_init(&ho, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&ho, "{sv}", "Name", g_variant_new_string("Handsfree"));
	g_variant_builder_add(&ho, "{sv}", "Role", g_variant_new_string("server"));
	g_variant_builder_add(&ho, "{sv}", "Version", g_variant_new_uint16(0x0107));
	g_variant_builder_add(&ho, "{sv}", "Features", g_variant_new_uint16(0));
	if (reg_profile(bus, ni, HFP_PATH, HFP_HF_UUID, g_variant_builder_end(&ho), &cr)) return -1;

	int nx = extras_register(bus, ni, &cr);
	fprintf(stderr, "aa: profiles up (AA %s ch %d + HFP%s%.0d%s); waiting for the phone\n",
		AA_BT_UUID, AA_BT_RFCOMM_CHANNEL, nx ? " + " : "", nx, nx ? " more" : "");
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
#endif // AA_BT

// ---- the phone access point: a dedicated 5 GHz ch36 AP (no station; CAN comes over BT) ----
// Proven on-device (aa-ap-test). Needs the concurrent/MCC driver's AP ops; here the AP interface is wlan1 if the
// concurrent driver created it, else wlan0. Writes its config to /run (mode 600) and execs hostapd + udhcpd.
static int sh(const char *cmd) { return system(cmd); }
static int aa_ap_up(const char *iface, const char *ssid, const char *key, const char *ip)
{
	char c[1024];
	FILE *f = fopen("/run/aa-hostapd.conf", "w");
	if (!f) return -1;
	fchmod(fileno(f), 0600);
	// ctrl_interface: dash-aa's stop tells each phone on it (hostapd_cli deauthenticate) before hostapd goes
	fprintf(f, "interface=%s\ndriver=nl80211\nctrl_interface=/run/aa-hostapd\nssid=%s\nhw_mode=a\nchannel=36\nieee80211n=1\nwmm_enabled=1\n"
		   "auth_algs=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=%s\nap_isolate=1\n", iface, ssid, key);   // ap_isolate: clients cannot reach each other
	fclose(f);
	snprintf(c, sizeof c, ": > /data/log/aa-hostapd.log; hostapd -B -P /run/aa-hostapd.pid -f /data/log/aa-hostapd.log /run/aa-hostapd.conf");
	if (sh(c)) { fprintf(stderr, "aa: hostapd failed\n"); return -1; }
	snprintf(c, sizeof c, "ip addr add %s/24 dev %s", ip, iface); sh(c);
	f = fopen("/run/aa-udhcpd.conf", "w");
	if (f) {
		// serve .10-.200 on the AP's /24 (derive the base from ip by trimming the last octet). The lease table starts
		// EMPTY each session: a phone can show a new (randomised) MAC per join, and a table kept across sessions
		// fills up -- then udhcpd says "no free IP addresses", the phone gets no offer and AA sticks at "connecting".
		char base[64]; snprintf(base, sizeof base, "%s", ip); char *d = strrchr(base, '.'); if (d) *d = 0;
		fprintf(f, "interface %s\nstart %s.10\nend %s.200\nmax_leases 190\n"
			   "lease_file /run/aa-udhcpd.leases\npidfile /run/aa-udhcpd.pid\noption subnet 255.255.255.0\noption lease 3600\n",
			iface, base, base);
		fclose(f);
		sh(": > /run/aa-udhcpd.leases; udhcpd -f -a 100 /run/aa-udhcpd.conf >/data/log/aa-udhcpd.log 2>&1 &");   // -a: 100 ms ARP probe, not 2 s (the phone gives up waiting for its offer)
	}
	fprintf(stderr, "aa: AP %s up on %s (ch36, %s/24)\n", ssid, iface, ip);
	return 0;
}

// ap, bt, mcu: a <key> of "-" means the passphrase is stdin's first line, so it is in no command line (every process
// can read another's, /proc/PID/cmdline): `printf '%s\n' "$PSK" | aa mcu "$IP" "$SSID" - "$BSSID"` (printf, a shell
// builtin, runs no program)
static const char *key_arg(const char *a)
{
	static char k[128];   // a WPA passphrase is 8 to 63 characters
	if (strcmp(a, "-")) return a;
	if (!fgets(k, sizeof k, stdin)) k[0] = 0;
	k[strcspn(k, "\r\n")] = 0;
	if (!k[0]) die("no passphrase on stdin");
	return k;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--touch-test")) {   /* print the touch events a session would send, for N s */
		int tf = touch_open(), secs = argc > 2 ? atoi(argv[2]) : 15;
		if (tf < 0) return 1;
		input_open = 1; mt_force_visible = 1; phone_off_ok = 1;
		vid = video_wanted();   /* the coordinates a session would send now */
		printf("touch-test: touchscreen 0..%d x 0..%d (%s), picture x %d..%d y %d..%d of the %dx%d canvas, sent as 0..%d x 0..%d; "
		       "touch now (%d s)\n", mt_max_x, mt_max_y, mt_portrait ? "portrait, turned 270" : "landscape", PJ_RGN_X, PJ_RGN_X + PJ_RGN_W - 1,
		       PJ_RGN_Y, PJ_RGN_Y + PJ_RGN_H - 1, PJ_PANEL_W, PJ_PANEL_H, vid.uw - 1, vid.uh - 1, secs); fflush(stdout);
		for (int64_t end = now_us() + (int64_t)secs * 1000000; now_us() < end; ) {
			struct pollfd pf = { tf, POLLIN, 0 };
			if (poll(&pf, 1, 200) > 0) touch_read(tf, -2, (aa_tls_t *)&mt_nlog);
		}
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--conn-test")) return conn_selftest();   /* the build host only (it says why) */
	if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest() | msg_selftest() | rx_selftest() | mt_selftest() | bt_selftest()
#ifdef AA_USB
		| usb_selftest()
#endif
		;
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "certcheck")) {   // certcheck <crt> [key]: 0 if usable (see cert_check)
		int r = cert_check(argv[2], argc == 4 ? argv[3] : NULL);
		if (r) fprintf(stderr, "aa: %s: %s\n", argv[2], r == 1 ? "self-signed: phones reject it (a Google-signed head-unit certificate is needed)"
						       : r == 2 ? "its key is missing, passphrase-protected, or another certificate's" : "no PEM certificate");
		return r == 1 ? 1 : r == 2 ? 2 : r ? 3 : 0;   // 1 self-signed, 2 the key, 3 no readable certificate (S06wattline)
	}
	if (argc == 4 && !strcmp(argv[1], "gencert"))   // gencert <crt> <key>: a self-signed cert (tests; phones reject it)
		return gen_cert(argv[2], argv[3], 10L * 365 * 86400) ? (fprintf(stderr, "aa: gencert failed\n"), 1) : 0;
	if ((argc == 4 || argc == 2) && !strcmp(argv[1], "serve")) {
		const char *cert = argc == 4 ? argv[2] : "/etc/wattline/aa.crt";
		const char *key  = argc == 4 ? argv[3] : "/etc/wattline/aa.key";
		if (argc == 2) {
			// auto-generate a 10-year self-signed cert once; reuse on subsequent runs
			if (access(cert, F_OK) != 0) {
				mkdir("/etc/wattline", 0700);
				if (gen_cert(cert, key, 10L * 365 * 86400)) die("serve: cert generation failed");
				fprintf(stderr, "aa: generated self-signed cert at %s\n", cert);
			}
		}
		return aa_serve(cert, key);
	}
	if (argc == 6 && !strcmp(argv[1], "ap")) return aa_ap_up(argv[2], argv[3], key_arg(argv[4]), argv[5]); // ap <iface> <ssid> <key> <ip>
#ifdef AA_USB
	if ((argc == 4 || argc == 2) && !strcmp(argv[1], "usb"))   // usb [<cert.pem> <key.pem>]: the wired receiver (AOA)
		return aa_usb_serve(argc == 4 ? argv[2] : "/etc/wattline/aa.crt", argc == 4 ? argv[3] : "/etc/wattline/aa.key");
#endif
#ifdef AA_MCU
	if (argc == 6 && !strcmp(argv[1], "mcu")) aa_mcu_serve(argv[2], argv[3], key_arg(argv[4]), argv[5]); // mcu <ip> <ssid> <key> <bssid>
#endif
#ifdef AA_BT
	if (argc == 6 && !strcmp(argv[1], "bt")) return aa_bt_serve(argv[2], argv[3], key_arg(argv[4]), argv[5]); // bt <ip> <ssid> <key> <bssid>
#endif
	fprintf(stderr, "usage: aa --selftest | aa serve [<cert.pem> <key.pem>]\n"
			"       aa ap <iface> <ssid> <key> <ip> | aa bt <ip> <ssid> <key> <bssid>\n"
			"  (serve: no cert args auto-generates a persistent self-signed cert at /etc/wattline/)\n"
			"  (<key> as -: the passphrase on stdin's first line, out of the process list)\n");
#ifdef AA_USB
	fprintf(stderr, "       aa usb [<cert.pem> <key.pem>]: the phone on USB (Android Open Accessory), the certificate as serve's\n");
#endif
#ifdef AA_MCU
	fprintf(stderr, "       aa mcu <ip> <ssid> <key> <bssid>: the Bluetooth start through the screen's MCU (mcud)\n");
#endif
	return 1;
}
