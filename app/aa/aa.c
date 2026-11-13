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
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <fcntl.h>
#include <sys/time.h>
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

// ServiceDiscoveryResponse advertising a VIDEO (H.264) channel at the given resolution (+ 30 fps)
static size_t build_service_discovery_response(uint8_t *buf, size_t cap, int res)
{
	uint8_t vc[16]; pb_out v = { vc, 0, sizeof vc, 0 };
	pb_u(&v, AA_VC_RESOLUTION, res); pb_u(&v, AA_VC_FPS, AA_FPS_30);
	uint8_t av[48]; pb_out a = { av, 0, sizeof av, 0 };
	pb_u(&a, AA_AVCH_STREAM_TYPE, AA_AVSTREAM_VIDEO);
	pb_b(&a, AA_AVCH_VIDEO_CONFIGS, vc, v.n);
	pb_u(&a, AA_AVCH_AVAILABLE_IN_CALL, 1);
	uint8_t chd[64]; pb_out c = { chd, 0, sizeof chd, 0 };
	pb_u(&c, AA_CHD_CHANNEL_ID, AA_CH_VIDEO);
	pb_b(&c, AA_CHD_AV_CHANNEL, av, a.n);
	pb_out o = { buf, 0, cap, 0 };
	pb_b(&o, AA_SDRESP_CHANNELS, chd, c.n);
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
	uint8_t sdr[256]; size_t sn = build_service_discovery_response(sdr, sizeof sdr, AA_RES_480p);
	if (!sn || !pb_ok(sdr, sn)) die("msg: build SDR");
	pb_in in = { sdr, sn, 0 }; pb_val f; int found = 0;
	while (pb_next(&in, &f) == 1) {
		if (f.field != AA_SDRESP_CHANNELS || f.wt != 2) continue;
		uint64_t chid; const uint8_t *avp; size_t avl;
		if (pb_get_u(f.p, f.len, AA_CHD_CHANNEL_ID, &chid) && chid == AA_CH_VIDEO &&
		    pb_get_b(f.p, f.len, AA_CHD_AV_CHANNEL, &avp, &avl)) {
			const uint8_t *vcp; size_t vcl; uint64_t st, res;
			if (pb_get_u(avp, avl, AA_AVCH_STREAM_TYPE, &st) && st == AA_AVSTREAM_VIDEO &&
			    pb_get_b(avp, avl, AA_AVCH_VIDEO_CONFIGS, &vcp, &vcl) &&
			    pb_get_u(vcp, vcl, AA_VC_RESOLUTION, &res) && res == AA_RES_480p) found = 1;
		}
	}
	if (!found) die("msg: SDR video config roundtrip");

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
	uint8_t payload[260], frame[280];
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
static int readn(int fd, uint8_t *b, size_t n) { size_t g = 0; while (g < n) { ssize_t r = read(fd, b + g, n - g); if (r <= 0) return -1; g += (size_t)r; } return 0; }
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

// receive one logical message (reassembling split frames); decrypts when the frames are ENCRYPTED.
static int recv_msg(int fd, aa_tls_t *tls, uint8_t *chan, uint8_t *out, size_t cap, size_t *mlen)
{
	uint8_t fr[AA_MAX_FRAME_PAYLOAD + 512], flags, c0 = AA_CH_NONE; int enc = 0;
	for (;;) {
		uint8_t ch; int pn = read_frame(fd, &ch, &flags, fr, sizeof fr);
		if (pn < 0) return -1;
		if (c0 == AA_CH_NONE) c0 = ch;
		if (flags & AA_ENC_ENCRYPTED) { tls_feed(tls, fr, (size_t)pn); enc = 1; }
		else { if ((size_t)pn > cap) return -1; memcpy(out, fr, (size_t)pn); *mlen = (size_t)pn; *chan = c0; return 0; }
		if (flags & AA_FRAME_LAST) break;   // LAST, or BULK (FIRST|LAST)
	}
	*chan = c0;
	if (enc) { int r = SSL_read(tls->ssl, out, (int)cap); if (r <= 0) return -1; *mlen = (size_t)r; }
	return 0;
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
		uint8_t chan, m[AA_MAX_FRAME_PAYLOAD + 16]; size_t mn;
		if (recv_msg(fd, NULL, &chan, m, sizeof m, &mn)) { fprintf(stderr, "aa: tls step %d: phone closed\n", i); return -1; }
		uint16_t id = msg_id(m, mn);
		if (id != AA_CTL_SSL_HANDSHAKE || mn < 2) { fprintf(stderr, "aa: tls step %d: got msg %u (%zu bytes), not a handshake\n", i, id, mn); return -1; }
		fprintf(stderr, "aa: tls step %d: got %zu bytes\n", i, mn - 2);
		tls_feed(tls, m + 2, mn - 2);
	}
	return -1;
}

// fork the decoder sink: spike-vdec --fb-stream reading H.264 on its stdin
static int spawn_sink(void)
{
	int p[2]; if (pipe(p)) return -1;
	pid_t c = fork();
	if (c == 0) { dup2(p[0], 0); close(p[0]); close(p[1]); execlp("spike-vdec", "spike-vdec", "--fb-stream", (char *)NULL); _exit(127); }
	close(p[0]);
	return c < 0 ? (close(p[1]), -1) : p[1];
}

static void ack(int fd, aa_tls_t *tls, uint32_t n)
{
	uint8_t a[16]; pb_out o = { a, 0, sizeof a, 0 }; pb_u(&o, AA_ACK_SESSION, 1); pb_u(&o, AA_ACK_VALUE, n);
	send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_MEDIA_ACK, a, o.n);
}

// the session after SSL + auth: respond to requests and route video media to the sink
static void aa_session(int fd, aa_tls_t *tls, int sink)
{
	uint8_t m[AA_MAX_FRAME_PAYLOAD + 16]; size_t mn; uint8_t chan; uint32_t acks = 0;
	while (recv_msg(fd, tls, &chan, m, sizeof m, &mn) == 0 && mn >= 2) {
		uint16_t id = msg_id(m, mn); const uint8_t *body = m + 2; size_t blen = mn - 2;
		if (chan == AA_CH_CONTROL && id == AA_CTL_SERVICE_DISCOVERY_REQUEST) {
			uint8_t sd[256]; size_t sn = build_service_discovery_response(sd, sizeof sd, AA_RES_480p);
			send_msg(fd, tls, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_SERVICE_DISCOVERY_RESPONSE, sd, sn);
		} else if (chan == AA_CH_CONTROL && id == AA_CTL_CHANNEL_OPEN_REQUEST) {
			uint64_t rch = AA_CH_VIDEO; pb_get_u(body, blen, AA_COREQ_CHANNEL_ID, &rch);
			uint8_t co[16]; size_t cn = build_channel_open_response(co, sizeof co, AA_STATUS_OK);
			send_msg(fd, tls, (uint8_t)rch, AA_MSG_CONTROL, AA_CTL_CHANNEL_OPEN_RESPONSE, co, cn);
		} else if (chan == AA_CH_VIDEO && id == AA_AV_SETUP_REQUEST) {
			uint8_t r[24]; size_t rn = build_av_setup_response(r, sizeof r);
			send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_SETUP_RESPONSE, r, rn);
		} else if (chan == AA_CH_VIDEO && id == AA_AV_VIDEO_FOCUS_REQUEST) {
			uint8_t vf[8]; pb_out o = { vf, 0, sizeof vf, 0 }; pb_u(&o, AA_VFI_FOCUS_MODE, AA_FOCUS_FOCUSED);
			send_msg(fd, tls, AA_CH_VIDEO, AA_MSG_SPECIFIC, AA_AV_VIDEO_FOCUS_INDICATION, vf, o.n);
		} else if (chan == AA_CH_VIDEO && id == AA_AV_MEDIA_WITH_TIMESTAMP) {
			if (!acks) fprintf(stderr, "aa: first video frame\n");
			if (blen > 8 && sink >= 0) writen(sink, body + 8, blen - 8);   // skip the 8-byte timestamp
			ack(fd, tls, ++acks);
		} else if (chan == AA_CH_VIDEO && id == AA_AV_MEDIA) {
			if (sink >= 0) writen(sink, body, blen);
			ack(fd, tls, ++acks);
		}
	}
}

static int aa_serve(const char *cert, const char *key)
{
	signal(SIGPIPE, SIG_IGN);
	int ls = socket(AF_INET, SOCK_STREAM, 0); if (ls < 0) die("socket: %s", strerror(errno));
	int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(AA_PORT);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) die("bind/listen %d: %s", AA_PORT, strerror(errno));
	SSL_CTX *ctx = tls_ctx(cert, key, 0);   // the head unit is the TLS client: it sends the ClientHello
	fprintf(stderr, "aa: listening on %d (phone must be on the AP; cert %s)\n", AA_PORT, cert);
	for (;;) {
		int fd = accept(ls, NULL, NULL); if (fd < 0) continue;
		fprintf(stderr, "aa: phone dialled in on %d\n", AA_PORT);
		struct timeval tmo = { 20, 0 }, none = { 0, 0 };   // a stalled handshake must not wedge the accept loop
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
		aa_tls_t t; tls_init(&t, ctx, 0);
		uint8_t vr[4] = { AA_VERSION_MAJOR >> 8, AA_VERSION_MAJOR & 0xff, AA_VERSION_MINOR >> 8, AA_VERSION_MINOR & 0xff };
		send_msg(fd, NULL, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_VERSION_REQUEST, vr, sizeof vr);
		uint8_t chan, m[64]; size_t mn;
		int ok = recv_msg(fd, NULL, &chan, m, sizeof m, &mn) == 0 && msg_id(m, mn) == AA_CTL_VERSION_RESPONSE;
		fprintf(stderr, "aa: version %s\n", ok ? "agreed" : "refused");
		if (ok && do_ssl_handshake(fd, &t) == 0) {
			fprintf(stderr, "aa: TLS up (%s, %s), session starting\n", SSL_get_version(t.ssl), SSL_get_cipher(t.ssl));
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
			uint8_t ac[8]; pb_out o = { ac, 0, sizeof ac, 0 }; pb_u(&o, AA_AC_STATUS, AA_STATUS_OK);
			send_msg(fd, &t, AA_CH_CONTROL, AA_MSG_SPECIFIC, AA_CTL_AUTH_COMPLETE, ac, o.n);
			int sink = spawn_sink();
			close(open("/run/dash-project", O_CREAT | O_WRONLY, 0644));   // the dash UI yields the video region
			aa_session(fd, &t, sink);
			unlink("/run/dash-project");
			if (sink >= 0) close(sink);
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

static void profile_method(GDBusConnection *conn, const char *sender, const char *path,
			   const char *iface, const char *method, GVariant *params,
			   GDBusMethodInvocation *inv, gpointer user)
{
	(void)conn; (void)sender; (void)iface;
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
		// BlueZ hands the socket over non-blocking; our readn/writen expect blocking I/O
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		if (!strcmp(path, HFP_PATH)) {
			// HFP link lives for the whole BT session -> handle in a child so the main loop keeps serving
			fprintf(stderr, "aa: phone %s connected HFP; establishing SLC\n", dev);
			pid_t c = fork();
			if (c == 0) { hfp_handle(fd); _exit(0); }
			close(fd);
		} else {
			fprintf(stderr, "aa: phone %s connected over RFCOMM; handing AP creds\n", dev);
			// runs on the main loop: bound each read so a silent phone can't stall the other profiles
			struct timeval tv = { 10, 0 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
			if (aa_bt_handshake(fd, cr->ip, cr->port, cr->ssid, cr->key, cr->bssid))
				fprintf(stderr, "aa: BT creds handshake failed\n");
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
// ponytail: accepts anyone while the Dash is pairable; gate on a pairing window/button if that matters.
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

static void agent_method(GDBusConnection *conn, const char *sender, const char *path,
			 const char *iface, const char *method, GVariant *params,
			 GDBusMethodInvocation *inv, gpointer user)
{
	(void)conn; (void)sender; (void)path; (void)iface; (void)params; (void)user;
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
	g_variant_builder_add(&ao, "{sv}", "RequireAuthentication", g_variant_new_boolean(FALSE));
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
		   "auth_algs=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=%s\n", iface, ssid, key);
	fclose(f);
	snprintf(c, sizeof c, "hostapd -B -P /run/aa-hostapd.pid -f /data/log/aa-hostapd.log /run/aa-hostapd.conf");
	if (sh(c)) { fprintf(stderr, "aa: hostapd failed\n"); return -1; }
	snprintf(c, sizeof c, "ip addr add %s/24 dev %s", ip, iface); sh(c);
	f = fopen("/run/aa-udhcpd.conf", "w");
	if (f) {
		// serve .10-.20 on the AP's /24 (derive the base from ip by trimming the last octet)
		char base[64]; snprintf(base, sizeof base, "%s", ip); char *d = strrchr(base, '.'); if (d) *d = 0;
		fprintf(f, "interface %s\nstart %s.10\nend %s.20\nmax_leases 10\n"
			   "lease_file /run/aa-udhcpd.leases\npidfile /run/aa-udhcpd.pid\noption subnet 255.255.255.0\noption lease 3600\n",
			iface, base, base);
		fclose(f);
		sh("touch /run/aa-udhcpd.leases; udhcpd /run/aa-udhcpd.conf");
	}
	fprintf(stderr, "aa: AP %s up on %s (ch36, %s/24)\n", ssid, iface, ip);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest() | msg_selftest() | bt_selftest();
	if (argc == 4 && !strcmp(argv[1], "gencert"))   // gencert <crt> <key>: a self-signed cert, for first boot / placeholder
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
