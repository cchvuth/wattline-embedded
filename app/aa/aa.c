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
	if (cert && SSL_CTX_use_certificate_file(c, cert, SSL_FILETYPE_PEM) != 1) die("load cert %s", cert);
	SSL_CTX_set_default_passwd_cb_userdata(c, (void *)"");   // a passphrase-protected key fails at once, never prompts
	if (key && SSL_CTX_use_PrivateKey_file(c, key, SSL_FILETYPE_PEM) != 1) die("load key %s", key);
	SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);  // AA pins at the app layer, not via the CA store
	if (!server) {   // the head unit's side, as aasdk's: TLS 1.2, and the phone's certificate taken as it comes
		SSL_CTX_set_max_proto_version(c, TLS1_2_VERSION);
		SSL_CTX_set_security_level(c, 0);
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

static int selftest(void)
{
	// an ephemeral self-signed cert/key for the server side (stands in for the user's head-unit cert)
	if (gen_cert("/tmp/aa-st.crt", "/tmp/aa-st.key", 86400)) die("selftest: cert generation failed");
	SSL_CTX *sc = tls_ctx("/tmp/aa-st.crt", "/tmp/aa-st.key", 1), *cc = tls_ctx("/tmp/aa-st.crt", "/tmp/aa-st.key", 0);
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
	tls_free(&c); tls_free(&s); SSL_CTX_free(sc); SSL_CTX_free(cc);
	printf("AA-SELFTEST OK: TLS-in-frames handshake + a message each way + protobuf roundtrip\n");
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
#define AA_FRAME_W 1280
#define AA_FRAME_H 720
#define AA_MARGIN_W (AA_FRAME_W - PJ_RGN_W)
#define AA_MARGIN_H (AA_FRAME_H - PJ_RGN_H)
_Static_assert(AA_MARGIN_W >= 0 && AA_MARGIN_H >= 0 && AA_MARGIN_W % 4 == 0 && AA_MARGIN_H % 4 == 0, "picture must fit a 720p frame, centred on even pixels");
// ponytail: the phone scales its UI by this density; 160 is a first value for this panel, tune it on the unit.
#define AA_DPI 160

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

// ServiceDiscoveryResponse: video (H.264 at res, 30 fps), sensors, touch, the three audio sinks and the mic — the
// set OpenAuto advertises; AA refuses to start without sensors, audio playback or a mic ("No audio/mic").
static size_t build_service_discovery_response(uint8_t *buf, size_t cap, int res)
{
	uint8_t vc[16]; pb_out v = { vc, 0, sizeof vc, 0 };
	pb_u(&v, AA_VC_RESOLUTION, res); pb_u(&v, AA_VC_FPS, AA_FPS_30);
	// margins + density are required too ("density missing" -> PROTOCOL_WRONG_CONFIGURATION)
	pb_u(&v, AA_VC_MARGIN_W, AA_MARGIN_W); pb_u(&v, AA_VC_MARGIN_H, AA_MARGIN_H); pb_u(&v, AA_VC_DPI, AA_DPI);
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
	// the touch screen is the phone's UI area: the picture, in its own pixels (see AA_FRAME_W)
	uint8_t tc[16]; pb_out t = { tc, 0, sizeof tc, 0 }; pb_u(&t, AA_TC_WIDTH, PJ_RGN_W); pb_u(&t, AA_TC_HEIGHT, PJ_RGN_H);
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
	pb_b(&o, AA_SDRESP_CHANNELS, media, mn);
	pb_b(&o, AA_SDRESP_CHANNELS, speech, spn);
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
	pb_u(&o, AA_AVSETUPRESP_MAX_UNACKED, 1);
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
	uint8_t sdr[512]; size_t sn = build_service_discovery_response(sdr, sizeof sdr, AA_RES_720p);
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
			    pb_get_u(vcp, vcl, AA_VC_RESOLUTION, &res) && res == AA_RES_720p) found = 1;
		}
	}
	if (!found) die("msg: SDR video config roundtrip");
	if (nch != 7) die("msg: SDR advertises %d channels, want 7 (video, sensor, input, 3 audio, mic)", nch);
	{   // the touch screen advertised = the picture (the phone's UI area): touches are sent in its pixels
		pb_in in2 = { sdr, sn, 0 }; pb_val f2; uint64_t tw = 0, th = 0;
		while (pb_next(&in2, &f2) == 1) {
			const uint8_t *ic, *tcp; size_t icl, tcl; uint64_t cid;
			if (f2.field == AA_SDRESP_CHANNELS && f2.wt == 2 && pb_get_u(f2.p, f2.len, AA_CHD_CHANNEL_ID, &cid) && cid == AA_CH_INPUT &&
			    pb_get_b(f2.p, f2.len, AA_CHD_INPUT_CHANNEL, &ic, &icl) && pb_get_b(ic, icl, AA_INCH_TOUCH_SCREEN, &tcp, &tcl))
				pb_get_u(tcp, tcl, AA_TC_WIDTH, &tw), pb_get_u(tcp, tcl, AA_TC_HEIGHT, &th);
		}
		if (tw != PJ_RGN_W || th != PJ_RGN_H) die("msg: touch screen %llux%llu, want the picture %dx%d", (unsigned long long)tw, (unsigned long long)th, PJ_RGN_W, PJ_RGN_H);
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
			if (!said++) fprintf(stderr, "aa: plaintext frame after TLS on channel %u: ignored\n", ch);
			continue;
		} else if (asm_add(a, fr, (size_t)pn, ch)) return -1;
		if (flags & AA_FRAME_LAST) { *chan = ch; *msg = a->p; *mlen = a->n; return 0; }   // LAST or BULK: complete
	}
}

// send a framed message; with tls, the [msgid][pb] is encrypted and the frame flagged ENCRYPTED.
static int send_msg(int fd, aa_tls_t *tls, uint8_t chan, uint8_t msgtype, uint16_t msgid, const uint8_t *pb, size_t pblen)
{
	uint8_t msg[AA_MAX_FRAME_PAYLOAD]; int mn = msg_wrap(msg, sizeof msg, msgid, pb, pblen);
	if (mn < 0) return -1;
	uint8_t cipher[AA_MAX_FRAME_PAYLOAD + 512], frame[AA_MAX_FRAME_PAYLOAD + 520];
	const uint8_t *payload = msg; size_t plen = (size_t)mn; uint8_t enc = AA_ENC_PLAIN;
	if (tls) { SSL_write(tls->ssl, msg, mn); int cn = tls_take(tls, cipher, sizeof cipher); if (cn <= 0) return -1; payload = cipher; plen = (size_t)cn; enc = AA_ENC_ENCRYPTED; }
	int fn = frame_put(frame, sizeof frame, chan, (uint8_t)(enc | msgtype), payload, plen);
	return fn < 0 ? -1 : writen(fd, frame, (size_t)fn);
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
// PJ_RGN_W x PJ_RGN_H window at the picture rectangle (--region X W)
#define STR_(x) #x
#define STR(x) STR_(x)
static pid_t sink_pid = -1;
static int spawn_sink(void)
{
	int p[2]; if (pipe(p)) return -1;
	pid_t c = fork();
	sink_pid = c;
	if (c == 0) {
		dup2(p[0], 0); close(p[0]); close(p[1]);
		execlp("spike-vdec", "spike-vdec", "--fb-stream", "--region", STR(PJ_RGN_X), STR(PJ_RGN_W), (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	return c < 0 ? (close(p[1]), -1) : p[1];
}

static uint32_t av_sess[16];   // per-AV-channel session id from AV_START, echoed in media ACKs
static int is_av(uint8_t ch) { return ch == AA_CH_VIDEO || ch == AA_CH_MEDIA_AUDIO || ch == AA_CH_SPEECH_AUDIO || ch == AA_CH_SYSTEM_AUDIO || ch == AA_CH_AV_INPUT; }
static int is_media(uint16_t id) { return id == AA_AV_MEDIA_WITH_TIMESTAMP || id == AA_AV_MEDIA; }

// ACK one media frame on its channel: the AV_START session, value 1 per frame (as OpenAuto); with max_unacked 1
// the phone sends the next frame only after this.
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
static int mt_max_x = PJ_PANEL_W - 1, mt_max_y = PJ_RGN_H - 1;
static int input_open;
static int touch_open(void)
{
	int f = open(PJ_TOUCH_DEV, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (f < 0) { fprintf(stderr, "aa: touchscreen %s: %s\n", PJ_TOUCH_DEV, strerror(errno)); return -1; }
	struct input_absinfo a;
	if (!ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &a) && a.maximum > 0) mt_max_x = a.maximum;
	if (!ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &a) && a.maximum > 0) mt_max_y = a.maximum;
	return f;
}
static int mt_panel_x(int raw) { return (int)((int64_t)raw * PJ_PANEL_W / (mt_max_x + 1)); }
static int mt_panel_y(int raw) { return (int)((int64_t)raw * PJ_RGN_H / (mt_max_y + 1)); }
static size_t build_mt(uint8_t *buf, size_t cap, uint64_t ts, const int *idx, int n, int action_index, int action)
{
	uint8_t te[320]; pb_out t = { te, 0, sizeof te, 0 };
	for (int k = 0; k < n; k++) {
		int px = mt_panel_x(mt[idx[k]].x) - PJ_RGN_X, py = mt_panel_y(mt[idx[k]].y);
		px = px < 0 ? 0 : px >= PJ_RGN_W ? PJ_RGN_W - 1 : px;
		py = py < 0 ? 0 : py >= PJ_RGN_H ? PJ_RGN_H - 1 : py;
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
static int mt_force_visible, mt_log[16][3], mt_nlog;   /* the self-test's view: no screen, events recorded */
static void mt_send(int fd, aa_tls_t *tls, uint64_t ts, int ai, int action, int with)
{
	int idx[MT_MAX], n = 0, at = 0;   /* the list: every finger on, plus `with` (one going up) */
	for (int k = 0; k < MT_MAX; k++) if (mt[k].on || k == with) { if (k == with) at = n; idx[n++] = k; }
	if (fd >= 0 && !tls) return;   /* nothing goes to a phone in plain text */
	if (fd < 0) {   /* the self-test records; --touch-test prints what would go to the phone */
		if (mt_nlog < 16) { mt_log[mt_nlog][0] = action; mt_log[mt_nlog][1] = n; mt_log[mt_nlog][2] = with >= 0 ? at : ai; mt_nlog++; }
		if (fd == -2) {
			printf("t=%llu.%06llu action %d index %d:", (unsigned long long)(ts / 1000000), (unsigned long long)(ts % 1000000), action, with >= 0 ? at : ai);
			for (int k = 0; k < n; k++) {
				int px = mt_panel_x(mt[idx[k]].x) - PJ_RGN_X, py = mt_panel_y(mt[idx[k]].y);
				px = px < 0 ? 0 : px >= PJ_RGN_W ? PJ_RGN_W - 1 : px; py = py < 0 ? 0 : py >= PJ_RGN_H ? PJ_RGN_H - 1 : py;
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
	if (!mt_prev_any) {   /* a gesture starts: ours only if its first finger is on the picture, now */
		int first = -1;
		for (int i = 0; i < MT_MAX && first < 0; i++) if (mt[i].active) first = i;
		if (first >= 0) {
			int x = mt_panel_x(mt[first].x);
			mt_gesture = tls && input_open && (mt_force_visible || access(PJ_VISIBLE, F_OK) == 0) && x >= PJ_RGN_X && x < PJ_RGN_X + PJ_RGN_W ? 1 : -1;
		}
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
				mt_frame(fd, tls, (uint64_t)e->input_event_sec * 1000000u + (uint64_t)e->input_event_usec);
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
	const uint8_t *m; size_t mn; uint8_t chan; uint32_t frames = 0, st_frames = 0, nlog = 0; uint64_t st_bytes = 0;
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
	int64_t next_ping = now_us() + AA_PING_EVERY_US, last_rx = now_us(), next_stat = now_us() + 5000000;
	for (;;) {
		// the decoder died (bad or unsupported stream): end the session rather than leave its last picture up as if
		// live; the phone reconnects and a fresh decoder starts
		if (sink == -2) { fprintf(stderr, "aa: ending the session: no video decoder\n"); break; }
		int64_t t = now_us(), left = next_ping - t;
		if (t - last_rx > AA_SILENT_US) { fprintf(stderr, "aa: phone silent for %d s, ending the session\n", AA_SILENT_US / 1000000); break; }
		if (t >= next_stat) {   // what the video path did: frames and data in from the phone
			fprintf(stderr, "aa: 5 s: %u video frames, %llu KB\n", st_frames, (unsigned long long)(st_bytes >> 10));
			st_frames = 0; st_bytes = 0; next_stat = t + 5000000;
		}
		if (left <= 0) {
			send_ping(fd, tls); next_ping = t + AA_PING_EVERY_US; left = AA_PING_EVERY_US;
			if (night_on) { int n = night_now(); if (n != night_sent) send_night(fd, tls, n), night_sent = n; }   /* dusk, dawn */
		}
		struct pollfd pf[2] = { { fd, POLLIN, 0 }, { touch, POLLIN, 0 } };
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
			else if (nlog == 300) fprintf(stderr, "aa: further messages not logged this session\n");   // never flooded
			nlog++;
		}
		if (chan == AA_CH_CONTROL && id == AA_CTL_SERVICE_DISCOVERY_REQUEST) {
			uint8_t sd[512]; size_t sn = build_service_discovery_response(sd, sizeof sd, AA_RES_720p);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SERVICE_DISCOVERY_RESPONSE, sd, sn);
		} else if (id == AA_CTL_CHANNEL_OPEN_REQUEST) {
			// The request comes ON the channel being opened (a control-type message there), and the answer goes back
			// on that same channel (aasdk *ServiceChannel::sendChannelOpenResponse, headunit hu_handle_ChannelOpenRequest).
			uint8_t co[16]; size_t cn = build_channel_open_response(co, sizeof co, AA_STATUS_OK);
			send_msg(fd, tls, chan, AA_MSG_CONTROL, AA_CTL_CHANNEL_OPEN_RESPONSE, co, cn);
			if (chan == AA_CH_INPUT) input_open = 1;
			fprintf(stderr, "aa: channel %u opened\n", chan);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_AUDIO_FOCUS_REQUEST) {
			uint64_t ty = 0; pb_get_u(body, blen, AA_AFREQ_TYPE, &ty);
			uint8_t r[8]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_AFRESP_STATE, ty == AA_AF_RELEASE ? AA_AF_LOSS : AA_AF_GAIN);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_AUDIO_FOCUS_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_NAV_FOCUS_REQUEST) {
			uint8_t r[8]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_NAVF_TYPE, 2);   // 2 = projected nav
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_NAV_FOCUS_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_PING_REQUEST) {
			uint64_t ts = 0; pb_get_u(body, blen, AA_PING_TS, &ts);
			uint8_t r[16]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_PING_TS, ts);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_PING_RESPONSE, r, o.n);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_SHUTDOWN_REQUEST) {
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SHUTDOWN_RESPONSE, (const uint8_t *)"", 0);
			fprintf(stderr, "aa: phone asked to shut down\n");
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
				fprintf(stderr, "aa: video setup done, focus taken\n");
			}
		} else if (chan == AA_CH_VIDEO && id == AA_AV_VIDEO_FOCUS_REQUEST) {
			// focus only on request, focused + not unrequested (OpenAuto VideoService::onVideoFocusRequest)
			uint8_t vf[8]; pb_out o = { vf, 0, sizeof vf, 0 }; pb_u(&o, AA_VFI_FOCUS_MODE, AA_FOCUS_FOCUSED); pb_u(&o, AA_VFI_UNREQUESTED, 0);
			send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_VIDEO_FOCUS_INDICATION, vf, o.n);
			fprintf(stderr, "aa: video focus granted\n");
		} else if (chan == AA_CH_AV_INPUT && id == AA_AV_INPUT_OPEN_REQUEST) {
			uint8_t r[16]; pb_out o = { r, 0, sizeof r, 0 }; pb_u(&o, AA_AVINRESP_SESSION, av_sess[AA_CH_AV_INPUT]); pb_u(&o, AA_AVINRESP_VALUE, 0);
			send_msg(fd, tls, AA_CH_AV_INPUT, AA_MSG_SPECIFIC, AA_AV_INPUT_OPEN_RESPONSE, r, o.n);
		} else if (is_av(chan) && id == AA_AV_START) {
			uint64_t ss = 0; pb_get_u(body, blen, AA_START_SESSION, &ss); av_sess[chan & 15] = (uint32_t)ss;
			fprintf(stderr, "aa: av start chan %u session %u\n", chan, av_sess[chan & 15]);
		} else if (is_av(chan) && is_media(id)) {
			if (chan == AA_CH_VIDEO) {
				if (!frames++) fprintf(stderr, "aa: first video frame (%zu bytes)\n", blen);
				st_frames++; st_bytes += blen;
				const uint8_t *nal = body; size_t nl = blen;
				if (id == AA_AV_MEDIA_WITH_TIMESTAMP) { if (nl <= 8) goto acked; nal += 8; nl -= 8; }   // 8-byte timestamp
				// then an access-unit delimiter: the decoder takes a NAL as complete when the next start code
				// comes, so without it every picture would wait for the following one (a still screen: forever)
				static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xf0 };
				if (sink >= 0 && (writen(sink, nal, nl) || writen(sink, aud, sizeof aud))) { fprintf(stderr, "aa: video sink died\n"); close(sink); sink = -2; }
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
	input_open = 1; mt_force_visible = 1; memset(mt, 0, sizeof mt); mt_prev_any = 0; mt_gesture = 0;
	#define F(slot, act, X, Y) (mt[slot].active = (act), mt[slot].x = (X), mt[slot].y = (Y), mt[slot].moved = 1)
	F(0, 1, 500, 200); mt_frame(-1, fake, 1);                       /* finger 0 down on the picture */
	F(1, 1, 700, 250); mt_frame(-1, fake, 2);                       /* finger 1 down */
	F(0, 1, 480, 190); F(1, 1, 720, 260); mt_frame(-1, fake, 3);    /* both move */
	mt[0].active = 0; mt_frame(-1, fake, 4);                        /* finger 0 up */
	mt[1].active = 0; mt_frame(-1, fake, 5);                        /* finger 1 up */
	F(0, 1, 1600, 200); mt_frame(-1, fake, 6);                      /* a stroke from the dash strip (x 1600) */
	F(0, 1, 600, 200); mt_frame(-1, fake, 7); mt[0].active = 0; mt_frame(-1, fake, 8);   /* ...even onto the picture */
	#undef F
	static const int want[][3] = { { AA_TOUCH_PRESS, 1, 0 }, { AA_TOUCH_POINTER_DOWN, 2, 1 }, { AA_TOUCH_DRAG, 2, 0 },
	                               { AA_TOUCH_POINTER_UP, 2, 0 }, { AA_TOUCH_RELEASE, 1, 0 } };
	int ok = mt_nlog == 5;
	for (int i = 0; ok && i < 5; i++) ok = !memcmp(mt_log[i], want[i], sizeof want[i]);
	if (!ok) {
		for (int i = 0; i < mt_nlog; i++) fprintf(stderr, "  got action %d, %d fingers, index %d\n", mt_log[i][0], mt_log[i][1], mt_log[i][2]);
		die("touch selftest: wrong event sequence (%d events)", mt_nlog);
	}
	uint8_t b[400]; int idx[2] = { 0, 1 }; mt[0].x = 120; mt[1].x = 1279; mt[0].y = 0; mt[1].y = 479;   /* picture corners */
	size_t n = build_mt(b, sizeof b, 9, idx, 2, 1, AA_TOUCH_POINTER_DOWN);
	const uint8_t *te; size_t tl; uint64_t act, ai;
	if (!n || !pb_get_b(b, n, AA_IEI_TOUCH_EVENT, &te, &tl) || !pb_get_u(te, tl, AA_TE_TOUCH_ACTION, &act) || act != AA_TOUCH_POINTER_DOWN ||
	    !pb_get_u(te, tl, AA_TE_ACTION_INDEX, &ai) || ai != 1) die("touch selftest: event encoding");
	input_open = 0; mt_force_visible = 0; memset(mt, 0, sizeof mt); mt_prev_any = 0; mt_gesture = 0;
	printf("AA-TOUCH-SELFTEST OK: pinch = PRESS, POINTER_DOWN, DRAG, POINTER_UP, RELEASE; off-picture stroke ignored\n");
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
	if (gen_cert("/tmp/aa-rx.crt", "/tmp/aa-rx.key", 86400)) die("rx selftest: cert");
	SSL_CTX *sc = tls_ctx("/tmp/aa-rx.crt", "/tmp/aa-rx.key", 1), *cc = tls_ctx("/tmp/aa-rx.crt", "/tmp/aa-rx.key", 0);
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

static int aa_serve(const char *cert, const char *key)
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);   // line-buffered: the serve log survives a hard kill (dash-aa stop)
	signal(SIGCHLD, SIG_IGN);           // the decoder child of each session: no zombies
	int ls = socket(AF_INET, SOCK_STREAM, 0); if (ls < 0) die("socket: %s", strerror(errno));
	int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(AA_PORT);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) die("bind/listen %d: %s", AA_PORT, strerror(errno));
	int touch = touch_open();   // after the bind: a second instance dies above, before touching anything
	SSL_CTX *ctx = tls_ctx(cert, key, 0);   // the head unit is the TLS client: it sends the ClientHello
	fprintf(stderr, "aa: listening on %d (phone must be on the AP; cert %s)\n", AA_PORT, cert);
	for (;;) {
		int fd = accept(ls, NULL, NULL); if (fd < 0) continue;
		// as aasdk (TCPWrapper) and headunit (hu_tcp): small writes (media ACKs, touches, pings) go out at once,
		// not held until the phone acknowledges the previous one -- with one frame in flight, that stalls video
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		fprintf(stderr, "aa: phone dialled in on %d\n", AA_PORT);
		struct timeval tmo = { 5, 0 }, live = { AA_SILENT_US / 1000000, 0 };   // per read: before auth, and in the session
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
		rd_deadline = now_us() + 20000000;   // version + TLS within 20 s, however the bytes trickle in
		aa_tls_t t; tls_init(&t, ctx, 0);
		uint8_t vr[4] = { AA_VERSION_MAJOR >> 8, AA_VERSION_MAJOR & 0xff, AA_VERSION_MINOR >> 8, AA_VERSION_MINOR & 0xff };
		send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_VERSION_REQUEST, vr, sizeof vr);
		uint8_t chan; const uint8_t *m; size_t mn;
		int ok = recv_msg(fd, NULL, &chan, &m, &mn) == 0 && msg_id(m, mn) == AA_CTL_VERSION_RESPONSE;
		fprintf(stderr, "aa: version %s\n", ok ? "agreed" : "refused");
		if (ok && do_ssl_handshake(fd, &t) == 0) {
			fprintf(stderr, "aa: TLS up (%s, %s), session starting\n", SSL_get_version(t.ssl), SSL_get_cipher(t.ssl));
			rd_deadline = 0;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &live, sizeof live);   // a frame stalled half-way ends the session
			// AUTH_COMPLETE is the PLAINTEXT handshake-done signal: the phone activates its SSL context on
			// receiving it, and only frames AFTER it are encrypted. Sent encrypted, the phone logs "encrypted
			// frame ... with no ssl context, waiting" and times out. So NULL (plain), like VERSION/SSL_HANDSHAKE.
			uint8_t ac[8]; pb_out o = { ac, 0, sizeof ac, 0 }; pb_u(&o, AA_AC_STATUS, AA_STATUS_OK);
			send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_AUTH_COMPLETE, ac, o.n);
			int sink = spawn_sink();
			close(open("/run/dash-project", O_CREAT | O_WRONLY, 0644));   // the dash UI yields the video region
			aa_session(fd, &t, sink, &touch);
			for (int k = 0; k < AA_NCHAN; k++) free(rx_asm[k].p), rx_asm[k] = (aa_asm_t){0};   /* the next session starts small */
			// stop the decoder and wait it out BEFORE dropping the marker: the UI repaints the dash when the marker
			// goes, and a decoder still drawing would leave its last picture over it
			if (sink >= 0) close(sink);
			if (sink_pid > 0) { kill(sink_pid, SIGTERM); waitpid(sink_pid, NULL, 0); sink_pid = -1; }   // SIGCHLD ignored: returns once it is gone
			unlink("/run/dash-project");
			fprintf(stderr, "aa: session ended\n");
		} else if (ok) {
			unsigned long err = ERR_get_error();
			fprintf(stderr, "aa: TLS handshake failed: %s\n", err ? ERR_error_string(err, NULL) : "peer closed or timed out");
		}
		tls_free(&t); close(fd);
	}
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
	pb_out w = { pb, 0, sizeof pb, 0 };
	pb_b(&w, AA_BT_WIR_SSID, (const uint8_t *)ssid, strlen(ssid));
	pb_b(&w, AA_BT_WIR_KEY, (const uint8_t *)key, strlen(key));
	pb_b(&w, AA_BT_WIR_BSSID, (const uint8_t *)bssid, strlen(bssid));
	pb_u(&w, AA_BT_WIR_SECURITY, AA_SEC_WPA2_PERSONAL);
	pb_u(&w, AA_BT_WIR_AP_TYPE, AA_AP_DYNAMIC);
	return bt_send(fd, AA_BT_WIFI_INFO_RESPONSE, pb, w.n);
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
		if (!strcmp(path, HFP_PATH)) {
			// HFP link lives for the whole BT session -> handle in a child so the main loop keeps serving
			fprintf(stderr, "aa: phone %s connected HFP; establishing SLC\n", dev);
			pid_t c = fork();
			if (c == 0) { hfp_handle(fd); _exit(0); }
			close(fd);
		} else {
			// RFCOMM lives for the whole wireless session: after the creds, the phone sends WifiConnectStatus here
			// and keeps this link as its "car present" signal while projecting. Closing it right after the creds
			// makes the phone see CAR_BLUETOOTH_DISAPPEAR and tear the session down. So hold it open (forked, like
			// HFP) and drain until the phone drops it, instead of closing.
			fprintf(stderr, "aa: phone %s connected over RFCOMM; handing AP creds\n", dev);
			pid_t c = fork();
			if (c == 0) {
				struct timeval tv = { 10, 0 };
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
				if (aa_bt_handshake(fd, cr->ip, cr->port, cr->ssid, cr->key, cr->bssid)) {
					fprintf(stderr, "aa: BT creds handshake failed\n"); _exit(1);
				}
				struct timeval none = { 0, 0 };   // creds done: block and keep the link alive for the session
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
				uint16_t id; uint8_t m[256]; size_t mn;
				while (bt_recv(fd, &id, m, sizeof m, &mn) == 0)
					fprintf(stderr, "aa: BT rx id %u (%zu bytes) — link held\n", id, mn);
				fprintf(stderr, "aa: RFCOMM closed by phone\n");
				_exit(0);
			}
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
	if (!g_dbus_connection_register_object(bus, obj_path, ni->interfaces[0], &vt, cr, NULL, &e)) {
		fprintf(stderr, "aa: register_object %s: %s\n", obj_path, e->message); return -1;
	}
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ_BUS, "/org/bluez", PROFILE_MGR_IF, "RegisterProfile",
		g_variant_new("(os@a{sv})", obj_path, uuid, opts),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &e);
	if (!r) { fprintf(stderr, "aa: RegisterProfile %s: %s\n", uuid, e->message); return -1; }
	g_variant_unref(r);
	return 0;
}

// aa bt <ip> <ssid> <key> <bssid>: register the AA + HFP profiles and serve connections until killed
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

	fprintf(stderr, "aa: profiles up (AA %s ch %d + HFP); waiting for the phone\n",
		AA_BT_UUID, AA_BT_RFCOMM_CHANNEL);
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
	fprintf(f, "interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=a\nchannel=36\nieee80211n=1\nwmm_enabled=1\n"
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

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--touch-test")) {   /* print the touch events a session would send, for N s */
		int tf = touch_open(), secs = argc > 2 ? atoi(argv[2]) : 15;
		if (tf < 0) return 1;
		input_open = 1; mt_force_visible = 1;
		printf("touch-test: touchscreen 0..%d x 0..%d, picture x %d..%d; touch now (%d s)\n", mt_max_x, mt_max_y, PJ_RGN_X, PJ_RGN_X + PJ_RGN_W - 1, secs); fflush(stdout);
		for (int64_t end = now_us() + (int64_t)secs * 1000000; now_us() < end; ) {
			struct pollfd pf = { tf, POLLIN, 0 };
			if (poll(&pf, 1, 200) > 0) touch_read(tf, -2, (aa_tls_t *)&mt_nlog);
		}
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest() | msg_selftest() | rx_selftest() | mt_selftest() | bt_selftest();
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "certcheck")) {   // certcheck <crt> [key]: 0 if usable (see cert_check)
		int r = cert_check(argv[2], argc == 4 ? argv[3] : NULL);
		if (r) fprintf(stderr, "aa: %s: %s\n", argv[2], r == 1 ? "self-signed: phones reject it (a Google-signed head-unit certificate is needed)"
						       : r == 2 ? "its key is missing, passphrase-protected, or another certificate's" : "no PEM certificate");
		return r ? 1 : 0;
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
	if (argc == 6 && !strcmp(argv[1], "ap")) return aa_ap_up(argv[2], argv[3], argv[4], argv[5]); // ap <iface> <ssid> <key> <ip>
#ifdef AA_BT
	if (argc == 6 && !strcmp(argv[1], "bt")) return aa_bt_serve(argv[2], argv[3], argv[4], argv[5]); // bt <ip> <ssid> <key> <bssid>
#endif
	fprintf(stderr, "usage: aa --selftest | aa serve [<cert.pem> <key.pem>]\n"
			"       aa ap <iface> <ssid> <key> <ip> | aa bt <ip> <ssid> <key> <bssid>\n"
			"  (serve: no cert args auto-generates a persistent self-signed cert at /etc/wattline/)\n");
	return 1;
}
