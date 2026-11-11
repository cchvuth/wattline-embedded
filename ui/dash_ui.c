/* The dash in LVGL: the tesla-dash app's look (lib/main.dart, widgets.dart, gauge.dart, glyphs.dart), arranged
 * for the Dash's trapezoid screen (PLAN-merged.md 5.2). Sizes are the app's, in dp, through px() (1.25 px per dp
 * at H = 480); the app's names are kept in comments so each piece can be checked against its widget. The icons
 * and car pictures are the app's own renders (ui/assets/export.sh), the text is Roboto through tiny_ttf. */
#include "dash_ui.h"
#include "gen/assets.h"
#include "../core/cmdr_msg.h" /* cmdr_action: what a tap asks for */
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
static const char *const act_label[ACT_COUNT] = { /* commander.dart S3xyAction */
	[ACT_FRUNK] = "Frunk", [ACT_TRUNK] = "Trunk", [ACT_LIGHT] = "Light", [ACT_MIRROR_TILT] = "Mirror tilt",
	[ACT_RECIRC_ON] = "Recirculation", [ACT_RECIRC_OFF] = "Fresh air", [ACT_RECIRC_TOGGLE] = "Recirculation",
	[ACT_DOOR_FL] = "Front left door", [ACT_DOOR_FR] = "Front right door", [ACT_DOOR_RL] = "Rear left door",
	[ACT_DOOR_RR] = "Rear right door", [ACT_UNLOCK] = "Unlock", [ACT_LOCK] = "Lock", [ACT_CHARGE_PORT] = "Charge port",
	[ACT_HONK] = "Honk", [ACT_FLASH] = "Flash", [ACT_CLIMATE] = "Climate",
};
/* The chips, in the order they are built (widgets.dart _Actions), and their tooltips (the app's labels). */
static const int chip_act[10] = {ACT_TRUNK, ACT_MIRROR_TILT, ACT_DOOR_FL, ACT_DOOR_RL, ACT_CHARGE_PORT,
				 ACT_FRUNK, ACT_RECIRC_TOGGLE, ACT_DOOR_FR, ACT_DOOR_RR, ACT_LIGHT};
static const char *const chip_tip[10] = {"Trunk", "Mirror tilt", "Front left door", "Rear left door", "Charge port",
					 "Frunk", "Recirculation", "Front right door", "Rear right door", "Interior lights"};

static lv_obj_t *steer;
static lv_obj_t *pair_view, *pair_qr, *pair_how, *pair_url_l, *pair_code_l, *pair_left, *pair_or, *pair_cmd, *pair_res, *pair_btn;
static void steer_draw(lv_event_t *e);
static void touch_init(void);
static void set_touch(const dash_state_t *s);

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
	/* The ring: the radius the app's foot brake left room for (r 247 at 480, 1.28 x the app's at 1536 dp; the owner
	 * keeps it), centred on the screen both ways now that the foot brake is under the g-meter. At 480 its arcs end
	 * 26 px from the top and bottom, the limit sign starts at 36, the gear letter ends at 469. */
	G.r = (G.H - 12 * G.k) / (0.83f * 1.0425f + 1.03f);
	G.cy = G.H / 2;
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
	steer = box(root); /* under the car, as the app paints the path lines */
	lv_obj_add_event_cb(steer, steer_draw, LV_EVENT_DRAW_MAIN, NULL);
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
	touch_init();
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
		bool pending = s->press_pending == chip_act[i]; /* a press out: its side in the accent until the answer */
		fill(c->box, on ? f : 0);
		border(c->box, pending ? ACCENT : on ? f : W70, pending ? PC(2) : PC(1));
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

/* The Commander link's word, colour and icon (widgets.dart _Controls). */
static const char *bt_word(dash_bt_t b, uint32_t *c, const lv_image_dsc_t **i)
{
	static const struct { const lv_image_dsc_t *i; uint32_t c; const char *w; } t[] = {
		[BT_IDLE] = {&ic_bt_off_l, W54, "IDLE"}, [BT_OFF] = {&ic_bt_off_l, ALERT, "OFF"},
		[BT_NO_LOCATION] = {&ic_bt_off_l, ALERT, "NO LOC"}, [BT_SCANNING] = {&ic_bt_find_l, ACCENT, "SCAN"},
		[BT_LINKING] = {&ic_bt_find_l, ACCENT, "LINK"}, [BT_APPROVING] = {&ic_bt_find_l, WARN, "PRESS"},
		[BT_REFUSED] = {&ic_bt_off_l, ALERT, "DENIED"}, [BT_DASH] = {&ic_bt_plain_l, W54, "DASH"},
		[BT_PAUSED] = {&ic_bt_find_l, WARN, "WAIT"}, [BT_AWAY] = {&ic_bt_find_l, W54, "AWAY"},
		[BT_HELD] = {&ic_bt_off_l, ALERT, "HELD"}, [BT_RETRYING] = {&ic_bt_find_l, WARN, "RETRY"},
		[BT_BORROWED] = {&ic_bt_l, ACCENT, "BORROW"}, [BT_BUTTONS] = {&ic_bt_l, ACCENT, "ACTION"},
		[BT_LEAN] = {&ic_bt_l, REGEN, "READY"}, [BT_FULL] = {&ic_bt_l, REGEN, "LIVE"},
	};
	if ((unsigned)b >= sizeof t / sizeof t[0]) b = BT_IDLE;
	*c = t[b].c;
	if (i) *i = t[b].i;
	return t[b].w;
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
	uint32_t bc;
	const lv_image_dsc_t *bi;
	const char *bw = bt_word(s->bt, &bc, &bi);
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
	place(steer, cab.x - cab.w, 0, 3 * cab.w, G.H); /* the path lines' room: redrawn only as the wheel turns */
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

/* ---------- touch: chips, snackbar, car panel, settings (main.dart _press, _say, _linksChanged; car_panel.dart) ----------
 * Material 3 dark, as the app's ThemeData.dark(useMaterial3: true) draws its snackbar, dialog, switches and slider. */
#define PB(dp) (px(dp) * UI_S_PANEL) /* the panels: bigger again, for a finger in a moving car */
#define M3_SURFACE 0xFF141218u
#define M3_DIALOG 0xFF2B2930u       /* surfaceContainerHigh */
#define M3_ON_SURFACE 0xFFE6E0E9u
#define M3_INVERSE 0xFFE6E0E9u      /* the snackbar */
#define M3_ON_INVERSE 0xFF322F35u
#define M3_PRIMARY 0xFFD0BCFFu
#define M3_ON_PRIMARY 0xFF381E72u
#define M3_OUTLINE 0xFF938F99u
#define M3_FIELD 0xFF1D1B20u
#define M3_KEY 0xFF49454Fu

static dash_action_cb act_cb;
static void *act_ctx;
static dash_settings_cb set_cb;
static void *set_ctx;
static dash_settings_t ST = {.minor_figures = true, .brightness_pct = 80, .follow_car = true};
static int view = DASH_VIEW_DASH, edit_slot = -1;

static lv_obj_t *tip, *snack, *snack_wheel, *snack_text;
static lv_obj_t *panel, *panel_msg, *settings, *set_status, *set_sw[4], *set_slider, *set_bl_note, *net_list, *net_edit, *net_ta[2],
	*net_kb, *net_err;
static lv_timer_t *tip_timer;

void dash_ui_set_action_cb(dash_action_cb cb, void *ctx) { act_cb = cb, act_ctx = ctx; }
void dash_ui_set_settings_cb(dash_settings_cb cb, void *ctx) { set_cb = cb, set_ctx = ctx; }

static void press(int a)
{
	if (act_cb) act_cb(a, act_ctx);
}
static void save(int what)
{
	if (set_cb) set_cb(what, &ST, set_ctx);
}

/* A tap target: no style of its own, a finger's room round it. */
static void tappable(lv_obj_t *o, lv_event_cb_t cb, intptr_t arg, float room)
{
	lv_obj_set_clickable(o, true);
	lv_obj_set_ext_click_area(o, ri(room));
	lv_obj_add_event_cb(o, cb, LV_EVENT_SHORT_CLICKED, (void *)arg);
}
static void pressed_look(lv_obj_t *o, uint32_t c) /* InkWell's highlight while held */
{
	lv_obj_set_style_bg_color(o, rgb(c), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(o, opa(c), LV_STATE_PRESSED);
}

/* Chips: a tap presses (once: the app guards and never resends); a long press names it (Tooltip). */
static void chip_tap(lv_event_t *e) { press(chip_act[(intptr_t)lv_event_get_user_data(e)]); }
static void tip_hide(lv_timer_t *t)
{
	(void)t;
	show(tip, false);
	lv_timer_pause(tip_timer);
}
static void chip_hold(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
		lv_timer_reset(tip_timer);
		lv_timer_resume(tip_timer); /* the tooltip stays a moment after the finger lifts */
		return;
	}
	float x, y;
	chip_geom(i, &x, &y);
	set_text(lv_obj_get_child(tip, 0), "%s", chip_tip[i]);
	align(lv_obj_get_child(tip, 0), LV_ALIGN_CENTER, 0, 0);
	lv_obj_update_layout(tip);
	float w = lv_obj_get_width(tip), h = lv_obj_get_height(tip);
	float tx = i < 5 ? x + PC(52) + PC(8) : x - PC(8) - w; /* beside the chip, towards the middle */
	lv_obj_set_pos(tip, ri(tx), ri(y + PC(21) - h / 2));
	show(tip, true);
	lv_timer_pause(tip_timer);
}

/* ---- the car panel (car_panel.dart): its own page, an app bar, the groups of outlined buttons, a message line ---- */
static void close_tap(lv_event_t *e) { (void)e, dash_ui_show(DASH_VIEW_DASH); }
static void panel_tap(lv_event_t *e) { press((intptr_t)lv_event_get_user_data(e)); }
static void open_tap(lv_event_t *e) { dash_ui_show((intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *outlined(lv_obj_t *parent, const lv_image_dsc_t *ic, const char *text, lv_event_cb_t cb, intptr_t arg)
{
	/* OutlinedButton(.icon): 40 dp, a stadium, white on a white70 side, 18 dp icon, 14 sp label (w500) */
	lv_obj_t *b = box(parent);
	lv_obj_set_height(b, ri(PB(40)));
	lv_obj_set_width(b, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	border(b, W70, PB(1));
	pressed_look(b, 0x1FFFFFFF);
	lv_obj_set_style_pad_left(b, ri(PB(ic ? 16 : 24)), 0);
	lv_obj_set_style_pad_right(b, ri(PB(24)), 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(b, ri(PB(8)), 0);
	if (ic) icon(b, ic, WHITE);
	lv_obj_t *l = label(b, FS(MEDIUM, 14, UI_S_PANEL), WHITE);
	lv_obj_set_style_text_letter_space(l, ri(PB(0.1)), 0);
	lv_label_set_text(l, text);
	tappable(b, cb, arg, PB(4));
	return b;
}
static lv_obj_t *group_title(lv_obj_t *parent, const char *t) /* 12 sp, spaced 2, white70 */
{
	lv_obj_t *l = label(parent, FS(REGULAR, 12, UI_S_PANEL), W70);
	lv_obj_set_style_text_letter_space(l, ri(PB(2)), 0);
	lv_label_set_text(l, t);
	return l;
}
static lv_obj_t *row(lv_obj_t *parent) /* Wrap(spacing 8) */
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(r, ri(PB(8)), 0);
	lv_obj_set_style_pad_row(r, ri(PB(8)), 0);
	return r;
}
static lv_obj_t *column(lv_obj_t *parent, float gap)
{
	lv_obj_t *c = box(parent);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, ri(gap), 0);
	return c;
}

static void panel_build(void)
{
	float left = G.ml + inset(0) + PB(8); /* clear of the hidden top corners all the way down */
	panel = box(lv_layer_top());
	place(panel, 0, 0, G.W, G.H);
	fill(panel, BLACK);
	lv_obj_set_clickable(panel, true); /* the page takes every touch while open */
	lv_obj_t *bar = box(panel);
	place(bar, 0, 0, G.W, PB(64) * 0.8f);
	fill(bar, M3_SURFACE);
	lv_obj_t *back = icon(bar, &ic_p_back, M3_ON_SURFACE);
	align(back, LV_ALIGN_LEFT_MID, ri(left + PB(4)), 0);
	tappable(back, close_tap, 0, PB(16));
	lv_obj_t *title = label(bar, FS(REGULAR, 22, UI_S_PANEL), M3_ON_SURFACE);
	lv_label_set_text(title, "Car controls");
	align(title, LV_ALIGN_LEFT_MID, ri(left + PB(24 + 32)), 0);
	lv_obj_t *body = column(panel, PB(10));
	place(body, left, PB(64) * 0.8f + PB(14), G.W - 2 * left, G.H - PB(64) * 0.8f - PB(14));
	group_title(body, "Locks");
	lv_obj_t *r = row(body);
	outlined(r, &ic_p_lock_open, "Unlock", panel_tap, ACT_UNLOCK);
	outlined(r, &ic_p_lock, "Lock", panel_tap, ACT_LOCK);
	lv_obj_set_style_margin_bottom(r, ri(PB(6)), 0);
	group_title(body, "Climate & misc");
	r = row(body);
	outlined(r, &ic_p_climate, "Climate", panel_tap, ACT_CLIMATE);
	outlined(r, &ic_p_flash, "Flash", panel_tap, ACT_FLASH);
	outlined(r, &ic_p_honk, "Honk", panel_tap, ACT_HONK);
	lv_obj_set_style_margin_bottom(r, ri(PB(6)), 0);
	panel_msg = label(body, FS(REGULAR, 14, UI_S_PANEL), WHITE);
	show(panel, false);
}

/* ---- settings: the Commander, the display, the networks (an AlertDialog of the app's style) ---- */
static void approve_tap(lv_event_t *e) { (void)e, save(DASH_CMDR_APPROVE); }
static void pair_tap(lv_event_t *e)
{
	(void)e;
	save(DASH_PAIR_START);
	dash_ui_show(DASH_VIEW_PAIR);
}
static void forget_tap(lv_event_t *e) { (void)e, save(DASH_CMDR_FORGET); }
static void slider_look(void);
static void switch_changed(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	if (i == 0) ST.minor_figures = on;
	else if (i == 1) ST.hour12 = !on; /* the switch says 24-hour */
	else if (i == 2) ST.dev_ssh = on;
	else ST.follow_car = on, slider_look();
	save(DASH_SETTINGS_SAVE);
}
/* Following the car, the slider shows the live backlight greyed; a finger on it takes over (follow off). */
static void slider_look(void)
{
	uint32_t c = ST.follow_car ? M3_OUTLINE : M3_PRIMARY;
	lv_obj_set_style_bg_color(set_slider, rgb(c), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(set_slider, rgb(c), LV_PART_KNOB);
	set_text(set_bl_note, "%s", ST.follow_car ? "following the car" : "");
	if (!ST.follow_car) lv_slider_set_value(set_slider, ST.brightness_pct, LV_ANIM_OFF);
}
static void slider_changed(lv_event_t *e)
{
	if (ST.follow_car) {
		ST.follow_car = false;
		lv_obj_set_state(set_sw[3], LV_STATE_CHECKED, false);
		ST.brightness_pct = lv_slider_get_value(lv_event_get_target(e));
		slider_look();
	}
	ST.brightness_pct = lv_slider_get_value(lv_event_get_target(e));
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) save(DASH_SETTINGS_SAVE);
}
static lv_obj_t *m3_switch(lv_obj_t *parent, int i)
{
	/* Switch: 52 x 32 dp; on, the primary track and an onPrimary thumb; off, an outlined dark track */
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_size(sw, ri(PB(52)), ri(PB(32)));
	lv_obj_set_style_bg_color(sw, rgb(M3_KEY), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_color(sw, rgb(M3_OUTLINE), LV_PART_MAIN);
	lv_obj_set_style_border_width(sw, ri(PB(2)), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, rgb(M3_PRIMARY), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, rgb(M3_OUTLINE), LV_PART_KNOB);
	lv_obj_set_style_bg_color(sw, rgb(M3_ON_PRIMARY), LV_PART_KNOB | LV_STATE_CHECKED);
	lv_obj_set_style_pad_all(sw, ri(-PB(2)), LV_PART_KNOB);
	lv_obj_set_ext_click_area(sw, ri(PB(10)));
	lv_obj_add_event_cb(sw, switch_changed, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
	return sw;
}
static lv_obj_t *switch_row(lv_obj_t *parent, const char *text, int i, float w)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), LV_SIZE_CONTENT);
	lv_obj_t *l = label(r, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE);
	lv_label_set_text(l, text);
	lv_obj_set_width(l, ri(w - PB(52) - PB(16)));
	align(l, LV_ALIGN_LEFT_MID, 0, 0);
	set_sw[i] = m3_switch(r, i);
	align(set_sw[i], LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_min_height(r, ri(PB(40)), 0);
	return r;
}
static void net_tap(lv_event_t *e) /* a network's row: replace it */
{
	edit_slot = (intptr_t)lv_event_get_user_data(e);
	dash_ui_show(DASH_VIEW_NETWORK);
}
static void net_delete(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i >= ST.net_count) return;
	memmove(&ST.net[i], &ST.net[i + 1], sizeof ST.net[0] * (ST.net_count - i - 1));
	ST.net_count--;
	memset(&ST.net[ST.net_count], 0, sizeof ST.net[0]);
	save(DASH_SETTINGS_SAVE);
	dash_ui_show(DASH_VIEW_SETTINGS); /* the list again */
}
static void net_row(lv_obj_t *parent, const char *ssid, int i, float w)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), ri(PB(26)));
	lv_obj_t *ic = icon(r, i < 0 ? &ic_p_fixed : &ic_p_net, W70);
	align(ic, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_t *l = label(r, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(l, ri(w - 2 * PB(18) - 2 * PB(12)));
	lv_label_set_text(l, ssid);
	align(l, LV_ALIGN_LEFT_MID, ri(PB(18) + PB(12)), 0);
	if (i < 0) {
		lv_obj_t *n = label(r, FS(REGULAR, 12, UI_S_PANEL), W54);
		lv_label_set_text(n, "the car's, built in");
		align(n, LV_ALIGN_RIGHT_MID, 0, 0);
		return;
	}
	pressed_look(r, 0x1FFFFFFF);
	tappable(r, net_tap, i, PB(2));
	lv_obj_t *del = icon(r, &ic_p_delete, W70);
	align(del, LV_ALIGN_RIGHT_MID, 0, 0);
	tappable(del, net_delete, i, PB(12));
}
static void settings_fill_nets(void)
{
	float w = lv_obj_get_style_width(net_list, 0);
	lv_obj_clean(net_list);
	group_title(net_list, "NETWORK");
	net_row(net_list, "S3XY_OBD", -1, w); /* the car's Commander: fixed */
	for (int i = 0; i < ST.net_count; i++) net_row(net_list, ST.net[i].ssid, i, w); /* never the passphrase */
	if (ST.net_count < DASH_NETS) lv_obj_set_height(outlined(net_list, &ic_p_add, "Add network", net_tap, ST.net_count), ri(PB(32)));
}
static void settings_build(void)
{
	float top = PB(6), x0 = G.ml + inset(top) + PB(6), w = G.W - 2 * x0, h = G.H - 2 * top, pad = PB(20);
	settings = box(lv_layer_top());
	place(settings, 0, 0, G.W, G.H);
	fill(settings, 0x8A000000); /* the dialog's barrier, black54 */
	lv_obj_set_clickable(settings, true);
	lv_obj_t *d = box(settings);
	place(d, x0, top, w, h);
	fill(d, M3_DIALOG);
	lv_obj_set_style_radius(d, ri(px(28)), 0);
	lv_obj_t *title = label(d, FS(REGULAR, 24, UI_S_PANEL * 0.8f), M3_ON_SURFACE);
	lv_label_set_text(title, "Settings");
	lv_obj_set_pos(title, ri(pad + PB(4)), ri(pad * 0.7f));
	lv_obj_t *x = icon(d, &ic_p_close, M3_ON_SURFACE);
	align(x, LV_ALIGN_TOP_RIGHT, ri(-pad), ri(pad * 0.7f));
	tappable(x, close_tap, 0, PB(16));
	float cw = (w - 2 * pad - 3 * PB(20)) / 4, gap = PB(20), cy = pad * 0.7f + PB(24) * 0.8f * 1.17f + PB(10);
	/* the Commander */
	lv_obj_t *c = column(d, PB(8));
	place(c, pad, cy, cw, h - cy - PB(8));
	group_title(c, "COMMANDER");
	set_status = label(c, FS(MEDIUM, 14, UI_S_PANEL), WHITE);
	outlined(c, NULL, "Approve this Dash", approve_tap, 0);
	lv_obj_t *hint = label(c, FS(REGULAR, 12, UI_S_PANEL), W70);
	lv_obj_set_width(hint, ri(cw));
	lv_label_set_text(hint, "Tap, then double-press the right scroll wheel in the car when it asks.");
	outlined(c, NULL, "Forget Commander", forget_tap, 0);
	/* the display */
	c = column(d, PB(3));
	place(c, pad + cw + gap, cy, cw, h - cy - PB(8));
	group_title(c, "DISPLAY");
	switch_row(c, "Show 12 V, other power and cell imbalance", 0, cw);
	switch_row(c, "24-hour clock", 1, cw);
	switch_row(c, "Follow car screen brightness", 3, cw);
	lv_obj_t *br = box(c); /* "Brightness", and what it does now */
	lv_obj_set_size(br, ri(cw), LV_SIZE_CONTENT);
	lv_obj_t *bl = label(br, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE);
	lv_label_set_text(bl, "Brightness");
	set_bl_note = label(br, FS(REGULAR, 12, UI_S_PANEL), W54);
	align(set_bl_note, LV_ALIGN_RIGHT_MID, 0, 0);
	set_slider = lv_slider_create(c);
	lv_obj_set_size(set_slider, ri(cw - PB(16)), ri(PB(6)));
	lv_obj_set_style_margin_left(set_slider, ri(PB(8)), 0);
	lv_slider_set_range(set_slider, 5, 100);
	lv_obj_set_style_bg_color(set_slider, rgb(M3_KEY), LV_PART_MAIN);
	lv_obj_set_style_bg_color(set_slider, rgb(M3_PRIMARY), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(set_slider, rgb(M3_PRIMARY), LV_PART_KNOB);
	lv_obj_set_style_pad_all(set_slider, ri(PB(7)), LV_PART_KNOB);
	lv_obj_set_ext_click_area(set_slider, ri(PB(16)));
	lv_obj_add_event_cb(set_slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(set_slider, slider_changed, LV_EVENT_RELEASED, NULL);
	/* the networks */
	net_list = column(d, PB(6));
	place(net_list, pad + 2 * (cw + gap), cy, cw, h - cy - PB(8));
	/* developer access */
	c = column(d, PB(10));
	place(c, pad + 3 * (cw + gap), cy, cw, h - cy - PB(8));
	group_title(c, "DEVELOPER");
	switch_row(c, "Developer SSH", 2, cw);
	lv_obj_t *note = label(c, FS(REGULAR, 12, UI_S_PANEL), W54);
	lv_obj_set_width(note, ri(cw));
	lv_label_set_text(note, "Needs an authorized key: pair a computer, or put one on the card");
	outlined(c, NULL, "Pair a computer", pair_tap, 0); /* only from this tap: someone at the Dash */
	show(settings, false);
}

/* ---- a network's SSID and passphrase, typed on the on-screen keyboard ---- */
static void net_save(lv_event_t *e)
{
	(void)e;
	const char *ssid = lv_textarea_get_text(net_ta[0]), *pass = lv_textarea_get_text(net_ta[1]);
	size_t pl = strlen(pass);
	bool replacing = edit_slot < ST.net_count;
	if (!ssid[0] || strlen(ssid) > 32) set_text(net_err, "The name: 1 to 32 characters");
	else if (!strcmp(ssid, "S3XY_OBD")) set_text(net_err, "S3XY_OBD is the car's, built in");
	else if ((pl || !replacing) && (pl < 8 || pl > 63)) set_text(net_err, "The passphrase: 8 to 63 characters");
	else {
		dash_net_t *n = &ST.net[edit_slot];
		snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
		snprintf(n->pass, sizeof n->pass, "%s", pass); /* "" when replacing: keep the one it has */
		if (!replacing) ST.net_count++;
		save(DASH_SETTINGS_SAVE);
		memset(n->pass, 0, sizeof n->pass); /* handed over; not kept here */
		lv_textarea_set_text(net_ta[1], "");
		dash_ui_show(DASH_VIEW_SETTINGS);
	}
}
static void net_cancel(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(net_ta[1], "");
	dash_ui_show(DASH_VIEW_SETTINGS);
}
static void ta_focus(lv_event_t *e) { lv_keyboard_set_textarea(net_kb, lv_event_get_target(e)); }
static void kb_done(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_READY) net_save(e);
	else net_cancel(e);
}
static void net_edit_build(void)
{
	float top = PB(6), x0 = G.ml + inset(top) + PB(6), w = G.W - 2 * x0, pad = PB(16);
	net_edit = box(lv_layer_top());
	place(net_edit, 0, 0, G.W, G.H);
	fill(net_edit, M3_DIALOG);
	lv_obj_set_clickable(net_edit, true);
	const char *hint[2] = {"Network name (SSID)", "Passphrase"};
	float fw = (w - 3 * pad - 2 * PB(110)) / 2;
	for (int i = 0; i < 2; i++) {
		lv_obj_t *ta = net_ta[i] = lv_textarea_create(net_edit);
		lv_textarea_set_one_line(ta, true);
		lv_textarea_set_max_length(ta, i ? 63 : 32);
		lv_textarea_set_placeholder_text(ta, hint[i]);
		if (i) lv_textarea_set_password_mode(ta, true); /* masked as typed */
		place(ta, x0 + i * (fw + pad), top + pad, fw, PB(46));
		lv_obj_set_style_text_font(ta, FS(REGULAR, 16, UI_S_PANEL), 0);
		lv_obj_set_style_text_color(ta, rgb(M3_ON_SURFACE), 0);
		lv_obj_set_style_bg_color(ta, rgb(M3_FIELD), 0);
		lv_obj_set_style_border_color(ta, rgb(M3_OUTLINE), 0);
		lv_obj_set_style_border_color(ta, rgb(M3_PRIMARY), LV_STATE_FOCUSED);
		lv_obj_set_style_text_color(ta, rgb(M3_OUTLINE), LV_PART_TEXTAREA_PLACEHOLDER);
		lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_FOCUSED, NULL);
		lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_CLICKED, NULL);
	}
	lv_obj_t *b = outlined(net_edit, NULL, "Cancel", (lv_event_cb_t)net_cancel, 0);
	lv_obj_set_pos(b, ri(x0 + 2 * (fw + pad)), ri(top + pad));
	b = outlined(net_edit, NULL, "Save", (lv_event_cb_t)net_save, 0);
	lv_obj_set_pos(b, ri(x0 + 2 * (fw + pad) + PB(110) + pad / 2), ri(top + pad));
	net_err = label(net_edit, FS(REGULAR, 12, UI_S_PANEL), 0xFFF2B8B5); /* M3 dark error */
	lv_obj_set_pos(net_err, ri(x0), ri(top + pad + PB(48)));
	net_kb = lv_keyboard_create(net_edit);
	float ky = top + pad + PB(48) + PB(18);
	lv_obj_set_size(net_kb, ri(w), ri(G.H - ky - top));
	align(net_kb, LV_ALIGN_TOP_LEFT, ri(x0), ri(ky));
	lv_obj_set_style_bg_color(net_kb, rgb(M3_DIALOG), 0);
	lv_obj_set_style_bg_color(net_kb, rgb(M3_KEY), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(net_kb, rgb(M3_OUTLINE), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(net_kb, rgb(0xFF332D41), LV_PART_ITEMS | LV_STATE_CHECKED); /* its control keys */
	lv_obj_set_style_text_color(net_kb, rgb(M3_ON_SURFACE), LV_PART_ITEMS | LV_STATE_CHECKED);
	((lv_font_t *)FS(REGULAR, 16, UI_S_PANEL))->fallback = &lv_font_montserrat_28; /* the keys' symbols */
	lv_obj_set_style_text_color(net_kb, rgb(M3_ON_SURFACE), LV_PART_ITEMS);
	lv_obj_set_style_text_font(net_kb, FS(REGULAR, 16, UI_S_PANEL), LV_PART_ITEMS);
	lv_obj_set_style_border_width(net_kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(net_kb, 0, LV_PART_ITEMS);
	lv_obj_add_event_cb(net_kb, kb_done, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(net_kb, kb_done, LV_EVENT_CANCEL, NULL);
	show(net_edit, false);
}

void dash_ui_settings(const dash_settings_t *s)
{
	ST = *s;
	for (int i = 0; i < DASH_NETS; i++) memset(ST.net[i].pass, 0, sizeof ST.net[i].pass); /* never kept for show */
	if (view == DASH_VIEW_SETTINGS) dash_ui_show(DASH_VIEW_SETTINGS);
}

void dash_ui_show(int v)
{
	view = v;
	show(panel, v == DASH_VIEW_CAR_PANEL);
	show(settings, v == DASH_VIEW_SETTINGS);
	show(net_edit, v == DASH_VIEW_NETWORK);
	show(pair_view, v == DASH_VIEW_PAIR);
	show(tip, false);
	if (v == DASH_VIEW_SETTINGS) {
		lv_obj_set_state(set_sw[0], LV_STATE_CHECKED, ST.minor_figures);
		lv_obj_set_state(set_sw[1], LV_STATE_CHECKED, !ST.hour12);
		lv_obj_set_state(set_sw[2], LV_STATE_CHECKED, ST.dev_ssh);
		lv_obj_set_state(set_sw[3], LV_STATE_CHECKED, ST.follow_car);
		lv_slider_set_value(set_slider, ST.follow_car && S.backlight_pct >= 0 ? S.backlight_pct : ST.brightness_pct, LV_ANIM_OFF);
		slider_look();
		settings_fill_nets();
	}
	if (v == DASH_VIEW_NETWORK) {
		if (edit_slot < 0 || edit_slot > ST.net_count || edit_slot >= DASH_NETS) edit_slot = ST.net_count;
		lv_textarea_set_text(net_ta[0], edit_slot < ST.net_count ? ST.net[edit_slot].ssid : "");
		lv_textarea_set_text(net_ta[1], "");
		lv_textarea_set_placeholder_text(net_ta[1], edit_slot < ST.net_count ? "Passphrase (empty: keep)" : "Passphrase");
		set_text(net_err, "");
		lv_keyboard_set_textarea(net_kb, net_ta[0]);
	}
}

/* ---- pairing a computer for SSH: the QR code of the one-time URL, the code, the time left, the command ---- */
static void pair_cancel(lv_event_t *e)
{
	(void)e;
	if (S.pair_open) save(DASH_PAIR_CANCEL);
	dash_ui_show(DASH_VIEW_SETTINGS);
}
static void pair_build(void)
{
	float top = PB(6), x0 = G.ml + inset(top) + PB(10), pad = PB(16), qs = G.H - 2 * top - 2 * pad;
	pair_view = box(lv_layer_top());
	place(pair_view, 0, 0, G.W, G.H);
	fill(pair_view, M3_DIALOG);
	lv_obj_set_clickable(pair_view, true);
	lv_obj_t *qbg = box(pair_view); /* the code dark on white, a quiet zone round it */
	place(qbg, x0, top + pad, qs, qs);
	fill(qbg, WHITE);
	lv_obj_set_style_radius(qbg, ri(PB(8)), 0);
	pair_qr = lv_qrcode_create(qbg);
	lv_qrcode_set_size(pair_qr, ri(qs - 2 * PB(14)));
	lv_qrcode_set_dark_color(pair_qr, rgb(BLACK));
	lv_qrcode_set_light_color(pair_qr, rgb(WHITE));
	align(pair_qr, LV_ALIGN_CENTER, 0, 0);
	lv_obj_t *c = column(pair_view, PB(8));
	float cx = x0 + qs + PB(28);
	place(c, cx, top + pad, G.W - cx - x0, G.H - 2 * top - 2 * pad);
	lv_obj_t *title = label(c, FS(REGULAR, 24, UI_S_PANEL * 0.8f), M3_ON_SURFACE);
	lv_label_set_text(title, "Pair a computer");
	pair_how = label(c, FS(REGULAR, 14, UI_S_PANEL), W70);
	lv_label_set_text(pair_how, "On the same WiFi, scan the code or open:");
	pair_url_l = label(c, FS(MEDIUM, 14, UI_S_PANEL), M3_PRIMARY);
	lv_obj_t *r = box(c);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(r, ri(PB(20)), 0);
	pair_code_l = label(r, font_px(LIGHT, ri(PB(40))), WHITE);
	lv_obj_set_style_text_letter_space(pair_code_l, ri(PB(4)), 0);
	pair_left = label(r, FS(REGULAR, 14, UI_S_PANEL), W70);
	pair_or = label(c, FS(REGULAR, 12, UI_S_PANEL), W70);
	lv_label_set_text(pair_or, "Or from the computer:");
	pair_cmd = label(c, FS(REGULAR, 12, UI_S_PANEL), M3_ON_SURFACE);
	lv_obj_set_width(pair_cmd, LV_PCT(100));
	lv_label_set_long_mode(pair_cmd, LV_LABEL_LONG_MODE_WRAP);
	pair_res = label(c, FS(MEDIUM, 14, UI_S_PANEL), WHITE);
	pair_btn = outlined(pair_view, NULL, "Cancel", pair_cancel, 0);
	align(pair_btn, LV_ALIGN_BOTTOM_RIGHT, ri(-x0), ri(-top - pad));
	show(pair_view, false);
}
static void set_pair(const dash_state_t *s)
{
	if (view != DASH_VIEW_PAIR) return;
	if (CHANGED(pair_url) && s->pair_url[0]) lv_qrcode_update(pair_qr, s->pair_url, strlen(s->pair_url));
	show(lv_obj_get_parent(pair_qr), s->pair_open);
	show(pair_url_l, s->pair_open), show(lv_obj_get_parent(pair_code_l), s->pair_open);
	show(pair_cmd, s->pair_open), show(pair_or, s->pair_open), show(pair_how, s->pair_open);
	set_text(pair_url_l, "%s", s->pair_open ? s->pair_url : "");
	set_text(pair_code_l, "%s", s->pair_open ? s->pair_code : "");
	if (s->pair_open) set_text(pair_left, "%d:%02d left", s->pair_secs / 60, s->pair_secs % 60);
	else set_text(pair_left, "");
	if (s->pair_open) {
		char cmd[200];
		snprintf(cmd, sizeof cmd, "curl -fsS --data-binary @$HOME/.ssh/id_ed25519.pub \"%s\"", s->pair_url);
		set_text(pair_cmd, "%s", cmd);
	} else
		set_text(pair_cmd, "");
	set_text(pair_res, "%s", s->pair_result);
	text_color(pair_res, !strncmp(s->pair_result, "Key added", 9) ? REGEN : s->pair_result[0] ? WARN : WHITE);
	set_text(lv_obj_get_child(pair_btn, 0), "%s", s->pair_open ? "Cancel" : "Close");
}

/* ---- the snackbar (main.dart _say), and the approval prompt in it ---- */
static void snack_build(void)
{
	snack = box(root);
	fill(snack, M3_INVERSE);
	lv_obj_set_style_radius(snack, ri(PB(4)), 0);
	lv_obj_set_size(snack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(snack, ri(PB(16)), 0);
	lv_obj_set_style_pad_ver(snack, ri(PB(10)), 0);
	lv_obj_set_flex_flow(snack, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(snack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(snack, ri(PB(16)), 0);
	snack_wheel = lv_image_create(snack);
	lv_obj_remove_style_all(snack_wheel);
	lv_image_set_src(snack_wheel, &img_wheel);
	snack_text = label(snack, FS(REGULAR, 14, UI_S_PANEL), M3_ON_INVERSE);
	lv_label_set_long_mode(snack_text, LV_LABEL_LONG_MODE_WRAP);
	show(snack, false);
}
static void set_snack(const dash_state_t *s)
{
	bool approving = s->bt == BT_APPROVING; /* the one step that needs a hand: said until it is done */
	bool say = approving || (s->toast[0] && view != DASH_VIEW_CAR_PANEL);
	show(snack, say);
	if (!say) return;
	show(snack_wheel, approving);
	const char *t = approving ? "Approve this Dash for the Commander: in the car, double-press the right scroll wheel" : s->toast;
	set_text(snack_text, "%s", t);
	lv_point_t sz;
	float max = G.W * 0.42f;
	lv_text_get_size(&sz, t, lv_obj_get_style_text_font(snack_text, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(snack_text, ri(fminf(sz.x + 1, max)));
	lv_obj_update_layout(snack);
	align(snack, LV_ALIGN_BOTTOM_MID, 0, ri(-PB(6)));
}

/* ---- the car's path either side, dotted (widgets.dart _CarLights, steerLines): under the car ---- */
static const float steer_ratio[4] = {13.7f, 13.7f, 13.7f, 10.6f}, max_road[4] = {32.8f, 32.8f, 32.8f, 34.1f};
static void steer_draw(lv_event_t *e)
{
	const dash_state_t *s = &S;
	if (!s->live || !s->wifi_live || !s->steer_lines || isnan(s->steer_deg) || s->no_cabin) return;
	lv_layer_t *layer = lv_event_get_layer(e);
	const ui_car_pics_t *p = &ui_car_pics[s->car];
	float w = cab.w, h = cab.h, ox = cab.x, oy = cab.y;
	float road = fmaxf(-max_road[s->car], fminf(max_road[s->car], s->steer_deg / steer_ratio[s->car])) * (float)M_PI / 180;
	float wheelbase = h * (p->axles[1] - p->axles[0]);
	float r = wheelbase / tanf(fmaxf(fabsf(road), 1e-4f)), side = road > 0 ? -1 : 1;
	float cx = w * p->centre, ccx = cx + side * r, ccy = h * p->axles[1];
	bool rev = s->gear == 'R';
	float step = h * 0.022f, len = h * (rev ? 0.53f : 0.7f), dot = cb(1.6f);
	lv_draw_rect_dsc_t rd;
	lv_draw_rect_dsc_init(&rd);
	rd.radius = LV_RADIUS_CIRCLE;
	rd.bg_color = rgb(WHITE);
	for (int sd = -1; sd <= 1; sd += 2) {
		float px0 = cx + sd * w * p->path_from[1], py0 = h * p->path_from[0];
		float rad = hypotf(px0 - ccx, py0 - ccy), a0 = atan2f(py0 - ccy, px0 - ccx);
#define AT(d, X, Y) do { float a_ = a0 + side * (d) / rad; X = ccx + cosf(a_) * rad, Y = ccy + sinf(a_) * rad; } while (0)
		float q0x, q0y, q1x, q1y, mx, my;
		AT(0, q0x, q0y);
		AT(len, q1x, q1y);
		AT(len / 2, mx, my);
		float chx = (q0x + q1x) / 2, chy = (q0y + q1y) / 2, ctx = chx + (mx - chx) * 6, cty = chy + (my - chy) * 6;
		for (float d = 0; d <= len; d += step) {
			float t = d / len, qx = q0x * (1 - t) * (1 - t) + ctx * 2 * t * (1 - t) + q1x * t * t,
			      qy = q0y * (1 - t) * (1 - t) + cty * 2 * t * (1 - t) + q1y * t * t;
			float x = ox + qx, y = oy + (rev ? h - qy : qy);
			rd.bg_opa = (lv_opa_t)(0.8f * 255 * (1 - d / len));
			lv_area_t a = {ri(x - dot), ri(y - dot), ri(x + dot) - 1, ri(y + dot) - 1};
			lv_draw_rect(layer, &rd, &a);
		}
	}
}

static void touch_init(void)
{
	for (int i = 0; i < 10; i++) {
		tappable(chips[i].box, chip_tap, i, (PC(48) - PC(42)) / 2);
		lv_obj_add_event_cb(chips[i].box, chip_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(chips[i].box, chip_hold, LV_EVENT_RELEASED, (void *)(intptr_t)i);
		pressed_look(chips[i].box, 0x4DFFFFFF);
	}
	tappable(link_w.box, open_tap, DASH_VIEW_SETTINGS, PL(4)); /* the app's link icons open its settings */
	tappable(link_b.box, open_tap, DASH_VIEW_SETTINGS, PL(4));
	tappable(car_panel, open_tap, DASH_VIEW_CAR_PANEL, PL(12));
	tip = box(root);
	fill(tip, 0xE6616161); /* Tooltip: grey 700 at 90 % */
	lv_obj_set_style_radius(tip, ri(PC(4)), 0);
	lv_obj_set_size(tip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(tip, ri(PC(8)), 0);
	lv_obj_set_style_pad_ver(tip, ri(PC(4)), 0);
	label(tip, FS(REGULAR, 14, UI_S_CHIP), WHITE);
	show(tip, false);
	tip_timer = lv_timer_create(tip_hide, 1500, NULL);
	lv_timer_pause(tip_timer);
	snack_build();
	panel_build();
	settings_build();
	net_edit_build();
	pair_build();
}

static void set_touch(const dash_state_t *s)
{
	set_snack(s);
	set_pair(s);
	if (view == DASH_VIEW_CAR_PANEL) { /* its message line: '<label>…' while out, then what came of it */
		int a = s->press_pending;
		if (a == ACT_UNLOCK || a == ACT_LOCK || a == ACT_CLIMATE || a == ACT_FLASH || a == ACT_HONK)
			set_text(panel_msg, "%s…", act_label[a]);
		else set_text(panel_msg, "%s", s->toast);
	}
	if (view == DASH_VIEW_SETTINGS && ST.follow_car && s->backlight_pct >= 0 && CHANGED(backlight_pct))
		lv_slider_set_value(set_slider, s->backlight_pct, LV_ANIM_OFF); /* the live value, greyed */
	if (view == DASH_VIEW_SETTINGS) {
		uint32_t c;
		const char *w = bt_word(s->bt, &c, NULL);
		set_text(set_status, "Commander: %s", w);
		text_color(set_status, c == W54 ? W70 : c);
	}
	if (CHANGED(steer_deg) || CHANGED(steer_lines) || CHANGED(car) || CHANGED(gear)) lv_obj_invalidate(steer);
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
	set_touch(s);
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
	s->steer_deg = NAN;
	s->press_pending = -1;
	s->backlight_pct = -1;
	s->g_lat = s->g_long = NAN;
	for (int i = 0; i < 4; i++) s->tyre_psi[i] = NAN;
	for (int i = 0; i < 3; i++) s->cabin_c[i] = NAN;
	s->route_len = s->route_kwh = s->arrive_range = s->range_drop = s->arrive_pct = s->pct_drop = s->arrive_kwh = NAN;
}
