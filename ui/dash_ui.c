/* The dash in LVGL: the tesla-dash app's look (lib/main.dart, widgets.dart, gauge.dart, glyphs.dart), arranged
 * for the Dash's trapezoid screen (PLAN-merged.md 5.2). Sizes are the app's, in dp, through px() (1.25 px per dp
 * at H = 480); the app's names are kept in comments so each piece can be checked against its widget. The icons
 * and car pictures are the app's own renders (ui/assets/export.sh), the text is Roboto through tiny_ttf. */
#include "dash_ui.h"
#include "gen/assets.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- colours (gauge.dart; Flutter's white tints) ---------- */
#define WHITE 0xFFFFFFFFu
#define W70 0xB3FFFFFFu
#define W60 0x99FFFFFFu
#define W54 0x8AFFFFFFu
#define W38 0x61FFFFFFu
#define W24 0x3DFFFFFFu
#define W12 0x1FFFFFFFu
#define W10 0x1AFFFFFFu
#define BLACK 0xFF000000u
#define ACCENT 0xFF4EA1FFu
#define REGEN 0xFF3DDC97u
#define ALERT 0xFFE23D3Du
#define LIT 0xFF9CD3FFu
#define COLD 0xFF8FD8FFu
#define WARN 0xFFFFB020u
#define AP_BLUE 0xFF0048FFu
#define AP_GREY 0xFF5E5E5Eu

static lv_color_t rgb(uint32_t c) { return lv_color_hex(c & 0xFFFFFF); }
static lv_opa_t opa(uint32_t c) { return c >> 24; }

/* ---------- geometry ---------- */
static struct {
	float W, H, k;
	float ml, mt, mb;       /* the app's padding: 14, 6, 10 dp */
	float chip_area;        /* the app's main row height: the chips keep it (no bottom bar here) */
	float D, cx, cy, r, aw; /* the ring: box side, centre, arcs' radius and width */
	float card_w;
	float pair_top;         /* the battery dial's and the g circle's top, level */
	float cab;              /* the cabin's fit in its column (the car pictures are baked for it) */
	float link_top;         /* the link buttons' row, under the left chips */
} G;

static float px(float dp) { return dp * 1.25f * G.k; }
/* The Dash draws each group bigger than the app (ui/assets/sizes.json; the icons are baked at these sizes). */
#define PC(dp) (px(dp) * UI_S_CHIP)   /* the side chips */
#define PL(dp) (px(dp) * UI_S_LINK)   /* the link buttons, the car panel button */
#define PLI(dp) (px(dp) * UI_S_LIGHTS) /* the lights row */
#define PN(dp) (px(dp) * UI_S_NAV)    /* the route column */
#define PR(dp) (px(dp) * UI_S_RING)   /* the ring's fixed-size parts: unit, gear, cue, assist disc, air outside */
#define PF(dp) (px(dp) * UI_S_FIG)    /* the figures and the charge card */
#define PG(dp) (px(dp) * UI_S_G)      /* the g-meter */
/* The housing hides the screen's top corners: about 115 px each side at the top row, none at the bottom. */
static float inset(float y) { return 115.0f * G.k * (1.0f - y / G.H); }
static int ri(float v) { return (int)lroundf(v); }

/* ---------- fonts: Roboto, as the app (gauge.dart kFont) ---------- */
extern const unsigned char roboto_light[], roboto_regular[], roboto_medium[], roboto_bold[];
extern const size_t roboto_light_size, roboto_regular_size, roboto_medium_size, roboto_bold_size;
enum { LIGHT, REGULAR, MEDIUM, BOLD }; /* w200, w400, w500, w600 and w700 */

static const lv_font_t *font_px(int weight, int size)
{
	static struct { int w, s; lv_font_t *f; } cache[64];
	static int n;
	for (int i = 0; i < n; i++)
		if (cache[i].w == weight && cache[i].s == size) return cache[i].f;
	static const unsigned char *data[] = {roboto_light, roboto_regular, roboto_medium, roboto_bold};
	const size_t sizes[] = {roboto_light_size, roboto_regular_size, roboto_medium_size, roboto_bold_size};
	lv_font_t *f = lv_tiny_ttf_create_data_ex(data[weight], sizes[weight], size, LV_FONT_KERNING_NORMAL, 64);
	if (n < 64) cache[n++] = (typeof(cache[0])){weight, size, f};
	return f;
}
static const lv_font_t *F(int weight, float dp) { return font_px(weight, ri(px(dp))); }
static const lv_font_t *FS(int weight, float dp, float scale) { return font_px(weight, ri(px(dp) * scale)); }
static int ascent(const lv_font_t *f) { return f->line_height - f->base_line; }

/* ---------- object helpers ---------- */
static lv_obj_t *box(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	return o;
}
static void place(lv_obj_t *o, float x, float y, float w, float h)
{
	lv_obj_set_pos(o, ri(x), ri(y));
	lv_obj_set_size(o, ri(w), ri(h));
}
/* Style setters that leave an unchanged object alone: LVGL redraws an object on every style set, and the dash
 * is set 30 times a second. */
#define SET_COLOR(o, get, set, c) do { if (!lv_color_eq(get(o, 0), c)) set(o, c, 0); } while (0)
#define SET_NUM(o, get, set, v) do { if (get(o, 0) != (v)) set(o, v, 0); } while (0)
static void fill(lv_obj_t *o, uint32_t c)
{
	SET_COLOR(o, lv_obj_get_style_bg_color, lv_obj_set_style_bg_color, rgb(c));
	SET_NUM(o, lv_obj_get_style_bg_opa, lv_obj_set_style_bg_opa, opa(c));
}
static void border(lv_obj_t *o, uint32_t c, float w)
{
	SET_COLOR(o, lv_obj_get_style_border_color, lv_obj_set_style_border_color, rgb(c));
	SET_NUM(o, lv_obj_get_style_border_opa, lv_obj_set_style_border_opa, opa(c));
	SET_NUM(o, lv_obj_get_style_border_width, lv_obj_set_style_border_width, w > 0 && w < 1 ? 1 : ri(w));
}
static void align(lv_obj_t *o, lv_align_t a, int32_t x, int32_t y)
{
	SET_NUM(o, lv_obj_get_style_align, lv_obj_set_style_align, a);
	lv_obj_set_pos(o, x, y);
}
static void img_src(lv_obj_t *i, const void *src)
{
	if (lv_image_get_src(i) != src) lv_image_set_src(i, src);
}
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t c)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_remove_style_all(l);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, rgb(c), 0);
	lv_obj_set_style_text_opa(l, opa(c), 0);
	lv_label_set_text(l, "");
	return l;
}
static void text_color(lv_obj_t *l, uint32_t c)
{
	SET_COLOR(l, lv_obj_get_style_text_color, lv_obj_set_style_text_color, rgb(c));
	SET_NUM(l, lv_obj_get_style_text_opa, lv_obj_set_style_text_opa, opa(c));
}
static void set_text(lv_obj_t *l, const char *fmt, ...)
{
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (strcmp(lv_label_get_text(l), buf)) lv_label_set_text(l, buf);
}
static lv_obj_t *icon(lv_obj_t *parent, const lv_image_dsc_t *src, uint32_t c)
{
	lv_obj_t *i = lv_image_create(parent);
	lv_obj_remove_style_all(i);
	img_src(i, src);
	lv_obj_set_style_image_recolor(i, rgb(c), 0);
	lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(i, opa(c), 0);
	return i;
}
static void icon_color(lv_obj_t *i, uint32_t c)
{
	SET_COLOR(i, lv_obj_get_style_image_recolor, lv_obj_set_style_image_recolor, rgb(c));
	SET_NUM(i, lv_obj_get_style_image_opa, lv_obj_set_style_image_opa, opa(c));
}
static void show(lv_obj_t *o, bool on)
{
	if (on == lv_obj_is_hidden(o)) lv_obj_set_hidden(o, !on);
}
/* Places a label by its text's horizontal anchor (0 left, 1 centre, 2 right) and its baseline. */
static void put_text(lv_obj_t *l, int anchor, float x, float baseline, float parent_w)
{
	const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
	float y = baseline - ascent(f);
	if (anchor == 0) align(l, LV_ALIGN_TOP_LEFT, ri(x), ri(y));
	else if (anchor == 1) align(l, LV_ALIGN_TOP_MID, ri(x - parent_w / 2), ri(y));
	else align(l, LV_ALIGN_TOP_RIGHT, ri(x - parent_w), ri(y));
}
/* Where a line of [f] with Flutter's `height` [h] (0: the font's own) puts its baseline, from the line's top. */
static float baseline_in(const lv_font_t *f, float h)
{
	return h > 0 ? h * ascent(f) / 1.1719f : ascent(f); /* Roboto: ascent + descent = 1.1719 em, no line gap */
}

/* ---------- a row of text and icons (Flutter's Row of Text.rich and Icons) ----------
 * Consecutive text parts share a baseline (one Text.rich); each child is centred on the row's height. */
typedef struct {
	const char *text; /* NULL: an icon */
	const lv_font_t *font;
	const lv_image_dsc_t *img;
	uint32_t color;
	float ls;  /* letter spacing, px */
	float gap; /* space before it, px */
} part_t;
#define TXT(f, c, s) ((part_t){.text = (s), .font = (f), .color = (c)})
#define IMG(i, c) ((part_t){.img = (i), .color = (c)})

static void run_set(lv_obj_t *run, const part_t *p, int n)
{
	/* Rebuilt only when what it says changes. */
	char key[256] = "", *k = key;
	for (int i = 0; i < n && k < key + sizeof key - 40; i++)
		k += snprintf(k, key + sizeof key - k, "%s|%p|%p|%x|%g|%g;", p[i].text ? p[i].text : "", (void *)p[i].font,
			      (const void *)p[i].img, (unsigned)p[i].color, p[i].ls, p[i].gap);
	char *old = lv_obj_get_user_data(run);
	if (old && !strcmp(old, key)) return;
	lv_free(old);
	lv_obj_set_user_data(run, lv_strdup(key));
	lv_obj_clean(run);

	float x[16], w[16], h[16], asc = 0, desc = 0, row_h = 0, total = 0;
	for (int i = 0; i < n; i++) {
		total += p[i].gap;
		x[i] = total;
		if (p[i].text) {
			lv_point_t sz;
			lv_text_get_size(&sz, p[i].text, p[i].font, ri(p[i].ls), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			w[i] = sz.x + (strlen(p[i].text) ? ri(p[i].ls) : 0); /* Flutter spaces after the last letter too */
			h[i] = lv_font_get_line_height(p[i].font);
		} else {
			w[i] = p[i].img->header.w;
			h[i] = p[i].img->header.h;
		}
		total += w[i];
	}
	/* Each run of text: its own baseline; the row: as tall as its tallest child. */
	float group_base[16], group_h[16];
	for (int i = 0; i < n;) {
		int j = i;
		asc = desc = 0;
		if (p[i].text)
			for (; j < n && p[j].text; j++) {
				asc = fmaxf(asc, ascent(p[j].font));
				desc = fmaxf(desc, p[j].font->base_line);
			}
		else
			j = i + 1;
		for (int m = i; m < j; m++) {
			group_base[m] = asc;
			group_h[m] = p[i].text ? asc + desc : h[i];
		}
		row_h = fmaxf(row_h, group_h[i]);
		i = j;
	}
	for (int i = 0; i < n; i++) {
		float top = (row_h - group_h[i]) / 2;
		if (p[i].text) {
			lv_obj_t *l = label(run, p[i].font, p[i].color);
			lv_obj_set_style_text_letter_space(l, ri(p[i].ls), 0);
			lv_label_set_text(l, p[i].text);
			lv_obj_set_pos(l, ri(x[i]), ri(top + group_base[i] - ascent(p[i].font)));
		} else {
			lv_obj_t *im = icon(run, p[i].img, p[i].color);
			lv_obj_set_pos(im, ri(x[i]), ri(top));
		}
	}
	lv_obj_set_size(run, ri(total), ri(row_h));
}
static void run_free(lv_event_t *e) { lv_free(lv_obj_get_user_data(lv_event_get_target(e))); }
static lv_obj_t *run_new(lv_obj_t *parent)
{
	lv_obj_t *r = box(parent);
	lv_obj_add_event_cb(r, run_free, LV_EVENT_DELETE, NULL);
	return r;
}

/* ---------- the widgets ---------- */
static dash_state_t S, P; /* shown now, shown before */
static bool first = true;

static lv_obj_t *root;
static lv_obj_t *glow_l, *glow_r, *glow_b, *pedal, *pedal_fill, *pedal_name, *pedal_val;

typedef struct { lv_obj_t *box, *icon, *note; } chip_t;
static chip_t chips[10];

static struct { lv_obj_t *box, *icon, *word, *stat; } link_w, link_b;
static lv_obj_t *car_panel;

/* left: lights row, the car */
static lv_obj_t *lights_row;
static struct {
	lv_obj_t *grp, *frame, *car, *seat[DS_SEAT_COUNT], *drl, *drl_glow, *light[12], *tyre[4], *psi[4], *pill[3],
		*pill_t[3], *pill_fan[3];
	float x, y, w, h;  /* the car picture on screen */
	float ox, oy;      /* the picture in the group, which reaches past it for the beams and glows */
	int car_type;
} cab;

/* the route column */
static lv_obj_t *dest_card, *dest_head, *dest_pin, *dest_name, *dest_row[6];

/* the ring */
static lv_obj_t *ring, *speed_l, *unit_l, *gear_l, *cue, *limit_sign, *limit_l, *rear_v, *rear_u, *front_v, *front_u;
static lv_obj_t *ap_disc, *ap_glyph, *outside_l;
static lv_timer_t *ap_blink;

/* the energy column */
static lv_obj_t *dial, *dial_temp, *dial_pct, *dial_range;
static lv_obj_t *charge_box, *charge_head, *charge_to, *charge_when;
static lv_obj_t *stats_box, *stat_lab[5], *stat_val[5];

/* the g-meter */
static lv_obj_t *gball, *g_val, *g_peak;

/* ---------- edge glows (main.dart _Edge), parallel to the bezel ---------- */
static lv_image_dsc_t glow_dsc[2];
static void glow_make(lv_image_dsc_t *d, bool right)
{
	int w = ri(115 * G.k + px(70)), h = ri(G.H);
	uint8_t *a = lv_malloc(w * h);
	for (int y = 0; y < h; y++) {
		float x0 = inset(y + 0.5f);
		for (int x = 0; x < w; x++) {
			float t = (x + 0.5f - x0) / px(70); /* from 75 % at the visible edge to 0 */
			float v = t < 0 ? 0.75f : t > 1 ? 0 : 0.75f * (1 - t);
			a[y * w + (right ? w - 1 - x : x)] = (uint8_t)lroundf(v * 255);
		}
	}
	*d = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = w, .h = h, .stride = w},
			      .data_size = w * h, .data = a};
}

/* ---------- the ring (gauge.dart RingGauge) ---------- */
#define DEG(a) ((a) * (float)M_PI / 180)
static const float SEC_L0 = 124, SEC_R0 = -56, SEC_SWEEP = 112; /* 120° sectors less 4° each end */
static const float HALF = 56, DROP = 15, MAX_KW = 160, MIN_KW = -60;

static void arc(lv_layer_t *layer, float start, float sweep, uint32_t c, float width)
{
	if (sweep <= 0.05f) return;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(G.cx);
	d.center.y = ri(G.cy);
	d.width = width < 1 ? 1 : ri(width);
	d.radius = ri(G.r + width / 2); /* LVGL: the outer edge; Flutter: the stroke's middle */
	int a0 = ((ri(start) % 360) + 360) % 360, a1 = a0 + (ri(sweep) ? ri(sweep) : 1); /* LVGL wants 0..360 */
	d.start_angle = a0;
	d.end_angle = a1 % 360;
	d.color = rgb(c);
	d.opa = opa(c);
	lv_draw_arc(layer, &d);
}
static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, uint32_t c, float w, bool round)
{
	lv_draw_line_dsc_t d;
	lv_draw_line_dsc_init(&d);
	d.p1.x = x1, d.p1.y = y1, d.p2.x = x2, d.p2.y = y2;
	d.color = rgb(c);
	d.opa = opa(c);
	d.width = w < 1 ? 1 : ri(w);
	d.round_start = d.round_end = round;
	lv_draw_line(layer, &d);
}
static void motor(lv_layer_t *layer, float kw, float zero, bool up)
{
	float pos = HALF + DROP, neg = HALF - DROP;
	if (kw >= 0) {
		float s = pos * fminf(kw, MAX_KW) / MAX_KW;
		arc(layer, up ? zero : zero - s, s, ACCENT, G.aw);
	} else {
		float s = neg * fmaxf(kw, MIN_KW) / MIN_KW;
		arc(layer, up ? zero - s : zero, s, REGEN, G.aw);
	}
}
static void ring_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	const dash_state_t *s = &S;
	float r = G.r, w = G.aw;
	arc(layer, SEC_L0, SEC_SWEEP, W24, px(1.5)); /* the empty track: a hairline */
	arc(layer, SEC_R0, SEC_SWEEP, W24, px(1.5));
	/* The app's brake in the ring's foot (a red arc and BRAKE APPLIED) is in the pedal's slot under the g-meter
	 * here (owner); a rear-motor car's brake stays on the ring's right arc. */
	float zl = SEC_L0 + HALF - DROP, zr = SEC_R0 + HALF + DROP;
	if (!isnan(s->rear_kw)) motor(layer, s->rear_kw, zl, true);
	float right = !s->rwd ? s->front_kw : s->brake_pct > 0 ? -s->brake_pct : s->pedal_pct;
	if (!isnan(right)) {
		if (s->rwd) {
			float pct = fmaxf(-100, fminf(100, right));
			if (pct >= 0) {
				float a = (HALF + DROP) * pct / 100;
				arc(layer, zr - a, a, ACCENT, w);
			} else
				arc(layer, zr, (HALF - DROP) * -pct / 100, ALERT, w);
		} else
			motor(layer, right, zr, false);
	}
	/* Regen the pack cannot take now, red past the cap: a motor's share is what the other is not using. */
	if (!isnan(s->regen_cap_kw)) {
		float total = s->regen_cap_kw;
		for (int side = 0; side < (s->rwd ? 1 : 2); side++) {
			float other = side == 0 ? (s->rwd ? 0 : s->front_kw) : s->rear_kw;
			float kw = total - fmaxf(0, isnan(other) ? 0 : -other);
			float at = fmaxf(0, fminf(kw, -MIN_KW)) / -MIN_KW * (HALF - DROP), rest = HALF - DROP - at;
			if (rest <= 0) continue;
			if (side == 0) arc(layer, zl - (HALF - DROP), rest, ALERT, w * 0.3f);
			else arc(layer, zr + at, rest, ALERT, w * 0.3f);
		}
	}
	for (int i = 0; i < 2; i++) { /* zero ticks */
		float a = DEG(i ? zr : zl), c = cosf(a), sn = sinf(a);
		line(layer, G.cx + c * (r - w), G.cy + sn * (r - w), G.cx + c * (r + w * 0.4f), G.cy + sn * (r + w * 0.4f), W54, px(2), false);
	}
}

static void ring_ext(lv_event_t *e) { lv_event_set_ext_draw_size(e, ri(G.aw)); } /* BRAKE APPLIED, under the ring */

/* ---------- the battery dial (widgets.dart _Battery, _DialPainter): drawn at 104 x 98, shown 128 wide ---------- */
static float dial_q(void) { return px(128) * UI_S_DIAL / 104; }
static void dial_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	const dash_state_t *s = &S;
	float q = dial_q(), r = (52 - 4) * q, cx = a.x1 + 52 * q, cy = a.y1 + 52 * q;
	float frac = isnan(s->soc_pct) ? 0 : fmaxf(0, fminf(100, s->soc_pct)) / 100;
	uint32_t color = (isnan(s->soc_pct) ? 100 : s->soc_pct) < 20 ? ALERT : REGEN;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(cx);
	d.center.y = ri(cy);
#define DIAL_ARC(st, sw, c, wd) do { if ((sw) > 0.2f) { d.width = ri(wd); d.radius = ri(r + (wd) / 2); d.start_angle = ri(st); \
		d.end_angle = ri((st) + (sw)); d.color = rgb(c); d.opa = opa(c); lv_draw_arc(layer, &d); } } while (0)
	DIAL_ARC(135, 270, W24, 1.5f * q);
	DIAL_ARC(135, 270 * frac, color, 6 * q);
	bool charging = s->wifi_live && s->charging;
	if (charging && s->charge_limit_pct > frac * 100) /* still to go in: dim green on to the limit */
		DIAL_ARC(135 + 270 * frac, 270 * (fminf(s->charge_limit_pct, 100) / 100 - frac), 0xFF12422D, 6 * q);
}

/* ---------- the g-meter (widgets.dart _GBall, _GPainter) ---------- */
#define TRAIL 10
static float trail[TRAIL][2];
static int trail_n;
static void gball_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	const dash_state_t *s = &S;
	float side = a.x2 - a.x1 + 1, cx = a.x1 + side / 2, cy = a.y1 + side / 2, r = side / 2 - px(1);
	line(layer, cx - r, cy, cx + r, cy, W12, px(1), false);
	line(layer, cx, cy - r, cx, cy + r, W12, px(1), false);
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(cx), d.center.y = ri(cy), d.start_angle = 0, d.end_angle = 360;
	d.width = ri(fmaxf(1, px(1))), d.radius = ri(r / 2 + px(0.5)), d.color = rgb(W12), d.opa = opa(W12);
	lv_draw_arc(layer, &d);
	float tw = px(fmaxf(1.5f, r / px(1) * 0.035f));
	d.width = ri(tw), d.radius = ri(r + tw / 2), d.color = rgb(W24), d.opa = opa(W24);
	lv_draw_arc(layer, &d);
	/* The reach: a quarter ellipse between each pair of peaks, kAccent at 40 %, filled row by row. */
	float b = fminf(1, fmaxf(0, s->g_peak_brake)) * r, ac = fminf(1, fmaxf(0, s->g_peak_accel)) * r,
	      l = fminf(1, fmaxf(0, s->g_peak_lat)) * r;
	if (b + ac + l > 0 && l > 0) {
		lv_draw_fill_dsc_t fd;
		lv_draw_fill_dsc_init(&fd);
		fd.color = rgb(ACCENT);
		for (int y = (int)floorf(cy - b); y <= (int)ceilf(cy + ac); y++) {
			float dy = y + 0.5f - cy, ry = dy < 0 ? b : ac;
			if (ry <= 0 || fabsf(dy) > ry) continue;
			float hw = l * sqrtf(1 - dy * dy / (ry * ry));
			/* the row's ends shaded by coverage, so the edge is smooth */
			int x0 = (int)ceilf(cx - hw), x1 = (int)floorf(cx + hw) - 1;
			if (x1 >= x0) {
				fd.opa = 102;
				lv_area_t row = {x0, y, x1, y};
				lv_draw_fill(layer, &fd, &row);
			}
			float cov = x0 - (cx - hw);
			fd.opa = (lv_opa_t)(102 * cov);
			lv_area_t e1 = {x0 - 1, y, x0 - 1, y}, e2 = {x1 + 1, y, x1 + 1, y};
			if (fd.opa) {
				lv_draw_fill(layer, &fd, &e1);
				lv_draw_fill(layer, &fd, &e2);
			}
		}
	}
	float gk = fmaxf(1, r / px(35)); /* the dot grows with the meter, never under the 72 dp one's */
	float pts[TRAIL][2];
	for (int i = 0; i < trail_n; i++) {
		float gx = trail[i][0], gy = trail[i][1], dist = sqrtf(gx * gx + gy * gy);
		if (dist > 1) gx /= dist, gy /= dist;
		pts[i][0] = cx + gx * r, pts[i][1] = cy + gy * r;
	}
	for (int i = 1; i < trail_n; i++)
		line(layer, pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1], ((uint32_t)(0.7f * 255 * i / trail_n) << 24) | 0xFFFFFF,
		     px(2) * gk, true);
	if (trail_n) {
		lv_draw_rect_dsc_t rd;
		lv_draw_rect_dsc_init(&rd);
		rd.radius = LV_RADIUS_CIRCLE;
		float x = pts[trail_n - 1][0], y = pts[trail_n - 1][1];
		for (int i = 0; i < 2; i++) {
			float rr = (i ? 4 : 5) * gk * px(1);
			rd.bg_color = rgb(i ? WHITE : BLACK);
			lv_area_t c = {ri(x - rr), ri(y - rr), ri(x + rr) - 1, ri(y + rr) - 1};
			lv_draw_rect(layer, &rd, &c);
		}
	}
}

/* The g-meter block's height: the ball, 6 dp, the figure's line, the peak's line; then the pedal's label and bar. */
#define PEDAL_H PG(3)
#define PEDAL_GAP PG(14) /* the pedal's labelled bar, well clear of the figures */
static const lv_font_t *label_font(void) { return FS(REGULAR, 12, UI_S_FIG); } /* as the dial's labels */
static float g_height(void)
{
	return PG(72) + PG(6) + lv_font_get_line_height(FS(REGULAR, 16, UI_S_FIG)) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_FIG)) +
	       PEDAL_GAP + lv_font_get_line_height(label_font()) + PG(3) + PEDAL_H;
}

/* ---------- the assist disc's strike (widgets.dart _Strike) ---------- */
static void strike_draw(lv_event_t *e)
{
	if (S.ap != AP_LOCKED && S.ap != AP_FAULT) return;
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	float s = a.x2 - a.x1 + 1, x0 = a.x1 + s * 0.24f, y0 = a.y1 + s * 0.24f, x1 = a.x1 + s * 0.76f, y1 = a.y1 + s * 0.76f;
	uint32_t disc = S.ap == AP_LOCKED ? ALERT : AP_GREY;
	line(layer, x0, y0, x1, y1, disc, s * 0.17f, true);
	line(layer, x0, y0, x1, y1, WHITE, s * 0.07f, true);
}
static void ap_blink_cb(lv_timer_t *t)
{
	(void)t;
	lv_obj_set_style_opa(ap_disc, lv_obj_get_style_opa(ap_disc, 0) == LV_OPA_COVER ? 51 : LV_OPA_COVER, 0);
}

/* ---------- build ---------- */
static const lv_image_dsc_t *chip_icons[10] = {&ic_trunk, &ic_mirror, &ic_door_fl, &ic_door_rl, &ic_charge_port,
					       &ic_frunk, &ic_recirc, &ic_door_fr, &ic_door_rr, &ic_interior};

/* Each side column: five chips, then the links (left) or the car panel button (right), spaced evenly in the full
 * height. */
static float chip_gap(void) { return (G.chip_area - 5 * PC(48) - PL(44)) / 7; }
static void chip_geom(int i, float *x, float *y)
{
	float item = PC(48), gap = chip_gap(); /* Column spaceEvenly, each chip 3 dp padded, the links' row the last */
	int row = i % 5;
	float cy = G.mt + gap * (row + 1) + item * row + item / 2;
	*y = cy - PC(21);
	*x = i < 5 ? G.ml + inset(cy) : G.W - G.ml - inset(cy) - PC(52);
}

static void link_make(typeof(link_w) *l)
{
	l->box = box(root);
	l->icon = icon(l->box, &ic_wifi_s, WHITE);
	l->word = label(l->box, FS(REGULAR, 10, UI_S_LINK), WHITE);
	lv_obj_set_style_text_letter_space(l->word, ri(PL(0.5)), 0);
	l->stat = label(l->box, FS(BOLD, 12, UI_S_LINK), WHITE);
}

void dash_ui_init(lv_obj_t *scr)
{
	lv_display_t *disp = lv_obj_get_display(scr);
	G.W = lv_display_get_horizontal_resolution(disp);
	G.H = lv_display_get_vertical_resolution(disp);
	G.k = G.H / 480;
	G.ml = px(14), G.mt = px(6), G.mb = px(10);
	G.chip_area = G.H - G.mt - G.mb;
	G.link_top = G.mt + 6 * chip_gap() + 5 * PC(48); /* the side columns' last row */
	/* The ring as big as the screen's height allows: its arcs' top ends 2 px under the top edge (they end at 0.83 of
	 * the outer radius up), 10 px over the bottom one (the app's foot brake, now under the g-meter, set that room; the gear fits in it). The limit sign, the
	 * assist disc and the gear then fit too (checked at 480: r 247, 1.28 x the app's at 1536 dp). */
	G.r = (G.H - 12 * G.k) / (0.83f * 1.0425f + 1.03f);
	G.cy = 0.83f * 1.0425f * G.r + 2 * G.k;
	G.D = 2 * (G.r + px(6)), G.aw = G.r * 0.085f;
	G.cx = G.W / 2; /* until the first layout */
	G.card_w = PN(154);
	G.cab = ui_car_pics[0].car->header.h / px(236);

#if LV_USE_PERF_MONITOR
	/* LVGL's fps / CPU overlay: off unless DASH_PERF=1 (performance checks) */
	const char *perf = getenv("DASH_PERF");
	if (!perf || strcmp(perf, "1")) lv_sysmon_hide_performance(disp);
#endif
	lv_obj_set_style_bg_color(scr, rgb(BLACK), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(scr, false);
	root = box(scr);
	place(root, 0, 0, G.W, G.H);

	/* Edge glows, under everything. */
	glow_make(&glow_dsc[0], false);
	glow_make(&glow_dsc[1], true);
	glow_l = icon(root, &glow_dsc[0], REGEN);
	glow_r = icon(root, &glow_dsc[1], REGEN);
	lv_obj_set_pos(glow_r, ri(G.W - glow_dsc[1].header.w), 0);
	glow_b = box(root);
	place(glow_b, 0, G.H - px(45), G.W, px(45));
	lv_obj_set_style_bg_opa(glow_b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_main_opa(glow_b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_grad_opa(glow_b, 191, 0);
	lv_obj_set_style_bg_grad_dir(glow_b, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_color(glow_b, rgb(ALERT), 0);
	lv_obj_set_style_bg_grad_color(glow_b, rgb(ALERT), 0);

	/* The pedal (main.dart's line along the top): a bar under the g-meter, a hairline track and the fill in kAccent. */
	pedal = box(root);
	fill(pedal, W24);
	lv_obj_set_style_radius(pedal, LV_RADIUS_CIRCLE, 0);
	pedal_name = label(root, label_font(), W70);
	lv_obj_set_style_text_letter_space(pedal_name, ri(PF(0.8)), 0);
	pedal_val = label(root, label_font(), WHITE);
	pedal_fill = box(pedal);
	fill(pedal_fill, ACCENT);
	lv_obj_set_style_radius(pedal_fill, LV_RADIUS_CIRCLE, 0);

	/* The action chips (widgets.dart _Actions). */
	for (int i = 0; i < 10; i++) {
		chip_t *c = &chips[i];
		float x, y;
		chip_geom(i, &x, &y);
		c->box = box(root);
		place(c->box, x, y, PC(52), PC(42));
		lv_obj_set_style_radius(c->box, ri(PC(20)), 0);
		c->icon = icon(c->box, chip_icons[i], WHITE);
		align(c->icon, LV_ALIGN_CENTER, 0, 0);
		c->note = label(c->box, FS(REGULAR, 11, UI_S_CHIP), WHITE);
	}

	/* The links (bottom left) and the car panel button (bottom right); the Maps button is phone-only. */
	link_make(&link_w);
	link_make(&link_b);
	car_panel = icon(root, &ic_car_panel, WHITE);
	{
		/* the right column's last row, its right edge on the chips' (IconButton: 40 dp, the 24 dp icon centred) */
		float cy = G.link_top + PL(22);
		lv_obj_set_pos(car_panel, ri(G.W - G.ml - inset(cy) - PL(20) - PL(12)), ri(cy - PL(12)));
	}

	/* Left: the lights row over the car. */
	lights_row = box(root);
	cab.frame = box(root);
	cab.grp = box(root);
	lv_obj_set_overflow_visible(cab.grp, true);
	cab.car_type = -1;

	/* The route column. */
	dest_card = box(root);
	border(dest_card, 0x30FFFFFF, 1);
	lv_obj_set_style_radius(dest_card, ri(PN(12)), 0);
	dest_head = label(dest_card, FS(REGULAR, 10, UI_S_NAV), W70);
	lv_obj_set_style_text_letter_space(dest_head, ri(PN(1.5)), 0);
	lv_label_set_text(dest_head, "DESTINATION");
	dest_pin = icon(dest_card, &ic_place, W70);
	dest_name = label(dest_card, FS(REGULAR, 15, UI_S_NAV), WHITE);
	lv_label_set_long_mode(dest_name, LV_LABEL_LONG_MODE_DOTS);
	for (int i = 0; i < 6; i++) dest_row[i] = run_new(dest_card);

	/* The ring. */
	ring = box(root);
	lv_obj_set_size(ring, ri(G.D), ri(G.D));
	lv_obj_add_event_cb(ring, ring_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(ring, ring_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_set_overflow_visible(ring, true);
	speed_l = label(ring, F(LIGHT, 100), WHITE);
	unit_l = label(ring, FS(REGULAR, 15, UI_S_RING), W60);
	lv_obj_set_style_text_letter_space(unit_l, ri(PR(3)), 0);
	gear_l = label(ring, FS(BOLD, 28, UI_S_RING), WHITE);
	cue = run_new(ring);
	{
		float side = G.D * 0.16f, k = side / px(52); /* _SpeedLimit, 52 dp, fitted to 0.16 of the ring */
		limit_sign = box(ring);
		place(limit_sign, (G.D - side) / 2, G.D * 0.1f, side, side);
		lv_obj_set_style_radius(limit_sign, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(limit_sign, ri(px(5) * k), 0);
		limit_l = label(limit_sign, font_px(BOLD, ri(px(20) * k)), BLACK);
		align(limit_l, LV_ALIGN_CENTER, 0, 0);
	}
	const lv_font_t *fv = font_px(LIGHT, ri(G.r * 0.16f)), *fu = font_px(MEDIUM, ri(G.r * 0.085f));
	rear_v = label(ring, fv, WHITE);
	front_v = label(ring, fv, WHITE);
	rear_u = label(ring, fu, W70);
	front_u = label(ring, fu, W70);
	lv_obj_t *mt[] = {rear_v, front_v, rear_u, front_u};
	for (int i = 0; i < 4; i++) {
		lv_obj_set_style_text_align(mt[i], LV_TEXT_ALIGN_CENTER, 0);
		float x = (i % 2 ? 1 : -1) * G.r * 0.72f, y = i < 2 ? -G.r * 0.06f : G.r * 0.155f;
		align(mt[i], LV_ALIGN_CENTER, ri(x), ri(y));
	}
	lv_obj_set_style_text_letter_space(rear_u, ri(px(0.5)), 0);
	lv_obj_set_style_text_letter_space(front_u, ri(px(0.5)), 0);
	lv_label_set_text(rear_u, "kW\nREAR");
	ap_disc = box(root);
	lv_obj_set_size(ap_disc, ri(PR(30)), ri(PR(30)));
	lv_obj_set_style_radius(ap_disc, LV_RADIUS_CIRCLE, 0);
	ap_glyph = icon(ap_disc, &ic_autopilot, WHITE);
	align(ap_glyph, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_event_cb(ap_disc, strike_draw, LV_EVENT_DRAW_POST, NULL);
	ap_blink = lv_timer_create(ap_blink_cb, 250, NULL);
	lv_timer_pause(ap_blink);
	outside_l = label(root, FS(REGULAR, 16, UI_S_RING), WHITE);

	/* The energy column. */
	dial = box(root);
	lv_obj_set_size(dial, ri(104 * dial_q()), ri(98 * dial_q()));
	lv_obj_add_event_cb(dial, dial_draw, LV_EVENT_DRAW_MAIN, NULL);
	dial_temp = run_new(dial);
	dial_pct = run_new(dial);
	dial_range = label(dial, font_px(REGULAR, ri(15 * dial_q())), W70);
	charge_box = box(root);
	charge_head = label(charge_box, FS(REGULAR, 12, UI_S_FIG), W70);
	lv_obj_set_style_text_letter_space(charge_head, ri(PF(1.5)), 0);
	lv_label_set_text(charge_head, "CHARGE");
	charge_to = label(charge_box, FS(REGULAR, 18, UI_S_FIG), WHITE);
	charge_when = label(charge_box, FS(REGULAR, 18, UI_S_FIG), WHITE);
	stats_box = box(root);
	for (int i = 0; i < 5; i++) {
		stat_lab[i] = label(stats_box, FS(REGULAR, 12, UI_S_FIG), W70);
		lv_obj_set_style_text_letter_space(stat_lab[i], ri(PF(0.8)), 0);
		stat_val[i] = label(stats_box, FS(REGULAR, 16, UI_S_FIG), WHITE);
	}

	/* The g-meter, its own column on the right. */
	gball = box(root);
	lv_obj_add_event_cb(gball, gball_draw, LV_EVENT_DRAW_MAIN, NULL);
	g_val = run_new(root);
	g_peak = label(root, FS(REGULAR, 12, UI_S_FIG), W70); /* as the dial's labels */
	lv_obj_set_style_text_letter_space(g_peak, ri(PF(0.8)), 0);
}

/* ---------- set ---------- */
#define CHANGED(f) (first || memcmp(&S.f, &P.f, sizeof S.f))

static void fmt_deg(char *b, size_t n, float c) { snprintf(b, n, "%d°", (int)lroundf(c)); }
static uint32_t air_color(float c) { return !isnan(c) && lroundf(c) <= 3 ? COLD : WHITE; }

static void set_chips(const dash_state_t *s)
{
	bool lit[10] = {s->trunk, s->mirror_tilt && s->wifi_live, s->door_fl, s->door_rl, s->charge_port && s->wifi_live,
			s->frunk, s->recirc, s->door_fr, s->door_rr, s->interior && s->wifi_live};
	for (int i = 0; i < 10; i++) {
		chip_t *c = &chips[i];
		bool on = lit[i] && s->live;
		int level = 0;
		bool note = false;
		if (i == 6 && s->co2_ppm >= 0) { /* the cabin's CO2 on the recirculation chip */
			note = true;
			int co2 = s->wifi_live && !s->co2_stale ? s->co2_ppm : -1;
			level = co2 >= 2500 ? 2 : co2 >= 1500 ? 1 : 0;
			if (co2 >= 0) set_text(c->note, "%d", co2);
			else set_text(c->note, "");
		}
		uint32_t f = level == 2 ? ALERT : level == 1 ? WARN : LIT;
		uint32_t col = on && level < 2 ? BLACK : WHITE;
		uint32_t note_col = on ? col : level == 2 ? ALERT : level == 1 ? WARN : col;
		fill(c->box, on ? f : 0);
		border(c->box, on ? f : W70, PC(1));
		icon_color(c->icon, col);
		show(c->note, note);
		if (note) {
			img_src(c->icon, &ic_recirc_s);
			text_color(c->note, note_col);
			/* 22 dp glyph over an 11 dp figure, the pair centred */
			float gh = PC(22), th = lv_font_get_line_height(lv_obj_get_style_text_font(c->note, 0)) * 1.1f / 1.172f;
			float top = (PC(42) - PC(2) - gh - th) / 2; /* the chip's 1 dp border each side */
			align(c->icon, LV_ALIGN_TOP_MID, 0, ri(top));
			put_text(c->note, 1, (PC(52) - 2 * ri(PC(1))) / 2, top + gh + baseline_in(lv_obj_get_style_text_font(c->note, 0), 1.1f), PC(52) - 2 * ri(PC(1)));
		} else if (i == 6) {
			img_src(c->icon, &ic_recirc);
			align(c->icon, LV_ALIGN_CENTER, 0, 0);
		}
	}
}

static void set_link(typeof(link_w) *l, int slot, const lv_image_dsc_t *big, const lv_image_dsc_t *small, uint32_t c,
		     const char *word, bool settled, int stat)
{
	float cy = G.link_top + PL(22), x0 = G.ml + inset(cy); /* the left column's last row, from its stepped edge */
	float x = x0 + (slot == 0 ? 0 : (S.wifi == WIFI_LIVE ? PL(36) : PL(60))), w = settled ? PL(36) : PL(60);
	place(l->box, x, G.link_top, w, PL(44));
	if (settled) { /* a small icon on the big buttons' icon line (Alignment(0, -0.5)), its stat under it */
		img_src(l->icon, small);
		icon_color(l->icon, c);
		show(l->word, false);
		show(l->stat, stat >= 0);
		float col = PL(18) + (stat >= 0 ? PL(12) * 1.1f : 0), top = (PL(44) - col) / 2 * 0.5f;
		align(l->icon, LV_ALIGN_TOP_MID, 0, ri(top));
		if (stat >= 0) {
			set_text(l->stat, "%d", stat);
			text_color(l->stat, stat >= 36 ? WHITE : stat >= 18 ? WARN : ALERT);
			put_text(l->stat, 1, w / 2, top + PL(18) + baseline_in(lv_obj_get_style_text_font(l->stat, 0), 1.1f), w);
		}
	} else { /* the icon in its 28 dp spot, the word under it, the pair centred */
		img_src(l->icon, big);
		icon_color(l->icon, c);
		show(l->stat, false);
		show(l->word, true);
		set_text(l->word, "%s", word);
		text_color(l->word, c);
		float wh = PL(10) * 1.1f, top = (PL(44) - PL(28) - PL(2) - wh) / 2;
		align(l->icon, LV_ALIGN_TOP_MID, 0, ri(top + (PL(28) - PL(22)) / 2));
		put_text(l->word, 1, w / 2, top + PL(28) + PL(2) + baseline_in(lv_obj_get_style_text_font(l->word, 0), 1.1f), w);
	}
}

static void set_links(const dash_state_t *s)
{
	const lv_image_dsc_t *wi = &ic_wifi_l;
	uint32_t wc = W54;
	const char *ww = "IDLE";
	switch (s->wifi) {
	case WIFI_IDLE: wi = &ic_wifi_off_l, wc = W54, ww = "IDLE"; break;
	case WIFI_OFF: wi = &ic_wifi_off_l, wc = ALERT, ww = "OFF"; break;
	case WIFI_SCANNING: wi = &ic_wifi_find_l, wc = ACCENT, ww = "SCAN"; break;
	case WIFI_JOINING: wi = &ic_wifi_find_l, wc = ACCENT, ww = "JOIN"; break;
	case WIFI_HELLO: wi = &ic_wifi_l, wc = ACCENT, ww = "HELLO"; break;
	case WIFI_LIVE: wi = &ic_wifi_l, wc = REGEN, ww = "LIVE"; break;
	case WIFI_QUIET: wi = &ic_wifi_l, wc = WARN, ww = "QUIET"; break;
	case WIFI_AWAY: wi = &ic_wifi_off_l, wc = W54, ww = "AWAY"; break;
	case WIFI_RETRYING: wi = &ic_wifi_find_l, wc = WARN, ww = "RETRY"; break;
	}
	set_link(&link_w, 0, wi, &ic_wifi_s, wc, ww, s->wifi == WIFI_LIVE, s->wifi == WIFI_LIVE ? s->speed_hz : -1);
	const lv_image_dsc_t *bi = &ic_bt_l;
	uint32_t bc = W54;
	const char *bw = "IDLE";
	switch (s->bt) {
	case BT_IDLE: bi = &ic_bt_off_l, bc = W54, bw = "IDLE"; break;
	case BT_OFF: bi = &ic_bt_off_l, bc = ALERT, bw = "OFF"; break;
	case BT_NO_LOCATION: bi = &ic_bt_off_l, bc = ALERT, bw = "NO LOC"; break;
	case BT_SCANNING: bi = &ic_bt_find_l, bc = ACCENT, bw = "SCAN"; break;
	case BT_LINKING: bi = &ic_bt_find_l, bc = ACCENT, bw = "LINK"; break;
	case BT_APPROVING: bi = &ic_bt_find_l, bc = WARN, bw = "PRESS"; break;
	case BT_REFUSED: bi = &ic_bt_off_l, bc = ALERT, bw = "DENIED"; break;
	case BT_DASH: bi = &ic_bt_plain_l, bc = W54, bw = "DASH"; break;
	case BT_PAUSED: bi = &ic_bt_find_l, bc = WARN, bw = "WAIT"; break;
	case BT_AWAY: bi = &ic_bt_find_l, bc = W54, bw = "AWAY"; break;
	case BT_HELD: bi = &ic_bt_off_l, bc = ALERT, bw = "HELD"; break;
	case BT_RETRYING: bi = &ic_bt_find_l, bc = WARN, bw = "RETRY"; break;
	case BT_BORROWED: bi = &ic_bt_l, bc = ACCENT, bw = "BORROW"; break;
	case BT_BUTTONS: bi = &ic_bt_l, bc = ACCENT, bw = "ACTION"; break;
	case BT_LEAN: bi = &ic_bt_l, bc = REGEN, bw = "READY"; break;
	case BT_FULL: bi = &ic_bt_l, bc = REGEN, bw = "LIVE"; break;
	}
	set_link(&link_b, 1, bi, &ic_bt_s, bc, bw, s->bt == BT_LEAN || s->bt == BT_FULL, -1);
}

static void set_lights(const dash_state_t *s, float cx, float top)
{
	struct { const char *w; const lv_image_dsc_t *i; uint32_t c; } lamp[4];
	int n = 0;
	if (s->live && s->drl) lamp[n++] = (typeof(lamp[0])){"DRL", &ic_drl, REGEN};
	if (s->live && s->low_beam) lamp[n++] = (typeof(lamp[0])){"LOW", &ic_low, REGEN};
	if (s->live && s->high_beam) lamp[n++] = (typeof(lamp[0])){"HIGH", &ic_high, ACCENT};
	if (s->wifi_live && s->grip != GRIP_NONE)
		lamp[n++] = (typeof(lamp[0])){s->grip_word[0] ? s->grip_word : "GRIP", &ic_grip, s->grip == GRIP_SLIPPERY ? COLD : WARN};
	char key[96] = "";
	for (int i = 0; i < n; i++) snprintf(key + strlen(key), sizeof key - strlen(key), "%s%x", lamp[i].w, (unsigned)lamp[i].c);
	snprintf(key + strlen(key), sizeof key - strlen(key), "@%d,%d", ri(cx), ri(top));
	static char shown[96] = "-";
	if (!strcmp(key, shown)) return;
	strcpy(shown, key);
	lv_obj_clean(lights_row);
	const lv_font_t *f = FS(REGULAR, 12, UI_S_LIGHTS);
	float x = 0, ls = PLI(1), wd[4];
	for (int i = 0; i < n; i++) {
		lv_point_t sz;
		lv_text_get_size(&sz, lamp[i].w, f, ri(ls), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		wd[i] = fmaxf(PLI(20), sz.x + ls) + 2 * PLI(5);
		x += wd[i];
	}
	place(lights_row, cx - x / 2, top, x, PLI(20) + PLI(2) + lv_font_get_line_height(f));
	x = 0;
	for (int i = 0; i < n; i++) {
		lv_obj_t *im = icon(lights_row, lamp[i].i, lamp[i].c);
		lv_obj_set_pos(im, ri(x + (wd[i] - PLI(20)) / 2), 0);
		lv_obj_t *l = label(lights_row, f, lamp[i].c);
		lv_obj_set_style_text_letter_space(l, ri(ls), 0);
		lv_label_set_text(l, lamp[i].w);
		lv_point_t sz;
		lv_text_get_size(&sz, lamp[i].w, f, ri(ls), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_obj_set_pos(l, ri(x + (wd[i] - sz.x - ls) / 2), ri(PLI(22)));
		x += wd[i];
	}
}

/* The car from above (widgets.dart _Cabin): its picture, seats, lights, tyres and each row's air. */
static const int seat_row[DS_SEAT_COUNT] = {1, 1, 2, 2, 2, 3, 3};
enum { L_CONE_LOW, L_CONE_HIGH, L_REAR_GLOW, L_REAR_GLOW_BRAKE, L_FRONT_GLOW, L_FRONT_DRL, L_FRONT_LOW, L_FRONT_HIGH,
       L_REAR_LINE, L_REAR_LINE_BRAKE, L_TURN_L, L_TURN_R };
static float cb(float dp) { return px(dp) * G.cab; } /* a size inside the cabin's fitted box */

static lv_obj_t *layer_img(const ui_layer_t *l)
{
	if (!l->img) return NULL;
	lv_obj_t *i = lv_image_create(cab.grp);
	lv_obj_remove_style_all(i);
	img_src(i, l->img);
	lv_obj_set_pos(i, ri(cab.ox + l->x), ri(cab.oy + l->y));
	lv_obj_set_hidden(i, true);
	return i;
}

static void cabin_build(int type)
{
	const ui_car_pics_t *p = &ui_car_pics[type];
	lv_obj_clean(cab.grp);
	memset(cab.seat, 0, sizeof cab.seat);
	cab.car_type = type;
	cab.w = p->car->header.w, cab.h = p->car->header.h;
	/* LVGL clips children to their parent: the group spans every layer */
	float x0 = 0, y0 = 0, x1 = cab.w, y1 = cab.h;
	const ui_layer_t *all[12 + 2];
	for (int i = 0; i < 12; i++) all[i] = &p->light[i];
	all[12] = &p->drl, all[13] = &p->drl_glow;
	for (int i = 0; i < 14; i++)
		if (all[i]->img) {
			x0 = fminf(x0, all[i]->x), y0 = fminf(y0, all[i]->y);
			x1 = fmaxf(x1, all[i]->x + all[i]->img->header.w), y1 = fmaxf(y1, all[i]->y + all[i]->img->header.h);
		}
	cab.ox = -x0, cab.oy = -y0;
	lv_obj_set_size(cab.grp, ri(x1 - x0), ri(y1 - y0));
	/* under the car: the beams and the glows; the body hides what would light it */
	for (int i = 0; i <= L_FRONT_GLOW; i++) cab.light[i] = layer_img(&p->light[i]);
	cab.car = lv_image_create(cab.grp);
	lv_obj_remove_style_all(cab.car);
	img_src(cab.car, p->car);
	lv_obj_set_pos(cab.car, ri(cab.ox), ri(cab.oy));
	for (int i = 0; i < DS_SEAT_COUNT; i++) cab.seat[i] = layer_img(&p->seat_green[i]);
	cab.drl_glow = layer_img(&p->drl_glow);
	cab.drl = layer_img(&p->drl);
	for (int i = L_FRONT_DRL; i <= L_TURN_R; i++) cab.light[i] = layer_img(&p->light[i]);
	/* tyres: the psi on the body over each wheel */
	for (int i = 0; i < 4; i++) {
		cab.tyre[i] = label(cab.grp, font_px(BOLD, ri(cb(14))), WHITE);
		cab.psi[i] = label(cab.grp, font_px(REGULAR, ri(cb(9))), W60);
		lv_label_set_text(cab.psi[i], "psi");
	}
	for (int r = 0; r < 3; r++) {
		cab.pill[r] = box(cab.grp);
		fill(cab.pill[r], 0x8A000000);
		lv_obj_set_style_radius(cab.pill[r], ri(cb(6)), 0);
		cab.pill_t[r] = label(cab.pill[r], font_px(BOLD, ri(cb(16))), WHITE);
		cab.pill_fan[r] = run_new(cab.pill[r]);
	}
}

static void set_cabin(const dash_state_t *s, float cx, float top)
{
	int type = s->car;
	if (type != cab.car_type) cabin_build(type);
	const ui_car_pics_t *p = &ui_car_pics[type];
	bool on = !s->no_cabin;
	show(cab.grp, on);
	/* The cabin's box: 4 dp frame + 4 dp above, 4 dp frame + 6 dp below, the picture 6 dp in each side; fitted to the
	 * column, then moved 7 % of its height down (more of the road ahead). */
	float box_h = cb(4 + 4) + cab.h + cb(4 + 6);
	(void)box_h;
	cab.x = cx - cab.w / 2, cab.y = top + cb(8);
	lv_obj_set_pos(cab.grp, ri(cab.x - cab.ox), ri(cab.y - cab.oy));
	bool seats = s->wifi_live && s->cabin, belt = false;
	for (int i = 0; i < DS_SEAT_COUNT; i++)
		if (seats && s->seat[i] == DS_SEAT_UNBUCKLED && p->seat_red[i].img) belt = true;
	show(cab.frame, on && belt);
	if (belt) {
		place(cab.frame, cab.x - cb(6), cab.y, cab.w + 2 * cb(6), cab.h);
		fill(cab.frame, 0x2EE23D3D);
		border(cab.frame, ALERT, cb(1));
		lv_obj_set_style_radius(cab.frame, ri(cb(12)), 0);
	}
	for (int i = 0; i < DS_SEAT_COUNT; i++) {
		if (!cab.seat[i]) continue;
		bool lit = seats && s->seat[i] != DS_SEAT_PLAIN;
		show(cab.seat[i], lit);
		if (lit) img_src(cab.seat[i], s->seat[i] == DS_SEAT_UNBUCKLED ? p->seat_red[i].img : p->seat_green[i].img);
	}
	bool front = s->drl || s->low_beam || s->high_beam, live = s->live;
	bool want[12] = {
		[L_CONE_LOW] = s->low_beam && !s->high_beam, [L_CONE_HIGH] = s->high_beam,
		[L_REAR_GLOW] = (front) && !s->braking, [L_REAR_GLOW_BRAKE] = s->braking,
		[L_FRONT_GLOW] = front,
		[L_FRONT_DRL] = front && !s->low_beam && !s->high_beam, [L_FRONT_LOW] = s->low_beam && !s->high_beam,
		[L_FRONT_HIGH] = s->high_beam,
		[L_REAR_LINE] = front && !s->braking, [L_REAR_LINE_BRAKE] = s->braking,
		[L_TURN_L] = s->ind_l, [L_TURN_R] = s->ind_r,
	};
	for (int i = 0; i < 12; i++)
		if (cab.light[i]) show(cab.light[i], live && want[i]);
	if (cab.drl) {
		show(cab.drl, live && front);
		show(cab.drl_glow, live && front);
	}
	/* tyres */
	for (int i = 0; i < 4; i++) {
		bool left = i % 2 == 0;
		float axle = p->axles[i / 2] * cab.h, bw = cab.w * p->tyre_w;
		float bx = left ? cab.w * p->tyre_l : cab.w - cab.w * p->tyre_r - bw;
		if (isnan(s->tyre_psi[i])) set_text(cab.tyre[i], "—");
		else set_text(cab.tyre[i], "%d", (int)lroundf(s->tyre_psi[i]));
		/* the pair (14 at a 1.1 line over 9 at 1.0) centred in a 30 dp box on the axle */
		float h1 = cb(14) * 1.1f, h2 = cb(9), y0 = axle - cb(15) + (cb(30) - h1 - h2) / 2;
		const lv_font_t *f1 = lv_obj_get_style_text_font(cab.tyre[i], 0), *f2 = lv_obj_get_style_text_font(cab.psi[i], 0);
		float gw = lv_obj_get_style_width(cab.grp, 0);
		put_text(cab.tyre[i], 1, cab.ox + bx + bw / 2, cab.oy + y0 + baseline_in(f1, 1.1f), gw);
		put_text(cab.psi[i], 1, cab.ox + bx + bw / 2, cab.oy + y0 + h1 + baseline_in(f2, 1.0f), gw);
	}
	/* each row's air and fan on a dark pill, centred on its seats; a row with a seat unbuckled leaves the red in view */
	for (int r = 0; r < 3; r++) {
		bool unb = false;
		for (int i = 0; i < DS_SEAT_COUNT; i++)
			if (seat_row[i] == r + 1 && s->seat[i] == DS_SEAT_UNBUCKLED && p->seat_red[i].img) unb = true;
		bool vis = seats && !isnan(s->cabin_c[r]) && !unb;
		show(cab.pill[r], vis);
		if (!vis) continue;
		const char *fan = r < 2 ? s->fan[r] : "";
		char t[16];
		fmt_deg(t, sizeof t, s->cabin_c[r]);
		set_text(cab.pill_t[r], "%s", t);
		const lv_font_t *ft = lv_obj_get_style_text_font(cab.pill_t[r], 0);
		lv_point_t tz;
		lv_text_get_size(&tz, t, ft, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		float th = cb(16) * 1.1f, fh = 0, fw = 0;
		show(cab.pill_fan[r], fan[0]);
		if (fan[0]) {
			char word[12];
			snprintf(word, sizeof word, " %s", fan);
			part_t parts[2] = {IMG(strcmp(fan, "OFF") ? &ic_fan : &ic_fan_off, W70),
					   TXT(font_px(REGULAR, ri(cb(11))), W70, word)};
			run_set(cab.pill_fan[r], parts, strcmp(fan, "OFF") ? 2 : 1);
			fh = cb(11) * 1.1f;
			fw = lv_obj_get_style_width(cab.pill_fan[r], 0);
		}
		float pw = fmaxf(tz.x, fw) + 2 * cb(5), ph = th + fh + 2 * cb(1);
		float pcx = cab.ox + cab.w * (p->pill_l + p->pill_w / 2), pcy = cab.oy + cab.h * p->rows[r];
		place(cab.pill[r], pcx - pw / 2, pcy - ph / 2, pw, ph);
		put_text(cab.pill_t[r], 1, pw / 2, cb(1) + baseline_in(ft, 1.1f), pw);
		if (fan[0]) align(cab.pill_fan[r], LV_ALIGN_TOP_MID, 0, ri(cb(1) + th + (fh - lv_obj_get_style_height(cab.pill_fan[r], 0)) / 2));
	}
}

/* A route row: its icon, 4 dp, the figure, and a smaller grey note after it (widgets.dart _Nav stat). */
static void stat_run(lv_obj_t *run, const lv_image_dsc_t *i, const char *text, uint32_t color, const char *note)
{
	part_t p[3] = {IMG(i, W70), TXT(FS(REGULAR, 18, UI_S_NAV), color, text), TXT(FS(REGULAR, 14, UI_S_NAV), W54, note)};
	p[1].gap = PN(4);
	run_set(run, p, note && note[0] ? 3 : 2);
}

static void set_route(const dash_state_t *s, float x, float cy)
{
	char b[6][32], n[6][16];
	const lv_image_dsc_t *ic[6];
	uint32_t col[6];
	int rows = 0;
	if (s->nav_minutes >= 0) {
		int m = s->nav_minutes;
		if (m < 1) snprintf(b[rows], 32, "<1 min");
		else if (m < 60) snprintf(b[rows], 32, "%d min", m);
		else snprintf(b[rows], 32, "%dh %dm", m / 60, m % 60);
		ic[rows] = &ic_hourglass, n[rows][0] = 0, col[rows++] = WHITE;
		snprintf(b[rows], 32, "%s", s->arrive_clock[0] ? s->arrive_clock : "--:--");
		ic[rows] = &ic_flag, n[rows][0] = 0, col[rows++] = WHITE;
	}
	const char *unit = s->kph ? "km" : "mi";
	if (!isnan(s->route_len)) {
		if (s->route_len < 9.95f) snprintf(b[rows], 32, "%.1f %s", s->route_len, unit);
		else snprintf(b[rows], 32, "%d %s", (int)lroundf(s->route_len), unit);
		ic[rows] = &ic_route, n[rows][0] = 0, col[rows++] = WHITE;
	}
	if (!isnan(s->route_kwh)) {
		snprintf(b[rows], 32, "%.1f kWh", s->route_kwh);
		ic[rows] = &ic_bolt, n[rows][0] = 0, col[rows++] = WHITE;
	}
	if (!isnan(s->arrive_range)) {
		snprintf(b[rows], 32, "%d %s", (int)lroundf(s->arrive_range), unit);
		int d = isnan(s->range_drop) ? 0 : (int)lroundf(s->range_drop);
		if (d > 0) snprintf(n[rows], 16, " −%d", d);
		else n[rows][0] = 0;
		ic[rows] = &ic_ecar, col[rows++] = WHITE;
	}
	if (!isnan(s->arrive_pct) || !isnan(s->arrive_kwh)) {
		if (!isnan(s->arrive_pct)) snprintf(b[rows], 32, "%d%%", (int)lroundf(s->arrive_pct));
		else snprintf(b[rows], 32, "%.1f kWh", s->arrive_kwh);
		int d = isnan(s->pct_drop) ? 0 : (int)lroundf(s->pct_drop);
		if (d > 0) snprintf(n[rows], 16, " −%d", d);
		else n[rows][0] = 0;
		ic[rows] = &ic_battery, col[rows++] = !isnan(s->arrive_pct) && s->arrive_pct < 10 ? ALERT : WHITE;
	}
	/* Laid out as the owner's mockup (the header, the place, then a row each), scaled with the column; the rows
	 * close up when there are too many for the height. */
	float pad = 20 * G.k * UI_S_NAV, pitch = 52 * G.k * UI_S_NAV, head_c = 27 * G.k * UI_S_NAV;
	float name_h = 0, name_x = pad + PN(15 + 4), name_w = G.card_w - pad - name_x;
	show(dest_name, s->dest[0]);
	show(dest_pin, s->dest[0]);
	if (s->dest[0]) { /* the place, after its pin, two lines at most */
		set_text(dest_name, "%s", s->dest);
		lv_obj_set_width(dest_name, ri(name_w));
		lv_point_t sz;
		lv_text_get_size(&sz, s->dest, lv_obj_get_style_text_font(dest_name, 0), 0, 0, ri(name_w), LV_TEXT_FLAG_NONE);
		name_h = fminf(sz.y, 2 * lv_font_get_line_height(lv_obj_get_style_text_font(dest_name, 0))) + 10 * G.k * UI_S_NAV;
		lv_obj_set_height(dest_name, ri(name_h - 10 * G.k * UI_S_NAV));
	}
	float first = 64 * G.k * UI_S_NAV + name_h, bottom = 41 * G.k * UI_S_NAV, room = G.H - 2 * G.mt;
	if (rows > 1 && first + (rows - 1) * pitch + bottom > room) pitch = (room - first - bottom) / (rows - 1);
	float h = first + (rows ? (rows - 1) * pitch : 0) + bottom;
	place(dest_card, x, cy - h / 2, G.card_w, h);
	const lv_font_t *hf = lv_obj_get_style_text_font(dest_head, 0);
	lv_obj_set_pos(dest_head, ri(pad), ri(head_c - lv_font_get_line_height(hf) / 2.0f));
	if (s->dest[0]) {
		float y = head_c + lv_font_get_line_height(hf) / 2.0f + 6 * G.k * UI_S_NAV, lh = lv_font_get_line_height(lv_obj_get_style_text_font(dest_name, 0));
		lv_obj_set_pos(dest_name, ri(name_x), ri(y));
		lv_obj_set_pos(dest_pin, ri(pad), ri(y + (lh - PN(15)) / 2));
	}
	for (int i = 0; i < 6; i++) {
		show(dest_row[i], i < rows);
		if (i >= rows) continue;
		stat_run(dest_row[i], ic[i], b[i], col[i], n[i]);
		lv_obj_set_pos(dest_row[i], ri(pad), ri(first + i * pitch - lv_obj_get_style_height(dest_row[i], 0) / 2.0f));
	}
}

static void set_ring(const dash_state_t *s)
{
	bool charge = s->wifi_live && s->charging && s->gear == 'P' && !isnan(s->charge_kw);
	/* The speed, scaled to the hole (0.5 x 0.4 of the ring) as the app's FittedBox does with its 200 sp text. */
	char t[16];
	bool known = charge || (s->live && !isnan(s->speed));
	if (charge) snprintf(t, sizeof t, "%d", (int)lroundf(s->charge_kw));
	else if (known) snprintf(t, sizeof t, "%d", (int)lroundf(s->speed));
	else snprintf(t, sizeof t, "—");
	float bw = G.D * 0.5f, bh = G.D * 0.4f;
	static float adv = 0;
	if (!adv) adv = lv_font_get_glyph_width(font_px(LIGHT, 200), '0', 0) / 200.0f;
	float em_w = known ? strlen(t) * adv : 1.0f; /* the em dash is an em wide */
	float fs = fminf(bh, bw / em_w);
	const lv_font_t *f = font_px(LIGHT, ri(fs));
	if (lv_obj_get_style_text_font(speed_l, 0) != f) lv_obj_set_style_text_font(speed_l, f, 0);
	set_text(speed_l, "%s", t);
	text_color(speed_l, charge ? REGEN : known ? WHITE : W24);
	/* Column centred: the box, then the unit's line. At height 1 the text's box is its size, the baseline at
	 * ascent / (ascent + descent) of it. */
	const lv_font_t *fu = lv_obj_get_style_text_font(unit_l, 0);
	float col_h = bh + lv_font_get_line_height(fu), top = (G.D - col_h) / 2;
	float a = ascent(f), d = f->base_line, base = top + (bh - fs) / 2 + fs * a / (a + d);
	put_text(speed_l, 1, G.D / 2, base, G.D);
	set_text(unit_l, "%s", charge ? "kW" : s->kph ? "km/h" : "mph");
	put_text(unit_l, 1, G.D / 2 + PR(3) / 2, top + bh + ascent(fu), G.D);

	/* The gear, in the gap at the ring's foot. */
	show(gear_l, s->gear != 0);
	if (s->gear) {
		set_text(gear_l, "%c", s->gear);
		text_color(gear_l, s->live && strchr("PRND", s->gear) ? WHITE : W38);
		const lv_font_t *fg = lv_obj_get_style_text_font(gear_l, 0);
		put_text(gear_l, 1, G.D / 2, G.D - G.D * 0.05f - lv_font_get_line_height(fg) + ascent(fg), G.D);
	}

	/* The limit sign. */
	bool lim = s->live && !isnan(s->speed_limit);
	fill(limit_sign, lim ? WHITE : W10);
	lv_obj_set_style_border_color(limit_sign, rgb(lim ? ALERT : W24), 0);
	lv_obj_set_style_border_opa(limit_sign, opa(lim ? ALERT : W24), 0);
	if (lim) set_text(limit_l, "%d", (int)lroundf(s->speed_limit));
	else set_text(limit_l, "—");
	text_color(limit_l, lim ? BLACK : W24);

	/* The motor readouts. */
	if (s->live && !isnan(s->rear_kw)) set_text(rear_v, "%d", (int)lroundf(s->rear_kw));
	else set_text(rear_v, "—");
	float right = !s->rwd ? s->front_kw : s->brake_pct > 0 ? -s->brake_pct : s->pedal_pct;
	if (s->live && !isnan(right)) set_text(front_v, "%d", (int)lroundf(s->rwd ? fabsf(right) : right));
	else set_text(front_v, "—");
	set_text(front_u, "%s", s->rwd ? (right < 0 ? "%\nBRAKE" : "%\nPEDAL") : "kW\nFRONT");

	/* The cue under the speed: charging, the cruise, or the washer fluid low. */
	part_t p[8];
	int n = 0;
	const lv_font_t *fs15 = FS(BOLD, 15, UI_S_RING);
	char a1[24], a2[24];
	bool washer = s->wifi_live && s->washer_low;
	if (charge && (!isnan(s->charge_max_kw) || s->charge_why[0])) {
		if (!isnan(s->charge_max_kw)) {
			snprintf(a1, sizeof a1, "MAX %d", (int)lroundf(s->charge_max_kw));
			p[n] = TXT(fs15, W70, a1), p[n++].ls = PR(1);
		}
		if (s->charge_why[0]) {
			snprintf(a2, sizeof a2, "%s%s", !isnan(s->charge_max_kw) ? " · " : "", s->charge_why);
			p[n] = TXT(fs15, WARN, a2), p[n++].ls = PR(1);
		}
		if (washer) p[n] = IMG(&ic_washer, WARN), p[n++].gap = PR(12);
	} else if (s->wifi_live && !isnan(s->cruise_set)) {
		snprintf(a1, sizeof a1, "SET %d", (int)lroundf(s->cruise_set));
		p[n] = TXT(fs15, ACCENT, a1), p[n++].ls = PR(1);
		if (s->follow_gap >= 0) {
			p[n] = IMG(&ic_gap_car, ACCENT), p[n++].gap = PR(10);
			snprintf(a2, sizeof a2, " %d", s->follow_gap);
			p[n] = TXT(fs15, ACCENT, a2), p[n++].ls = PR(1);
		}
		if (washer) p[n] = IMG(&ic_washer, WARN), p[n++].gap = PR(12);
	} else if (washer) {
		p[n++] = IMG(&ic_washer, WARN);
		p[n] = TXT(FS(BOLD, 13, UI_S_RING), WARN, "WASHER LOW"), p[n].ls = PR(1), p[n++].gap = PR(4);
	}
	show(cue, n > 0);
	if (n) {
		run_set(cue, p, n);
		float cy = G.D - G.D * 0.17f - G.D * 0.075f / 2;
		align(cue, LV_ALIGN_TOP_MID, 0, ri(cy - lv_obj_get_style_height(cue, 0) / 2.0f));
	}

	/* The assist disc. */
	uint32_t disc = 0;
	bool flash = false;
	const lv_image_dsc_t *g = &ic_autopilot;
	switch (s->live ? s->ap : AP_NONE) {
	case AP_NONE: break;
	case AP_READY: disc = AP_GREY; break;
	case AP_STEERING: disc = AP_BLUE; break;
	case AP_TACC: disc = AP_BLUE, g = &ic_cruise; break;
	case AP_HOLD: disc = WARN, flash = true; break;
	case AP_HOLD_RED: case AP_SLOWING: case AP_TAKEOVER: disc = ALERT, flash = true; break;
	case AP_LOCKED: disc = ALERT; break;
	case AP_FAULT: disc = AP_GREY; break;
	}
	if (s->live && s->ap == AP_READY && !isnan(s->cruise_set) && s->wifi_live) disc = AP_BLUE, g = &ic_cruise; /* TACC */
	show(ap_disc, disc != 0);
	if (disc) {
		fill(ap_disc, disc);
		img_src(ap_glyph, g);
		align(ap_glyph, LV_ALIGN_CENTER, 0, 0);
		icon_color(ap_glyph, disc == WARN ? BLACK : WHITE);
		if (CHANGED(ap)) lv_obj_invalidate(ap_disc); /* the strike */
	}
	if (flash) lv_timer_resume(ap_blink);
	else {
		lv_timer_pause(ap_blink);
		SET_NUM(ap_disc, lv_obj_get_style_opa, lv_obj_set_style_opa, LV_OPA_COVER);
	}

	char o[16];
	if (isnan(s->outside_c)) snprintf(o, sizeof o, "—");
	else fmt_deg(o, sizeof o, s->outside_c);
	set_text(outside_l, "%s", o);
	text_color(outside_l, air_color(s->outside_c));
}

/* The ring where the layout puts it: its box, the assist disc at its top right, the air outside mirroring it. */
static void ring_place(void)
{
	lv_obj_set_pos(ring, ri(G.cx - G.D / 2), ri(G.cy - G.D / 2));
	float size = PR(30), d = G.r * (1 + 0.085f / 2) + size / 2 + PR(5), a = DEG(-38);
	float x = G.cx + d * cosf(a), y = G.cy + d * sinf(a);
	lv_obj_set_pos(ap_disc, ri(x - size / 2), ri(y - size / 2));
	float right = G.cx - d * cosf(a) + size / 2 - PR(3);
	const lv_font_t *f = lv_obj_get_style_text_font(outside_l, 0);
	align(outside_l, LV_ALIGN_TOP_RIGHT, ri(right - G.W), ri(y - lv_font_get_line_height(f) / 2.0f));
}

/* Redraws only the ring's parts whose values changed: a sector's box, or its foot. */
static void ring_dirty(const dash_state_t *s)
{
	float R = G.r + G.aw, in = 0.5f * (G.r - G.aw), h = 0.83f * R;
	bool both = first || CHANGED(live) || CHANGED(rwd) || CHANGED(regen_cap_kw);
	bool left = both || CHANGED(rear_kw) || CHANGED(front_kw);
	bool right = both || CHANGED(front_kw) || CHANGED(rear_kw) || CHANGED(pedal_pct) || CHANGED(brake_pct);
	lv_area_t a;
	if (left) {
		a = (lv_area_t){ri(G.cx - R) - 2, ri(G.cy - h) - 2, ri(G.cx - in) + 2, ri(G.cy + h) + 2};
		lv_obj_invalidate_area(ring, &a);
	}
	if (right) {
		a = (lv_area_t){ri(G.cx + in) - 2, ri(G.cy - h) - 2, ri(G.cx + R) + 2, ri(G.cy + h) + 2};
		lv_obj_invalidate_area(ring, &a);
	}
}

static void set_energy(const dash_state_t *s, float cx)
{
	float q = dial_q();
	bool charging = s->wifi_live && s->charging;
	/* the pack's coldest cells over the %: a bolt while charging, ice blue and the snowflake while cold, a red arrow
	 * while it heats */
	float t = s->pack_temp_c;
	bool cold = (isnan(t) ? 99 : t) <= 10, heating = s->pack_heating || !isnan(s->precond_c);
	part_t p[6];
	int n = 0;
	char tt[32];
	if (charging) p[n++] = IMG(&ic_bolt_dial, REGEN);
	if (!isnan(t)) {
		if (cold) p[n] = IMG(&ic_snow_dial, COLD), p[n].gap = n ? q : 0, n++;
		float a = fabsf(t);
		if (a < 10) snprintf(tt, sizeof tt, "%.1f°", a < 0.05f ? 0.0f : t);
		else snprintf(tt, sizeof tt, "%d°", (int)lroundf(t));
		p[n] = TXT(font_px(REGULAR, ri(14 * q)), cold ? COLD : W70, tt), p[n].gap = n ? q : 0, n++;
		if (heating) p[n] = IMG(&ic_up_dial, ALERT), p[n++].gap = q;
	}
	show(dial_temp, (charging || !isnan(t)) && n);
	if (n) {
		run_set(dial_temp, p, n);
		align(dial_temp, LV_ALIGN_TOP_MID, 0, ri(15 * q + (19 * q - lv_obj_get_style_height(dial_temp, 0)) / 2));
	}
	char pc[8];
	bool known = !isnan(s->soc_pct);
	uint32_t color = known && s->soc_pct < 20 ? ALERT : WHITE;
	snprintf(pc, sizeof pc, known ? "%d" : "—", (int)lroundf(known ? s->soc_pct : 0));
	part_t pp[2] = {TXT(font_px(REGULAR, ri(28 * q)), known ? color : W38, pc), TXT(font_px(REGULAR, ri(16 * q)), color, "%")};
	run_set(dial_pct, pp, known ? 2 : 1);
	align(dial_pct, LV_ALIGN_TOP_MID, 0, ri(34 * q + (36 * q - lv_obj_get_style_height(dial_pct, 0)) / 2));
	show(dial_range, !isnan(s->range));
	if (!isnan(s->range)) set_text(dial_range, "%d %s", (int)lroundf(s->range), s->kph ? "km" : "mi");
	align(dial_range, LV_ALIGN_BOTTOM_MID, 0, ri(-(18 * q - lv_font_get_line_height(lv_obj_get_style_text_font(dial_range, 0))) / 2));

	/* The charge card. */
	bool card = charging && (s->charge_limit_pct >= 0 || s->charge_minutes >= 0);
	float ch_h = 0;
	show(charge_box, card);
	if (card) {
		const lv_font_t *fh = lv_obj_get_style_text_font(charge_head, 0), *fb = lv_obj_get_style_text_font(charge_to, 0);
		float y = lv_font_get_line_height(fh) + PF(2), w = 0;
		show(charge_to, s->charge_limit_pct >= 0);
		if (s->charge_limit_pct >= 0) {
			set_text(charge_to, "to %d %%", s->charge_limit_pct);
			lv_obj_set_pos(charge_to, 0, ri(y));
			y += lv_font_get_line_height(fb);
		}
		show(charge_when, s->charge_minutes >= 0);
		if (s->charge_minutes >= 0) {
			int m = s->charge_minutes;
			char d[16];
			if (m < 60) snprintf(d, sizeof d, "%d min", m);
			else snprintf(d, sizeof d, "%dh %dm", m / 60, m % 60);
			set_text(charge_when, "%s   %s", d, s->charge_clock[0] ? s->charge_clock : "--:--");
			lv_obj_set_pos(charge_when, 0, ri(y));
			y += lv_font_get_line_height(fb);
		}
		lv_point_t sz;
		for (lv_obj_t *l = charge_head; l; l = l == charge_head ? charge_to : l == charge_to ? charge_when : NULL) {
			lv_text_get_size(&sz, lv_label_get_text(l), lv_obj_get_style_text_font(l, 0),
					 lv_obj_get_style_text_letter_space(l, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			w = fmaxf(w, sz.x);
		}
		lv_obj_set_size(charge_box, ri(w), ri(y));
		ch_h = y;
	}

	/* The figures (widgets.dart _Draws, _Pack): what the pack feeds besides the motors (WiFi), its health; with the
	 * minor figures on, the 12 V side, the rest and the cell imbalance too. Labels one width, so the values line up. */
	int rows = 0;
	const char *lab[5];
	char val[5][24];
#define KW_ROW(name, v) do { \
		lab[rows] = name; \
		snprintf(val[rows], 24, isnan(v) ? "—" : "%.1f kW", v); \
		rows++; \
	} while (0)
	if (s->wifi_live && s->draws) {
		KW_ROW("CLIMATE", s->climate_kw);
		if (s->minor_figures) {
			KW_ROW("12 V", s->lv_kw);
			KW_ROW("OTHER", s->other_kw);
		}
	}
	if (s->minor_figures) {
		lab[rows] = "IMBALANCE";
		if (isnan(s->imbalance_mv)) snprintf(val[rows], 24, "—");
		else snprintf(val[rows], 24, "%d mV", (int)lroundf(s->imbalance_mv));
		rows++;
	}
	lab[rows] = "DEGRADATION";
	if (isnan(s->degradation_pct)) snprintf(val[rows], 24, "—");
	else snprintf(val[rows], 24, "%.1f %%", s->degradation_pct);
	rows++;
	const lv_font_t *fl = FS(REGULAR, 12, UI_S_FIG), *fv = FS(REGULAR, 16, UI_S_FIG);
	float row_h = fmaxf(lv_font_get_line_height(fl), lv_font_get_line_height(fv)), vw = 0;
	for (int i = 0; i < 5; i++) {
		show(stat_lab[i], i < rows);
		show(stat_val[i], i < rows);
		if (i >= rows) continue;
		set_text(stat_lab[i], "%s", lab[i]);
		set_text(stat_val[i], "%s", val[i]);
		float base = i * row_h + row_h / 2 + (ascent(fv) - fv->base_line) / 2.0f;
		put_text(stat_lab[i], 0, 0, base, 0);
		put_text(stat_val[i], 0, PF(104), base, 0);
		lv_point_t sz;
		lv_text_get_size(&sz, val[i], fv, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		vw = fmaxf(vw, sz.x);
	}
	float st_w = PF(104) + vw, st_h = rows * row_h;
	lv_obj_set_size(stats_box, ri(st_w), ri(st_h));

	/* The group in the column's middle, each piece 5 dp above and below (widgets.dart _SideColumn). */
	/* The dial level with the g-meter's circle and as big (ui/assets/sizes.json), its figures under it, centred. */
	/* The pair's top: the taller of the two blocks centred in the height. */
	float dh = 98 * q, pad = PF(5);
	float eh = dh + pad + (card ? ch_h + 2 * pad : 0) + st_h;
	bool g_on = s->wifi_live && s->gear != 'P'; /* the g-meter's place, kept while driving */
	G.pair_top = G.H / 2 - fmaxf(eh, g_on ? g_height() : 0) / 2;
	float y = G.pair_top - q; /* the arc's outer edge 1 unit into the box */
	lv_obj_set_pos(dial, ri(cx - 52 * q), ri(y));
	y -= pad;
	y += dh + 2 * pad;
	if (card) {
		lv_obj_set_pos(charge_box, ri(cx - lv_obj_get_style_width(charge_box, 0) / 2.0f), ri(y));
		y += ch_h + 2 * pad;
	}
	lv_obj_set_pos(stats_box, ri(cx - st_w / 2), ri(y));
	if (CHANGED(soc_pct) || CHANGED(charge_limit_pct) || CHANGED(charging)) lv_obj_invalidate(dial);
}

static void set_g(const dash_state_t *s, float cx)
{
	/* The pedal (main.dart's line along the top) as a labelled bar at the g-meter block's foot, the circle's width:
	 * not on a rear-motor car, whose ring has the pedal and the brake; only with the g-meter's place (WiFi, not in
	 * P). Braking, it is the app's foot brake instead (gauge.dart: kAlert, BRAKE APPLIED, its reach the pressure).
	 * Only the fill and the figure change, so only they redraw. */
	float side = PG(72), bar_y = G.pair_top + g_height() - PEDAL_H, lab_y = bar_y - PG(3) - lv_font_get_line_height(label_font());
	bool bar = s->live && !s->rwd && s->wifi_live && s->gear != 'P', braking = bar && s->brake_pct > 0;
	show(pedal, bar);
	show(pedal_name, bar);
	show(pedal_val, bar);
	if (bar) {
		float pct = braking ? s->brake_pct : s->pedal_pct;
		place(pedal, cx - side / 2, bar_y, side, PEDAL_H);
		float f = side * fmaxf(0, fminf(100, isnan(pct) ? 0 : pct)) / 100;
		show(pedal_fill, f >= 1);
		fill(pedal_fill, braking ? ALERT : ACCENT);
		place(pedal_fill, 0, 0, fmaxf(f, PEDAL_H), PEDAL_H);
		set_text(pedal_name, "%s", braking ? "BRAKE APPLIED" : "PEDAL");
		text_color(pedal_name, braking ? ALERT : W70);
		if (isnan(pct)) set_text(pedal_val, " —");
		else set_text(pedal_val, " %d %%", (int)lroundf(pct));
		/* the name and the figure, centred under the circle */
		lv_point_t a, b;
		lv_text_get_size(&a, lv_label_get_text(pedal_name), label_font(), ri(PF(0.8)), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_text_get_size(&b, lv_label_get_text(pedal_val), label_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		float x = cx - (a.x + b.x) / 2.0f;
		lv_obj_set_pos(pedal_name, ri(x), ri(lab_y));
		lv_obj_set_pos(pedal_val, ri(x + a.x), ri(lab_y));
	}
	bool on = s->wifi_live && s->g_shown && s->gear != 'P';
	show(gball, on);
	show(g_val, on);
	show(g_peak, on);
	if (!on) {
		trail_n = 0;
		return;
	}
	float gx = isnan(s->g_lat) ? 0 : s->g_lat, gy = isnan(s->g_long) ? 0 : s->g_long;
	if (trail_n == TRAIL) memmove(trail, trail + 1, sizeof trail[0] * (TRAIL - 1)), trail_n--;
	trail[trail_n][0] = gx, trail[trail_n][1] = gy, trail_n++;
	char v[16], pk[24];
	snprintf(v, sizeof v, "%.2f", sqrtf(gx * gx + gy * gy));
	part_t p[2] = {TXT(FS(REGULAR, 16, UI_S_FIG), WHITE, v), TXT(FS(REGULAR, 12, UI_S_FIG), W70, " g")}; /* as the dial's figures */
	run_set(g_val, p, 2);
	float peak = fmaxf(s->g_peak_brake, fmaxf(s->g_peak_accel, s->g_peak_lat));
	snprintf(pk, sizeof pk, "PEAK %.2f", peak);
	set_text(g_peak, "%s", pk);
	/* the ball, then its figures centred under it, the block centred in the column's height */
	float h1 = lv_obj_get_style_height(g_val, 0), top = G.pair_top;
	place(gball, cx - side / 2, top, side, side);
	lv_obj_set_pos(g_val, ri(cx - lv_obj_get_style_width(g_val, 0) / 2.0f), ri(top + side + PG(6)));
	align(g_peak, LV_ALIGN_TOP_MID, ri(cx - G.W / 2), ri(top + side + PG(6) + h1));
	if (CHANGED(g_lat) || CHANGED(g_long) || CHANGED(g_peak_brake) || CHANGED(g_peak_accel) || CHANGED(g_peak_lat))
		lv_obj_invalidate(gball);
}

void dash_ui_set(const dash_state_t *s)
{
	S = *s;
	/* glows: indicators green in step with the flasher, blind spot red, brake red at the foot */
	bool gl = s->live && (s->blind_l || s->ind_l), gr = s->live && (s->blind_r || s->ind_r);
	show(glow_l, gl);
	show(glow_r, gr);
	icon_color(glow_l, s->blind_l ? ALERT : REGEN);
	icon_color(glow_r, s->blind_r ? ALERT : REGEN);
	show(glow_b, s->live && s->braking);

	/* The blocks: the car and the route (only with one) left of the ring, the energy figures and the g-meter right of
	 * it. Widths are fixed per block (the widest it gets), so nothing moves as the figures change. A route coming or
	 * going re-spreads the left side only; parking or losing WiFi drops the g-meter's place (while driving it is kept
	 * even with nothing to show, as the app keeps it); Bluetooth alone has no car picture (its lights row keeps a
	 * place). */
	bool route = s->route;
	float q = dial_q(), w[5], sum = 0;
	lv_point_t t1, t2;
	w[0] = s->no_cabin ? PLI(4 * 40) : ui_car_pics[DASH_CAR_Y7].car->header.w + 2 * cb(6);
	w[1] = route ? G.card_w : 0;
	w[2] = 2 * (G.r + G.aw / 2);
	lv_text_get_size(&t1, "88.8 kW", FS(REGULAR, 16, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_text_get_size(&t2, "35 min   12:51 AM", FS(REGULAR, 18, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	w[3] = fmaxf(104 * q, fmaxf(PF(104) + t1.x, t2.x));
	lv_text_get_size(&t1, "0.88 g", FS(REGULAR, 16, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_text_get_size(&t2, "PEAK 0.88", FS(REGULAR, 12, UI_S_FIG), ri(PF(0.8)), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	w[4] = s->wifi_live && s->gear != 'P' ? fmaxf(PG(72), fmaxf(t1.x, t2.x)) : 0; /* the ball over its figures */
	for (int i = 0; i < 5; i++) sum += w[i];
	/* The chips step in towards the top: bound by those level with the car picture (the first sits over it, beside
	 * the lights row) and with the g-meter's ball. */
	float car_top = G.mt + PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS)) + PLI(4) + cb(8);
	float chips_r = 0, chips_l = G.W;
	for (int i = 0; i < 10; i++) {
		float cx, cy;
		chip_geom(i, &cx, &cy);
		if (i < 5 && cy + PC(42) > car_top) chips_r = fmaxf(chips_r, cx + PC(52));
		if (i == 0) chips_r = fmaxf(chips_r, G.ml + inset(G.link_top + PL(22)) + 2 * PL(60)); /* the links' row, widest */
		if (i >= 5 && cy < G.H / 2 + g_height() / 2 && cy + PC(42) > G.H / 2 - g_height() / 2) chips_l = fminf(chips_l, cx);
	}
	/* The ring fixed at the centre (owner); each side's blocks spread evenly between it and the chips. */
	G.cx = G.W / 2;
	float at[5];
	for (int side = 0; side < 2; side++) {
		int i0 = side ? 3 : 0, i1 = side ? 5 : 2, n = 0;
		float from = side ? G.cx + w[2] / 2 : chips_r, to = side ? chips_l : G.cx - w[2] / 2, used = 0;
		for (int i = i0; i < i1; i++) used += w[i], n += w[i] > 0;
		float gap = (to - from - used) / (n + 1), x = from + gap;
		for (int i = i0; i < i1; i++) {
			at[i] = x;
			if (w[i] > 0) x += w[i] + gap;
		}
	}
	(void)sum;
	ring_place();

	set_chips(s);
	set_links(s);
	show(dest_card, route);
	if (route) set_route(s, at[1], G.H / 2);
	/* the lights row at the top, the car under it down to the foot */
	float car_cx = at[0] + w[0] / 2;
	set_lights(s, car_cx, G.mt);
	set_cabin(s, car_cx, G.mt + PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS)) + PLI(4));

	set_ring(s);
	ring_dirty(s);
	set_energy(s, at[3] + w[3] / 2);
	set_g(s, at[4] + w[4] / 2);
	P = *s;
	first = false;
}

void dash_state_blank(dash_state_t *s)
{
	memset(s, 0, sizeof *s);
	s->kph = true;
	s->speed_hz = -1;
	s->speed = s->front_kw = s->rear_kw = s->pedal_pct = s->brake_pct = s->regen_cap_kw = NAN;
	s->speed_limit = s->cruise_set = s->outside_c = s->charge_kw = s->charge_max_kw = NAN;
	s->follow_gap = s->charge_limit_pct = -1;
	s->charge_minutes = s->nav_minutes = s->co2_ppm = -1;
	s->soc_pct = s->range = s->pack_temp_c = s->precond_c = s->climate_kw = s->degradation_pct = NAN;
	s->minor_figures = true; /* the owner's default on the Dash */
	s->lv_kw = s->other_kw = s->imbalance_mv = NAN;
	s->g_lat = s->g_long = NAN;
	for (int i = 0; i < 4; i++) s->tyre_psi[i] = NAN;
	for (int i = 0; i < 3; i++) s->cabin_c[i] = NAN;
	s->route_len = s->route_kwh = s->arrive_range = s->range_drop = s->arrive_pct = s->pct_drop = s->arrive_kwh = NAN;
}
