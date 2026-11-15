// SPDX-License-Identifier: MIT
// spike-vdec: R4 hardware spike. H.264 decode on the video engine through the V4L2 stateless (request) API
// (cedrus), with frame rate, CPU use and a bit-exact check against a software decode made at build time;
// plus a CPU estimate of the projection's rotate (NV12 1280x480 -> the portrait panel's BGRA, 90 degrees).
//
// Not a general decoder (Phase 1 needs one): one slice per picture, I and P only (no B), frames only, no
// ref list modification, no MMCO, no weighted prediction, no scaling matrices, POC types 0 and 2. The spike
// clips are encoded to fit (br-r4.sh). Anything else is refused with a message, never decoded wrongly.
//
//   spike-vdec                   every /usr/share/spike/*.h264: timed decode, verify, then the rotate estimate
//   spike-vdec FILE.h264 ...     the same for these clips (FILE.crc next to it: per-frame CRC-32 reference)
//   spike-vdec --info            the V4L2 video/media devices, their formats (stands in for v4l2-ctl/media-ctl)
//   spike-vdec --parse FILE      parse only (no device): per-slice header facts; runs on a build host too
//   spike-vdec --rotate          the rotate estimate only (from malloc'd memory)
//   spike-vdec --fb [FILE]       decode a clip straight to /dev/fb0 (the panel's projection region), looping
//   spike-vdec --fb-stream [--region X W]
//                                decode a live Annex-B H.264 stream from stdin to the panel (the AA video sink). With
//                                --region, the frame's middle W x 480 window is shown unscaled at landscape x = X..X+W-1
//                                (the AA receiver asks the phone for a frame whose margins leave exactly that window).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/media.h>
#include <linux/videodev2.h>

#define die(...) do { fprintf(stderr, "spike-vdec: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

/* ---- bit reader over the RBSP (emulation prevention bytes removed) ---- */
typedef struct { const uint8_t *p; size_t n, pos; } br_t;
static unsigned u(br_t *b, int k)
{
	unsigned v = 0;
	while (k--) {
		v <<= 1;
		if (b->pos < b->n * 8) v |= (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
		b->pos++;
	}
	return v;
}
static unsigned ue(br_t *b)
{
	int z = 0;
	while (!u(b, 1)) if (++z > 31) die("bad exp-Golomb code");
	return ((1u << z) - 1) + u(b, z);
}
static int se(br_t *b) { unsigned k = ue(b); return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2); }
static int more_rbsp_data(br_t *b)
{
	size_t last = b->n * 8;
	while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) last--;
	return b->pos + 1 < last;	/* the last 1 is the stop bit */
}
static size_t unescape(const uint8_t *s, size_t n, uint8_t *d, size_t max)
{
	size_t o = 0, z = 0;
	for (size_t i = 0; i < n && o < max; i++) {
		if (z >= 2 && s[i] == 3) { z = 0; continue; }
		z = s[i] ? 0 : z + 1;
		d[o++] = s[i];
	}
	return o;
}

/* ---- parameter sets and slice headers ---- */
static struct v4l2_ctrl_h264_sps sps;
static struct v4l2_ctrl_h264_pps pps;
static int have_sps, have_pps, crop_w, crop_h, crop_x, crop_y;

static void parse_sps(br_t *b)
{
	memset(&sps, 0, sizeof sps);
	sps.profile_idc = u(b, 8); sps.constraint_set_flags = u(b, 8); sps.level_idc = u(b, 8);
	sps.seq_parameter_set_id = ue(b);
	sps.chroma_format_idc = 1;
	switch (sps.profile_idc) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
		sps.chroma_format_idc = ue(b);
		if (sps.chroma_format_idc != 1) die("only 4:2:0");
		sps.bit_depth_luma_minus8 = ue(b); sps.bit_depth_chroma_minus8 = ue(b);
		if (sps.bit_depth_luma_minus8 || sps.bit_depth_chroma_minus8) die("only 8-bit");
		if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
		if (u(b, 1)) die("SPS scaling matrices: not in this spike decoder");
	}
	sps.log2_max_frame_num_minus4 = ue(b);
	sps.pic_order_cnt_type = ue(b);
	if (sps.pic_order_cnt_type == 0) sps.log2_max_pic_order_cnt_lsb_minus4 = ue(b);
	else if (sps.pic_order_cnt_type == 1) die("POC type 1: not in this spike decoder");
	sps.max_num_ref_frames = ue(b);
	if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	sps.pic_width_in_mbs_minus1 = ue(b); sps.pic_height_in_map_units_minus1 = ue(b);
	if (!u(b, 1)) die("interlaced: not in this spike decoder");
	sps.flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	int cl = 0, cr = 0, ct = 0, cb = 0;
	if (u(b, 1)) { cl = ue(b); cr = ue(b); ct = ue(b); cb = ue(b); }
	crop_x = 2 * cl; crop_y = 2 * ct;
	crop_w = 16 * (sps.pic_width_in_mbs_minus1 + 1) - 2 * (cl + cr);
	crop_h = 16 * (sps.pic_height_in_map_units_minus1 + 1) - 2 * (ct + cb);
	have_sps = 1;
}

static void parse_pps(br_t *b)
{
	memset(&pps, 0, sizeof pps);
	pps.pic_parameter_set_id = ue(b); pps.seq_parameter_set_id = ue(b);
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	if ((pps.num_slice_groups_minus1 = ue(b))) die("slice groups: not in this spike decoder");
	pps.num_ref_idx_l0_default_active_minus1 = ue(b); pps.num_ref_idx_l1_default_active_minus1 = ue(b);
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	pps.weighted_bipred_idc = u(b, 2);
	pps.pic_init_qp_minus26 = se(b); pps.pic_init_qs_minus26 = se(b);
	pps.chroma_qp_index_offset = se(b);
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
	if (more_rbsp_data(b)) {
		if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
		if (u(b, 1)) die("PPS scaling matrices: not in this spike decoder");
		pps.second_chroma_qp_index_offset = se(b);
	}
	have_pps = 1;
}

typedef struct {
	struct v4l2_ctrl_h264_slice_params sl;
	struct v4l2_ctrl_h264_decode_params dp;
	int idr, ref;
} pic_t;

static void parse_slice(br_t *b, int nal_type, int ref_idc, pic_t *p)
{
	memset(p, 0, sizeof *p);
	p->idr = nal_type == 5; p->ref = ref_idc != 0;
	b->pos = 8;	/* header_bit_size counts from the NAL header byte (the buffer starts there: START_CODE_NONE) */
	if ((p->sl.first_mb_in_slice = ue(b))) die("several slices per picture: not in this spike decoder");
	unsigned t = ue(b) % 5;
	if (t != V4L2_H264_SLICE_TYPE_P && t != V4L2_H264_SLICE_TYPE_I) die("slice type %u: only I and P here", t);
	p->sl.slice_type = t;
	if (ue(b) != pps.pic_parameter_set_id) die("a second PPS: not in this spike decoder");
	p->dp.frame_num = u(b, sps.log2_max_frame_num_minus4 + 4);
	if (p->idr) p->dp.idr_pic_id = ue(b);
	size_t poc0 = b->pos;
	if (sps.pic_order_cnt_type == 0) {
		p->dp.pic_order_cnt_lsb = u(b, sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) p->dp.delta_pic_order_cnt_bottom = se(b);
	}
	p->dp.pic_order_cnt_bit_size = b->pos - poc0;
	if (pps.flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT) p->sl.redundant_pic_cnt = ue(b);
	p->sl.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
	if (t == V4L2_H264_SLICE_TYPE_P) {
		if (u(b, 1)) p->sl.num_ref_idx_l0_active_minus1 = ue(b);
		if (u(b, 1)) die("ref pic list modification: not in this spike decoder");
		if (pps.flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) die("weighted prediction: not in this spike decoder");
	}
	if (p->ref) {
		size_t m0 = b->pos;
		if (p->idr) { u(b, 1); if (u(b, 1)) die("long-term references: not in this spike decoder"); }
		else if (u(b, 1)) die("MMCO: not in this spike decoder");
		p->dp.dec_ref_pic_marking_bit_size = b->pos - m0;
	}
	if ((pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) && t != V4L2_H264_SLICE_TYPE_I) p->sl.cabac_init_idc = ue(b);
	p->sl.slice_qp_delta = se(b);
	if (pps.flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		p->sl.disable_deblocking_filter_idc = ue(b);
		if (p->sl.disable_deblocking_filter_idc != 1) {
			p->sl.slice_alpha_c0_offset_div2 = se(b); p->sl.slice_beta_offset_div2 = se(b);
		}
	}
	p->sl.header_bit_size = b->pos;
	p->dp.nal_ref_idc = ref_idc;
	if (p->idr) p->dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	if (t == V4L2_H264_SLICE_TYPE_P) p->dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
}

/* ---- POC and the short-term reference list (sliding window, frames) ---- */
typedef struct { uint64_t ts; int frame_num, poc, cap; } ref_t;
static ref_t refs[16];
static int nrefs, prev_frame_num, frame_num_offset, prev_poc_msb, prev_poc_lsb;

static int calc_poc(pic_t *p)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4);
	if (p->idr) { prev_frame_num = 0; frame_num_offset = 0; prev_poc_msb = prev_poc_lsb = 0; nrefs = 0; }
	if (sps.pic_order_cnt_type == 0) {
		int max_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4), lsb = p->dp.pic_order_cnt_lsb, msb;
		if (lsb < prev_poc_lsb && prev_poc_lsb - lsb >= max_lsb / 2) msb = prev_poc_msb + max_lsb;
		else if (lsb > prev_poc_lsb && lsb - prev_poc_lsb > max_lsb / 2) msb = prev_poc_msb - max_lsb;
		else msb = prev_poc_msb;
		if (p->ref) { prev_poc_msb = msb; prev_poc_lsb = lsb; }
		prev_frame_num = p->dp.frame_num;
		return msb + lsb;
	}
	if (!p->idr && prev_frame_num > p->dp.frame_num) frame_num_offset += max_fn;
	prev_frame_num = p->dp.frame_num;
	return p->idr ? 0 : 2 * (frame_num_offset + p->dp.frame_num) - !p->ref;
}

/* fills dpb[] and ref_pic_list0 (PicNum descending) for this picture */
static void fill_refs(pic_t *p)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4), order[16], n = 0;
	for (int i = 0; i < nrefs; i++) {
		struct v4l2_h264_dpb_entry *e = &p->dp.dpb[i];
		int wrap = refs[i].frame_num > p->dp.frame_num ? refs[i].frame_num - max_fn : refs[i].frame_num;
		e->reference_ts = refs[i].ts; e->frame_num = refs[i].frame_num; e->pic_num = wrap;
		e->fields = V4L2_H264_FRAME_REF; e->top_field_order_cnt = e->bottom_field_order_cnt = refs[i].poc;
		e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
		int k = n++;
		while (k > 0 && (int)p->dp.dpb[order[k - 1]].pic_num < wrap) { order[k] = order[k - 1]; k--; }
		order[k] = i;
	}
	if (p->sl.slice_type == V4L2_H264_SLICE_TYPE_P) {
		if (!n) die("P picture with no reference");
		for (int k = 0; k <= p->sl.num_ref_idx_l0_active_minus1; k++) {
			p->sl.ref_pic_list0[k].index = order[k < n ? k : n - 1];
			p->sl.ref_pic_list0[k].fields = V4L2_H264_FRAME_REF;
		}
	}
}

static void add_ref(pic_t *p, uint64_t ts, int poc, int cap)
{
	int max = sps.max_num_ref_frames ? sps.max_num_ref_frames : 1, max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4);
	if (nrefs == max) {	/* sliding window: drop the smallest FrameNumWrap */
		int lo = 0, low = 1 << 30;
		for (int i = 0; i < nrefs; i++) {
			int w = refs[i].frame_num > p->dp.frame_num ? refs[i].frame_num - max_fn : refs[i].frame_num;
			if (w < low) { low = w; lo = i; }
		}
		refs[lo] = refs[--nrefs];
	}
	refs[nrefs++] = (ref_t){ ts, p->dp.frame_num, poc, cap };
}

/* ---- the clip ---- */
typedef struct { const uint8_t *p; size_t n; } nal_t;
static nal_t *nals; static int nnals;
static uint8_t *load(const char *f, size_t *n)
{
	FILE *fp = fopen(f, "rb"); if (!fp) die("%s: %s", f, strerror(errno));
	fseek(fp, 0, SEEK_END); *n = ftell(fp); rewind(fp);
	uint8_t *d = malloc(*n); if (!d || fread(d, 1, *n, fp) != *n) die("%s: read failed", f);
	fclose(fp); return d;
}
static void split(const uint8_t *d, size_t n)
{
	nnals = 0;
	for (size_t i = 0; i + 3 <= n; i++) {
		if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
		if (nnals) { size_t e = i; while (e > 0 && !d[e - 1]) e--; nals[nnals - 1].n = e - (nals[nnals - 1].p - d); }
		nals = realloc(nals, (nnals + 1) * sizeof *nals);
		nals[nnals++] = (nal_t){ d + i + 3, n - (i + 3) };
		i += 2;
	}
}

/* walks the clip: SPS/PPS kept, each slice parsed into *p; returns the next index or -1 at the end */
static uint8_t rbsp[4096];
static int next_pic(int i, pic_t *p, nal_t *out)
{
	for (; i < nnals; i++) {
		int t = nals[i].p[0] & 0x1f, r = (nals[i].p[0] >> 5) & 3;
		size_t full = t == 7 || t == 8;
		br_t b = { rbsp, unescape(nals[i].p, nals[i].n, rbsp, full ? sizeof rbsp : 512), 8 };
		if (t == 7) parse_sps(&b);
		else if (t == 8) parse_pps(&b);
		else if (t == 1 || t == 5) {
			if (!have_sps || !have_pps) die("slice before SPS/PPS");
			parse_slice(&b, t, r, p); *out = nals[i]; return i + 1;
		}
	}
	return -1;
}

static int parse_only(const char *f)
{
	size_t n; uint8_t *d = load(f, &n); split(d, n);
	pic_t p; nal_t s; int i = 0, k = 0;
	while ((i = next_pic(i, &p, &s)) >= 0) {
		int poc = calc_poc(&p);
		fill_refs(&p);
		printf("pic %d: %c%s frame_num %u poc %d header_bit_size %u qp_delta %d cabac_init %u refs %d bytes %zu\n", k++,
		       p.sl.slice_type == V4L2_H264_SLICE_TYPE_I ? 'I' : 'P', p.idr ? " IDR" : "", p.dp.frame_num, poc,
		       p.sl.header_bit_size, p.sl.slice_qp_delta, p.sl.cabac_init_idc, nrefs, s.n);
		if (p.ref) add_ref(&p, k, poc, 0);
	}
	printf("parsed %d pictures, %dx%d, profile %u level %u, %s, max refs %u\n", k, crop_w, crop_h, sps.profile_idc,
	       sps.level_idc, pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE ? "CABAC" : "CAVLC", sps.max_num_ref_frames);
	free(d); return 0;
}

/* ---- devices ---- */
static int find_dev(const char *pat, int media, char *name)
{
	glob_t g; int fd = -1;
	if (glob(pat, 0, NULL, &g)) return -1;
	for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
		int f = open(g.gl_pathv[i], O_RDWR);
		if (f < 0) continue;
		if (media) {
			struct media_device_info mi = { 0 };
			if (!ioctl(f, MEDIA_IOC_DEVICE_INFO, &mi) && !strcmp(mi.driver, "cedrus")) fd = f;
		} else {
			struct v4l2_capability c = { 0 };
			if (!ioctl(f, VIDIOC_QUERYCAP, &c) && !strcmp((char *)c.driver, "cedrus")) fd = f;
		}
		if (fd < 0) close(f); else strcpy(name, g.gl_pathv[i]);
	}
	globfree(&g); return fd;
}

static int info(void)
{
	glob_t g; int any = 0;
	if (!glob("/dev/video*", 0, NULL, &g)) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			int f = open(g.gl_pathv[i], O_RDWR); struct v4l2_capability c = { 0 };
			if (f < 0 || ioctl(f, VIDIOC_QUERYCAP, &c)) { printf("%s: cannot query\n", g.gl_pathv[i]); if (f >= 0) close(f); continue; }
			any = 1;
			printf("%s: driver %s, card %s, bus %s, caps 0x%08x%s\n", g.gl_pathv[i], c.driver, c.card, c.bus_info,
			       c.device_caps, c.device_caps & V4L2_CAP_VIDEO_M2M ? " (m2m)" : "");
			for (int t = 0; t < 2; t++) {
				struct v4l2_fmtdesc fd = { .type = t ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT };
				printf("  %s:", t ? "capture" : "output ");
				for (fd.index = 0; !ioctl(f, VIDIOC_ENUM_FMT, &fd); fd.index++)
					printf(" %.4s", (char *)&fd.pixelformat);
				printf("\n");
			}
			close(f);
		}
		globfree(&g);
	}
	if (!glob("/dev/media*", 0, NULL, &g)) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			int f = open(g.gl_pathv[i], O_RDWR); struct media_device_info mi = { 0 };
			if (f >= 0 && !ioctl(f, MEDIA_IOC_DEVICE_INFO, &mi))
				printf("%s: driver %s, model %s, bus %s\n", g.gl_pathv[i], mi.driver, mi.model, mi.bus_info);
			if (f >= 0) close(f);
		}
		globfree(&g);
	}
	if (!any) printf("no /dev/video* device: is the video-codec node in the DT and cedrus probed? (dmesg | grep -i cedrus)\n");
	return !any;
}

/* ---- CRC-32 (zlib's), for the bit-exact check ---- */
static uint32_t crc_tab[256];
static uint32_t crc32_buf(uint32_t c, const uint8_t *p, size_t n)
{
	if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320 ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
	c = ~c;
	while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu_self(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6; }
static void cpu_sys(unsigned long long *busy, unsigned long long *all)
{
	unsigned long long v[8] = { 0 }; FILE *f = fopen("/proc/stat", "r");
	if (!f || fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) { *busy = *all = 0; if (f) fclose(f); return; }
	fclose(f);
	*all = 0; for (int i = 0; i < 8; i++) *all += v[i];
	*busy = *all - v[3] - v[4];
}

/* ---- the rotate estimate: NV12 W x H landscape -> BGRA H x W portrait (the panel is scanned 480x1920) ---- */
static uint8_t clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void rotate_nv12_bgra(const uint8_t *y, const uint8_t *uv, int stride, int w, int h, uint32_t *dst, int y0, int y1)
{
	/* landscape (x, y) -> panel (h-1-y, x): dst row x, column h-1-y. 16x16 tiles keep the column writes cached.
	 * Rows y0..y1-1 only (even bounds): two threads each take a band of rows, and their writes never overlap. */
	for (int ty = y0; ty < y1; ty += 16)
		for (int tx = 0; tx < w; tx += 16)
			for (int yy = ty; yy < ty + 16 && yy < y1; yy += 2)
				for (int xx = tx; xx < tx + 16 && xx < w; xx += 2) {
					const uint8_t *c = uv + (yy / 2) * stride + xx;
					int d = c[0] - 128, e = c[1] - 128;
					int r = 409 * e + 128, g = -100 * d - 208 * e + 128, b = 516 * d + 128;
					for (int k = 0; k < 4; k++) {
						int px = xx + (k & 1), py = yy + (k >> 1);
						int l = 298 * (y[py * stride + px] - 16);
						dst[px * h + (h - 1 - py)] = 0xff000000u | clip8((l + r) >> 8) << 16 | clip8((l + g) >> 8) << 8 | clip8((l + b) >> 8);
					}
				}
}
// rotate + scale NV12 (source sw x sh) into the panel region (w x h landscape) as panel-native BGRA.
// Nearest-neighbour; folds the 90-degree rotation in. AA sends 800x480 (480p), the region is 1280x480,
// so the map is scaled up to fill it. Per-pixel (not tiled): used only when a scale is actually needed.
static void rotate_scale_nv12_bgra(const uint8_t *y, const uint8_t *uv, int stride, int sw, int sh, int w, int h, uint32_t *dst)
{
	for (int ox = 0; ox < w; ox++) {
		int sx = sw == w ? ox : ox * sw / w;
		const uint8_t *yr = y + sx, *uvc = uv + (sx & ~1);
		for (int oy = 0; oy < h; oy++) {
			int sy = sh == h ? oy : oy * sh / h;
			const uint8_t *c = uvc + (sy / 2) * stride;
			int d = c[0] - 128, e = c[1] - 128;
			int l = 298 * (yr[sy * stride] - 16);
			int r = 409 * e + 128, g = -100 * d - 208 * e + 128, b = 516 * d + 128;
			dst[ox * h + (h - 1 - oy)] = 0xff000000u | clip8((l + r) >> 8) << 16 | clip8((l + g) >> 8) << 8 | clip8((l + b) >> 8);
		}
	}
}
static void rotate_bench(const char *what, const uint8_t *src, int stride, int w, int h)
{
	uint32_t *dst = malloc((size_t)w * h * 4); if (!dst) die("out of memory");
	int n = 30; double t = now(), c = cpu_self();
	for (int i = 0; i < n; i++) rotate_nv12_bgra(src, src + (size_t)stride * h, stride, w, h, dst, 0, h);
	t = now() - t; c = cpu_self() - c;
	printf("ROTATE %s %dx%d NV12 -> %dx%d BGRA: %.1f ms/frame = %.1f fps on one core (CPU %.0f%% of one core)\n",
	       what, w, h, h, w, t * 1000 / n, n / t, 100 * c / t);
	free(dst);
}

/* ---- framebuffer output (spike --fb: blit decoded+rotated frames to the panel's projection region) ----
 * rotate_nv12_bgra gives a panel-native buffer: row = landscape x, col = 479 - landscape y (= LVGL's 270 rotation).
 * The left-2/3 video region is landscape x 0..w-1 -> physical rows 0..w-1, each row = h pixels (= panel width). */
#define RGN_W 1280   /* the projection region: left 2/3 of the 1920-wide landscape canvas */
#define RGN_H 480
static int rgn_x = 0, rgn_w = RGN_W;   /* --region: where on the canvas, and how wide (height is always RGN_H) */
static int fb_fd = -1; static uint8_t *fb_mem = NULL; static long fb_line = 0, fb_len = 0;
static uint32_t *fb_bgra = NULL;
static void fb_open(void)
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) die("open /dev/fb0: %s", strerror(errno));
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &f)) die("fb screeninfo: %s", strerror(errno));
	if (v.bits_per_pixel != 32) die("/dev/fb0 is %u bpp, need 32", v.bits_per_pixel);
	fb_line = f.line_length; fb_len = f.smem_len;
	fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) die("mmap fb: %s", strerror(errno));
	fprintf(stderr, "spike-vdec: /dev/fb0 %ux%u %ubpp line=%ld\n", v.xres, v.yres, v.bits_per_pixel, fb_line);
}
static void fb_blit(int w, int h)	/* w = frame width (= physical rows), h = frame height (= physical cols) */
{
	for (int r = 0; r < w; r++) {
		long off = (long)(rgn_x + r) * fb_line;
		if (off + (long)h * 4 > fb_len) break;
		memcpy(fb_mem + off, fb_bgra + (size_t)r * h, (size_t)h * 4);
	}
}

/* ---- decode ---- */
#define NOUT 2
#define MAXCAP 20
struct buf { void *p; size_t n; };

/* persistent decoder state, shared by the file path (decode) and the live-stream path (decode_stream) */
typedef struct {
	int vfd, mfd, rq, ncap, stride, ah, frames, errors;
	struct buf out[NOUT], cap[MAXCAP];
	uint8_t *fb_cram;	/* cached NV12 copy so the rotate reads fast (the V4L2 mmap is uncached) */
	int show_each;		/* the file path: show every picture. The live stream shows only the newest (fb_show) */
	int last_ci;		/* the capture buffer of the newest picture not yet shown, or -1 */
	int shown; double show_s;	/* stats: pictures shown and the time spent showing them */
} dec_t;

/* open the video engine and set it up from the current (already-parsed) global sps */
static void dec_setup(dec_t *D, int show_each)
{
	memset(D, 0, sizeof *D);
	D->show_each = show_each; D->last_ci = -1;
	char vname[64], mname[64];
	D->vfd = find_dev("/dev/video*", 0, vname); D->mfd = find_dev("/dev/media*", 1, mname);
	if (D->vfd < 0 || D->mfd < 0) die("no cedrus video/media device (spike-vdec --info)");
	int cw = 16 * (sps.pic_width_in_mbs_minus1 + 1), ch = 16 * (sps.pic_height_in_map_units_minus1 + 1);
	struct v4l2_format fo = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	fo.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE; fo.fmt.pix.width = cw; fo.fmt.pix.height = ch;
	fo.fmt.pix.sizeimage = 1024 * 1024;
	if (ioctl(D->vfd, VIDIOC_S_FMT, &fo)) die("S_FMT output: %s", strerror(errno));
	struct v4l2_ext_control c0[] = { { .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof sps, .ptr = &sps } };
	struct v4l2_ext_controls cs0 = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = c0 };
	if (ioctl(D->vfd, VIDIOC_S_EXT_CTRLS, &cs0)) die("SPS control: %s", strerror(errno));
	struct v4l2_format fc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	fc.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12; fc.fmt.pix.width = cw; fc.fmt.pix.height = ch;
	if (ioctl(D->vfd, VIDIOC_S_FMT, &fc) || fc.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) die("S_FMT capture NV12: %s", strerror(errno));
	D->stride = fc.fmt.pix.bytesperline; D->ah = fc.fmt.pix.height;
	D->ncap = sps.max_num_ref_frames + 3; if (D->ncap < 4) D->ncap = 4; if (D->ncap > MAXCAP) D->ncap = MAXCAP;
	struct v4l2_requestbuffers rb = { .count = NOUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(D->vfd, VIDIOC_REQBUFS, &rb) || rb.count < NOUT) die("REQBUFS output: %s", strerror(errno));
	if (!(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) die("the output queue has no request support");
	rb = (struct v4l2_requestbuffers){ .count = D->ncap, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(D->vfd, VIDIOC_REQBUFS, &rb) || (int)rb.count < D->ncap) die("REQBUFS capture (%d buffers; CMA too small?): %s", D->ncap, strerror(errno));
	for (int k = 0; k < NOUT + D->ncap; k++) {
		int isout = k < NOUT, i = isout ? k : k - NOUT;
		struct v4l2_buffer b = { .type = isout ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(D->vfd, VIDIOC_QUERYBUF, &b)) die("QUERYBUF: %s", strerror(errno));
		struct buf *x = isout ? &D->out[i] : &D->cap[i];
		x->n = b.length; x->p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, D->vfd, b.m.offset);
		if (x->p == MAP_FAILED) die("mmap: %s", strerror(errno));
	}
	int ty[2] = { V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_BUF_TYPE_VIDEO_CAPTURE };
	if (ioctl(D->vfd, VIDIOC_STREAMON, &ty[0]) || ioctl(D->vfd, VIDIOC_STREAMON, &ty[1])) die("STREAMON: %s", strerror(errno));
	if (ioctl(D->mfd, MEDIA_IOC_REQUEST_ALLOC, &D->rq)) die("MEDIA_IOC_REQUEST_ALLOC: %s", strerror(errno));
	if (fb_fd >= 0) {
		fb_bgra = realloc(fb_bgra, (size_t)rgn_w * RGN_H * 4);   /* the region, not the source size */
		D->fb_cram = malloc((size_t)D->stride * D->ah * 3 / 2);
		if (!fb_bgra || !D->fb_cram) die("out of memory (fb buffers)");
	}
}

/* show the newest decoded picture on the panel: copy the rows the region needs out of the (uncached) capture
 * buffer, rotate them into panel order, blit. This is the costly step (the decode itself is ~3 ms in hardware),
 * so the live stream runs it once per batch of input, on the newest picture only: when the source is faster than
 * this, older pictures are decoded (they are references) but not shown, and the screen never falls behind. */
/* one band of the region's rows: copy them (and their chroma rows) out of the uncached capture buffer, rotate them */
typedef struct { const dec_t *D; const uint8_t *cy, *cuv; uint8_t *yy, *uv; int sx, sy, y0, y1; } band_t;
static void *band_show(void *arg)
{
	const band_t *b = arg; size_t st = (size_t)b->D->stride;
	memcpy(b->yy + (b->sy + b->y0) * st, b->cy + (b->sy + b->y0) * st, (size_t)(b->y1 - b->y0) * st);
	memcpy(b->uv + (b->sy + b->y0) / 2 * st, b->cuv + (b->sy + b->y0) / 2 * st, (size_t)(b->y1 - b->y0) / 2 * st);
	rotate_nv12_bgra(b->yy + b->sy * st + b->sx, b->uv + b->sy / 2 * st + b->sx, (int)st, rgn_w, RGN_H, fb_bgra, b->y0, b->y1);
	return NULL;
}
static void fb_show(dec_t *D)
{
	if (D->last_ci < 0) return;
	double t = now();
	const uint8_t *cy = D->cap[D->last_ci].p, *cuv = cy + (size_t)D->stride * D->ah;
	uint8_t *yy = D->fb_cram, *uv = D->fb_cram + (size_t)D->stride * D->ah;
	if (crop_w >= rgn_w && crop_h >= RGN_H) {   /* the region's window from the frame's middle, unscaled */
		int sx = ((crop_w - rgn_w) / 2 + crop_x) & ~1, sy = ((crop_h - RGN_H) / 2 + crop_y) & ~1;   /* even: NV12 */
		/* both cores (the T113 has two): the top band on a thread, the bottom one here. RGN_H / 2 = 240 keeps both
		 * bands even and tile-aligned. If the thread cannot start, this thread does both bands. */
		band_t top = { D, cy, cuv, yy, uv, sx, sy, 0, RGN_H / 2 }, bot = { D, cy, cuv, yy, uv, sx, sy, RGN_H / 2, RGN_H };
		pthread_t th; int threaded = pthread_create(&th, NULL, band_show, &top) == 0;
		band_show(&bot);
		if (threaded) pthread_join(th, NULL); else band_show(&top);
	} else {                                    /* a smaller frame than asked for: scale it into the region */
		memcpy(D->fb_cram, D->cap[D->last_ci].p, (size_t)D->stride * D->ah * 3 / 2);
		rotate_scale_nv12_bgra(yy, uv, D->stride, crop_w, crop_h, rgn_w, RGN_H, fb_bgra);
	}
	fb_blit(rgn_w, RGN_H);
	D->last_ci = -1; D->shown++; D->show_s += now() - t;
}

/* decode one already-parsed picture (slice bytes in *s); show it if this path shows every one; returns its capture index */
static int dec_one(dec_t *D, pic_t *p, nal_t *s)
{
	int poc = calc_poc(p);
	p->dp.top_field_order_cnt = p->dp.bottom_field_order_cnt = poc;
	fill_refs(p);
	int ci = -1;	/* a capture buffer that holds no reference */
	for (int k = 0; k < D->ncap && ci < 0; k++) { int used = 0; for (int r = 0; r < nrefs; r++) used |= refs[r].cap == k; if (!used) ci = k; }
	if (ci < 0) die("no free capture buffer");
	if (s->n > D->out[D->frames % NOUT].n) die("slice of %zu bytes > output buffer", s->n);
	memcpy(D->out[D->frames % NOUT].p, s->p, s->n);
	struct v4l2_ext_control cc[] = {
		{ .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof sps, .ptr = &sps },
		{ .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof pps, .ptr = &pps },
		{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof p->dp, .ptr = &p->dp },
		{ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .size = sizeof p->sl, .ptr = &p->sl },
	};
	struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = 4, .request_fd = D->rq, .controls = cc };
	if (ioctl(D->vfd, VIDIOC_S_EXT_CTRLS, &cs)) die("frame %d: controls: %s (error_idx %u)", D->frames, strerror(errno), cs.error_idx);
	uint64_t ts = (uint64_t)(D->frames + 1) * 1000;	/* ns, as v4l2_timeval_to_ns() makes it */
	struct v4l2_buffer bo = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP, .index = D->frames % NOUT,
		.bytesused = s->n, .flags = V4L2_BUF_FLAG_REQUEST_FD, .request_fd = D->rq };
	bo.timestamp.tv_sec = (D->frames + 1) / 1000000; bo.timestamp.tv_usec = (D->frames + 1) % 1000000;
	struct v4l2_buffer bc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = ci };
	if (ioctl(D->vfd, VIDIOC_QBUF, &bo) || ioctl(D->vfd, VIDIOC_QBUF, &bc)) die("QBUF: %s", strerror(errno));
	if (ioctl(D->rq, MEDIA_REQUEST_IOC_QUEUE)) die("MEDIA_REQUEST_IOC_QUEUE: %s", strerror(errno));
	struct pollfd pf = { .fd = D->rq, .events = POLLPRI };
	if (poll(&pf, 1, 2000) != 1) die("frame %d: the video engine did not finish within 2 s", D->frames);
	if (ioctl(D->vfd, VIDIOC_DQBUF, &bo) || ioctl(D->vfd, VIDIOC_DQBUF, &bc)) die("DQBUF: %s", strerror(errno));
	if (bc.flags & V4L2_BUF_FLAG_ERROR) D->errors++;
	if (ioctl(D->rq, MEDIA_REQUEST_IOC_REINIT)) die("MEDIA_REQUEST_IOC_REINIT: %s", strerror(errno));
	if (p->ref) add_ref(p, ts, poc, ci);
	D->frames++;
	D->last_ci = ci;	/* decode order, not POC order: fine for the display (AA sends IPPP, no reorder) */
	if (fb_fd >= 0 && D->show_each) fb_show(D);
	return ci;
}

static void dec_free(dec_t *D)
{
	free(D->fb_cram);
	close(D->rq); close(D->vfd); close(D->mfd);
	for (int k = 0; k < NOUT; k++) munmap(D->out[k].p, D->out[k].n);
	for (int k = 0; k < D->ncap; k++) munmap(D->cap[k].p, D->cap[k].n);
}

static int decode(const char *file, int verify, int do_rotate)
{
	size_t n; uint8_t *d = load(file, &n); split(d, n);
	have_sps = have_pps = 0; nrefs = 0;
	pic_t p; nal_t s;
	if (next_pic(0, &p, &s) < 0) die("%s: no picture", file);	/* parse the SPS so dec_setup has the size */
	dec_t D; dec_setup(&D, 1);
	FILE *crc = NULL; char crcname[512];
	if (verify) { snprintf(crcname, sizeof crcname, "%.*s.crc", (int)(strlen(file) - 5), file); crc = fopen(crcname, "r"); }
	unsigned long long b0, a0, b1, a1; cpu_sys(&b0, &a0);
	double t0 = now(), c0s = cpu_self();
	int i = 0, bad = 0, checked = 0, last_cap = 0;
	have_sps = have_pps = 0; nrefs = 0;
	while ((i = next_pic(i, &p, &s)) >= 0) {
		int ci = dec_one(&D, &p, &s);
		if (crc) {
			const uint8_t *y = D.cap[ci].p, *uv = y + (size_t)D.stride * D.ah; uint32_t v = 0, want;
			for (int r = 0; r < crop_h; r++) v = crc32_buf(v, y + (size_t)(crop_y + r) * D.stride + crop_x, crop_w);
			for (int r = 0; r < crop_h / 2; r++) v = crc32_buf(v, uv + (size_t)(crop_y / 2 + r) * D.stride + crop_x, crop_w);
			if (fscanf(crc, "%x", &want) == 1) { checked++; if (v != want) { if (!bad) printf("  frame %d: CRC %08x, reference %08x\n", D.frames - 1, v, want); bad++; } }
		}
		last_cap = ci;
	}
	double t = now() - t0, c = cpu_self() - c0s; cpu_sys(&b1, &a1);
	if (!verify)
		printf("DECODE %s: %dx%d %s, %d frames in %.2f s = %.1f fps; CPU: this process %.0f%% of one core, system %.0f%% busy (all cores); buffer errors %d\n",
		       file, crop_w, crop_h, pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE ? "CABAC" : "CAVLC", D.frames, t, D.frames / t,
		       100 * c / t, a1 > a0 ? 100.0 * (b1 - b0) / (a1 - a0) : 0, D.errors);
	else if (crc)
		printf("VERIFY %s: %d/%d frames bit-exact with the software decode%s\n", file, checked - bad, checked, bad ? " (MISMATCH)" : "");
	else
		printf("VERIFY %s: no %s, skipped\n", file, crcname);
	if (do_rotate && crop_w == 1280 && crop_h == 480) {
		rotate_bench("from the decoded buffer (V4L2 mmap)", D.cap[last_cap].p, D.stride, crop_w, crop_h);
		size_t sz = (size_t)D.stride * D.ah * 3 / 2; uint8_t *m = malloc(sz); memcpy(m, D.cap[last_cap].p, sz);
		rotate_bench("from a copy in cached RAM", m, D.stride, crop_w, crop_h); free(m);
	}
	if (crc) fclose(crc);
	int rc = D.errors || bad;
	dec_free(&D);
	free(d);
	return rc;
}

/* ---- live stream: decode an Annex-B H.264 elementary stream (what the AA video channel carries) to the panel ---- */
static void process_nal(const uint8_t *p, size_t n, dec_t *D, int *set)
{
	if (!n) return;
	int t = p[0] & 0x1f, r = (p[0] >> 5) & 3;
	size_t full = t == 7 || t == 8;
	br_t b = { rbsp, unescape(p, n, rbsp, full ? sizeof rbsp : 512), 8 };
	if (t == 7) parse_sps(&b);
	else if (t == 8) parse_pps(&b);
	else if (t == 1 || t == 5) {
		if (!have_sps || !have_pps) return;	/* joined mid-stream: wait for the next SPS/PPS before decoding */
		if (!*set) { dec_setup(D, 0); *set = 1; }
		pic_t p2; nal_t s = { p, n };
		parse_slice(&b, t, r, &p2);
		dec_one(D, &p2, &s);
	}
}

/* process every complete NAL in buf[0..len) (one ending where the next start code begins); keep the last, possibly
 * partial, NAL unless at EOF. Returns how many bytes may be dropped from the front. */
static size_t stream_nals(uint8_t *buf, size_t len, dec_t *D, int *set, int eof)
{
	size_t prev = (size_t)-1, last = (size_t)-1;
	for (size_t i = 0; i + 3 <= len; i++) {
		if (buf[i] || buf[i + 1] || buf[i + 2] != 1) continue;
		if (prev != (size_t)-1) {
			size_t e = i; while (e > prev + 3 && !buf[e - 1]) e--;	/* trim the trailing zeros of the start code */
			process_nal(buf + prev + 3, e - (prev + 3), D, set);
		}
		prev = i; last = i; i += 2;
	}
	if (eof && last != (size_t)-1) {
		size_t e = len; while (e > last + 3 && !buf[e - 1]) e--;
		process_nal(buf + last + 3, e - (last + 3), D, set);
		return len;
	}
	return last == (size_t)-1 ? 0 : last;	/* keep from the last start code: its NAL may still be arriving */
}

static int decode_stream(int fd)
{
	have_sps = have_pps = 0; nrefs = 0;
	dec_t D; int set = 0;
	size_t cap = 1u << 20, len = 0; uint8_t *buf = malloc(cap);
	if (!buf) die("out of memory");
	double next_stat = now() + 5, last_show_t = 0; int last_frames = 0, last_shown = 0; double last_show_s = 0;
	for (;;) {
		if (cap - len < 65536) { cap *= 2; buf = realloc(buf, cap); if (!buf) die("out of memory"); }
		ssize_t r = read(fd, buf + len, cap - len);
		if (r < 0) { if (errno == EINTR) continue; die("read: %s", strerror(errno)); }
		if (r == 0) { stream_nals(buf, len, &D, &set, 1); if (set) fb_show(&D); break; }	/* EOF: the last NAL */
		len += r;
		size_t used = stream_nals(buf, len, &D, &set, 0);
		if (used) { memmove(buf, buf + used, len - used); len -= used; }
		/* show the newest picture once the input is drained: if more is already waiting, decode that first */
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		/* ...but at least every 100 ms, so a burst of input cannot freeze the picture */
		if (set && D.last_ci >= 0 && (poll(&pf, 1, 0) == 0 || now() - last_show_t >= 0.1)) { fb_show(&D); last_show_t = now(); }
		if (set && now() >= next_stat) {	/* the live picture's health: decoded vs shown, cost of showing */
			int sh = D.shown - last_shown;
			fprintf(stderr, "spike-vdec: 5 s: %d decoded, %d shown, %.1f ms per show\n", D.frames - last_frames, sh,
				sh ? (D.show_s - last_show_s) * 1000 / sh : 0.0);
			last_frames = D.frames; last_shown = D.shown; last_show_s = D.show_s; next_stat = now() + 5;
		}
	}
	free(buf);
	if (set) dec_free(&D);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--info")) return info();
	if (argc > 2 && !strcmp(argv[1], "--parse")) return parse_only(argv[2]);
	if (argc > 1 && !strcmp(argv[1], "--rotate")) {
		int w = 1280, h = 480; uint8_t *m = malloc(w * h * 3 / 2);
		for (int k = 0; k < w * h * 3 / 2; k++) m[k] = k * 7;
		rotate_bench("from cached RAM (synthetic)", m, w, w, h); free(m); return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--fb")) {	/* decode a clip straight to the panel's projection region, looping */
		const char *f = argc > 2 ? argv[2] : "/usr/share/spike/vdec-1280x480-baseline.h264";
		fb_open();
		for (;;) decode(f, 0, 0);	/* loops the clip until killed */
	}
	if (argc > 1 && !strcmp(argv[1], "--fb-stream")) {	/* decode a live Annex-B H.264 stream from stdin to the panel */
		if (argc > 4 && !strcmp(argv[2], "--region")) {
			rgn_x = atoi(argv[3]), rgn_w = atoi(argv[4]);
			if (rgn_x < 0 || rgn_w < 16 || rgn_w % 2 || rgn_x + rgn_w > 1920) die("--region %d %d: off the 1920-wide canvas", rgn_x, rgn_w);
		}
		fb_open();
		return decode_stream(0);	/* the AA video channel pipes H.264 in; EOF ends it */
	}
	glob_t g = { 0 };
	char **files = argv + 1; int nfiles = argc - 1;
	if (!nfiles) { if (glob("/usr/share/spike/*.h264", 0, NULL, &g)) die("no clips in /usr/share/spike"); files = g.gl_pathv; nfiles = g.gl_pathc; }
	int rc = 0;
	for (int k = 0; k < nfiles; k++) {
		rc |= decode(files[k], 0, 0);	/* timed, nothing read back */
		rc |= decode(files[k], 1, 1);	/* bit-exact check (reads the frames back: slow, untimed), rotate from the last frame */
	}
	printf(rc ? "SPIKE-VDEC FAIL\n" : "SPIKE-VDEC OK\n");
	return rc;
}
