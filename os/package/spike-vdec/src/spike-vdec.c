// SPDX-License-Identifier: MIT
// spike-vdec: R4 hardware spike. H.264 decode on the video engine through the V4L2 stateless (request) API
// (cedrus), with frame rate, CPU use and a bit-exact check against a software decode made at build time;
// plus a CPU estimate of the projection's rotate (NV12 1280x480 -> the portrait panel's BGRA, 90 degrees).
//
// Not a general decoder (Phase 1 needs one): one slice per picture, I and P only (no B), frames only, no
// weighted prediction, no scaling matrices, POC types 0 and 2. Long-term references, their MMCOs (all but 5) and
// list modifications are in (some phones' encoders use them for error resilience). The spike clips are encoded to
// fit (br-r4.sh). Anything else is refused with a message, never decoded wrongly.
//
//   spike-vdec                   every /usr/share/spike/*.h264: timed decode, verify, then the rotate estimate
//   spike-vdec FILE.h264 ...     the same for these clips (FILE.crc next to it: per-frame CRC-32 reference)
//   spike-vdec --info            the V4L2 video/media devices, their formats (stands in for v4l2-ctl/media-ctl)
//   spike-vdec --parse FILE      parse only (no device): per-slice header facts; runs on a build host too
//   spike-vdec --rotate          the show path's rotate, checked against the plain C and timed (malloc'd memory)
//   spike-vdec --fb [FILE]       decode a clip straight to /dev/fb0 (the panel's projection region), looping
//   spike-vdec --fb-stream [--region X W]
//                                decode a live Annex-B H.264 stream from stdin to the panel (the AA video sink). With
//                                --region, the frame's middle W x 480 window is shown unscaled at landscape x = X..X+W-1
//                                (a smaller frame: its middle window of that shape, scaled up to fill it)
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
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

/* YUV -> RGB in 8.8 fixed point, R = cy (Y - oy) + crv V', G = cy (Y - oy) - cgu U' - cgv V', B = cy (Y - oy) + cbu U':
 * as the SPS's VUI says (limited range and BT.601 when it says nothing; some phones send full-range BT.601) */
static int cy = 298, oy = 16, crv = 409, cgu = 100, cgv = 208, cbu = 516;
static void yuv_coef(int full, int bt709)
{
	static const int k[4][6] = {
		{ 298, 16, 409, 100, 208, 516 },	/* BT.601, limited */
		{ 256,  0, 359,  88, 183, 454 },	/* BT.601, full */
		{ 298, 16, 459,  55, 136, 541 },	/* BT.709, limited */
		{ 256,  0, 403,  48, 120, 475 },	/* BT.709, full */
	};
	const int *c = k[2 * !!bt709 + !!full];
	cy = c[0], oy = c[1], crv = c[2], cgu = c[3], cgv = c[4], cbu = c[5];
}

static void parse_sps(br_t *b)
{
	memset(&sps, 0, sizeof sps);
	sps.profile_idc = u(b, 8); sps.constraint_set_flags = u(b, 8); sps.level_idc = u(b, 8);
	unsigned id = ue(b);	/* each value range-checked before a narrow field holds it (stored, 256 would read as 0) */
	if (id > 31) die("SPS id %u > 31", id);
	sps.seq_parameter_set_id = id;
	sps.chroma_format_idc = 1;
	switch (sps.profile_idc) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
		if (ue(b) != 1) die("only 4:2:0");	/* (chroma_format_idc 1 as set above) */
		if (ue(b) || ue(b)) die("only 8-bit");	/* bit_depth_luma_minus8, bit_depth_chroma_minus8 (left 0) */
		if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
		if (u(b, 1)) die("SPS scaling matrices: not in this spike decoder");
	}
	/* The stream comes from the phone (or whoever reached the AA socket): every value is checked against the
	 * H.264 limits and our buffers before it is used, and a bad one ends the decoder (aa then ends the session). */
	unsigned v;
	if ((v = ue(b)) > 12) die("SPS log2_max_frame_num_minus4 %u > 12", v);
	sps.log2_max_frame_num_minus4 = v;
	if ((v = ue(b)) > 2) die("SPS pic_order_cnt_type %u", v);
	sps.pic_order_cnt_type = v;
	if (sps.pic_order_cnt_type == 0) {
		if ((v = ue(b)) > 12) die("SPS log2_max_pic_order_cnt_lsb_minus4 %u > 12", v);
		sps.log2_max_pic_order_cnt_lsb_minus4 = v;
	} else if (sps.pic_order_cnt_type == 1) die("POC type 1: not in this spike decoder");
	else if (sps.pic_order_cnt_type != 2) die("SPS pic_order_cnt_type %u", sps.pic_order_cnt_type);
	if ((v = ue(b)) > 16) die("SPS max_num_ref_frames %u > 16", v);   /* refs[] and the DPB hold 16 */
	sps.max_num_ref_frames = v;
	if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	unsigned wm = ue(b), hm = ue(b);
	if (wm >= 1920 / 16 || hm >= 1088 / 16) die("SPS %ux%u macroblocks: larger than 1920x1088", wm + 1, hm + 1);
	sps.pic_width_in_mbs_minus1 = wm; sps.pic_height_in_map_units_minus1 = hm;
	if (!u(b, 1)) die("interlaced: not in this spike decoder");
	sps.flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (u(b, 1)) sps.flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	unsigned cl = 0, cr = 0, ct = 0, cb = 0, fw = 16 * (wm + 1), fh = 16 * (hm + 1);
	if (u(b, 1)) { cl = ue(b); cr = ue(b); ct = ue(b); cb = ue(b); }
	/* each offset alone below the frame first (no overflow in the sum), then the window non-empty */
	if (cl >= fw || cr >= fw || ct >= fh || cb >= fh || 2 * (cl + cr) >= fw || 2 * (ct + cb) >= fh) die("SPS crop outside the frame");
	crop_x = (int)(2 * cl); crop_y = (int)(2 * ct);
	crop_w = (int)(fw - 2 * (cl + cr)); crop_h = (int)(fh - 2 * (ct + cb));
	int full = 0, bt709 = 0;
	if (u(b, 1)) {	/* vui_parameters_present_flag: read up to the video signal type */
		if (u(b, 1) && u(b, 8) == 255) u(b, 16), u(b, 16);	/* aspect_ratio_idc, Extended_SAR's width and height */
		if (u(b, 1)) u(b, 1);					/* overscan_appropriate_flag */
		if (u(b, 1)) {						/* video_signal_type_present_flag */
			u(b, 3); full = u(b, 1);			/* video_format, video_full_range_flag */
			if (u(b, 1)) { u(b, 8); u(b, 8); bt709 = u(b, 8) == 1; }	/* primaries, transfer, matrix_coefficients */
		}
	}
	yuv_coef(full, bt709);
	have_sps = 1;
}

static void parse_pps(br_t *b)
{
	memset(&pps, 0, sizeof pps);
	unsigned pid = ue(b), sid = ue(b), v, l0, l1;	/* range-checked before the narrow fields hold them */
	if (pid > 255 || sid > 31) die("PPS id %u / SPS id %u out of range", pid, sid);
	pps.pic_parameter_set_id = pid; pps.seq_parameter_set_id = sid;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	if ((v = ue(b))) die("slice groups: not in this spike decoder");
	l0 = ue(b); l1 = ue(b);
	if (l0 > 31 || l1 > 31) die("PPS num_ref_idx > 32");
	pps.num_ref_idx_l0_default_active_minus1 = l0; pps.num_ref_idx_l1_default_active_minus1 = l1;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	if ((pps.weighted_bipred_idc = u(b, 2)) > 2) die("PPS weighted_bipred_idc 3");
	int qp = se(b), qs = se(b), cq = se(b);	/* the spec's ranges, checked before the narrow fields hold them */
	if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || cq < -12 || cq > 12) die("PPS QP %d / %d / %d out of range", qp, qs, cq);
	pps.pic_init_qp_minus26 = qp; pps.pic_init_qs_minus26 = qs; pps.chroma_qp_index_offset = cq;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
	if (more_rbsp_data(b)) {
		if (u(b, 1)) pps.flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
		if (u(b, 1)) die("PPS scaling matrices: not in this spike decoder");
		if ((cq = se(b)) < -12 || cq > 12) die("PPS second_chroma_qp_index_offset %d out of range", cq);
		pps.second_chroma_qp_index_offset = cq;
	}
	have_pps = 1;
}

typedef struct {
	struct v4l2_ctrl_h264_slice_params sl;
	struct v4l2_ctrl_h264_decode_params dp;
	int idr, ref, redundant;	/* redundant: a copy (redundant_pic_cnt > 0), skipped */
	int lt;				/* an IDR's long_term_reference_flag */
	int adaptive, nmmco;		/* adaptive_ref_pic_marking_mode_flag and its operations (5 is refused) */
	struct { unsigned op, a, c; } mmco[32];	/* a: difference_of_pic_nums_minus1, long_term_pic_num or
						 * max_long_term_frame_idx_plus1; c: long_term_frame_idx */
	int nmod;			/* ref_pic_list_modification, list 0 */
	struct { unsigned idc, v; } mod[33];	/* v: abs_diff_pic_num_minus1 (idc 0, 1) or long_term_pic_num (2) */
} pic_t;

static void parse_slice(br_t *b, int nal_type, int ref_idc, pic_t *p)
{
	memset(p, 0, sizeof *p);
	p->idr = nal_type == 5; p->ref = ref_idc != 0;
	if (p->idr && !p->ref) die("an IDR with nal_ref_idc 0");
	b->pos = 8;	/* header_bit_size counts from the NAL header byte (the buffer starts there: START_CODE_NONE) */
	if ((p->sl.first_mb_in_slice = ue(b))) die("several slices per picture: not in this spike decoder");
	unsigned t = ue(b);
	if (t > 9) die("slice_type %u", t);
	t %= 5;
	if (t != V4L2_H264_SLICE_TYPE_P && t != V4L2_H264_SLICE_TYPE_I) die("slice type %u: only I and P here", t);
	p->sl.slice_type = t;
	if (ue(b) != pps.pic_parameter_set_id) die("a second PPS: not in this spike decoder");
	if (pps.seq_parameter_set_id != sps.seq_parameter_set_id) die("the PPS names SPS %u, the stream's is %u: not in this spike decoder",
		pps.seq_parameter_set_id, sps.seq_parameter_set_id);
	p->dp.frame_num = u(b, sps.log2_max_frame_num_minus4 + 4);
	if (p->idr) { unsigned id = ue(b); if (id > 65535) die("idr_pic_id %u > 65535", id); p->dp.idr_pic_id = id; }
	size_t poc0 = b->pos;
	if (sps.pic_order_cnt_type == 0) {
		p->dp.pic_order_cnt_lsb = u(b, sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) p->dp.delta_pic_order_cnt_bottom = se(b);
	}
	p->dp.pic_order_cnt_bit_size = b->pos - poc0;
	if ((pps.flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT) && ue(b)) {	/* a redundant copy: the primary picture */
		p->redundant = 1; return;	/* never goes missing over TCP, so the copy is skipped, as the spec allows */
	}
	p->sl.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
	if (t == V4L2_H264_SLICE_TYPE_P) {
		if (u(b, 1)) { unsigned n = ue(b); if (n > 31) die("slice num_ref_idx > 32"); p->sl.num_ref_idx_l0_active_minus1 = n; }
		if (p->sl.num_ref_idx_l0_active_minus1 > 15) die("slice num_ref_idx %u > 16: frames take 16 at most", p->sl.num_ref_idx_l0_active_minus1 + 1);
		if (u(b, 1)) for (;;) {	/* ref_pic_list_modification_flag_l0: at most one per active reference */
			unsigned idc = ue(b);
			if (idc == 3) break;
			if (idc > 2) die("modification_of_pic_nums_idc %u", idc);
			if (p->nmod > (int)p->sl.num_ref_idx_l0_active_minus1) die("more list modifications than references");
			unsigned v = ue(b);	/* abs_diff_pic_num_minus1 < MaxPicNum; a frame's LongTermPicNum <= 15 */
			if (idc < 2 ? v >= 1u << (sps.log2_max_frame_num_minus4 + 4) : v > 15) die("list modification %u: %u out of range", idc, v);
			p->mod[p->nmod].idc = idc, p->mod[p->nmod++].v = v;
		}
		if (pps.flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) die("weighted prediction: not in this spike decoder");
	}
	if (p->ref) {
		size_t m0 = b->pos;
		if (p->idr) { u(b, 1); p->lt = u(b, 1); }	/* no_output_of_prior_pics_flag, long_term_reference_flag */
		else if ((p->adaptive = u(b, 1))) for (;;) {
			unsigned op = ue(b), a = 0, c = 0;
			if (!op) break;
			if (op == 5) die("MMCO 5: not in this spike decoder");
			if (op > 6) die("MMCO %u", op);
			if (p->nmmco == 32) die("more than 32 MMCOs in a slice");
			if (op != 6) a = ue(b);			/* 1, 3: difference_of_pic_nums_minus1; 2: long_term_pic_num; 4: max + 1 */
			if (op == 3 || op == 6) c = ue(b);	/* long_term_frame_idx */
			if ((op == 1 || op == 3) && a >= 1u << (sps.log2_max_frame_num_minus4 + 4)) die("MMCO %u: difference %u out of range", op, a);
			if ((op == 2 && a > 15) || (op == 4 && a > 16) || c > 15) die("MMCO %u: %u/%u out of range", op, a, c);
			p->mmco[p->nmmco].op = op, p->mmco[p->nmmco].a = a, p->mmco[p->nmmco++].c = c;
		}
		p->dp.dec_ref_pic_marking_bit_size = b->pos - m0;
	}
	if ((pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) && t != V4L2_H264_SLICE_TYPE_I) {
		unsigned ci = ue(b);
		if (ci > 2) die("cabac_init_idc %u > 2", ci);
		p->sl.cabac_init_idc = ci;
	}
	int qd = se(b);
	/* SliceQPY 0..51; the delta's own bound first, so the sum cannot overflow */
	if (qd < -51 || qd > 51 || 26 + pps.pic_init_qp_minus26 + qd < 0 || 26 + pps.pic_init_qp_minus26 + qd > 51)
		die("slice_qp_delta %d out of range", qd);
	p->sl.slice_qp_delta = qd;
	if (pps.flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
		unsigned dd = ue(b);
		if (dd > 2) die("disable_deblocking_filter_idc %u > 2", dd);
		p->sl.disable_deblocking_filter_idc = dd;
		if (p->sl.disable_deblocking_filter_idc != 1) {
			int al = se(b), be = se(b);
			if (al < -6 || al > 6 || be < -6 || be > 6) die("deblocking offsets %d / %d out of range", al, be);
			p->sl.slice_alpha_c0_offset_div2 = al; p->sl.slice_beta_offset_div2 = be;
		}
	}
	if (b->pos >= b->n * 8) die("slice header runs past the slice's data");	/* the engine would skip bits that are not there */
	p->sl.header_bit_size = b->pos;
	p->dp.nal_ref_idc = ref_idc;
	if (p->idr) p->dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	if (t == V4L2_H264_SLICE_TYPE_P) p->dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
}

/* ---- POC and the reference frames: short-term and long-term (8.2.4, 8.2.5; frames only) ---- */
typedef struct { uint64_t ts; int frame_num, poc, cap, lt, ltfi; } ref_t;	/* lt: long-term, at LongTermFrameIdx ltfi */
static ref_t refs[17];	/* up to 16, and the picture just decoded until the list is trimmed */
static int nrefs, max_ltfi = -1, prev_frame_num, frame_num_offset, prev_poc_msb, prev_poc_lsb;	/* max_ltfi -1: none */
static int prev_ref_fn = -1;	/* PrevRefFrameNum; -1 before the stream's first reference picture */

static void gap_frames(int fn);
static int calc_poc(pic_t *p)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4);
	if (p->idr) { prev_frame_num = 0; frame_num_offset = 0; prev_poc_msb = prev_poc_lsb = 0; nrefs = 0; }
	/* frames only, MMCO 5 refused: each picture after a reference one carries its frame_num + 1 (a non-reference one
	 * repeats it for the next). A gap the SPS allows gets its "non-existing" frames (gap_frames); a gap it does not
	 * allow is a picture lost, and a repeat is wrong: both refused rather than decoded wrongly */
	if (!p->idr && prev_ref_fn >= 0 && p->dp.frame_num != (unsigned)((prev_ref_fn + 1) % max_fn)) {
		if (p->dp.frame_num == (unsigned)prev_ref_fn || !(sps.flags & V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED))
			die("frame_num %u after %d: a repeat, or a gap the SPS does not allow", p->dp.frame_num, prev_ref_fn);
		gap_frames((int)p->dp.frame_num);
	}
	if (p->ref) prev_ref_fn = p->dp.frame_num;
	if (sps.pic_order_cnt_type == 0) {
		int max_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4), lsb = p->dp.pic_order_cnt_lsb, msb;
		if (lsb < prev_poc_lsb && prev_poc_lsb - lsb >= max_lsb / 2) msb = prev_poc_msb + max_lsb;
		else if (lsb > prev_poc_lsb && lsb - prev_poc_lsb > max_lsb / 2) msb = prev_poc_msb - max_lsb;
		else msb = prev_poc_msb;
		if (msb > (1 << 28) || msb < -(1 << 28)) die("POC past 2^28 (no IDR for that long): not in this spike decoder");
		if (p->ref) { prev_poc_msb = msb; prev_poc_lsb = lsb; }
		prev_frame_num = p->dp.frame_num;
		return msb + lsb;
	}
	if (!p->idr && prev_frame_num > p->dp.frame_num) frame_num_offset += max_fn;
	if (frame_num_offset > (1 << 28)) die("POC past 2^28 (no IDR for that long): not in this spike decoder");
	prev_frame_num = p->dp.frame_num;
	return p->idr ? 0 : 2 * (frame_num_offset + p->dp.frame_num) - !p->ref;
}

/* PicNum (FrameNumWrap) of a short-term frame, or LongTermPicNum (= LongTermFrameIdx) of a long-term one, as seen
 * from the picture with frame_num cur */
static int pic_num(const ref_t *r, int cur)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4);
	return r->lt ? r->ltfi : r->frame_num > cur ? r->frame_num - max_fn : r->frame_num;
}
static int find_ref(int lt, int num, int cur) { for (int i = 0; i < nrefs; i++) if (refs[i].lt == lt && pic_num(&refs[i], cur) == num) return i; return -1; }
static void unmark(int i) { if (i >= 0) refs[i] = refs[--nrefs]; }

/* the sliding window (8.2.5.3), once a frame joined the list: while it holds more than max_num_ref_frames, the oldest
 * short-term frame (the smallest FrameNumWrap, seen from frame_num cur) goes; with none, a long-term one */
static void slide(int cur)
{
	int max = sps.max_num_ref_frames ? sps.max_num_ref_frames : 1;
	if (max > 16) max = 16;
	while (nrefs > max) {
		int lo = -1;
		for (int i = 0; i < nrefs; i++) if (!refs[i].lt && (lo < 0 || pic_num(&refs[i], cur) < pic_num(&refs[lo], cur))) lo = i;
		if (lo < 0) for (int i = 0; i < nrefs; i++) if (lo < 0 || refs[i].ltfi < refs[lo].ltfi) lo = i;
		unmark(lo);
	}
}

/* A frame_num gap the SPS allows (8.2.5.2): the frames it skipped are "non-existing", with no picture, yet each takes
 * its place in the sliding window as in the encoder, so the frames kept, and their places in the list, stay the
 * encoder's (one that drops pictures goes on from a long-term frame). In the DPB they are not valid, so the engine
 * skips them; a conforming stream never refers to one. */
static void gap_frames(int fn)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4);
	for (int f = (prev_ref_fn + 1) % max_fn; f != fn; f = (f + 1) % max_fn) {
		refs[nrefs++] = (ref_t){ 0, f, 0, -1, 0, 0 };	/* cap -1: no picture */
		slide(f);
		prev_ref_fn = f;
	}
}

/* fills dpb[] and ref_pic_list0 for this picture: the initial list (8.2.4.2.1: short-term by PicNum descending, then
 * long-term by LongTermPicNum ascending), then the slice's modifications (8.2.4.3) */
static void fill_refs(pic_t *p)
{
	int max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4), cur = p->dp.frame_num, list[34], n = 0;
	int real = 0;	/* the frames with a picture (not "non-existing" ones) */
	for (int i = 0; i < nrefs; i++) {
		if (!refs[i].lt && refs[i].frame_num == cur) die("a short-term frame already has frame_num %d (kept for MaxFrameNum pictures)", cur);
		real += refs[i].cap >= 0;
		struct v4l2_h264_dpb_entry *e = &p->dp.dpb[i];
		e->reference_ts = refs[i].ts;
		e->frame_num = refs[i].lt ? refs[i].ltfi : refs[i].frame_num;	/* V4L2: long_term_frame_idx for a long-term one */
		e->pic_num = pic_num(&refs[i], cur);
		e->fields = V4L2_H264_FRAME_REF; e->top_field_order_cnt = e->bottom_field_order_cnt = refs[i].poc;
		e->flags = refs[i].cap < 0 ? 0 : V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |	/* non-existing: */
			   (refs[i].lt ? V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0);	/* not valid, it has no picture */
		int k = n++, pn = pic_num(&refs[i], cur);
		while (k > 0) {	/* before list[k - 1]? short-term first, by PicNum down; then long-term, by number up */
			const ref_t *o = &refs[list[k - 1]];
			if (!(refs[i].lt < o->lt || (refs[i].lt == o->lt && (refs[i].lt ? pn < pic_num(o, cur) : pn > pic_num(o, cur))))) break;
			list[k] = list[k - 1]; k--;
		}
		list[k] = i;
	}
	if (p->sl.slice_type != V4L2_H264_SLICE_TYPE_P) return;
	if (!real) die("P picture with no reference");
	int num = p->sl.num_ref_idx_l0_active_minus1 + 1, pred = cur;	/* picNumL0Pred starts at CurrPicNum */
	for (int k = n; k <= num; k++) list[k] = -1;	/* "no reference picture" */
	for (int m = 0; m < p->nmod; m++) {
		int want;
		if (p->mod[m].idc < 2) {	/* a short-term frame, by its PicNum's difference to the previous one */
			int d = (int)p->mod[m].v + 1;
			if (d > max_fn) die("abs_diff_pic_num_minus1 %u past MaxPicNum", p->mod[m].v);
			pred = p->mod[m].idc == 0 ? (pred - d < 0 ? pred - d + max_fn : pred - d) : (pred + d >= max_fn ? pred + d - max_fn : pred + d);
			want = find_ref(0, pred > cur ? pred - max_fn : pred, cur);
		} else want = find_ref(1, (int)p->mod[m].v, cur);	/* a long-term frame, by its LongTermPicNum */
		if (want < 0) die("list modification names a frame not held (a picture lost before this one)");
		for (int k = num; k > m; k--) list[k] = list[k - 1];	/* in at m, the rest one down, its later copy out */
		list[m] = want;
		int w = m + 1;
		for (int k = m + 1; k <= num; k++) if (list[k] != want) list[w++] = list[k];
	}
	int first = -1;	/* the first frame in the list with a picture: what an empty or "non-existing" place names, so the */
	for (int k = 0; k < n && first < 0; k++) if (refs[list[k]].cap >= 0) first = list[k];	/* engine never reads none */
	for (int k = 0; k < num; k++) {
		p->sl.ref_pic_list0[k].index = list[k] >= 0 && refs[list[k]].cap >= 0 ? list[k] : first;
		p->sl.ref_pic_list0[k].fields = V4L2_H264_FRAME_REF;
	}
}

/* the reference marking after a picture (8.2.5): an IDR as short- or long-term; otherwise the slice's MMCOs, or the
 * sliding window. The window runs after adding the picture here (the same frame goes: the new one has the largest
 * FrameNumWrap), so a stream that marks more than max_num_ref_frames loses its oldest short-term frame, as in ffmpeg */
static void add_ref(pic_t *p, uint64_t ts, int poc, int cap)
{
	int cur = p->dp.frame_num;
	ref_t me = { ts, cur, poc, cap, 0, 0 };
	int keep = 1;	/* the picture stays a reference (an MMCO after its own MMCO 6 may unmark it) */
	if (p->idr) {	/* calc_poc emptied the list */
		if (p->lt) me.lt = 1, max_ltfi = 0; else max_ltfi = -1;
	} else for (int m = 0; m < p->nmmco; m++) {
		int a = (int)p->mmco[m].a, c = (int)p->mmco[m].c, x = cur - (a + 1);	/* x: picNumX (8-39) */
		switch (p->mmco[m].op) {	/* (MMCO 3 and 6 take an index past MaxLongTermFrameIdx, as ffmpeg does: within 15) */
		case 1: unmark(find_ref(0, x, cur)); break;
		case 2:
			if (me.lt && me.ltfi == a) keep = 0, me.lt = 0;	/* this picture, made long-term by an MMCO 6 before */
			else unmark(find_ref(1, a, cur));
			break;
		case 3: {
			if (me.lt && me.ltfi == c) keep = 0, me.lt = 0;	/* the index held by this picture (an MMCO 6 before) */
			else unmark(find_ref(1, c, cur));
			int s = find_ref(0, x, cur);	/* looked up after the unmark, which moves entries */
			if (s >= 0) refs[s].lt = 1, refs[s].ltfi = c;
			break;
		}
		case 4:
			max_ltfi = a - 1;
			for (int i = 0; i < nrefs;) if (refs[i].lt && refs[i].ltfi > max_ltfi) unmark(i); else i++;
			if (me.lt && me.ltfi > max_ltfi) keep = 0, me.lt = 0;
			break;
		case 6:
			unmark(find_ref(1, c, cur));
			me.lt = 1, me.ltfi = c, keep = 1;
			break;
		}
	}
	if (!keep) return;	/* unmarked by its own MMCOs: not a reference after all */
	refs[nrefs++] = me;
	slide(cur);
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
		if (!nals[i].n) continue;	/* a clip that ends in a start code */
		int t = nals[i].p[0] & 0x1f, r = (nals[i].p[0] >> 5) & 3;
		size_t full = t == 7 || t == 8;
		br_t b = { rbsp, unescape(nals[i].p, nals[i].n, rbsp, full ? sizeof rbsp : 512), 8 };
		if (t == 7) parse_sps(&b);
		else if (t == 8) parse_pps(&b);
		else if (t == 1 || t == 5) {
			if (!have_sps || !have_pps) die("slice before SPS/PPS");
			parse_slice(&b, t, r, p); if (p->redundant) continue;
			*out = nals[i]; return i + 1;
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
static void rotate_nv12_bgra(const uint8_t *y, const uint8_t *uv, int stride, int w, int h, uint8_t *dst, long dst_stride,
			     int y0, int y1)
{
	/* landscape (x, y) -> panel (h-1-y, x): dst row x (dst_stride bytes apart), column h-1-y. 16x16 tiles keep the
	 * column writes cached. Rows y0..y1-1 only (even bounds): two threads each take a band of rows, and their writes
	 * never overlap. The coefficients in locals: a store through dst could alias the globals and reload them for every
	 * pixel. The reference for rotate_neon (spike-vdec --rotate checks they agree), and the show path without NEON. */
	const int Cy = cy, Oy = oy, Crv = crv, Cgu = cgu, Cgv = cgv, Cbu = cbu;
	for (int ty = y0; ty < y1; ty += 16)
		for (int tx = 0; tx < w; tx += 16)
			for (int yy = ty; yy < ty + 16 && yy < y1; yy += 2)
				for (int xx = tx; xx < tx + 16 && xx < w; xx += 2) {
					const uint8_t *c = uv + (yy / 2) * stride + xx;
					int d = c[0] - 128, e = c[1] - 128;
					int r = Crv * e + 128, g = -Cgu * d - Cgv * e + 128, b = Cbu * d + 128;
					for (int k = 0; k < 4; k++) {
						int px = xx + (k & 1), py = yy + (k >> 1);
						int l = Cy * (y[py * stride + px] - Oy);
						uint32_t v = 0xff000000u | clip8((l + r) >> 8) << 16 | clip8((l + g) >> 8) << 8 | clip8((l + b) >> 8);
						memcpy(dst + (size_t)px * dst_stride + (size_t)(h - 1 - py) * 4, &v, 4);
					}
				}
}
#if defined(__ARM_NEON)
/* rotate_nv12_bgra with NEON, the same bytes. The A7 is in-order with 32 64-bit registers, so: no block held, no
 * transpose (both spilled): per pair of panel rows (landscape columns x, x+1), lane loads gather their 8 luma pairs
 * (landscape rows ty+7 down to ty: the panel row's order) and the 4 chroma pairs under them, which are converted and
 * stored. 16-bit arithmetic, exact: with off = the chroma term - Cy*Oy split as 256*oh + ol (0 <= ol < 256),
 * (Cy*Y + off + 128) >> 8 = Y + oh + ((Cy-256)*Y + ol + 128) >> 8, for 256 <= Cy < 512. */
static inline void split_off(int32x4_t off, int16x8_t *oh, uint16x8_t *ol)   /* off = 256*oh + ol; each lane twice */
{
	int16x4_t h4 = vshrn_n_s32(off, 8);
	uint16x4_t l4 = vand_u16(vreinterpret_u16_s16(vmovn_s32(off)), vdup_n_u16(0xff));
	int16x4x2_t hz = vzip_s16(h4, h4); uint16x4x2_t lz = vzip_u16(l4, l4);
	*oh = vcombine_s16(hz.val[0], hz.val[1]), *ol = vcombine_u16(lz.val[0], lz.val[1]);
}
static inline uint8x8_t chan(uint8x8_t Y, uint16x8_t yc, int16x8_t oh, uint16x8_t ol)   /* Y + oh + (yc + ol + 128) >> 8, 0..255 */
{
	return vqmovun_s16(vreinterpretq_s16_u16(vaddw_u8(vaddw_u8(vreinterpretq_u16_s16(oh), Y), vraddhn_u16(yc, ol))));
}
static void rotate_neon(const uint8_t *y, const uint8_t *uv, int stride, int w, int h, uint8_t *dst, long dst_stride, int y0, int y1)
{
	const int16x4_t K = { (int16_t)cbu, (int16_t)cgu, (int16_t)cgv, (int16_t)crv };   /* one register, for vmlal_lane */
	const int32x4_t ko = vdupq_n_s32(-cy * oy);
	const uint8x8_t c8 = vdup_n_u8(cy - 256), alpha = vdup_n_u8(0xff);
	const int s2 = stride / 2;
	for (int ty = y0; ty < y1; ty += 8) {
		const uint8_t *yr = y + (ty + 7) * stride, *cr = uv + (ty / 2 + 3) * stride;
		uint8_t *q = dst + (size_t)(h - 8 - ty) * 4;
		for (int x = 0; x < w; x += 2, q += 2 * dst_stride) {
			const uint16_t *p = (const uint16_t *)(yr + x);   /* x, stride and the buffers even: 2-byte aligned */
			uint16x8_t l = vdupq_n_u16(0);
			l = vld1q_lane_u16(p, l, 0), p -= s2; l = vld1q_lane_u16(p, l, 1), p -= s2;
			l = vld1q_lane_u16(p, l, 2), p -= s2; l = vld1q_lane_u16(p, l, 3), p -= s2;
			l = vld1q_lane_u16(p, l, 4), p -= s2; l = vld1q_lane_u16(p, l, 5), p -= s2;
			l = vld1q_lane_u16(p, l, 6), p -= s2; l = vld1q_lane_u16(p, l, 7);
			uint8x8x2_t ab = vuzp_u8(vget_low_u8(vreinterpretq_u8_u16(l)), vget_high_u8(vreinterpretq_u8_u16(l)));   /* x, x+1 */
			const uint16_t *pc = (const uint16_t *)(cr + x);
			uint16x4_t c = vdup_n_u16(0);   /* U | V << 8, chroma rows ty/2+3 down to ty/2 */
			c = vld1_lane_u16(pc, c, 0), pc -= s2; c = vld1_lane_u16(pc, c, 1), pc -= s2;
			c = vld1_lane_u16(pc, c, 2), pc -= s2; c = vld1_lane_u16(pc, c, 3);
			int16x4_t d = vsub_s16(vreinterpret_s16_u16(vand_u16(c, vdup_n_u16(0xff))), vdup_n_s16(128));
			int16x4_t e = vsub_s16(vreinterpret_s16_u16(vshr_n_u16(c, 8)), vdup_n_s16(128));
			uint8x8_t ya = ab.val[0], yb = ab.val[1];
			uint16x8_t ca = vmull_u8(ya, c8), cb = vmull_u8(yb, c8);
			uint8x8x4_t pa, pb; int16x8_t oh; uint16x8_t ol;
			split_off(vmlal_lane_s16(ko, d, K, 0), &oh, &ol);   /* B */
			pa.val[0] = chan(ya, ca, oh, ol), pb.val[0] = chan(yb, cb, oh, ol);
			split_off(vmlsl_lane_s16(vmlsl_lane_s16(ko, d, K, 1), e, K, 2), &oh, &ol);   /* G */
			pa.val[1] = chan(ya, ca, oh, ol), pb.val[1] = chan(yb, cb, oh, ol);
			split_off(vmlal_lane_s16(ko, e, K, 3), &oh, &ol);   /* R */
			pa.val[2] = chan(ya, ca, oh, ol), pb.val[2] = chan(yb, cb, oh, ol);
			pa.val[3] = pb.val[3] = alpha;
			vst4_u8(q, pa), vst4_u8(q + dst_stride, pb);   /* panel rows x, x+1: B, G, R, A interleaved */
		}
	}
}
#endif
/* the show path's rotate: NEON where the CPU has it (even sizes and bands in 8s, as every caller's are) */
static void rotate_band(const uint8_t *y, const uint8_t *uv, int stride, int w, int h, uint8_t *dst, long dst_stride, int y0, int y1)
{
#if defined(__ARM_NEON)
	if (w % 2 == 0 && stride % 2 == 0 && y0 % 8 == 0 && y1 % 8 == 0 && cy >= 256 && cy < 512) {
		rotate_neon(y, uv, stride, w, h, dst, dst_stride, y0, y1);
		return;
	}
#endif
	rotate_nv12_bgra(y, uv, stride, w, h, dst, dst_stride, y0, y1);
}
/* A frame smaller than the region (Android Auto's 480p): its window (sw x sh) scaled up, nearest neighbour, into an NV12
 * picture of the region's size (w x h, stride w), output rows y0..y1-1 (even); rotate_band then shows it as it shows a
 * full-size frame. Source column xmap[ox] for output column ox, source row oy * sh / h. A chroma pair per 2x2 output
 * pixels, the one under their top-left pixel. */
static void scale_nv12(const uint8_t *y, const uint8_t *uv, int stride, const uint16_t *xmap, int sh, int w, int h, uint8_t *o,
		       int y0, int y1)
{
	uint8_t *ouv = o + (size_t)w * h;
	for (int oy = y0; oy < y1; oy++) {
		int sy = oy * sh / h;
		uint8_t *d = o + (size_t)oy * w;
		if (oy > y0 && sy == (oy - 1) * sh / h) memcpy(d, d - w, w);   /* the same source row as the row above */
		else for (int ox = 0; ox < w; ox++) d[ox] = y[(size_t)sy * stride + xmap[ox]];
		if (oy & 1) continue;
		const uint8_t *c = uv + (size_t)(sy / 2) * stride;
		uint8_t *dc = ouv + (size_t)(oy / 2) * w;
		for (int ox = 0; ox < w; ox += 2) { int sx = xmap[ox] & ~1; dc[ox] = c[sx], dc[ox + 1] = c[sx + 1]; }
	}
}
static void rotate_bench(const char *what, const uint8_t *src, int stride, int w, int h, int plain)
{
	uint8_t *dst = malloc((size_t)w * h * 4); if (!dst) die("out of memory");
	int n = 30; double t = now(), c = cpu_self();
	for (int i = 0; i < n; i++) (plain ? rotate_nv12_bgra : rotate_band)(src, src + (size_t)stride * h, stride, w, h, dst, h * 4L, 0, h);
	t = now() - t; c = cpu_self() - c;
	printf("ROTATE %s%s %dx%d NV12 -> %dx%d BGRA: %.1f ms/frame = %.1f fps on one core (CPU %.0f%% of one core)\n",
	       what, plain ? ", plain C" : "", w, h, h, w, t * 1000 / n, n / t, 100 * c / t);
	free(dst);
}

/* ---- framebuffer output (spike --fb: decoded+rotated frames into the panel's projection region) ----
 * rotate_nv12_bgra gives a panel-native buffer: row = landscape x, col = 479 - landscape y (= LVGL's 270 rotation).
 * The left-2/3 video region is landscape x 0..w-1 -> physical rows 0..w-1, each row = h pixels (= panel width). */
#define RGN_W 1280   /* the projection region: left 2/3 of the 1920-wide landscape canvas */
#define RGN_H 480
static int rgn_x = 0, rgn_w = RGN_W;   /* --region: where on the canvas, and how wide (height is always RGN_H) */
static int fb_fd = -1; static uint8_t *fb_mem = NULL; static long fb_line = 0, fb_len = 0;
static uint8_t *fb_scaled = NULL;   /* a smaller frame scaled up to the region's size (NV12), for the rotate */
static uint16_t xmap[1920];         /* its source column for each of the region's columns (fb_show sets it) */
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
/* ---- decode ---- */
#define NOUT 2
#define MAXCAP 20
struct buf { void *p; size_t n; };

/* persistent decoder state, shared by the file path (decode) and the live-stream path (decode_stream) */
typedef struct {
	int vfd, mfd, rq, ncap, stride, ah, frames, errors;
	struct buf out[NOUT], cap[MAXCAP];
	uint8_t *fb_cram;	/* an uncached picture's copy in cached RAM, for the rotate */
	int cached;		/* the capture buffers are cached (the kernel took V4L2_MEMORY_FLAG_NON_COHERENT): shown from in place */
	int show_each;		/* the file path: show every picture. The live stream paces them (pend, show_next) */
	int cw, ch;		/* the coded size the engine was set up for: a later SPS must not change it */
	int last_ci;		/* the capture buffer fb_show shows next, or -1 */
	int pend[2], npend;	/* the live stream's pictures decoded and not shown yet, oldest first (decode_stream paces them) */
	int newest_ci;		/* the newest picture's, shown or not (shown again when the UI brings the picture back) */
	int shown; double show_s;	/* stats: pictures shown and the time spent showing them */
} dec_t;

/* open the video engine and set it up from the current (already-parsed) global sps */
static void dec_setup(dec_t *D, int show_each)
{
	memset(D, 0, sizeof *D);
	D->show_each = show_each; D->last_ci = D->newest_ci = -1;
	char vname[64], mname[64];
	D->vfd = find_dev("/dev/video*", 0, vname); D->mfd = find_dev("/dev/media*", 1, mname);
	if (D->vfd < 0 || D->mfd < 0) die("no cedrus video/media device (spike-vdec --info)");
	int cw = 16 * (sps.pic_width_in_mbs_minus1 + 1), ch = 16 * (sps.pic_height_in_map_units_minus1 + 1);
	D->cw = cw; D->ch = ch;
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
	if ((int)fc.fmt.pix.width < cw || (int)fc.fmt.pix.height < ch || (int)fc.fmt.pix.bytesperline < cw) die("capture %ux%u < the coded %dx%d", fc.fmt.pix.width, fc.fmt.pix.height, cw, ch);
	D->stride = fc.fmt.pix.bytesperline; D->ah = fc.fmt.pix.height;
	D->ncap = sps.max_num_ref_frames + 3; if (D->ncap < 4) D->ncap = 4; if (D->ncap > MAXCAP) D->ncap = MAXCAP;
	struct v4l2_requestbuffers rb = { .count = NOUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(D->vfd, VIDIOC_REQBUFS, &rb) || rb.count < NOUT) die("REQBUFS output: %s", strerror(errno));
	if (!(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) die("the output queue has no request support");
	/* cached pictures where the kernel allows it (R4's cedrus patch): uncached, the CPU reads them at ~100 MB/s */
	rb = (struct v4l2_requestbuffers){ .count = D->ncap, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
					   .flags = V4L2_MEMORY_FLAG_NON_COHERENT };
	if (ioctl(D->vfd, VIDIOC_REQBUFS, &rb) || (int)rb.count < D->ncap) die("REQBUFS capture (%d buffers; CMA too small?): %s", D->ncap, strerror(errno));
	D->cached = !!(rb.flags & V4L2_MEMORY_FLAG_NON_COHERENT);
	for (int k = 0; k < NOUT + D->ncap; k++) {
		int isout = k < NOUT, i = isout ? k : k - NOUT;
		struct v4l2_buffer b = { .type = isout ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(D->vfd, VIDIOC_QUERYBUF, &b)) die("QUERYBUF: %s", strerror(errno));
		if (!isout && b.length < (size_t)D->stride * D->ah * 3 / 2) die("capture buffer of %u bytes: the NV12 picture needs %d", b.length, D->stride * D->ah * 3 / 2);
		struct buf *x = isout ? &D->out[i] : &D->cap[i];
		x->n = b.length; x->p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, D->vfd, b.m.offset);
		if (x->p == MAP_FAILED) die("mmap: %s", strerror(errno));
	}
	int ty[2] = { V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_BUF_TYPE_VIDEO_CAPTURE };
	if (ioctl(D->vfd, VIDIOC_STREAMON, &ty[0]) || ioctl(D->vfd, VIDIOC_STREAMON, &ty[1])) die("STREAMON: %s", strerror(errno));
	if (ioctl(D->mfd, MEDIA_IOC_REQUEST_ALLOC, &D->rq)) die("MEDIA_IOC_REQUEST_ALLOC: %s", strerror(errno));
	if (fb_fd >= 0) {
		if ((long)(rgn_x + rgn_w) * fb_line > fb_len || RGN_H * 4L > fb_line) die("the region is off /dev/fb0");   /* band_show writes there */
		fb_scaled = realloc(fb_scaled, (size_t)rgn_w * RGN_H * 3 / 2);   /* the region, not the source size */
		if (!D->cached) D->fb_cram = malloc((size_t)D->stride * D->ah * 3 / 2);
		if (!fb_scaled || (!D->cached && !D->fb_cram)) die("out of memory (fb buffers)");
	}
	fprintf(stderr, "spike-vdec: %d pictures, %s\n", D->ncap, D->cached ? "cached" : "uncached (copied out to show)");
}

/* show a decoded picture (last_ci) on the panel: rotate the rows the region needs into panel order, straight into the
 * framebuffer, from the capture buffer (copied out to cached RAM first when it is uncached). About 6 ms, with the
 * decode itself ~3 ms in hardware: the live stream shows each picture, paced (show_next), and drops the oldest waiting
 * only when it falls behind. */
/* one band of the region's rows: rotate them from the picture; an uncached one's rows (and their chroma rows) copied
 * out to cached RAM first */
typedef struct { const dec_t *D; const uint8_t *cy, *cuv; uint8_t *yy, *uv; int sx, sy, y0, y1; } band_t;
static void *band_show(void *arg)
{
	const band_t *b = arg; size_t st = (size_t)b->D->stride;
	const uint8_t *y = b->cy, *uv = b->cuv;
	if (!b->D->cached) {
		memcpy(b->yy + (b->sy + b->y0) * st, b->cy + (b->sy + b->y0) * st, (size_t)(b->y1 - b->y0) * st);
		memcpy(b->uv + (b->sy + b->y0) / 2 * st, b->cuv + (b->sy + b->y0) / 2 * st, (size_t)(b->y1 - b->y0) / 2 * st);
		y = b->yy, uv = b->uv;
	}
	rotate_band(y + b->sy * st + b->sx, uv + b->sy / 2 * st + b->sx, (int)st, rgn_w, RGN_H, fb_mem + (size_t)rgn_x * fb_line,
		    fb_line, b->y0, b->y1);   /* dec_setup checked the region fits */
	return NULL;
}
typedef struct { const uint8_t *y, *uv; int stride, sh, y0, y1; } scale_t;
static void *scale_band(void *arg)   /* one band of the region's rows: scaled up, then rotated as band_show does */
{
	const scale_t *s = arg;
	scale_nv12(s->y, s->uv, s->stride, xmap, s->sh, rgn_w, RGN_H, fb_scaled, s->y0, s->y1);
	rotate_band(fb_scaled, fb_scaled + (size_t)rgn_w * RGN_H, rgn_w, rgn_w, RGN_H, fb_mem + (size_t)rgn_x * fb_line, fb_line,
		    s->y0, s->y1);
	return NULL;
}
static void fb_show(dec_t *D)
{
	if (D->last_ci < 0) return;
	double t = now();
	const uint8_t *cy = D->cap[D->last_ci].p, *cuv = cy + (size_t)D->stride * D->ah;
	uint8_t *yy = D->fb_cram, *uv = D->cached ? NULL : D->fb_cram + (size_t)D->stride * D->ah;
	if (crop_w >= rgn_w && crop_h >= RGN_H) {   /* the region's window from the frame's middle, unscaled */
		int sx = ((crop_w - rgn_w) / 2 + crop_x) & ~1, sy = ((crop_h - RGN_H) / 2 + crop_y) & ~1;   /* even: NV12 */
		if (sx < 0 || sy < 0 || sx + rgn_w > D->stride || sy + RGN_H > D->ah) { D->last_ci = -1; return; }   /* parse_sps bounds it: belt and braces */
		/* both cores (the T113 has two): the top band on a thread, the bottom one here. RGN_H / 2 = 240 keeps both
		 * bands even and tile-aligned. If the thread cannot start, this thread does both bands. */
		band_t top = { D, cy, cuv, yy, uv, sx, sy, 0, RGN_H / 2 }, bot = { D, cy, cuv, yy, uv, sx, sy, RGN_H / 2, RGN_H };
		pthread_t th; int threaded = pthread_create(&th, NULL, band_show, &top) == 0;
		band_show(&bot);
		if (threaded) pthread_join(th, NULL); else band_show(&top);
	} else {   /* a smaller frame (AA's lower setting: 480p): its middle window of the region's shape (where the phone
	            * lays its UI out, aa.c), scaled up to fill the region without stretching; both cores, a band each */
		if (crop_w > D->stride || crop_h > D->ah) { D->last_ci = -1; return; }
		int sw = crop_w, sh = (int)(((int64_t)crop_w * RGN_H + rgn_w - 1) / rgn_w + 1) & ~1;   /* up to even: 800 -> 332, aa.c's UI area */
		if (sh > crop_h) sh = crop_h & ~1, sw = (int)((int64_t)crop_h * rgn_w / RGN_H) & ~1;   /* a wider frame: its middle columns */
		if (sw < 2 || sh < 2) { D->last_ci = -1; return; }
		int sx = (crop_x + (crop_w - sw) / 2) & ~1, sy = (crop_y + (crop_h - sh) / 2) & ~1;   /* even: 4:2:0 */
		const uint8_t *sy0 = cy, *suv = cuv;
		if (!D->cached) memcpy(D->fb_cram, D->cap[D->last_ci].p, (size_t)D->stride * D->ah * 3 / 2), sy0 = yy, suv = uv;
		for (int ox = 0; ox < rgn_w; ox++) xmap[ox] = ox * sw / rgn_w;
		scale_t top = { sy0 + (size_t)sy * D->stride + sx, suv + (size_t)(sy / 2) * D->stride + sx, D->stride, sh, 0, RGN_H / 2 }, bot = top;
		bot.y0 = RGN_H / 2, bot.y1 = RGN_H;
		pthread_t th; int threaded = pthread_create(&th, NULL, scale_band, &top) == 0;
		scale_band(&bot);
		if (threaded) pthread_join(th, NULL); else scale_band(&top);
	}
	D->last_ci = -1; D->shown++; D->show_s += now() - t;
}

/* decode one already-parsed picture (slice bytes in *s); show it if this path shows every one; returns its capture index */
static int dec_one(dec_t *D, pic_t *p, nal_t *s)
{
	int poc = calc_poc(p);
	p->dp.top_field_order_cnt = p->dp.bottom_field_order_cnt = poc;
	fill_refs(p);
	int ci = -1;	/* a capture buffer that holds no reference */
	for (int k = 0; k < D->ncap && ci < 0; k++) {   /* refs + 2 waiting to be shown + this one fit in ncap (refs + 3) */
		int used = 0; for (int r = 0; r < nrefs; r++) used |= refs[r].cap == k;
		for (int j = 0; j < D->npend; j++) used |= D->pend[j] == k;
		if (!used) ci = k;
	}
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
	int bad = !!(bc.flags & V4L2_BUF_FLAG_ERROR);	/* the engine flagged the picture: counted, not shown live */
	D->errors += bad;
	if (ioctl(D->rq, MEDIA_REQUEST_IOC_REINIT)) die("MEDIA_REQUEST_IOC_REINIT: %s", strerror(errno));
	if (p->ref) add_ref(p, ts, poc, ci);
	D->frames++;
	if (!bad) D->newest_ci = ci;	/* decode order, not POC order: fine for the display (AA sends IPPP, no reorder) */
	if (fb_fd >= 0 && D->show_each) D->last_ci = ci, fb_show(D);
	else if (fb_fd >= 0 && !bad) {   /* the live stream: in the queue to show; when it is full (the screen behind), the oldest is dropped */
		if (D->npend == 2) D->pend[0] = D->pend[1], D->npend = 1;
		D->pend[D->npend++] = ci;
	}
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
	have_sps = have_pps = 0; nrefs = 0; prev_ref_fn = -1;
	pic_t p; nal_t s;
	if (next_pic(0, &p, &s) < 0) die("%s: no picture", file);	/* parse the SPS so dec_setup has the size */
	dec_t D; dec_setup(&D, 1);
	FILE *crc = NULL; char crcname[512];
	if (verify) { snprintf(crcname, sizeof crcname, "%.*s.crc", (int)(strlen(file) - 5), file); crc = fopen(crcname, "r"); }
	unsigned long long b0, a0, b1, a1; cpu_sys(&b0, &a0);
	double t0 = now(), c0s = cpu_self();
	int i = 0, bad = 0, checked = 0, last_cap = 0;
	have_sps = have_pps = 0; nrefs = 0; prev_ref_fn = -1;
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
		rotate_bench("from the decoded buffer (V4L2 mmap)", D.cap[last_cap].p, D.stride, crop_w, crop_h, 0);
		size_t sz = (size_t)D.stride * D.ah * 3 / 2; uint8_t *m = malloc(sz); memcpy(m, D.cap[last_cap].p, sz);
		rotate_bench("from a copy in cached RAM", m, D.stride, crop_w, crop_h, 0); free(m);
	}
	if (crc) fclose(crc);
	int rc = D.errors || bad;
	dec_free(&D);
	free(d);
	return rc;
}

/* ---- live stream: decode an Annex-B H.264 elementary stream (what the AA video channel carries) to the panel ---- */
static int need_idr;	/* an SPS changed mid-stream: its pictures wait for the next IDR (held frames are of the old one) */
static void process_nal(const uint8_t *p, size_t n, dec_t *D, int *set)
{
	if (!n) return;
	int t = p[0] & 0x1f, r = (p[0] >> 5) & 3;
	size_t full = t == 7 || t == 8;
	br_t b = { rbsp, unescape(p, n, rbsp, full ? sizeof rbsp : 512), 8 };
	if (t == 7) {
		struct v4l2_ctrl_h264_sps old = sps; int had = have_sps;
		parse_sps(&b);
		if (had && memcmp(&old, &sps, sizeof sps)) need_idr = 1;
		if (*set && (16 * (sps.pic_width_in_mbs_minus1 + 1) != D->cw || 16 * (sps.pic_height_in_map_units_minus1 + 1) != D->ch))
			die("SPS changed the picture size mid-stream (%dx%d set up)", D->cw, D->ch);   /* the engine's buffers are for it */
		if (*set && (int)sps.max_num_ref_frames + 3 > D->ncap)
			die("SPS asks for %d reference frames mid-stream (%d buffers set up)", (int)sps.max_num_ref_frames, D->ncap);
	} else if (t == 8) parse_pps(&b);
	else if (t == 1 || t == 5) {
		if (!have_sps || !have_pps) return;	/* joined mid-stream: wait for the next SPS/PPS before decoding */
		if (need_idr) { if (t != 5) return; need_idr = 0; }
		if (!*set) { dec_setup(D, 0); *set = 1; }
		pic_t p2; nal_t s = { p, n };
		parse_slice(&b, t, r, &p2);
		if (!p2.redundant) dec_one(D, &p2, &s);
	}
}

/* process every complete NAL in buf[0..len) (one ending where the next start code begins); keep the last, possibly
 * partial, NAL unless at EOF. Returns how many bytes may be dropped from the front. */
static size_t stream_nals(uint8_t *buf, size_t len, dec_t *D, int *set, int eof, size_t *scan)
{
	size_t prev = (size_t)-1, last = (size_t)-1, i = 0;
	/* The caller keeps the pending NAL at buf[0] (its start code) and buf[0..*scan) is already scanned: resume
	 * there instead of rescanning a large NAL on every read (quadratic in its size). */
	if (*scan > 3 && len >= 3 && !buf[0] && !buf[1] && buf[2] == 1) prev = last = 0, i = *scan - 3;
	for (; i + 3 <= len; i++) {
		if (buf[i] || buf[i + 1] || buf[i + 2] != 1) continue;
		if (prev != (size_t)-1) {
			size_t e = i; while (e > prev + 3 && !buf[e - 1]) e--;	/* trim the trailing zeros of the start code */
			process_nal(buf + prev + 3, e - (prev + 3), D, set);
		}
		prev = i; last = i; i += 2;
	}
	*scan = len;
	if (eof && last != (size_t)-1) {
		size_t e = len; while (e > last + 3 && !buf[e - 1]) e--;
		process_nal(buf + last + 3, e - (last + 3), D, set);
		return len;
	}
	return last == (size_t)-1 ? 0 : last;	/* keep from the last start code: its NAL may still be arriving */
}

/* The dash UI raises this while it shows the picture (ui/project_region.h PJ_VISIBLE): over the settings, the car panel
 * or a dash the user went back to, the decoder draws nothing (a tap there must not land on a "map" over other buttons). */
#define PJ_VISIBLE "/run/dash-pj-visible"
#define PJ_FPS "/run/dash-pj-fps"   /* the pictures shown each second, for the dash (ui/project_region.h: under the AA word) */
/* The least time between two pictures shown: about the panel's own frame (its glass shows 30 a second, whatever the
 * display link's 60), so nearly every picture is seen: a backlog drains by the glass skipping about one in ten. WiFi
 * often delivers a clump of them together: shown this far apart rather than all but the newest dropped; a 30 fps stream
 * keeps up with a little room for its jitter. */
#define SHOW_GAP 0.030
static void show_next(dec_t *D, double *last_show_t, int *vis, double *reshow_at)   /* the oldest waiting, in its turn */
{
	if (!D->npend || now() - *last_show_t < SHOW_GAP) return;
	int v = access(PJ_VISIBLE, F_OK) == 0;
	if (v && !*vis) *reshow_at = now() + 0.25;   /* just shown again: drawn now, and again after the UI's repaint */
	*vis = v;
	if (!v) { D->npend = 0; return; }   /* not on screen: nothing to catch up on (newest_ci is shown when it comes back) */
	D->last_ci = D->pend[0], D->pend[0] = D->pend[1], D->npend--;
	*last_show_t = now(), fb_show(D);   /* the gap from this show's start: its own time counts in it */
	if (*reshow_at && *last_show_t >= *reshow_at) *reshow_at = 0;   /* drawn after the UI's repaint: no reshow needed */
}
static int decode_stream(int fd)
{
	have_sps = have_pps = 0; nrefs = 0; prev_ref_fn = -1; need_idr = 0;
	dec_t D; int set = 0;
	size_t cap = 1u << 20, len = 0; uint8_t *buf = malloc(cap);
	if (!buf) die("out of memory");
	size_t scan = 0;
	double next_stat = now() + 5, last_show_t = 0, reshow_at = 0; int last_frames = 0, last_shown = 0, vis = 0; double last_show_s = 0;
	double next_fps = now() + 1; int fps_shown = 0;
	for (;;) {
		if (set && now() >= next_fps) {   /* each second (the loop wakes every 100 ms at least): the pictures shown */
			FILE *f = fopen(PJ_FPS ".new", "w");
			if (f) fprintf(f, "%d\n", D.shown - fps_shown), fclose(f), rename(PJ_FPS ".new", PJ_FPS);
			fps_shown = D.shown, next_fps += 1;
			if (next_fps < now()) next_fps = now() + 1;
		}
		if (cap - len < 65536) {   /* a NAL this large is not a 720p picture: refuse rather than grow without end */
			if (cap >= (16u << 20)) die("over 16 MB without a complete NAL");
			cap *= 2; buf = realloc(buf, cap); if (!buf) die("out of memory");
		}
		struct pollfd in = { .fd = fd, .events = POLLIN };
		int tmo = 100;   /* the loop wakes at least every 100 ms, and in time for the next picture waiting to be shown */
		if (set && D.npend) { double w = last_show_t + SHOW_GAP - now(); tmo = w <= 0 ? 0 : (int)(w * 1000) + 1; }
		if (set && poll(&in, 1, tmo) == 0) {   /* no input now: a picture's turn, or the UI brought the picture back */
			int v = access(PJ_VISIBLE, F_OK) == 0;
			if (v && !vis) reshow_at = now() + 0.25;   /* after the UI's own repaint of the split, which would cover it */
			if (!v) reshow_at = 0;
			if (v && reshow_at && now() >= reshow_at && D.newest_ci >= 0) {
				D.last_ci = D.newest_ci; last_show_t = now(); fb_show(&D); reshow_at = 0, D.npend = 0;
			}
			vis = v;
			show_next(&D, &last_show_t, &vis, &reshow_at);
			continue;
		}
		ssize_t r = read(fd, buf + len, cap - len);
		if (r < 0) { if (errno == EINTR) continue; die("read: %s", strerror(errno)); }
		if (r == 0) {	/* EOF: the last NAL; drawn only where the picture is shown */
			stream_nals(buf, len, &D, &set, 1, &scan);
			if (set && D.npend && access(PJ_VISIBLE, F_OK) == 0) D.last_ci = D.pend[D.npend - 1], fb_show(&D);
			break;
		}
		len += r;
		size_t used = stream_nals(buf, len, &D, &set, 0, &scan);
		if (used) { memmove(buf, buf + used, len - used); len -= used; scan -= used; }
		if (set) show_next(&D, &last_show_t, &vis, &reshow_at);
		if (set && now() >= next_stat) {	/* the live picture's health: decoded vs shown, cost of showing */
			int sh = D.shown - last_shown;
			fprintf(stderr, "spike-vdec: 5 s: %d decoded, %d shown, %.1f ms per show\n", D.frames - last_frames, sh,
				sh ? (D.show_s - last_show_s) * 1000 / sh : 0.0);
			last_frames = D.frames; last_shown = D.shown; last_show_s = D.show_s; next_stat = now() + 5;
		}
	}
	free(buf);
	unlink(PJ_FPS);
	if (set) dec_free(&D);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--info")) return info();
	if (argc > 2 && !strcmp(argv[1], "--parse")) return parse_only(argv[2]);
	if (argc > 1 && !strcmp(argv[1], "--rotate")) {	/* the show path's rotate: checked against the plain C, timed */
		int st = 1280, h = 480, w = 1160, x = 60;   /* AA's window (--region 120 1160) out of a 1280-wide frame */
		size_t n = (size_t)st * h * 3 / 2, o = (size_t)w * h * 4;
		uint8_t *m = malloc(n), *a = malloc(o), *b = malloc(o);
		if (!m || !a || !b) die("out of memory");
		uint32_t r = 1; for (size_t k = 0; k < n; k++) r = r * 1103515245 + 12345, m[k] = r >> 24;   /* every Y, U, V */
		for (int c = 0; c < 4; c++) {   /* BT.601/709, limited/full */
			yuv_coef(c & 1, c >> 1);
			memset(a, 0, o), memset(b, 1, o);
			rotate_nv12_bgra(m + x, m + (size_t)st * h + x, st, w, h, a, h * 4L, 0, h);
			rotate_band(m + x, m + (size_t)st * h + x, st, w, h, b, h * 4L, 0, h / 2);   /* in fb_show's two bands */
			rotate_band(m + x, m + (size_t)st * h + x, st, w, h, b, h * 4L, h / 2, h);
			if (memcmp(a, b, o)) die("--rotate: the show path's rotate differs from the plain C (coefficients %d)", c);
		}
		printf("ROTATE the show path's rotate = the plain C, bit for bit\n");
		int sw = 800, sh = 332;   /* AA's 480p window, scaled up to the region (fb_show's smaller-frame path) */
		for (int ox = 0; ox < w; ox++) xmap[ox] = ox * sw / w;
		scale_nv12(m + x, m + (size_t)st * h + x, st, xmap, sh, w, h, a, 0, h / 2), scale_nv12(m + x, m + (size_t)st * h + x, st, xmap, sh, w, h, a, h / 2, h);
		for (int oy = 0; oy < h; oy++)
			for (int ox = 0; ox < w; ox++) {
				int sx = ox * sw / w, sy = oy * sh / h, cx = (ox & ~1) * sw / w & ~1, cy2 = (oy & ~1) * sh / h / 2;
				const uint8_t *c = m + (size_t)st * h + x + (size_t)cy2 * st + cx + (ox & 1), *o = a + (size_t)w * h + (size_t)(oy / 2) * w + ox;
				if (a[(size_t)oy * w + ox] != m[x + (size_t)sy * st + sx] || *o != *c) die("--rotate: the scale-up picks the wrong pixel at %d,%d", ox, oy);
			}
		printf("ROTATE the scale-up picks its pixels\n");
		yuv_coef(0, 0);
		rotate_bench("from cached RAM (synthetic)", m + x, st, w, h, 1);
		rotate_bench("from cached RAM (synthetic)", m + x, st, w, h, 0);
		free(m), free(a), free(b); return 0;
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
