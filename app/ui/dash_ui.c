/* The dash in LVGL: the wattline app's look (lib/main.dart, widgets.dart, gauge.dart, glyphs.dart), arranged
 * for the Dash's trapezoid screen. Sizes are the app's, in dp, through px() (1.25 px per dp
 * at H = 480); the app's names are kept in comments so each piece can be checked against its widget. The icons
 * and car pictures are the app's own renders (ui/assets/export.sh), the text is Roboto through tiny_ttf. */
#include "dash_ui.h"
#include "assets.h"   /* the device's icon set (its profile's UI_GEN: ui/gen, ui/gen/f133) */
#include "aa_icon.h"
#include "../core/cmdr_msg.h" /* cmdr_action: what a tap asks for */
#include "project_region.h"    /* the phone picture's rectangle, shared with the AA receiver */
#include "src/draw/lv_draw_image_private.h"      /* the wheel scaled once (wheel_2x): LVGL's transform, */
#include "src/draw/sw/lv_draw_sw.h"              /* its area and the decoded image */
#include "src/image/lv_image_decoder_private.h"
#include "src/indev/lv_indev_private.h"          /* where a swipe began (swipe) */
#include "src/display/lv_display_private.h"      /* the frame counter's label and its data (fps_show) */
#include "src/debugging/sysmon/lv_sysmon_private.h"
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	float energy_cx, energy_foot; /* the battery block's centre and foot (its figures' bottom): Bluetooth's pedal bar */
	float cab;              /* the cabin's fit in its column (the car pictures are baked for it) */
	float link_top;         /* the link buttons' row, under the left chips */
	/* The phone app's full layout (main.dart), on a plain rectangular panel (the F133 screen's 1920x720) rather than
	 * the Dash's 1920x480 strip: the chips' columns, the car, the ring as tall as the row, the dial over the figures
	 * over the g-meter; the links, the route and the buttons in the bar along the foot. Its icons are baked at its
	 * own size (ui/assets/sizes-f133.json: the app's sizes at 1.875 px/dp). */
	bool phone;
	float row_b;            /* phone: the row's foot, over the bar */
	float bar_l, bar_r;     /* phone: the route's pill, between the links and the buttons */
	float bar_cy;           /* phone: the bar's middle line (the full layout's foot) */
} G;
static float car_bar_r; /* the phone layout's full-screen pill's right end, by the buttons (init) */
static int car_at[2];   /* the car panel button's place in the full layout (init); AA's is beside it */
static bool strip(void);
/* the phone layout's basic layout (basic_layout) */
static int card_slot[DASH_CARDS]; /* each card's slot (basic's 0..3; the split's SPLIT_SLOT, its buttons BAR_SLOT), -1 none */
static float slot_x[5];           /* the slots' left edges and the last one's right edge, for the swipes */
static lv_obj_t *dots[4][DASH_CARDS]; /* each slot's page dots (widgets.dart _Cards), its card's lit */
static int layout_key = INT_MIN;       /* the parts as last shown: 0 the full layout, > 0 basic and its cards, < 0 the split's card */
#define SPLIT_SLOT 3              /* the split's card: the right slot's dots */
#define BAR_SLOT 4                /* the split's bar over the picture: the buttons, always (not a card there) */
static float split_x;             /* the split's card's left edge (the ring left of it), for the swipes */
/* the settings as last handed over (dash_ui_settings); the screen's edits go back through save() */
static dash_settings_t ST = {.minor_figures = true, .brightness_pct = 80, .follow_car = true, .unit_miles = -1, .unit_f = -1, .unit_psi = -1,
			      .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}};

/* what this unit is called in its own texts: the Dash, or the phone layout's screen */
#if DASH_PHONE_LAYOUT
#define UNIT "screen"
#else
#define UNIT "Dash"
#endif
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
static float inset(float y) { return G.phone ? 0 : 115.0f * G.k * (1.0f - y / G.H); }
static int ri(float v) { return (int)lroundf(v); }

/* ---------- fonts: Roboto, as the app (gauge.dart kFont) ---------- */
extern const unsigned char roboto_light[], roboto_regular[], roboto_medium[], roboto_bold[];
extern const size_t roboto_light_size, roboto_regular_size, roboto_medium_size, roboto_bold_size;
enum { LIGHT, REGULAR, MEDIUM, BOLD }; /* w200, w400, w500, w600 and w700 */

/* A glyph's box and advance, remembered. tiny_ttf answers each from three of its caches (the glyph, the next one, the
 * pair's kerning: a tree search and an LRU move each), and a label asks for every letter twice a strip (its lines
 * broken, then drawn): a fifth of a scrolled Settings frame. One table for all the fonts, direct mapped (72 KiB); the
 * fonts live as long as the program, at one size each. */
static bool (*ttf_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
static bool glyph_dsc(const lv_font_t *f, lv_font_glyph_dsc_t *g, uint32_t c, uint32_t next)
{
	static struct { const lv_font_t *f; uint32_t c, next; bool ok; lv_font_glyph_dsc_t g; } memo[1024], *e;
	e = &memo[(((uintptr_t)f >> 4) ^ c * 31u ^ next * 1021u) & 1023];
	if (e->f == f && e->c == c && e->next == next) return *g = e->g, e->ok;
	e->ok = ttf_glyph_dsc(f, g, c, next);
	e->f = f, e->c = c, e->next = next, e->g = *g;
	return e->ok;
}

static const lv_font_t *font_px(int weight, int size)
{
	enum { MAX = 128 };   /* the shots use ~50; fonts are never freed (glyph_dsc's memo keys on them) */
	static struct { int w, s; lv_font_t *f; } cache[MAX];
	static int n;
	int near = 0;
	for (int i = 0; i < n; i++) {
		if (cache[i].w == weight && cache[i].s == size) return cache[i].f;
		int d = cache[i].s - size, dn = cache[near].s - size;
		if (cache[i].w == weight && (cache[near].w != weight || d * d < dn * dn)) near = i;
	}
	if (n == MAX) return cache[near].f;   /* full: the nearest size, not a new font on every call */
	static const unsigned char *data[] = {roboto_light, roboto_regular, roboto_medium, roboto_bold};
	const size_t sizes[] = {roboto_light_size, roboto_regular_size, roboto_medium_size, roboto_bold_size};
	/* the glyph cache: lv_conf.h's count (a profile's DASH_TTF_GLYPHS) */
	lv_font_t *f = lv_tiny_ttf_create_data_ex(data[weight], sizes[weight], size, LV_FONT_KERNING_NORMAL,
						  LV_TINY_TTF_CACHE_GLYPH_CNT);
	if (f) ttf_glyph_dsc = f->get_glyph_dsc, f->get_glyph_dsc = glyph_dsc;
	cache[n++] = (typeof(cache[0])){weight, size, f};
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
/* A label's text, set only when it differs (a set label is laid out and redrawn); true when it was set. */
static bool set_text(lv_obj_t *l, const char *fmt, ...)
{
	char buf[256]; /* the pairing command (~82 characters and the URL), the Commander's status line */
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (lv_label_get_long_mode(l) == LV_LABEL_LONG_MODE_DOTS) {
		/* a label cut short holds its "..." in its own text, which then never equals what is set: a long destination
		 * was set again, and redrawn, at every frame. Against what was last set instead: its checksum, in the label's
		 * user data (FNV-1a, never 0) */
		uintptr_t h = 2166136261u;
		for (const char *c = buf; *c; c++) h = (uint32_t)((h ^ (uint8_t)*c) * 16777619u);
		h |= (uintptr_t)1 << 31;
		if ((uintptr_t)lv_obj_get_user_data(l) == h) return false;
		lv_obj_set_user_data(l, (void *)h);
	} else {
		if (!strcmp(lv_label_get_text(l), buf)) return false;
		lv_obj_set_user_data(l, NULL); /* (a label that is cut short only at times: its checksum is of another text now) */
	}
	lv_label_set_text(l, buf);
	return true;
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

/* What a run shows, kept to tell a change: its parts and their words (copied: the callers' are on their stacks). Compared
 * field by field, not through a formatted key: a run is set every frame, and formatting its floats was a third of an
 * unchanged frame's work. */
typedef struct {
	int n; /* -1: words too long to keep (never the same) */
	part_t p[16];
	char words[640];
} run_seen_t;
static bool run_same(const run_seen_t *k, const part_t *p, int n)
{
	if (!k || k->n != n) return false;
	for (int i = 0; i < n; i++) {
		const part_t *a = &k->p[i], *b = &p[i];
		if (a->font != b->font || a->img != b->img || a->color != b->color || a->ls != b->ls || a->gap != b->gap ||
		    !a->text != !b->text || (a->text && strcmp(a->text, b->text)))
			return false;
	}
	return true;
}
static void run_set(lv_obj_t *run, const part_t *p, int n)
{
	run_seen_t *k = lv_obj_get_user_data(run);
	if (run_same(k, p, n)) return;
	/* The same parts (text or icon, in order) as shown: each child set in place, so only what changed is redrawn
	 * (cleaned and built again, every child's box was, and each new label's at the run's corner) */
	bool same = k && k->n == n && (int)lv_obj_get_child_count(run) == n;
	for (int i = 0; same && i < n; i++) same = !k->p[i].text == !p[i].text;
	if (!k && !(k = lv_malloc(sizeof *k))) return;
	lv_obj_set_user_data(run, k);
	k->n = n;
	char *at = k->words;
	for (int i = 0; i < n; i++) {
		k->p[i] = p[i];
		if (!p[i].text) continue;
		size_t len = strlen(p[i].text) + 1;
		if (at + len > k->words + sizeof k->words) { k->n = -1; break; }
		k->p[i].text = memcpy(at, p[i].text, len), at += len;
	}
	if (!same) lv_obj_clean(run);

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
			lv_obj_t *l = same ? lv_obj_get_child(run, i) : label(run, p[i].font, p[i].color);
			if (same) {
				if (lv_obj_get_style_text_font(l, 0) != p[i].font) lv_obj_set_style_text_font(l, p[i].font, 0);
				text_color(l, p[i].color);
				SET_NUM(l, lv_obj_get_style_text_letter_space, lv_obj_set_style_text_letter_space, ri(p[i].ls));
			} else
				lv_obj_set_style_text_letter_space(l, ri(p[i].ls), 0);
			if (strcmp(lv_label_get_text(l), p[i].text)) lv_label_set_text(l, p[i].text);
			lv_obj_set_pos(l, ri(x[i]), ri(top + group_base[i] - ascent(p[i].font)));
		} else {
			lv_obj_t *im = same ? lv_obj_get_child(run, i) : icon(run, p[i].img, p[i].color);
			if (same) img_src(im, p[i].img), icon_color(im, p[i].color);
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
/* The chips, in the order they are built (widgets.dart _Actions), and their tooltips (the app's labels): each side's
 * column top to bottom, the left one first. The massage chips offer their modes (massage_menu). */
enum { CHIP_SIDE = 6, CHIPS = 2 * CHIP_SIDE, CHIP_RECIRC = CHIP_SIDE + 1, CHIP_MASSAGE_L = CHIP_SIDE - 1, CHIP_MASSAGE_R = CHIPS - 1 };
static const int chip_act[CHIPS] = {ACT_TRUNK, ACT_MIRROR_TILT, ACT_DOOR_FL, ACT_DOOR_RL, ACT_CHARGE_PORT, ACT_MASSAGE_L_SOFT,
				    ACT_FRUNK, ACT_RECIRC_TOGGLE, ACT_DOOR_FR, ACT_DOOR_RR, ACT_LIGHT, ACT_MASSAGE_R_SOFT};
static const char *const chip_tip[CHIPS] = {"Trunk", "Mirror tilt", "Front left door", "Rear left door", "Charge port",
					    "Left Seat Massage", "Frunk", "Recirculation", "Front right door", "Rear right door",
					    "Interior lights", "Right Seat Massage"};

static lv_obj_t *steer;
static lv_obj_t *pair_view, *pair_qr, *pair_how, *pair_url_l, *pair_code_l, *pair_left, *pair_or, *pair_cmd, *pair_res, *pair_btn;
static void steer_draw(lv_event_t *e);
static void touch_init(void);
static void swipe(lv_event_t *e);
static void set_touch(const dash_state_t *s);

static dash_state_t S, P; /* shown now, shown before */
static bool first = true;
static int view = DASH_VIEW_DASH; /* the view on screen; the projection strip lays the ring out differently */

static lv_obj_t *root;
static lv_obj_t *glow_l, *glow_r, *glow_b, *pedal, *pedal_fill, *pedal_name, *pedal_val;

typedef struct { lv_obj_t *box, *icon, *note, *word; } chip_t; /* word: a car panel action's name in its place */
static chip_t chips[CHIPS];
/* The seats' massage: a mode as pressed (3 soft, 2 pulse, 4 hard) is running; a press's side ('L', 'R', 0 none). */
static bool massage_mode(uint8_t m) { return m >= 2 && m <= 4; }
static char massage_side(int a)
{
	return a >= ACT_MASSAGE_L_SOFT && a <= ACT_MASSAGE_L_HARD ? 'L' : a >= ACT_MASSAGE_R_SOFT && a <= ACT_MASSAGE_R_HARD ? 'R' : 0;
}

static struct {
	lv_obj_t *box, *icon, *word, *stat, *ring, *secs; /* ring, secs: a timer's countdown round the icon */
	int16_t left, span;                               /* as drawn: whole seconds */
	uint32_t c;
} link_w, link_b;
static lv_obj_t *car_panel, *aa_btn;   /* bottom-right: the car panel, and the Android Auto start/stop (cert-gated) */
/* the second projection button left of AA, NULL without its addition's files (dash_ui.h): read once, at the start */
static lv_obj_t *x_btn;
const char *dash_ui_extra_hook = "/data/hooks/extra-start", *dash_ui_extra_icon = "/data/hooks/extra-icon",
	   *dash_ui_extra_up = "/run/dash-aa.extra";
static lv_image_dsc_t ic_x;
static bool extra_icon(void) /* its hook there and its icon exactly AA's size (A8): else no button */
{
	if (access(dash_ui_extra_hook, X_OK) != 0) return false;
	size_t n = (size_t)ic_aa.header.w * ic_aa.header.h, got = 0;
	uint8_t *px = malloc(n + 1);
	FILE *f = px ? fopen(dash_ui_extra_icon, "rb") : NULL;
	if (f) got = fread(px, 1, n + 1, f), fclose(f);
	if (got != n) { free(px); return false; }
	ic_x = ic_aa, ic_x.data = px;
	return true;
}
/* AA (and the second button left of it) a gap to the car panel's left, a little lower than it (2 dp: their glyphs fill
 * their box, the car's sits low in its own; level, they looked higher) */
static void aa_place(int car_x, int y)
{
	int x = car_x - ri(PL(16)) - (int)ic_aa.header.w;
	y += ri(PL(2));
	lv_obj_set_pos(aa_btn, x, y);
	if (x_btn) lv_obj_set_pos(x_btn, x - ri(PL(16)) - (int)ic_aa.header.w, y);
}

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
static lv_obj_t *no_route; /* basic's route card with no route: the app's NO ROUTE (main.dart _Nav, tall) */

/* the ring */
static lv_obj_t *ring, *speed_l, *unit_l, *gear_l, *cue, *limit_sign, *limit_l, *rear_v, *rear_u, *front_v, *front_u;
static lv_obj_t *ap_disc, *ap_glyph, *outside_l;
static lv_timer_t *ap_blink;

/* the energy column */
static lv_obj_t *dial, *dial_temp, *dial_pct, *dial_range;
static lv_obj_t *charge_box, *charge_head, *charge_to, *charge_when;
static lv_obj_t *flow_n[3], *flow_v[3];   /* plugged in: the wall, the climate, the battery */
static lv_obj_t *stats_box, *stat_lab[5], *stat_val[5];

/* the g-meter */
static lv_obj_t *gball, *g_val, *g_peak;

/* the phone layout's power card (widgets.dart _PowerDial): the pack's power round a circle, its figure in the middle */
static lv_obj_t *pdial, *pdial_kw, *pdial_unit, *pdial_range;

/* ---------- edge glows (main.dart _Edge), parallel to the bezel ---------- */
static lv_image_dsc_t glow_dsc[2];
/* The split's left glow over the picture, the decoder's to draw (ui/project_region.h PJ_GLOW): its colour, 0 none.
 * Written only as it changes (the flasher: a few times a second). */
static void pj_glow(uint32_t c)
{
	static uint32_t said;
	if (c == said) return;
	if (!c) { unlink(PJ_GLOW), said = 0; return; }
	FILE *f = fopen(PJ_GLOW ".new", "w");
	if (!f) return;   /* (said as it was: tried again at the next frame) */
	bool ok = fprintf(f, "%06X %d\n", (unsigned)(c & 0xFFFFFF), (int)ri(px(70))) > 0;
	if (fclose(f) == 0 && ok && rename(PJ_GLOW ".new", PJ_GLOW) == 0) said = c;
}
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
/* Plugged in, in P (charging or at the limit): the two halves one ring over the top (the limit sign off), open 88° at
 * the foot round the gear, filling with the wall's power out of what the plug gives (11 kW until that is known) */
static const float PLUG_A0 = 134, PLUG_SWEEP = 272, PLUG_MAX_KW = 11;
/* the Dash's full ring has its top above the screen (the two halves' gap, 236° to 304°): one ring there skips it, the
 * left side filling first, then the right */
static const float PLUG_GAP0 = 236, PLUG_GAP1 = 304;
static bool plug_gap(void) { return G.cy < G.r; } /* (the arc's middle above the top: the Dash, not the phone layouts) */
static bool speedo(void);
static bool plug_ring(const dash_state_t *s)
{
	return s->wifi_live && s->plugged && s->gear == 'P' && !isnan(s->wall_kw) && !speedo();
}

/* An arc of the ring as it is drawn: whole degrees from a0 to a1 (not turned into 0..360), its outer edge's radius and
 * its width in pixels; c 0: none. Kept as drawn, so a change redraws only the degrees it moved (ring_dirty). */
typedef struct { int a0, a1, rad, w; uint32_t c; } ring_arc_t;
static ring_arc_t arc_at(float start, float sweep, uint32_t c, float width)
{
	if (sweep <= 0.05f) return (ring_arc_t){0};
	int a0 = ri(start); /* the radius: LVGL's is the outer edge, Flutter's the stroke's middle */
	return (ring_arc_t){a0, a0 + (ri(sweep) ? ri(sweep) : 1), ri(G.r + width / 2), width < 1 ? 1 : ri(width), c};
}
static void arc_draw(lv_layer_t *layer, ring_arc_t a)
{
	if (!a.c) return;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(G.cx);
	d.center.y = ri(G.cy);
	d.width = a.w;
	d.radius = a.rad;
	int a0 = ((a.a0 % 360) + 360) % 360; /* LVGL wants 0..360 */
	d.start_angle = a0;
	d.end_angle = (a0 + a.a1 - a.a0) % 360;
	d.color = rgb(a.c);
	d.opa = opa(a.c);
	lv_draw_arc(layer, &d);
}
static void arc(lv_layer_t *layer, float start, float sweep, uint32_t c, float width)
{
	arc_draw(layer, arc_at(start, sweep, c, width));
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
static ring_arc_t motor(float kw, float zero, bool up)
{
	float pos = HALF + DROP, neg = HALF - DROP;
	if (kw >= 0) {
		float s = pos * fminf(kw, MAX_KW) / MAX_KW;
		return arc_at(up ? zero : zero - s, s, ACCENT, G.aw);
	}
	float s = neg * fmaxf(kw, MIN_KW) / MIN_KW;
	return arc_at(up ? zero - s : zero, s, REGEN, G.aw);
}
/* The ring's coloured arcs, [side: left, right][the motor's (a rear-motor car's right: its pedal or brake), the regen
 * the pack cannot take (red past the cap: a motor's share is what the other is not using)]. */
static void ring_arcs(const dash_state_t *s, ring_arc_t a[2][2])
{
	float w = G.aw, zl = SEC_L0 + HALF - DROP, zr = SEC_R0 + HALF + DROP;
	memset(a, 0, sizeof(ring_arc_t[2][2]));
	if (plug_ring(s)) {
		float max = isnan(s->wall_max_kw) || s->wall_max_kw <= 0 ? PLUG_MAX_KW : s->wall_max_kw;
		float f = fminf(fmaxf(s->wall_kw, 0) / max, 1);
		if (!plug_gap()) {
			a[0][0] = arc_at(PLUG_A0, PLUG_SWEEP * f, REGEN, w);
			return;
		}
		float left = PLUG_GAP0 - PLUG_A0, sw = f * (PLUG_SWEEP - (PLUG_GAP1 - PLUG_GAP0));
		a[0][0] = arc_at(PLUG_A0, fminf(sw, left), REGEN, w);
		a[1][0] = arc_at(PLUG_GAP1, sw - left, REGEN, w);
		return;
	}
	if (!isnan(s->rear_kw)) a[0][0] = motor(s->rear_kw, zl, true);
	float right = !s->rwd ? s->front_kw : s->brake_pct > 0 ? -s->brake_pct : s->pedal_pct;
	if (!isnan(right)) {
		if (s->rwd) {
			float pct = fmaxf(-100, fminf(100, right));
			if (pct >= 0) {
				float d = (HALF + DROP) * pct / 100;
				a[1][0] = arc_at(zr - d, d, ACCENT, w);
			} else
				a[1][0] = arc_at(zr, (HALF - DROP) * -pct / 100, ALERT, w);
		} else
			a[1][0] = motor(right, zr, false);
	}
	if (!isnan(s->regen_cap_kw)) {
		float total = s->regen_cap_kw;
		for (int side = 0; side < (s->rwd ? 1 : 2); side++) {
			float other = side == 0 ? (s->rwd ? 0 : s->front_kw) : s->rear_kw;
			float kw = total - fmaxf(0, isnan(other) ? 0 : -other);
			float at = fmaxf(0, fminf(kw, -MIN_KW)) / -MIN_KW * (HALF - DROP), rest = HALF - DROP - at;
			if (rest <= 0) continue;
			if (side == 0) a[0][1] = arc_at(zl - (HALF - DROP), rest, ALERT, w * 0.3f);
			else a[1][1] = arc_at(zr + at, rest, ALERT, w * 0.3f);
		}
	}
}
/* ---------- the speedometer (widgets.dart _Speedo, _SpeedoScale, _SpeedoNeedle): the ring's other face ----------
 * Basic's ring and the split's, swiped to (main.dart _ring's pages, kept as ring_page): a gas car's speedometer, one arc
 * over the top (260°, its ends 40° under the level) dashed every 10 and numbered every 50 km/h (20 mph) up to past the
 * car's top speed, the dial 0.15 of the radius lower than the ring's centre; the figure small inside it on the left, the
 * limit inside its top right, the needle over them (on a live speed); the gear a little higher than the power ring's,
 * the cue under it. The assist disc and the air outside stay put. No glow trailing the needle: the app's is its
 * animation's lag, and the dash draws the speed as it is. */
#define SPEEDO_A0 140.0f
#define SPEEDO_SWEEP 260.0f
static bool speedo(void) { return G.phone && ST.ring_page == DASH_RING_SPEEDO && (view == DASH_VIEW_PROJECT || ST.basic); }
static float speedo_cy(void) { return G.cy + G.r * 0.15f; }
static int speedo_top(void) { return S.kph ? 250 : 160; } /* the arc's end: past the car's top speed */
static void speedo_draw(lv_layer_t *layer)
{
	float r = G.r, w = G.aw, cy = speedo_cy(), inner = r - w / 2 - px(3);
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(G.cx), d.center.y = ri(cy);
	d.width = ri(w * 0.5f), d.radius = ri(r + w * 0.25f); /* LVGL: the outer edge; the app's: the stroke's middle on r */
	d.start_angle = 140, d.end_angle = 40;                /* SPEEDO_A0, and on round by SPEEDO_SWEEP */
	d.color = rgb(W24), d.opa = opa(W24);
	lv_draw_arc(layer, &d);
	int hi = speedo_top(), major = hi > 200 ? 50 : 20;
	const lv_font_t *f = font_px(REGULAR, ri(r * 0.085f));
	float lh = lv_font_get_line_height(f);
	lv_draw_label_dsc_t ld;
	lv_draw_label_dsc_init(&ld);
	ld.font = f, ld.color = rgb(W70), ld.opa = opa(W70), ld.align = LV_TEXT_ALIGN_CENTER;
	for (int v = 0; v <= hi; v += 10) { /* a dash every 10, longer and numbered every major */
		bool mj = v % major == 0;
		float a = DEG(SPEEDO_A0 + SPEEDO_SWEEP * v / hi), c = cosf(a), sn = sinf(a), len = r * (mj ? 0.09f : 0.045f);
		line(layer, G.cx + c * inner, cy + sn * inner, G.cx + c * (inner - len), cy + sn * (inner - len), mj ? W70 : W38,
		     px(mj ? 2 : 1.2f), false);
		if (!mj) continue;
		char t[8];
		snprintf(t, sizeof t, "%d", v);
		float tx = G.cx + c * (inner - r * 0.17f), ty = cy + sn * (inner - r * 0.17f);
		lv_area_t ta = {ri(tx - r * 0.2f), ri(ty - lh / 2), ri(tx + r * 0.2f), ri(ty + lh / 2)};
		ld.text = t, ld.text_local = 1;
		lv_draw_label(layer, &ld, &ta);
	}
}
static float needle_at = NAN; /* the needle as drawn: its place on the arc (0..1, in half degrees); NAN none */
static void needle_box(float f, lv_area_t *a) /* the hub to the tip, with the shadow's drop and the hub's round */
{
	float an = DEG(SPEEDO_A0 + SPEEDO_SWEEP * f), len = G.r + G.aw / 2, cy = speedo_cy(), m = fmaxf(G.r * 0.08f, px(5)) + px(3);
	float x1 = G.cx + cosf(an) * len, y1 = cy + sinf(an) * len;
	*a = (lv_area_t){ri(fminf(G.cx, x1) - m), ri(fminf(cy, y1) - m), ri(fmaxf(G.cx, x1) + m), ri(fmaxf(cy, y1) + m)};
}
static void speedo_needle(lv_event_t *e) /* the ring's DRAW_POST: over the figure and the limit, as the app's */
{
	if (!speedo() || isnan(needle_at)) return;
	lv_layer_t *layer = lv_event_get_layer(e);
	float an = DEG(SPEEDO_A0 + SPEEDO_SWEEP * needle_at), len = G.r + G.aw / 2, cy = speedo_cy();
	float x1 = G.cx + cosf(an) * len, y1 = cy + sinf(an) * len, sx = px(1.5f), sy = px(2);
	line(layer, G.cx + sx, cy + sy, x1 + sx, y1 + sy, 0x8A000000u, px(6), true); /* its shadow: unblurred, so lighter */
	line(layer, G.cx, cy, x1, y1, WHITE, px(3.5f), true);
	lv_draw_rect_dsc_t rd; /* the hub it turns on: a dark cap, faintly rimmed */
	lv_draw_rect_dsc_init(&rd);
	rd.radius = LV_RADIUS_CIRCLE;
	rd.bg_color = rgb(0xFF2A2A2Au);
	rd.border_color = rgb(W24), rd.border_opa = opa(W24), rd.border_width = ri(px(1));
	float hr = G.r * 0.08f;
	lv_area_t h = {ri(G.cx - hr), ri(cy - hr), ri(G.cx + hr) - 1, ri(cy + hr) - 1};
	lv_draw_rect(layer, &rd, &h);
}
/* The needle moved (by half a degree), came or went: its old place and its new one redrawn, not the whole face. */
static void needle_dirty(const dash_state_t *s)
{
	float f = s->live && !isnan(s->speed) ? fmaxf(0, fminf(1, s->speed / speedo_top())) : NAN;
	if (!isnan(f)) f = roundf(f * SPEEDO_SWEEP * 2) / (SPEEDO_SWEEP * 2);
	if (isnan(f) ? isnan(needle_at) : f == needle_at) return;
	lv_area_t a;
	if (!isnan(needle_at)) needle_box(needle_at, &a), lv_obj_invalidate_area(ring, &a);
	needle_at = f;
	if (!isnan(f)) needle_box(f, &a), lv_obj_invalidate_area(ring, &a);
}

static void ring_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	if (speedo()) {
		speedo_draw(layer);
		return;
	}
	const dash_state_t *s = &S;
	float r = G.r, w = G.aw;
	bool plug = plug_ring(s);
	if (plug && !plug_gap()) arc(layer, PLUG_A0, PLUG_SWEEP, W24, px(1.5)); /* one ring's track */
	else if (plug) {
		arc(layer, PLUG_A0, PLUG_GAP0 - PLUG_A0, W24, px(1.5));
		arc(layer, PLUG_GAP1, PLUG_A0 + PLUG_SWEEP - PLUG_GAP1, W24, px(1.5));
	} else {
		arc(layer, SEC_L0, SEC_SWEEP, W24, px(1.5)); /* the empty track: a hairline */
		arc(layer, SEC_R0, SEC_SWEEP, W24, px(1.5));
	}
	/* The app's brake in the ring's foot (a red arc and BRAKE APPLIED; WiFi's, two motors) is in the pedal's slot
	 * under the g-meter on the Dash; the phone layout has it here, as the app. A rear-motor car's brake stays
	 * on the ring's right arc. */
	if (G.phone && s->wifi_live && !s->rwd && s->brake_pct > 0) {
		float room = 90 - (SEC_R0 + SEC_SWEEP) - 10, reach = room * fminf(s->brake_pct, 100) / 100;
		arc(layer, 90 - reach, 2 * reach, ALERT, w * 0.35f);
		/* BRAKE APPLIED under it, each letter's top on the circle, turned to its tangent (gauge.dart _curved) */
		const lv_font_t *f = font_px(BOLD, ri(r * 0.04f));
		const char *t = "BRAKE APPLIED";
		float ls = r * 0.004f, rad = r + w * 0.35f, total = 0, wd[16];
		for (int i = 0; t[i]; i++) total += wd[i] = lv_font_get_glyph_width(f, t[i], t[i + 1]) + ls;
		float a = DEG(90) + total / 2 / rad;
		for (int i = 0; t[i]; i++) {
			float mid = a - wd[i] / 2 / rad, b = rad + ascent(f);
			lv_draw_letter_dsc_t d;
			lv_draw_letter_dsc_init(&d);
			d.font = f;
			d.color = rgb(ALERT);
			d.unicode = t[i];
			d.rotation = ri((mid * 180 / (float)M_PI - 90) * 10);
			lv_point_t pt = {ri(G.cx + b * cosf(mid)), ri(G.cy + b * sinf(mid))};
			if (t[i] != ' ') lv_draw_letter(layer, &d, &pt);
			a -= wd[i] / rad;
		}
	}
	float zl = SEC_L0 + HALF - DROP, zr = SEC_R0 + HALF + DROP;
	ring_arc_t arcs[2][2];
	ring_arcs(s, arcs);
	arc_draw(layer, arcs[0][0]), arc_draw(layer, arcs[1][0]); /* the motors, then the regen caps over them */
	arc_draw(layer, arcs[0][1]), arc_draw(layer, arcs[1][1]);
	for (int i = 0; i < 2 && !plug; i++) { /* zero ticks (not on the one ring) */
		float a = DEG(i ? zr : zl), c = cosf(a), sn = sinf(a);
		line(layer, G.cx + c * (r - w), G.cy + sn * (r - w), G.cx + c * (r + w * 0.4f), G.cy + sn * (r + w * 0.4f), W54, px(2), false);
	}
}

static void ring_ext(lv_event_t *e) { lv_event_set_ext_draw_size(e, ri(G.aw)); } /* BRAKE APPLIED, under the ring */
/* A figure's reach past its label's box, for LVGL's redraw round it. LVGL gives a label a quarter of its line height
 * round its box (for letters that lean out: italics) and redraws all of it with each new text: round the speed, more
 * than its box again, every new speed. The digits, the point, the minus and the dash reach out by this much at most
 * (by none at the sizes drawn). Set again with each new font (the speed's changes with its digits): LVGL asks only when
 * the box changes. */
static void figure_ext(lv_event_t *e)
{
	const lv_font_t *f = lv_obj_get_style_text_font(lv_event_get_target(e), 0);
	static const uint32_t ch[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '-', 0x2014};
	int32_t m = 0;
	for (size_t i = 0; i < sizeof ch / sizeof ch[0]; i++) {
		lv_font_glyph_dsc_t g;
		if (!lv_font_get_glyph_dsc(f, &g, ch[i], 0)) continue;
		int32_t top = f->line_height - f->base_line - g.box_h - g.ofs_y; /* its box's top in the line (lv_draw_label) */
		m = LV_MAX(m, LV_MAX(LV_MAX(-g.ofs_x, g.ofs_x + g.box_w - g.adv_w), LV_MAX(-top, top + g.box_h - f->line_height)));
	}
	*(int32_t *)lv_event_get_param(e) = m; /* in place of the label's own: a larger one would be kept */
}
static void figure_font(lv_obj_t *l, const lv_font_t *f)
{
	if (lv_obj_get_style_text_font(l, 0) == f) return;
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_refresh_ext_draw_size(l);
}

/* The ring's geometry, set per frame so the full dash and the projection strip can each size and place the one ring.
 * The full dash: the app's radius, centred on the screen. The strip (right third of the phone-map split): centred in
 * the strip, as big as the short height leaves once the top and bottom rows are taken. */
static void ring_geom_full(void)
{
	if (G.phone) { /* the app's: as tall as the row, the arcs 6 dp in (gauge.dart RingGauge.radiusIn) */
		G.D = G.row_b - G.mt;
		G.r = G.D / 2 - px(6);
		G.cy = G.mt + G.D / 2;
	} else {
		G.r = (G.H - 12 * G.k) / (0.83f * 1.0425f + 1.03f);
		G.cy = G.H / 2;
		G.D = 2 * (G.r + px(6));
	}
	G.aw = G.r * 0.085f;
}
#define PJ_TOP px(34)                    /* the gear + battery row */
static float pj_bot(void) { return PR(30) + px(7); } /* the assist disc / nav line; the exit pill keeps the corner, clear of the round ring */
static void ring_geom_compact(void)
{
	float vw = G.W * 2.0f / 3.0f, sw = G.W - vw, pad = px(10);
	float band = G.H - PJ_TOP - pj_bot();
	G.D = fminf(band, sw - 2 * pad);
	G.r = G.D / 2 - px(6);
	G.aw = G.r * 0.085f;
	G.cx = vw + sw / 2;
	G.cy = PJ_TOP + band / 2;
}
/* The ring's fixed-text parts (limit sign, the motor readouts) sized and placed for the current G.r / G.D, so the one
 * ring works at both sizes. Was done once at init; now per frame, a no-op cost for the full dash (same values). */
static void ring_figs_geom(float dy) /* dy: how far the speed column sits lower than in the full dash (the strip) */
{
	if (lv_obj_get_style_width(ring, 0) != ri(G.D)) lv_obj_set_size(ring, ri(G.D), ri(G.D)); /* full and compact sizes */
	float side = G.D * 0.16f, k = side / px(52);
	if (speedo()) { /* the speedometer's: 0.22 of the radius, inside its arc's top right, under its numbers */
		side = G.r * 0.22f, k = side / px(52);
		place(limit_sign, G.D / 2 + G.r * 0.3f, G.D / 2 - G.r * 0.31f, side, side);
	} else
		place(limit_sign, (G.D - side) / 2, G.D * 0.1f, side, side);
	SET_NUM(limit_sign, lv_obj_get_style_border_width, lv_obj_set_style_border_width, ri(px(5) * k));
	const lv_font_t *lf = font_px(BOLD, ri(px(20) * k));
	if (lv_obj_get_style_text_font(limit_l, 0) != lf) lv_obj_set_style_text_font(limit_l, lf, 0);
	const lv_font_t *fv = font_px(LIGHT, ri(G.r * 0.16f)), *fu = font_px(MEDIUM, ri(G.r * 0.085f));
	lv_obj_t *mt[4] = {rear_v, front_v, rear_u, front_u};
	const lv_font_t *mf[4] = {fv, fv, fu, fu};
	for (int i = 0; i < 4; i++) {
		if (i < 2) figure_font(mt[i], mf[i]);
		else if (lv_obj_get_style_text_font(mt[i], 0) != mf[i]) lv_obj_set_style_text_font(mt[i], mf[i], 0);
		float x = (i % 2 ? 1 : -1) * G.r * 0.72f, y = (i < 2 ? -G.r * 0.06f : G.r * 0.155f) + dy;
		align(mt[i], LV_ALIGN_CENTER, ri(x), ri(y));
	}
}

/* ---------- the battery dial (widgets.dart _Battery, _DialPainter): drawn at 104 x 98, shown 128 wide ---------- */
static float dial_q(void) { return px(128) * UI_S_DIAL / 104; }
/* The box round an arc's band, from radius r1 to r2 about cx, cy, a0 to a1 degrees, and a few pixels for the edges. */
static void arc_box(float cx, float cy, float r1, float r2, int a0, int a1, lv_area_t *box)
{
	float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
#define BAND_PT(rr, deg) do { float x_ = cx + (rr) * cosf(DEG(deg)), y_ = cy + (rr) * sinf(DEG(deg)); \
		x0 = fminf(x0, x_), y0 = fminf(y0, y_), x1 = fmaxf(x1, x_), y1 = fmaxf(y1, y_); } while (0)
	BAND_PT(r1, a0); BAND_PT(r2, a0); BAND_PT(r1, a1); BAND_PT(r2, a1);
	for (int q = (int)ceilf(a0 / 90.0f) * 90; q <= a1; q += 90) BAND_PT(r2, q); /* where it reaches furthest out */
#undef BAND_PT
	*box = (lv_area_t){(int32_t)floorf(x0) - 3, (int32_t)floorf(y0) - 3, (int32_t)ceilf(x1) + 3, (int32_t)ceilf(y1) + 3};
}
/* The dial's two arcs as drawn: the charge, and while charging the rest to the limit (dim green); whole degrees, c 0
 * none. Kept as drawn, so a new charge redraws only the degrees its end moved (dial_dirty). */
typedef struct { int a0, a1; uint32_t c; } dial_arc_t;
static void dial_arcs(const dash_state_t *s, dial_arc_t a[2])
{
	float frac = isnan(s->soc_pct) ? 0 : fmaxf(0, fminf(100, s->soc_pct)) / 100, sw = 270 * frac;
	uint32_t color = (isnan(s->soc_pct) ? 100 : s->soc_pct) < 20 ? ALERT : REGEN;
	a[0] = sw > 0.2f ? (dial_arc_t){135, ri(135 + sw), color} : (dial_arc_t){0};
	a[1] = (dial_arc_t){0};
	bool charging = s->wifi_live && s->charging;
	if (charging && s->charge_limit_pct > frac * 100) { /* still to go in: dim green on to the limit */
		float st = 135 + sw, sw2 = 270 * (fminf(s->charge_limit_pct, 100) / 100 - frac);
		if (sw2 > 0.2f) a[1] = (dial_arc_t){ri(st), ri(st + sw2), 0xFF12422D};
	}
}
static void dial_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	float q = dial_q(), r = (52 - 4) * q, cx = a.x1 + 52 * q, cy = a.y1 + 52 * q;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(cx);
	d.center.y = ri(cy);
#define DIAL_ARC(st, end, c, wd) do { d.width = ri(wd); d.radius = ri(r + (wd) / 2); d.start_angle = (st); \
		d.end_angle = (end); d.color = rgb(c); d.opa = opa(c); lv_draw_arc(layer, &d); } while (0)
	DIAL_ARC(135, 405, W24, 1.5f * q);
	dial_arc_t arcs[2];
	dial_arcs(&S, arcs);
	for (int i = 0; i < 2; i++)
		if (arcs[i].c) DIAL_ARC(arcs[i].a0, arcs[i].a1, arcs[i].c, 6 * q);
}
/* Redraws the dial's arcs where they changed: the degrees the charge's end moved over (all of an arc before and after
 * when it changed colour or came or went), not the whole dial (its figures in the middle, every arc again). */
static void dial_dirty(const dash_state_t *s)
{
	static dial_arc_t shown[2];
	dial_arc_t now[2];
	dial_arcs(s, now);
	lv_area_t a, b;
	lv_obj_get_coords(dial, &a);
	float q = dial_q(), r2 = ri((52 - 4) * q + 3 * q), r1 = r2 - ri(6 * q), cx = ri(a.x1 + 52 * q), cy = ri(a.y1 + 52 * q);
	if (first) lv_obj_invalidate(dial); /* all of it: nothing drawn yet, or (after the settings) arcs from before them */
	for (int i = 0; i < 2 && !first; i++) {
		dial_arc_t o = shown[i], n = now[i];
		if (!memcmp(&o, &n, sizeof o)) continue;
		if (o.c && o.c == n.c && o.a0 == n.a0) { /* its end moved */
			arc_box(cx, cy, r1, r2, LV_MIN(o.a1, n.a1), LV_MAX(o.a1, n.a1), &b);
			lv_obj_invalidate_area(dial, &b);
			continue;
		}
		if (o.c) arc_box(cx, cy, r1, r2, o.a0, o.a1, &b), lv_obj_invalidate_area(dial, &b);
		if (n.c) arc_box(cx, cy, r1, r2, n.a0, n.a1, &b), lv_obj_invalidate_area(dial, &b);
	}
	memcpy(shown, now, sizeof shown);
}

/* ---------- the g-meter (widgets.dart _GBall, _GPainter) ---------- */
#define TRAIL 10
static float trail[TRAIL][2];
static int trail_n;
static bool g_skel; /* basic's drive card with no link: the circles alone */
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
	if (g_skel) return; /* the skeleton: no reach, no dot */
	/* The reach: a quarter ellipse between each pair of peaks, kAccent at 40 %, filled row by row. */
	float b = fminf(1, fmaxf(0, s->g_peak_brake)) * r, ac = fminf(1, fmaxf(0, s->g_peak_accel)) * r,
	      l = fminf(1, fmaxf(0, s->g_peak_lat)) * r;
	if (b + ac + l > 0 && l > 0) {
		lv_draw_fill_dsc_t fd;
		lv_draw_fill_dsc_init(&fd);
		fd.color = rgb(ACCENT);
		const lv_area_t *clip = &layer->_clip_area; /* only the rows in the part being redrawn: three fills a row */
		for (int y = LV_MAX((int)floorf(cy - b), clip->y1); y <= LV_MIN((int)ceilf(cy + ac), clip->y2); y++) {
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

static void gball_ext(lv_event_t *e) { lv_event_set_ext_draw_size(e, ri(px(14))); } /* the dot: 5 dp x its growth */
/* The box round a trail of n points as gball_draw puts them on the ball [a]: its lines and the dot at its head. */
static void trail_box(const lv_area_t *a, const float (*t)[2], int n, lv_area_t *box)
{
	float side = a->x2 - a->x1 + 1, cx = a->x1 + side / 2, cy = a->y1 + side / 2, r = side / 2 - px(1);
	float gk = fmaxf(1, r / px(35)), m = fmaxf(px(2) * gk / 2, 5 * gk * px(1)) + 2; /* a line's half width, the dot */
	float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
	for (int i = 0; i < n; i++) {
		float gx = t[i][0], gy = t[i][1], dist = sqrtf(gx * gx + gy * gy);
		if (dist > 1) gx /= dist, gy /= dist;
		x0 = fminf(x0, cx + gx * r), y0 = fminf(y0, cy + gy * r), x1 = fmaxf(x1, cx + gx * r), y1 = fmaxf(y1, cy + gy * r);
	}
	*box = (lv_area_t){(int32_t)floorf(x0 - m), (int32_t)floorf(y0 - m), (int32_t)ceilf(x1 + m), (int32_t)ceilf(y1 + m)};
}
/* The ball redraws when its picture moved on: a trail point or a peak other than the last drawn. Parked, or the g
 * holding for the trail's ten frames, it is the same picture again: it was redrawn all the same, every frame it showed
 * (three quarters of a parked frame's work with the full layout, nine tenths with the drive card's bigger ball). The
 * trail alone moving, only where it was and where it is now: the whole ball and its margin for the dot were ten times
 * the pixels, every frame the g moved. */
static void gball_dirty(const dash_state_t *s)
{
	static float was[TRAIL][2], was_pk[3];
	static int was_n;
	static lv_area_t was_at; /* the ball's box when the trail was last drawn */
	float pk[3] = {s->g_peak_brake, s->g_peak_accel, s->g_peak_lat};
	if (trail_n == was_n && !memcmp(trail, was, sizeof trail[0] * trail_n) && !memcmp(pk, was_pk, sizeof pk)) return;
	lv_area_t at, b;
	lv_obj_get_coords(gball, &at);
	if (memcmp(pk, was_pk, sizeof pk) || memcmp(&at, &was_at, sizeof at)) lv_obj_invalidate(gball); /* the reach too */
	else {
		if (was_n) trail_box(&at, (const float (*)[2])was, was_n, &b), lv_obj_invalidate_area(gball, &b);
		if (trail_n) trail_box(&at, (const float (*)[2])trail, trail_n, &b), lv_obj_invalidate_area(gball, &b);
	}
	memcpy(was, trail, sizeof was), memcpy(was_pk, pk, sizeof pk), was_n = trail_n, was_at = at;
}

/* ---------- the power card (widgets.dart _PowerDial, _PowerFace): the phone layout's sixth card ----------
 * After the S3XY Dash's: the pack's power as a white dash round a thin track over the top, filled inside from the top to
 * it (draw clockwise in the accent, regen green the other way; 300 kW at either end), its figure in the middle, and the
 * battery as an arc round the foot, under the range. Its circle the ring's and the drive card's (6 dp in). */
#define PD_ENDS 300.0f /* kW either way: the arc's ends */
#define PD_REACH 120.0f /* the power's either way from the top: to 10° short of the battery's 100° round the foot */
static float pd_p = NAN; /* as drawn: the power, of the way to an end (half-degree steps); NAN unknown */
static int pd_soc = -1;  /* as drawn: the battery, % (-1 unknown) */
static void pdial_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	float side = lv_area_get_width(&a), cx = a.x1 + side / 2, cy = a.y1 + side / 2, r = side / 2 - px(6), tw = r * 0.035f;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = ri(cx), d.center.y = ri(cy);
#define PD_ARC(a0, a1, rad, wd, c, round) do { d.start_angle = (((int)lroundf(a0)) % 360 + 360) % 360; \
		d.end_angle = (((int)lroundf(a1)) % 360 + 360) % 360; d.radius = ri((rad) + (wd) / 2); d.width = ri(fmaxf(1, wd)); \
		d.color = rgb(c), d.opa = opa(c), d.rounded = round; if (lroundf(a1) != lroundf(a0)) lv_draw_arc(layer, &d); } while (0)
	PD_ARC(270 - PD_REACH, 270 + PD_REACH, r, tw, W24, 1); /* the power's track */
	float p = pd_p;
	if (!isnan(p) && fabsf(p) > 0.002f) { /* the fill: from the top to it, three quarters of the dash's depth */
		float band = r * 0.26f * 0.75f, mid = r * (1 + 0.035f / 2) - band / 2;
		PD_ARC(p > 0 ? 270 : 270 + PD_REACH * p, p > 0 ? 270 + PD_REACH * p : 270, mid, band, p < 0 ? REGEN : ACCENT, 0);
	}
	if (!isnan(p)) { /* the dash, square at its inner end, round at the track; its shadow unblurred, so lighter */
		float an = DEG(270 + PD_REACH * p), c = cosf(an), sn = sinf(an), x1 = cx + c * r, y1 = cy + sn * r;
		float x0 = cx + c * r * (1 - 0.26f), y0 = cy + sn * r * (1 - 0.26f), sx = px(1.5f), sy = px(2);
		line(layer, x1 + sx, y1 + sy, x0 + sx, y0 + sy, 0x8A000000u, px(5), false);
		line(layer, x1, y1, x0, y0, WHITE, px(3), false);
		lv_draw_rect_dsc_t rd;
		lv_draw_rect_dsc_init(&rd);
		rd.radius = LV_RADIUS_CIRCLE;
		rd.bg_color = rgb(WHITE);
		lv_area_t tip = {ri(x1 - px(1.5f)), ri(y1 - px(1.5f)), ri(x1 + px(1.5f)) - 1, ri(y1 + px(1.5f)) - 1};
		lv_draw_rect(layer, &rd, &tip);
	}
	PD_ARC(40, 140, r, tw, W24, 1); /* the battery: round the foot from its left, under the range */
	if (pd_soc > 0) PD_ARC(140 - 100 * pd_soc / 100.0f, 140, r, tw, ACCENT, 1);
#undef PD_ARC
}
/* Braking: the car from above, its four tyres (FL FR / RL RR) joined by the axles and a spine. Each tyre is a bar in a
 * red outline, filled from the bottom to its own wheel's share (0x185's torque per wheel, on the total's scale: equal
 * wheels read the total), its figure beside it: which wheel the brake, or the stability control, is on. */
static lv_obj_t *brake_car;
/* the steering wheel's angle to the road wheels' (per car type: Y, Y, YL, 3), as the path lines draw it */
static const float steer_ratio[4] = {13.7f, 13.7f, 13.7f, 10.6f}, max_road[4] = {32.8f, 32.8f, 32.8f, 34.1f};
static float road_deg(const dash_state_t *s)
{
	if (isnan(s->steer_deg) || (unsigned)s->car >= 4) return 0;
	return fmaxf(-max_road[s->car], fminf(max_road[s->car], s->steer_deg / steer_ratio[s->car]));
}
static void tyres_ext(lv_event_t *e) { lv_event_set_ext_draw_size(e, ri(px(10))); }
static float wheel_pct(float nm) { return isnan(nm) ? NAN : sqrtf(fminf(1, fmaxf(0, nm) / 1000)) * 100; }
static void tyres_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	const lv_font_t *f = FS(REGULAR, 12, UI_S_FIG); /* as the dial's labels */
	lv_point_t tz;
	lv_text_get_size(&tz, "100%", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	float h = lv_area_get_height(&a), th = h * 0.44f, tw = th * 0.62f, gap = px(7), lw = fmaxf(1, px(1.5f)); /* gap: room for a
	                                                         front tyre's corner at full lock (its figure steps out with it) */
	float xs[2] = {a.x1 + tz.x + gap + tw / 2, a.x2 - tz.x - gap - tw / 2}, ys[2] = {a.y1 + th / 2, a.y2 - th / 2};
	/* greys opaque (W38 / W54 over black): a translucent line drawn in pieces stacks its alpha where they meet */
	const uint32_t AXLE = 0xFF616161u, TYRE_OFF = 0xFF8A8A8Au;
	for (int r = 0; r < 2; r++) line(layer, xs[0] + tw / 2, ys[r], xs[1] - tw / 2, ys[r], AXLE, lw, false); /* the axles */
	line(layer, (xs[0] + xs[1]) / 2, ys[0], (xs[0] + xs[1]) / 2, ys[1], AXLE, lw, false);                   /* the spine */
	float steer = road_deg(&S) * (float)M_PI / 180; /* the fronts turn with the wheel (left positive: counter-clockwise) */
	for (int i = 0; i < 4; i++) {
		float pct = wheel_pct(S.brake_nm[i]), cx = xs[i % 2], cy = ys[i / 2], th_ = i < 2 ? steer : 0;
		float co = cosf(th_), si = sinf(th_), hw = tw / 2, hh = th / 2;
		/* a point of the tyre (x right, y down, from its middle) on the screen, turned */
#define TP(x, y) {cx + (x) * co + (y) * si, cy - (x) * si + (y) * co}
		/* the tyre: an outline (red while it brakes, grey at 0), its corners slightly rounded (a fifth of its width) */
		float r = tw * 0.2f, o[24][2];
		const float ccx[4] = {hw - r, hw - r, -(hw - r), -(hw - r)}, ccy[4] = {-(hh - r), hh - r, hh - r, -(hh - r)};
		for (int c = 0, n = 0; c < 4; c++)   /* top right, bottom right, bottom left, top left: clockwise */
			for (int k = 0; k < 6; k++, n++) {
				float an = ((float)M_PI / 2) * (c - 1 + k / 5.0f), lx = ccx[c] + r * cosf(an), ly = ccy[c] + r * sinf(an);
				float pt[2] = TP(lx, ly);
				o[n][0] = pt[0], o[n][1] = pt[1];
			}
		uint32_t oc = !isnan(pct) && pct > 0 ? ALERT : TYRE_OFF; /* red only while that wheel brakes */
		for (int k = 0; k < 24; k++) line(layer, o[k][0], o[k][1], o[(k + 1) % 24][0], o[(k + 1) % 24][1], oc, lw, true);
		/* its bar, from the bottom up: one wide line along the tyre, inside the outline (one shape: no seams when turned) */
		float in = 1.5f * lw, y1 = hh - in, fh = (th - 2 * in) * (isnan(pct) ? 0 : pct) / 100;
		if (fh >= 1) {
			float b0[2] = TP(0, y1), b1[2] = TP(0, y1 - fh);
			line(layer, b0[0], b0[1], b1[0], b1[1], ALERT, tw - 2 * in, false);
		}
#undef TP
		char txt[8];   /* its figure, outside: left of the left tyres, right of the right ones */
		if (isnan(pct)) snprintf(txt, sizeof txt, "—"); else snprintf(txt, sizeof txt, "%d%%", (int)lroundf(pct));
		lv_draw_label_dsc_t ld;
		lv_draw_label_dsc_init(&ld);
		ld.font = f, ld.text = txt, ld.text_local = 1;
		ld.color = rgb(!isnan(pct) && pct > 0 ? ALERT : W54);
		ld.align = i % 2 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT;
		float lh = lv_font_get_line_height(f);
		float ext = hw * fabsf(co) + hh * fabsf(si), tg = px(4); /* the tyre's half width as turned: the figure clears it */
		lv_area_t ta = i % 2 ? (lv_area_t){ri(cx + ext + tg), ri(cy - lh / 2), a.x2 + ri(px(10)), ri(cy + lh / 2)}
				     : (lv_area_t){a.x1 - ri(px(10)), ri(cy - lh / 2), ri(cx - ext - tg), ri(cy + lh / 2)};
		lv_draw_label(layer, &ld, &ta);
	}
}
#define TYRES_H PG(31) /* the four tyres' height: between the g figures and the brake glow at the foot */

/* The g-meter block's height: the ball, 6 dp, the figure's line, the peak's line; then the pedal's label and bar. */
#define PEDAL_H PG(3)
#define PEDAL_GAP PG(14) /* the pedal's labelled bar, well clear of the figures */
static const lv_font_t *label_font(void) { return FS(REGULAR, 12, UI_S_FIG); } /* as the dial's labels */
static float g_height(void)
{
	return PG(72) + PG(6) + lv_font_get_line_height(FS(REGULAR, 16, UI_S_FIG)) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_FIG)) +
	       PEDAL_GAP + lv_font_get_line_height(label_font()) + PG(3) + PEDAL_H;
}

/* ---------- the brakes' temperatures (widgets.dart _BrakeTemps, _BrakeCross) ----------
 * Bluetooth alone on a two-motor car, once the Commander pushes them: BRAKES over the cross of the braking tyres (the
 * axles, the spine), a disc brake at each wheel, its figure outside it; in the g-meter's ball and figures' place, the
 * pedal under it where the g-meter's is. No steering angle over Bluetooth: the fronts stay straight. */
static lv_obj_t *brake_temps;
/* A temperature as shown: °F or °C as read (panda.dart temp). */
static float shown_deg(float c) { return S.fahrenheit ? c * 1.8f + 32 : c; }
static bool bt_brakes(const dash_state_t *s)
{
	if (!s->live || s->wifi_live || s->rwd) return false;
	for (int i = 0; i < 4; i++)
		if (!isnan(s->brake_temp_c[i])) return true;
	return false;
}
static const lv_font_t *brake_font(void) { return FS(REGULAR, 16, UI_S_FIG); } /* as the dial's figures */
static float brakes_h(void) { return PG(72) + PG(6) + lv_font_get_line_height(brake_font()) + lv_font_get_line_height(label_font()); }
static float brake_icon(void) /* the disc's size: under a third of the cross's height */
{
	return fminf(PG(17), (brakes_h() - lv_font_get_line_height(label_font()) - PG(8)) * 0.3f);
}
static float brake_room(void) /* a figure's width */
{
	lv_point_t t;
	lv_text_get_size(&t, "888°", brake_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return t.x;
}
static float brakes_w(void) { return 2 * (brake_room() + px(6) + brake_icon()) + PG(12); }
/* The disc brake (glyphs.dart kDiscBrake, the same shape): on a 102-unit square, the disc (r 34, its centre open to
 * r 14, five holes on r 24), the hub (r 9, its bore 3.5) and the caliper over it (r 38 to 50, 90°, ends round), the
 * disc's centre 6 under the square's; turned rot degrees clockwise. */
static void disc_brake(lv_layer_t *layer, float cx, float cy, float size, float rot, uint32_t c)
{
	float k = size / 102, t = DEG(rot), ox = cx - 6 * k * sinf(t), oy = cy + 6 * k * cosf(t);
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
#define RING(px_, py_, ro, rin, a0, a1, col) do { \
		d.center.x = ri(px_), d.center.y = ri(py_), d.radius = ri((ro) * k); \
		d.width = ri(((ro) - (rin)) * k) < 1 ? 1 : ri(((ro) - (rin)) * k); \
		d.start_angle = (a0), d.end_angle = (a1), d.color = rgb(col), d.opa = opa(col); \
		lv_draw_arc(layer, &d); \
	} while (0)
	RING(ox, oy, 34, 14, 0, 360, c); /* the disc */
	RING(ox, oy, 9, 3.5f, 0, 360, c); /* the hub */
	for (int i = 0; i < 5; i++) {     /* the holes: one under the caliper's middle, as turned */
		float a = DEG(-90 + 72 * i + rot);
		RING(ox + 24 * k * cosf(a), oy + 24 * k * sinf(a), 3, 0, 0, 360, BLACK);
	}
	d.rounded = 1;
	int r = (int)lroundf(rot);
	RING(ox, oy, 50, 38, ((225 + r) % 360 + 360) % 360, ((315 + r) % 360 + 360) % 360, c); /* the caliper */
#undef RING
}
static void brakes_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	const lv_font_t *lf = label_font(), *f = brake_font();
	lv_draw_label_dsc_t ld;
	lv_draw_label_dsc_init(&ld);
	ld.font = lf, ld.text = "BRAKES", ld.color = rgb(W70), ld.opa = opa(W70), ld.align = LV_TEXT_ALIGN_CENTER;
	ld.letter_space = ri(PF(0.8));
	float lh = lv_font_get_line_height(lf);
	lv_area_t la = {a.x1, a.y1, a.x2, ri(a.y1 + lh)};
	lv_draw_label(layer, &ld, &la);
	float top = a.y1 + lh + PG(8), s = brake_icon(), room = brake_room(), gap = px(6), lw = fmaxf(1, px(1.5f));
	float xs[2] = {a.x1 + room + gap + s / 2, a.x2 - room - gap - s / 2}, ys[2] = {top + s / 2, a.y2 - s / 2};
	const uint32_t AXLE = 0xFF616161u; /* as the braking tyres' */
	for (int r = 0; r < 2; r++) line(layer, xs[0] + s / 2, ys[r], xs[1] - s / 2, ys[r], AXLE, lw, false); /* the axles */
	line(layer, (xs[0] + xs[1]) / 2, ys[0], (xs[0] + xs[1]) / 2, ys[1], AXLE, lw, false);                 /* the spine */
	float fh = lv_font_get_line_height(f);
	for (int i = 0; i < 4; i++) {
		float cx = xs[i % 2], cy = ys[i / 2];
		disc_brake(layer, cx, cy, s, i % 2 ? 90 : -90, W70); /* the caliper outward */
		char txt[12];
		if (isnan(S.brake_temp_c[i])) snprintf(txt, sizeof txt, "—");
		else snprintf(txt, sizeof txt, "%d°", (int)lroundf(shown_deg(S.brake_temp_c[i])));
		lv_draw_label_dsc_init(&ld);
		ld.font = f, ld.text = txt, ld.text_local = 1, ld.color = rgb(WHITE);
		ld.align = i % 2 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT;
		lv_area_t ta = i % 2 ? (lv_area_t){ri(cx + s / 2 + px(4)), ri(cy - fh / 2), a.x2, ri(cy + fh / 2)}
				     : (lv_area_t){a.x1, ri(cy - fh / 2), ri(cx - s / 2 - px(4)), ri(cy + fh / 2)};
		lv_draw_label(layer, &ld, &ta);
	}
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
static const lv_image_dsc_t *chip_icons[CHIPS] = {&ic_trunk, &ic_mirror, &ic_door_fl, &ic_door_rl, &ic_charge_port, &ic_massage,
						  &ic_frunk, &ic_recirc, &ic_door_fr, &ic_door_rr, &ic_interior, &ic_massage};

/* Each side column: its chips, then the links (left) or the car panel button (right), spaced evenly in the full
 * height. */
static float chip_gap(void) /* the phone layout: the chips in the row, the links in the bar under it */
{
	return G.phone ? (G.chip_area - CHIP_SIDE * PC(48)) / (CHIP_SIDE + 1) : (G.chip_area - CHIP_SIDE * PC(48) - PL(44)) / (CHIP_SIDE + 2);
}
static void chip_geom(int i, float *x, float *y)
{
	float item = PC(48), gap = chip_gap(); /* Column spaceEvenly, each chip 3 dp padded, the links' row the last */
	int row = i % CHIP_SIDE;
	float cy = G.mt + gap * (row + 1) + item * row + item / 2;
	*y = cy - PC(21);
	*x = i < CHIP_SIDE ? G.ml + inset(cy) : G.W - G.ml - inset(cy) - PC(52);
}

/* The countdown round a link's icon (widgets.dart _RingPainter): a faint track, and over it what is left, from the top
 * clockwise. Stepped once a second (the phone eases it four times a second; not worth the T113's CPU). */
static void link_ring_draw(lv_event_t *e)
{
	typeof(link_w) *l = lv_event_get_user_data(e);
	if (l->left <= 0 || l->span <= 0) return;
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.center.x = (a.x1 + a.x2 + 1) / 2;
	d.center.y = (a.y1 + a.y2 + 1) / 2;
	d.color = rgb(l->c);
	float r = PL(12); /* the 28 dp square deflated by 2: the stroke's middle */
	d.width = ri(PL(2)), d.radius = ri(r + PL(1)), d.opa = (lv_opa_t)(opa(l->c) * 0.18f);
	d.start_angle = 0, d.end_angle = 360;
	lv_draw_arc(lv_event_get_layer(e), &d);
	float left = fminf(1, (float)l->left / l->span);
	d.width = ri(PL(2.5)), d.radius = ri(r + PL(1.25)), d.opa = opa(l->c), d.rounded = 1;
	d.start_angle = 270, d.end_angle = (270 + ri(360 * left)) % 360;
	if (left >= 1) d.start_angle = 0, d.end_angle = 360;
	lv_draw_arc(lv_event_get_layer(e), &d);
}
static void link_make(typeof(link_w) *l)
{
	l->box = box(root);
	l->ring = box(l->box);
	lv_obj_add_event_cb(l->ring, link_ring_draw, LV_EVENT_DRAW_MAIN, l);
	l->secs = label(l->box, FS(BOLD, 13, UI_S_LINK), WHITE);
	l->icon = icon(l->box, &ic_wifi_s, WHITE);
	l->word = label(l->box, FS(REGULAR, 10, UI_S_LINK), WHITE);
	lv_obj_set_style_text_letter_space(l->word, ri(PL(0.5)), 0);
	l->stat = label(l->box, FS(BOLD, 12, UI_S_LINK), WHITE);
}

static void fps_show(void);
void dash_ui_init(lv_obj_t *scr)
{
	unlink(PJ_VISIBLE); /* a restarted UI starts on the dash, so no touches go to a phone until it shows the picture */
	unlink(PJ_GLOW);    /* (and no glow of the UI before this one stays on the picture) */
	lv_display_t *disp = lv_obj_get_display(scr);
	G.W = lv_display_get_horizontal_resolution(disp);
	G.H = lv_display_get_vertical_resolution(disp);
	G.phone = DASH_PHONE_LAYOUT; /* the device profile's layout set: the Dash's strip or the phone app's */
	G.k = G.H / 480;
	G.ml = px(14), G.mt = px(6), G.mb = px(10);
	G.row_b = G.phone ? G.H - G.mb - PL(44) - px(6) : G.H; /* main.dart: the bar 6 dp under the row */
	G.chip_area = (G.phone ? G.row_b : G.H - G.mb) - G.mt;
	/* the side columns' last row; the phone layout's bar */
	G.link_top = G.phone ? G.H - G.mb - PL(44) : G.mt + (CHIP_SIDE + 1) * chip_gap() + CHIP_SIDE * PC(48);
	/* The ring: the radius the app's foot brake left room for (r 247 at 480, 1.28 x the app's at 1536 dp, kept as
	 * it is), centred on the screen both ways now that the foot brake is under the g-meter. At 480 its arcs end
	 * 26 px from the top and bottom, the limit sign starts at 36, the gear letter ends at 469. */
	ring_geom_full();
	G.cx = G.W / 2; /* until the first layout */
	G.card_w = PN(154);
	G.cab = ui_car_pics[0].car->header.h / px(236);

	fps_show(); /* (LVGL shows its counter by itself) */
	lv_obj_set_style_bg_color(scr, rgb(BLACK), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(scr, false);
	root = box(scr);
	place(root, 0, 0, G.W, G.H);

	/* Edge glows, under everything. */
	glow_make(&glow_dsc[0], false);
	glow_make(&glow_dsc[1], true);
	glow_l = icon(root, &glow_dsc[0], REGEN);
	lv_image_set_inner_align(glow_l, LV_IMAGE_ALIGN_TOP_LEFT); /* cut to the split's strip, it keeps its bright edge */
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
	/* the path lines: over the edge glows (as the app paints them, under the whole dash), under the chips, the route
	 * card and the car; over the whole screen (only the dots' areas are repainted), dots under a chip or the card left out */
	cab.frame = box(root); /* an unbuckled seat's red frame: the decoration under the picture, so under the dots too */
	steer = box(root);
	place(steer, 0, 0, G.W, G.H);
	lv_obj_add_event_cb(steer, steer_draw, LV_EVENT_DRAW_MAIN, NULL);

	/* The pedal (main.dart's line along the top): a bar under the g-meter, a hairline track and the fill in kAccent. */
	pedal = box(root);
	fill(pedal, W24);
	lv_obj_set_style_radius(pedal, LV_RADIUS_CIRCLE, 0);
	pedal_name = label(root, label_font(), W70);
	lv_obj_set_style_text_letter_space(pedal_name, ri(PF(0.8)), 0);
	pedal_val = label(root, label_font(), WHITE);
	brake_car = box(root); /* braking: the four tyres in the bar's place, each lit by its own wheel's torque */
	lv_obj_add_event_cb(brake_car, tyres_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(brake_car, tyres_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL); /* the turned fronts' corners, past the box */
	show(brake_car, false);
	pedal_fill = box(pedal);
	fill(pedal_fill, ACCENT);
	lv_obj_set_style_radius(pedal_fill, LV_RADIUS_CIRCLE, 0);

	/* The action chips (widgets.dart _Actions). */
	for (int i = 0; i < CHIPS; i++) {
		chip_t *c = &chips[i];
		float x, y;
		chip_geom(i, &x, &y);
		c->box = box(root);
		place(c->box, x, y, PC(52), PC(42));
		lv_obj_set_style_radius(c->box, ri(PC(20)), 0);
		c->icon = icon(c->box, chip_icons[i], WHITE);
		align(c->icon, LV_ALIGN_CENTER, 0, 0);
		c->note = label(c->box, FS(REGULAR, 11, UI_S_CHIP), WHITE);
		c->word = label(c->box, FS(REGULAR, 10, UI_S_CHIP), WHITE);
		lv_obj_set_style_text_align(c->word, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_line_space(c->word, -ri(PC(1)), 0);
		show(c->word, false);
	}

	/* The links (bottom left) and the car panel button (bottom right); the Maps button is phone-only. */
	link_make(&link_w);
	link_make(&link_b);
	car_panel = icon(root, &ic_car_panel, WHITE);
	{
		/* the right column's last row, its right edge on the chips' (IconButton: 40 dp, the 24 dp icon centred) */
		float cy = G.link_top + PL(22);
		int car_x = ri(G.W - G.ml - inset(cy) - PL(20) - PL(12)), car_y = ri(cy - PL(12));
		lv_obj_set_pos(car_panel, car_x, car_y);
		car_at[0] = car_x, car_at[1] = car_y;
		/* Android Auto on demand, a gap to the car panel's left: the Android Auto logo, outline (gen/aa_icon.c, from
		 * assets/android_auto.svg), a 36 px button as ic_car_panel, level with it. Cert-gated. */
		aa_btn = icon(root, &ic_aa, WHITE);
		if (extra_icon()) x_btn = icon(root, &ic_x, WHITE);
		aa_place(car_x, car_y);
		show(aa_btn, false);
		/* the phone layout's route pill ends at the AA button's 40 dp box (the app's Maps IconButton), shown or not
		 * (the second button's, where there is one) */
		car_bar_r = car_x - ri(PL(16)) - ic_aa.header.w / 2.0f - PL(20) - (x_btn ? ri(PL(16)) + ic_aa.header.w : 0);
	}

	/* Left: the lights row over the car. */
	lights_row = box(root);
	cab.grp = box(root);
	lv_obj_set_overflow_visible(cab.grp, true);
	cab.car_type = -1;

	/* basic's page dots (hidden in the full layout) */
	for (int i = 0; i < 4 && G.phone; i++)
		for (int c = 0; c < DASH_CARDS; c++) {
			dots[i][c] = box(root);
			lv_obj_set_style_radius(dots[i][c], LV_RADIUS_CIRCLE, 0);
			show(dots[i][c], false);
		}

	/* The route column. */
	dest_card = box(root);
	if (G.phone) fill(dest_card, 0x24FFFFFF); /* the app's bar: white at 0.14 */
	else border(dest_card, 0x30FFFFFF, 1);
	lv_obj_set_style_radius(dest_card, ri(PN(12)), 0);
	no_route = label(root, FS(REGULAR, 14, UI_S_NAV), W38);
	lv_obj_set_style_text_letter_space(no_route, ri(PN(2)), 0);
	lv_label_set_text(no_route, "NO ROUTE");
	lv_obj_set_style_text_align(no_route, LV_TEXT_ALIGN_CENTER, 0);
	show(no_route, false);
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
	lv_obj_add_event_cb(speed_l, figure_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	unit_l = label(ring, FS(REGULAR, 15, UI_S_RING), W60);
	lv_obj_set_style_text_letter_space(unit_l, ri(PR(3)), 0);
	gear_l = label(ring, FS(BOLD, 28, UI_S_RING), WHITE);
	cue = run_new(ring);
	lv_obj_set_style_transform_pivot_x(cue, LV_PCT(50), 0), lv_obj_set_style_transform_pivot_y(cue, LV_PCT(50), 0);
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
	lv_obj_add_event_cb(rear_v, figure_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_add_event_cb(front_v, figure_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
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
	for (int i = 0; i < 3; i++) {   /* as the figures under them (stat_lab, stat_val): one table */
		flow_n[i] = label(charge_box, FS(REGULAR, 12, UI_S_FIG), W70);
		lv_obj_set_style_text_letter_space(flow_n[i], ri(PF(0.8)), 0);
		flow_v[i] = label(charge_box, FS(REGULAR, 16, UI_S_FIG), WHITE);
	}
	stats_box = box(root);
	for (int i = 0; i < 5; i++) {
		stat_lab[i] = label(stats_box, FS(REGULAR, 12, UI_S_FIG), W70);
		lv_obj_set_style_text_letter_space(stat_lab[i], ri(PF(0.8)), 0);
		stat_val[i] = label(stats_box, FS(REGULAR, 16, UI_S_FIG), WHITE);
	}

	/* The g-meter, its own column on the right. */
	gball = box(root);
	lv_obj_add_event_cb(gball, gball_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(gball, gball_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL); /* the dot on the rim, past the box */
	g_val = run_new(root);
	g_peak = label(root, FS(REGULAR, 12, UI_S_FIG), W70); /* as the dial's labels */
	lv_obj_set_style_text_letter_space(g_peak, ri(PF(0.8)), 0);
	brake_temps = box(root); /* Bluetooth alone: the brakes' temperatures in the ball's place */
	lv_obj_add_event_cb(brake_temps, brakes_draw, LV_EVENT_DRAW_MAIN, NULL);
	show(brake_temps, false);
	if (G.phone) { /* the power card (basic's and the split's: cards_parts shows it), the speedometer's needle */
		pdial = box(root);
		lv_obj_add_event_cb(pdial, pdial_draw, LV_EVENT_DRAW_MAIN, NULL);
		pdial_kw = label(pdial, F(BOLD, 40), WHITE); /* sized to the dial as it is placed (pdial_set) */
		lv_obj_add_event_cb(pdial_kw, figure_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
		pdial_unit = label(pdial, F(REGULAR, 14), W54);
		lv_label_set_text(pdial_unit, "kW");
		pdial_range = label(pdial, F(REGULAR, 15), W70);
		show(pdial, false);
		lv_obj_add_event_cb(ring, speedo_needle, LV_EVENT_DRAW_POST, NULL);
	}
	touch_init();
}

/* ---------- set ---------- */
#define CHANGED(f) (first || memcmp(&S.f, &P.f, sizeof S.f))

static void fmt_deg(char *b, size_t n, float c) { snprintf(b, n, "%d°", (int)lroundf(shown_deg(c))); }
static uint32_t air_color(float c) { return !isnan(c) && lroundf(c) <= 3 ? COLD : WHITE; }

/* The car panel action each button's place holds (ST.slots: "Group|Name", car_actions.dart homeSlots), an index of
 * cmdr_car_acts, or -1: its own button. Worked out again whenever the settings change (slots_refresh). */
static int slot_act[CHIPS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static void slots_refresh(void)
{
	for (int i = 0; i < CHIPS; i++) {
		slot_act[i] = -1;
		const char *bar = strchr(ST.slots[i], '|');
		if (!bar) continue;
		size_t g = (size_t)(bar - ST.slots[i]);
		for (int k = 0; k < cmdr_car_act_count; k++)
			if (strlen(cmdr_car_acts[k].group) == g && !strncmp(cmdr_car_acts[k].group, ST.slots[i], g) &&
			    !strcmp(cmdr_car_acts[k].name, bar + 1))
				slot_act[i] = k;
	}
}
/* [t] on two lines at the space nearest its middle (widgets.dart _twoLines), into [o] */
static void two_lines(const char *t, char *o, size_t n)
{
	snprintf(o, n, "%s", t);
	int at = -1, len = (int)strlen(o);
	for (int i = 0; i < len; i++)
		if (o[i] == ' ' && (at < 0 || abs(2 * i - len) < abs(2 * at - len))) at = i;
	if (at >= 0) o[at] = '\n';
}

static void set_chips(const dash_state_t *s)
{
	bool lit[CHIPS] = {s->trunk, s->mirror_tilt && s->wifi_live, s->door_fl, s->door_rl, s->charge_port && s->wifi_live,
			   massage_mode(s->massage_l), s->frunk, s->recirc, s->door_fr, s->door_rr, s->interior && s->wifi_live,
			   massage_mode(s->massage_r)};
	for (int i = 0; i < CHIPS; i++) {
		chip_t *c = &chips[i];
		if (slot_act[i] >= 0) { /* a car panel action in its place: its name, two lines, shrunk to the chip (custom) */
			const cmdr_car_act *a = &cmdr_car_acts[slot_act[i]];
			char t[64];
			two_lines(a->name, t, sizeof t);
			bool pending = s->press_pending >= ACT_COUNT + a->first && s->press_pending < ACT_COUNT + a->first + a->n;
			show(c->icon, false), show(c->note, false), show(c->word, true);
			fill(c->box, 0);
			border(c->box, pending ? ACCENT : W70, pending ? PC(2) : PC(1));
			if (!strcmp(lv_label_get_text(c->word), t)) continue; /* laid out already */
			lv_label_set_text(c->word, t);
			lv_obj_update_layout(c->word);
			float w = lv_obj_get_width(c->word), room = PC(52) - 2 * PC(4);
			lv_obj_set_style_transform_scale(c->word, w > room ? (int32_t)(256 * room / w) : 256, 0);
			lv_obj_set_style_transform_pivot_x(c->word, ri(w / 2), 0);
			lv_obj_set_style_transform_pivot_y(c->word, lv_obj_get_height(c->word) / 2, 0);
			lv_obj_center(c->word);
			continue;
		}
		show(c->word, false), show(c->icon, true);
		bool on = lit[i] && s->live;
		int level = 0;
		bool note = false;
		if (i == CHIP_RECIRC && s->co2_ppm >= 0) { /* the cabin's CO2 on the recirculation chip */
			note = true;
			int co2 = s->wifi_live && !s->co2_stale ? s->co2_ppm : -1;
			level = co2 >= 2500 ? 2 : co2 >= 1500 ? 1 : 0;
			if (co2 >= 0) set_text(c->note, "%d", co2);
			else set_text(c->note, "");
		}
		if (i == CHIP_MASSAGE_L || i == CHIP_MASSAGE_R) { /* which seat: L or R under it */
			note = true;
			set_text(c->note, i == CHIP_MASSAGE_L ? "L" : "R");
		}
		uint32_t f = level == 2 ? ALERT : level == 1 ? WARN : LIT;
		uint32_t col = on && level < 2 ? BLACK : WHITE;
		uint32_t note_col = on ? col : level == 2 ? ALERT : level == 1 ? WARN : col;
		bool pending = s->press_pending == chip_act[i] || /* a press out: its side in the accent until the answer */
			       (i == CHIP_MASSAGE_L && massage_side(s->press_pending) == 'L') ||
			       (i == CHIP_MASSAGE_R && massage_side(s->press_pending) == 'R');
		fill(c->box, on ? f : 0);
		border(c->box, pending ? ACCENT : on ? f : W70, pending ? PC(2) : PC(1));
		icon_color(c->icon, col);
		show(c->note, note);
		if (note) {
			img_src(c->icon, i == CHIP_RECIRC ? &ic_recirc_s : &ic_massage_s);
			text_color(c->note, note_col);
			/* 22 dp glyph over an 11 dp figure, the pair centred */
			float gh = PC(22), th = lv_font_get_line_height(lv_obj_get_style_text_font(c->note, 0)) * 1.1f / 1.172f;
			float top = (PC(42) - PC(2) - gh - th) / 2; /* the chip's 1 dp border each side */
			align(c->icon, LV_ALIGN_TOP_MID, 0, ri(top));
			put_text(c->note, 1, (PC(52) - 2 * ri(PC(1))) / 2, top + gh + baseline_in(lv_obj_get_style_text_font(c->note, 0), 1.1f), PC(52) - 2 * ri(PC(1)));
		} else if (i == CHIP_RECIRC) {
			img_src(c->icon, &ic_recirc);
			align(c->icon, LV_ALIGN_CENTER, 0, 0);
		}
	}
}

static void set_link(typeof(link_w) *l, int slot, const lv_image_dsc_t *big, const lv_image_dsc_t *small, uint32_t c,
		     const char *word, bool settled, int stat, int secs, int span)
{
	bool timer = !settled && secs > 0;
	show(l->ring, timer), show(l->secs, timer);
	if (!timer) secs = 0;
	if (secs != l->left || span != l->span || (timer && c != l->c)) { /* the ring: once a second at most */
		l->left = (int16_t)secs, l->span = (int16_t)span, l->c = c;
		lv_obj_invalidate(l->ring);
	}
	int32_t scale = timer ? 256 * 16 / 22 : 256; /* the icon at 16 dp inside the ring, 22 without */
	if (lv_image_get_scale(l->icon) != scale) lv_image_set_scale(l->icon, scale);
	float cy = G.link_top + PL(22), x0 = G.ml + inset(cy); /* the left column's last row, from its stepped edge */
	float x = x0 + (slot == 0 ? 0 : (S.wifi == WIFI_LIVE ? PL(36) : PL(60))), w = settled ? PL(36) : PL(60), y = G.link_top;
	if (view == DASH_VIEW_PROJECT && G.phone) { /* the phone layout's split: stacked at the bar's left end over the
						    * picture, the buttons the rest of it; one width for every state (status only here) */
		w = PL(60), x = x0, y = (PJ_RGN_Y - 2 * PL(44) - PL(4)) / 2 + slot * (PL(44) + PL(4));
	} else if (G.phone && ST.basic) { /* basic: the bar under the ring, from its left end */
		x += G.cx - G.D / 2 - x0;
	} else if (view == DASH_VIEW_PROJECT) { /* the split: the slanted strip left of the picture, stacked, right edge on it; */
		y = G.H - G.mb - (2 - slot) * PL(44) - (1 - slot) * PL(6); /* one width for every state: centred on one line, */
		w = PL(60), x = PJ_RGN_X - px(4) - w;                         /* and never resized across into the picture */
	}
	place(l->box, x, y, w, PL(44));
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
	} else { /* the icon in its 28 dp spot, the word under it, the pair centred; a timer's ring round the icon and its
		  * seconds beside it, in two digits' room (the phone's left 45, width 15) */
		img_src(l->icon, big);
		icon_color(l->icon, c);
		show(l->stat, false);
		show(l->word, true);
		set_text(l->word, "%s", word);
		text_color(l->word, c);
		float wh = PL(10) * 1.1f, top = (PL(44) - PL(28) - PL(2) - wh) / 2;
		align(l->icon, LV_ALIGN_TOP_MID, 0, ri(top + (PL(28) - PL(22)) / 2));
		if (timer) {
			place(l->ring, (w - PL(28)) / 2, top, PL(28), PL(28));
			set_text(l->secs, "%d", secs);
			text_color(l->secs, c);
			const lv_font_t *fs = lv_obj_get_style_text_font(l->secs, 0);
			put_text(l->secs, 0, PL(45), top + PL(14) - lv_font_get_line_height(fs) / 2.0f + ascent(fs), w);
		}
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

static dash_settings_t ST; /* the settings as shown (defined with the settings screen below) */
static bool projecting;    /* an Android Auto session is up (/run/dash-project, raised by aa serve) */
const char *dash_ui_pj_marker = "/run/dash-project"; /* the shots harness points these at files of its own */
const char *dash_ui_aa_cert = "/etc/wattline/aa.crt", *dash_ui_aa_up = "/run/dash-aa", *dash_ui_pj_fps = PJ_FPS;
static const char *aa_start_lock = "/run/dash-aa.starting";   /* dash-aa start holds it until it ends, up or not */
static bool aa_pending;    /* tapped start, waiting for /run/dash-aa to appear: shown on at once */
static bool aa_pending_x;  /* that start is the second button's (dash-aa extra) */
static bool extra_up(void) { return access(dash_ui_extra_up, F_OK) == 0; }
static bool aa_stopping;   /* tapped stop (or Exit), waiting for /run/dash-aa to go: shown off at once */
#define AA_STOP_MS 45000   /* dash-aa stop's longest: a sync, its hook 12 s, the receiver's goodbye and the kills 5 s, the
                             access point's 5 s, two bluetoothctl calls 5 s each, and a margin */
#define AA_START_MS 90000  /* dash-aa start's longest: the Wi-Fi lock 30 s, the AP, its hook 12 s, bluetoothctl, a sync */
static uint32_t aa_stop_at;
#define AA_BLUE 0xFF03A9F4u /* Android Auto's blue (its logo's): the button while AA is on */
static bool pj_left;       /* Exit tapped in the split: stay on the dash until this session's marker is gone */
static uint32_t pj_leaving; /* Exit tapped where a frame can be shown whole: the split stays until the marker is gone */
void (*dash_ui_whole_frame)(void);
/* After leaving the split, repaint everything once more a moment later: the decoder is stopped in the background
 * (dash-aa stop) and may draw a last picture after the first repaint. */
static void pj_repaint_cb(lv_timer_t *t)
{
	lv_timer_delete(t);
	if (view == DASH_VIEW_PROJECT) return; /* back in the split already: a full repaint would cover the picture */
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
}
/* The phone's pictures shown in the last second (spike-vdec, rewritten each second), looked at once a second; -1 none,
 * or older than 3 s (a decoder gone) */
static int pj_fps(void)
{
	static uint32_t at;
	static int fps = -1;
	static const char *path; /* the shots harness points it elsewhere: looked at anew then */
	if (at && path == dash_ui_pj_fps && lv_tick_elaps(at) < 1000) return fps;
	at = lv_tick_get() | 1, path = dash_ui_pj_fps;
	struct stat st;
	FILE *f;
	fps = -1;
	if (stat(dash_ui_pj_fps, &st) == 0 && time(NULL) - st.st_mtime <= 3 && (f = fopen(dash_ui_pj_fps, "r"))) {
		if (fscanf(f, "%d", &fps) != 1 || fps < 0 || fps > 999) fps = -1;
		fclose(f);
	}
	return fps;
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
	int secs = s->wifi_secs;
	/* on a network that is not the car's adapter (home WiFi: OTA, SSH): say so, not an endless hello and retry */
	const char *car_net = "S3XY_OBD"; /* the Commander's network: by name (the saved list's order changes on save) */
	if (s->wifi != WIFI_LIVE && s->net_ssid[0] && strcmp(s->net_ssid, car_net)) {
		wi = &ic_wifi_l, wc = s->ssh > 0 ? REGEN : W70, ww = s->ssh > 0 ? "SSH" : "NET", secs = -1;
	}
	if (projecting && s->wifi != WIFI_LIVE) { /* the phone on our AP: the pictures it shows per second, once known */
		static char fw[16];   /* any int's "%d fps" */
		int fps = pj_fps();
		if (fps >= 0) snprintf(fw, sizeof fw, "%d fps", fps);
		wi = &ic_wifi_l, wc = REGEN, ww = fps >= 0 ? fw : "AA", secs = -1;
	}
	set_link(&link_w, 0, wi, &ic_wifi_s, wc, ww, s->wifi == WIFI_LIVE, s->wifi == WIFI_LIVE ? s->speed_hz : -1, secs,
		 s->wifi_span);
	uint32_t bc;
	const lv_image_dsc_t *bi;
	const char *bw = bt_word(s->bt, &bc, &bi);
	set_link(&link_b, 1, bi, &ic_bt_s, bc, bw, s->bt == BT_LEAN || s->bt == BT_FULL, -1, s->bt_secs, s->bt_span);
}

static char lights_shown[96] = "-"; /* the lamps' row as built and placed ("-": build it again) */
/* [maxw]: the room it has; wider (every lamp lit on a narrow column) it is scaled down to it, the app's FittedBox */
static void set_lights(const dash_state_t *s, float cx, float top, float maxw)
{
	struct { const char *w; const lv_image_dsc_t *i; uint32_t c; } lamp[5];
	int n = 0;
	if (s->live && s->drl) lamp[n++] = (typeof(lamp[0])){"DRL", &ic_drl, REGEN};
	if (s->live && s->low_beam) lamp[n++] = (typeof(lamp[0])){"LOW", &ic_low, REGEN};
	if (s->live && s->high_beam) lamp[n++] = (typeof(lamp[0])){"HIGH", &ic_high, ACCENT};
	if (s->wifi_live && s->grip != GRIP_NONE)
		lamp[n++] = (typeof(lamp[0])){s->grip_word[0] ? s->grip_word : "GRIP", &ic_grip, s->grip == GRIP_SLIPPERY ? COLD : WARN};
	if (s->live && s->defog[0]) lamp[n++] = (typeof(lamp[0])){s->defog, &ic_defrost, REGEN}; /* the windscreen's, while on */
	char key[96] = "";
	for (int i = 0; i < n; i++) snprintf(key + strlen(key), sizeof key - strlen(key), "%s%x", lamp[i].w, (unsigned)lamp[i].c);
	snprintf(key + strlen(key), sizeof key - strlen(key), "@%d,%d,%d", ri(cx), ri(top), ri(maxw));
	if (!strcmp(key, lights_shown)) return;
	strcpy(lights_shown, key);
	lv_obj_clean(lights_row);
	const lv_font_t *f = FS(REGULAR, 12, UI_S_LIGHTS);
	float x = 0, ls = PLI(1), wd[sizeof lamp / sizeof lamp[0]];
	for (int i = 0; i < n; i++) {
		lv_point_t sz;
		lv_text_get_size(&sz, lamp[i].w, f, ri(ls), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		wd[i] = fmaxf(PLI(20), sz.x + ls) + 2 * PLI(5);
		x += wd[i];
	}
	place(lights_row, cx - x / 2, top, x, PLI(20) + PLI(2) + lv_font_get_line_height(f));
	lv_obj_set_style_transform_scale(lights_row, x > maxw && maxw > 0 ? (int32_t)(256 * maxw / x) : 256, 0);
	lv_obj_set_style_transform_pivot_x(lights_row, ri(x / 2), 0); /* its middle and top kept */
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
	show(cab.grp, true);
	/* The cabin's box: 4 dp frame + 4 dp above, 4 dp frame + 6 dp below, the picture 6 dp in each side; fitted to the
	 * column, then moved 7 % of its height down (more of the road ahead). */
	float box_h = cb(4 + 4) + cab.h + cb(4 + 6);
	(void)box_h;
	cab.x = cx - cab.w / 2, cab.y = top + cb(8);
	lv_obj_set_pos(cab.grp, ri(cab.x - cab.ox), ri(cab.y - cab.oy));
	bool seats = s->wifi_live && s->cabin, belt = false;
	for (int i = 0; i < DS_SEAT_COUNT; i++)
		if (seats && s->seat[i] == DS_SEAT_UNBUCKLED && p->seat_red[i].img) belt = true;
	show(cab.frame, belt);
	if (belt) {
		place(cab.frame, cab.x - cb(6), cab.y, cab.w + 2 * cb(6), cab.h);
		fill(cab.frame, 0x2EE23D3D);
		border(cab.frame, ALERT, cb(1));
		SET_NUM(cab.frame, lv_obj_get_style_radius, lv_obj_set_style_radius, ri(cb(12)));
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
		/* that wheel braking while moving (WiFi's torque, NAN without it): its brake % in red instead, as the brake
		 * car's scale; at a standstill the held brake would hide the pressures */
		int bk = isnan(s->brake_nm[i]) ? 0 : (int)lroundf(wheel_pct(s->brake_nm[i]));
		bool braked = bk >= 1 && s->speed > 0; /* (NAN speed: false) */
		if (braked) set_text(cab.tyre[i], "%d", bk);
		else if (isnan(s->tyre_psi[i])) set_text(cab.tyre[i], "—");
		else if (s->psi) set_text(cab.tyre[i], "%d", (int)lroundf(s->tyre_psi[i]));
		else set_text(cab.tyre[i], "%.1f", s->tyre_psi[i] / 14.5038f);
		set_text(cab.psi[i], "%s", braked ? "%" : s->psi ? "psi" : "bar"); /* at every frame (kept when the same): a change
								      * made while the car was not shown (basic without its card) was missed */
		text_color(cab.tyre[i], braked ? ALERT : WHITE);
		text_color(cab.psi[i], braked ? ALERT : W60);
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
		for (int i = 0; seats && i < DS_SEAT_COUNT; i++)
			if (seat_row[i] == r + 1 && s->seat[i] == DS_SEAT_UNBUCKLED && p->seat_red[i].img) unb = true;
		/* over Bluetooth (no seat map) the rows it has: the bridge leaves only the front's, once pushed */
		bool vis = (seats || (s->live && !s->wifi_live)) && !isnan(s->cabin_c[r]) && !unb;
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

/* The phone layout's route: in the full layout's bar, else a card (basic's; the split's, its bar has the buttons). */
static bool route_bar(void) { return view != DASH_VIEW_PROJECT && !ST.basic; }
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
	if (G.phone && route_bar()) { /* the app's bar (widgets.dart _Nav): DESTINATION (and the place after it) over the figures in one
			* row, 20 dp apart, centred on the pill 12 dp in; the gaps close up rather than overflow */
		float w = G.bar_r - G.bar_l, ph = PN(12), pv = PN(s->dest[0] ? 1 : 3), gap = PN(20), tot, rh;
		const lv_font_t *hf = lv_obj_get_style_text_font(dest_head, 0), *nf = lv_obj_get_style_text_font(dest_name, 0);
		/* too narrow (the split's bar): the lesser figures go, as the app's one-line route does */
		const lv_image_dsc_t *drop[] = {&ic_route, &ic_bolt, &ic_flag, &ic_battery, &ic_ecar};
		for (int d = 0;; d++) {
			tot = 0, rh = 0;
			for (int i = 0; i < rows; i++) {
				stat_run(dest_row[i], ic[i], b[i], col[i], n[i]);
				tot += lv_obj_get_style_width(dest_row[i], 0);
				rh = fmaxf(rh, lv_obj_get_style_height(dest_row[i], 0));
			}
			if (rows <= 1 || tot + (rows - 1) * PN(8) <= w - 2 * ph || d == (int)(sizeof drop / sizeof drop[0])) break;
			for (int i = 0; i < rows; i++)
				if (ic[i] == drop[d]) {
					for (int j = i; j < rows - 1; j++)
						memcpy(b[j], b[j + 1], 32), memcpy(n[j], n[j + 1], 16), ic[j] = ic[j + 1], col[j] = col[j + 1];
					rows--;
					break;
				}
		}
		for (int i = 0; i < 6; i++) show(dest_row[i], i < rows);
		if (rows > 1 && tot + (rows - 1) * gap > w - 2 * ph) gap = fmaxf(0, (w - 2 * ph - tot) / (rows - 1));
		lv_point_t hs;
		lv_text_get_size(&hs, "DESTINATION", hf, ri(PN(1.5)), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		float l1 = lv_font_get_line_height(hf), nw = 0, after = PN(10 + 15 + 4); /* 10 dp, the pin, 4 dp */
		show(dest_name, s->dest[0]);
		show(dest_pin, s->dest[0]);
		if (s->dest[0]) {
			lv_point_t ns;
			lv_text_get_size(&ns, s->dest, nf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			nw = fmaxf(0, fminf(ns.x, w - 2 * ph - hs.x - after));
			set_text(dest_name, "%s", s->dest);
			lv_obj_set_size(dest_name, ri(nw), lv_font_get_line_height(nf));
			l1 = fmaxf(l1, lv_font_get_line_height(nf));
		}
		/* the pill as wide as what it holds (the wider line, 20 dp each side), centred where the bar's room is */
		float row_w = tot + (rows > 1 ? (rows - 1) * gap : 0), head_w = hs.x + (s->dest[0] ? after + nw : 0);
		float cw = fminf(w, fmaxf(row_w, head_w) + 2 * PN(20)), cl = G.bar_l + (w - cw) / 2;
		w = cw;
		float h = 2 * pv + l1 + rh, x = (w - head_w) / 2;
		place(dest_card, cl, G.bar_cy - h / 2, w, h);
		lv_obj_set_pos(dest_head, ri(x), ri(pv + (l1 - lv_font_get_line_height(hf)) / 2));
		if (s->dest[0]) {
			lv_obj_set_pos(dest_pin, ri(x + hs.x + PN(10)), ri(pv + (l1 - PN(15)) / 2));
			lv_obj_set_pos(dest_name, ri(x + hs.x + after), ri(pv + (l1 - lv_font_get_line_height(nf)) / 2));
		}
		x = (w - tot - (rows > 1 ? (rows - 1) * gap : 0)) / 2;
		for (int i = 0; i < rows; i++) {
			lv_obj_set_pos(dest_row[i], ri(x), ri(pv + l1 + (rh - lv_obj_get_style_height(dest_row[i], 0)) / 2));
			x += lv_obj_get_style_width(dest_row[i], 0) + gap;
		}
		return;
	}
	/* Laid out as the mockup (the header, the place, then a row each), scaled with the column; the rows
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
	/* plugged in, not charging (at the limit): the wall's power, what the charger feeds in the background */
	bool wall = !charge && s->wifi_live && s->plugged && s->gear == 'P' && !isnan(s->wall_kw);
	/* The speed, scaled to the hole (0.5 x 0.4 of the ring) as the app's FittedBox does with its 200 sp text. */
	char t[16];
	bool known = charge || wall || (s->live && !isnan(s->speed));
	if (charge) snprintf(t, sizeof t, "%d", (int)lroundf(s->charge_kw));
	else if (wall) snprintf(t, sizeof t, s->wall_kw < 9.95f ? "%.1f" : "%.0f", s->wall_kw);
	else if (known) snprintf(t, sizeof t, "%d", (int)lroundf(s->speed));
	else snprintf(t, sizeof t, "—");
	bool sp = speedo();
	if (G.phone) { /* a face swiped to: all of the ring drawn again (its parts move on their own) */
		static int face = -1;
		if (face != sp || (sp && CHANGED(kph))) face = sp, lv_obj_invalidate(ring); /* km/h or mph: the other scale */
	}
	float bw = G.D * 0.5f, bh = G.D * 0.4f;
	if (sp) bw = G.r * 0.42f, bh = G.r * 0.24f; /* the speedometer's: small, inside its arc on the left */
	static float adv = 0;
	if (!adv) adv = lv_font_get_glyph_width(font_px(LIGHT, 200), '0', 0) / 200.0f;
	float em_w = known ? strlen(t) * adv : 1.0f; /* the em dash is an em wide */
	float fs = fminf(bh, bw / em_w);
	const lv_font_t *f = font_px(LIGHT, ri(fs));
	figure_font(speed_l, f);
	set_text(speed_l, "%s", t);
	text_color(speed_l, charge ? REGEN : known ? WHITE : W24);
	/* Column centred: the box, then the unit's line. At height 1 the text's box is its size, the baseline at
	 * ascent / (ascent + descent) of it. The speedometer's column 0.62 of the radius left of the dial's centre, its top
	 * 0.04 under it, the unit 11 sp spaced 2. */
	if (G.phone) {
		const lv_font_t *uf = FS(REGULAR, sp ? 11 : 15, UI_S_RING);
		if (lv_obj_get_style_text_font(unit_l, 0) != uf) lv_obj_set_style_text_font(unit_l, uf, 0);
		SET_NUM(unit_l, lv_obj_get_style_text_letter_space, lv_obj_set_style_text_letter_space, ri(PR(sp ? 2 : 3)));
	}
	const lv_font_t *fu = lv_obj_get_style_text_font(unit_l, 0);
	float col_h = bh + lv_font_get_line_height(fu), top = (G.D - col_h) / 2, xc = G.D / 2;
	if (sp) top = G.D / 2 + G.r * 0.11f, xc = G.D / 2 - G.r * 0.41f;
	float a = ascent(f), d = f->base_line, base = top + (bh - fs) / 2 + fs * a / (a + d), dy = 0;
	lv_font_glyph_dsc_t g0;
	if (strip() && lv_font_get_glyph_dsc(f, &g0, '0', 0)) /* the strip: no gear in the ring's foot, */
		dy = G.D / 2 + g0.ofs_y + g0.box_h / 2.0f - base;                    /* so the figures sit in the middle */
	base += dy;
	ring_figs_geom(dy); /* size the limit sign and the motor readouts to the current ring (full or the strip) */
	put_text(speed_l, 1, xc, base, G.D);
	set_text(unit_l, "%s", charge || wall ? "kW" : s->kph ? "km/h" : "mph");
	float unit_base = top + bh + ascent(fu) + dy;
	put_text(unit_l, 1, xc + PR(sp ? 2 : 3) / 2, unit_base, G.D);

	/* The gear, in the gap at the ring's foot; unknown, a dim dash (widgets.dart: Panda's gear starts as '—'). */
	if (s->gear) set_text(gear_l, "%c", s->gear);
	else set_text(gear_l, "—");
	text_color(gear_l, s->live && s->gear && strchr("PRND", s->gear) ? WHITE : W38);
	const lv_font_t *fg = lv_obj_get_style_text_font(gear_l, 0);
	put_text(gear_l, 1, G.D / 2, G.D - G.D * (sp ? 0.11f : 0.05f) - lv_font_get_line_height(fg) + ascent(fg), G.D);
	show(gear_l, !strip()); /* the strip shows the gear in its top row, not in the ring's foot */

	/* The limit sign. */
	bool lim = s->live && !isnan(s->speed_limit);
	fill(limit_sign, lim ? WHITE : W10);
	static int lim_was = -1;
	if (lim != lim_was) {
		lim_was = lim;
		lv_obj_set_style_border_color(limit_sign, rgb(lim ? ALERT : W24), 0);
		lv_obj_set_style_border_opa(limit_sign, opa(lim ? ALERT : W24), 0);
	}
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
	bool one = plug_ring(s); /* plugged in: one ring, no motor readouts, no limit sign */
	show(rear_v, !sp && !one), show(front_v, !sp && !one), show(rear_u, !sp && !one), show(front_u, !sp && !one); /* the power ring's alone */
	show(limit_sign, !one);

	/* The cue under the speed: charging, the cruise, or the washer fluid low. */
	part_t p[8];
	int n = 0;
	float ck = strip() ? 0.8f : 1; /* the split's smaller ring: a smaller cue */
	const lv_font_t *fs15 = FS(BOLD, 15 * ck, UI_S_RING);
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
	} else if (wall) {   /* where it comes from, and what the plug gives */
		if (isnan(s->wall_max_kw)) snprintf(a1, sizeof a1, "FROM THE WALL");
		else snprintf(a1, sizeof a1, "FROM THE WALL · OF %d", (int)lroundf(s->wall_max_kw));
		p[n] = TXT(fs15, W70, a1), p[n++].ls = PR(1);
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
		p[n] = TXT(FS(BOLD, 13 * ck, UI_S_RING), WARN, "WASHER LOW"), p[n].ls = PR(1), p[n++].gap = PR(4);
	}
	show(cue, n > 0);
	if (n) {
		run_set(cue, p, n);
		float cy = G.D - G.D * 0.17f - G.D * 0.075f / 2, ch = lv_obj_get_style_height(cue, 0);
		if (sp) cy = G.D - G.D * 0.03f + px(4) - G.D * 0.075f / 2; /* the speedometer's: under its raised gear */
		else cy = fmaxf(cy, unit_base + lv_font_get_line_height(fu) - ascent(fu) + PR(6) + ch / 2.0f); /* under the unit */
		align(cue, LV_ALIGN_TOP_MID, 0, ri(cy - ch / 2.0f));
		/* inside the arc's opening at that height: a long line on the split's smaller ring is drawn smaller (the
		 * speedometer's foot is open: half the ring's width) */
		float ri_ = G.r - G.aw, dy = cy - G.D / 2, room = sp ? G.D * 0.5f : 2 * (sqrtf(fmaxf(0, ri_ * ri_ - dy * dy)) - PR(4));
		float cw = lv_obj_get_style_width(cue, 0);
		int32_t sc = cw > room + px(4) && room > 0 ? (int32_t)(256 * room / cw) : 256; /* rare: a layer each redraw */
		/* (about its middle, set once at its making; where it stood at the scale it had: LVGL redraws only where the
		 * new one puts it, and a seam of the old line stayed) */
		if (sc != lv_obj_get_style_transform_scale_x(cue, 0)) lv_obj_invalidate(cue);
		SET_NUM(cue, lv_obj_get_style_transform_scale_x, lv_obj_set_style_transform_scale_x, sc);
		SET_NUM(cue, lv_obj_get_style_transform_scale_y, lv_obj_set_style_transform_scale_y, sc);
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
	show(outside_l, true); /* the split hides it while unknown; the dash always shows it (a dash when unknown) */
	align(outside_l, LV_ALIGN_TOP_RIGHT, ri(right - G.W), ri(y - lv_font_get_line_height(f) / 2.0f));
}

/* The box round the ring's band (every arc, the zero ticks) from a0 to a1 degrees, and a few pixels for the edges. */
static void band_box(int a0, int a1, lv_area_t *box) { arc_box(G.cx, G.cy, G.r - G.aw, G.r + G.aw, a0, a1, box); }
/* Redraws only the ring's parts whose values changed: the degrees an arc moved over (its end, as a figure moves; all of
 * it before and after when it changed colour or came or went), a sector's box when the ring itself changed, its foot;
 * on the speedometer the needle. The two sectors' boxes, every frame a motor's power moved, were nearly half of a
 * driving frame's pixels. */
static void ring_dirty(const dash_state_t *s)
{
	if (speedo()) {
		needle_dirty(s);
		return;
	}
	static struct { float cx, cy, r, aw; ring_arc_t a[2][2]; } shown; /* as drawn (this frame's, once it is) */
	typeof(shown) now = {G.cx, G.cy, G.r, G.aw};
	ring_arcs(s, now.a);
	float R = G.r + G.aw, in = 0.5f * (G.r - G.aw), h = 0.83f * R;
	bool both = first || CHANGED(live) || CHANGED(rwd) || memcmp(&now, &shown, 4 * sizeof(float));
	static bool was_plug;   /* two halves to one ring or back: the tracks, the ticks, all of it */
	if (plug_ring(s) != was_plug) was_plug = !was_plug, lv_obj_invalidate(ring);
	else if (both && was_plug) lv_obj_invalidate(ring); /* the side boxes miss the one ring's top */
	lv_area_t a;
	for (int side = 0; side < 2; side++) {
		if (both) {
			a = side ? (lv_area_t){ri(G.cx + in) - 2, ri(G.cy - h) - 2, ri(G.cx + R) + 2, ri(G.cy + h) + 2}
				 : (lv_area_t){ri(G.cx - R) - 2, ri(G.cy - h) - 2, ri(G.cx - in) + 2, ri(G.cy + h) + 2};
			lv_obj_invalidate_area(ring, &a);
			continue;
		}
		for (int k = 0; k < 2; k++) {
			ring_arc_t o = shown.a[side][k], n = now.a[side][k];
			if (!memcmp(&o, &n, sizeof o)) continue;
			if (o.c && o.c == n.c && o.rad == n.rad && o.w == n.w && (o.a0 == n.a0 || o.a1 == n.a1)) { /* one end moved */
				int e0 = o.a0 == n.a0 ? o.a1 : o.a0, e1 = o.a0 == n.a0 ? n.a1 : n.a0;
				band_box(LV_MIN(e0, e1), LV_MAX(e0, e1), &a);
				lv_obj_invalidate_area(ring, &a);
				continue;
			}
			if (o.c) band_box(o.a0, o.a1, &a), lv_obj_invalidate_area(ring, &a);
			if (n.c) band_box(n.a0, n.a1, &a), lv_obj_invalidate_area(ring, &a);
		}
	}
	shown = now;
	if (G.phone && (both || CHANGED(brake_pct) || CHANGED(wifi_live))) { /* the foot's brake and its words */
		a = (lv_area_t){ri(G.cx - 0.45f * R) - 2, ri(G.cy + 0.85f * G.r), ri(G.cx + 0.45f * R) + 2, ri(G.cy + G.r + 2.5f * G.aw) + 2};
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
	char tt[32], to[32];
	if (charging) p[n++] = IMG(&ic_bolt_dial, REGEN);
	if (!isnan(t)) {
		/* What the pack is being heated to, for a charger on the route: the app's amber "→ 30°" after its temperature
		 * (Roboto has no arrow: the heating's own, in amber, points at it). The app scales that wider row down, icons
		 * and all, into the dial's opening (66 dp); here the snowflake and the bolt give way (the ice blue and the charge's lines say them) and the figures
		 * are set in the biggest of a few sizes that fits: no layer at each redraw, and no font for every width */
		bool target = !isnan(s->precond_c);
		float d = shown_deg(t), a = fabsf(d), fs = 14 * q;
		if (a < 10) snprintf(tt, sizeof tt, "%.1f°", a < 0.05f ? 0.0f : d);
		else snprintf(tt, sizeof tt, "%d°", (int)lroundf(d));
		if (target) {
			char both[64];
			snprintf(to, sizeof to, "%d°", (int)lroundf(shown_deg(s->precond_c)));
			snprintf(both, sizeof both, "%s%s", tt, to);
			float room = 60 * q - ic_up_dial.header.w - 2 * q; /* (clear of the arc) */
			for (int k = 0;; k++, fs -= 2 * q) { /* 14, 12, 10 */
				lv_point_t sz;
				lv_text_get_size(&sz, both, font_px(REGULAR, ri(fs)), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
				if (sz.x <= room || k == 2) break;
			}
			n = 0; /* (nor the bolt: the charge's own lines say it, and the row has no room for both) */
		} else if (cold) p[n] = IMG(&ic_snow_dial, COLD), p[n].gap = n ? q : 0, n++;
		p[n] = TXT(font_px(REGULAR, ri(fs)), cold ? COLD : W70, tt), p[n].gap = n ? q : 0, n++;
		if (heating) p[n] = IMG(&ic_up_dial, target ? WARN : ALERT), p[n++].gap = q;
		if (target) p[n] = TXT(font_px(REGULAR, ri(fs)), WARN, to), p[n++].gap = q;
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
	/* the up arrow (A8) turned over, made once: the font has no arrows */
	static lv_image_dsc_t ic_down;
	static uint8_t *down_px;
	if (!down_px && (down_px = malloc(ic_up_dial.data_size))) {
		uint32_t st = ic_up_dial.header.stride, h = ic_up_dial.header.h;
		for (uint32_t r = 0; r < h; r++) memcpy(down_px + r * st, ic_up_dial.data + (h - 1 - r) * st, st);
		ic_down = ic_up_dial, ic_down.data = down_px;
	}
	part_t pp[3] = {TXT(font_px(REGULAR, ri(28 * q)), known ? color : W38, pc), TXT(font_px(REGULAR, ri(16 * q)), color, "%"),
			IMG(&ic_down, ALERT)};
	/* plugged in and the battery still going down (the charger short of the climate): its arrow */
	bool down = known && s->wifi_live && s->plugged && !s->charging && !isnan(s->pack_kw) && s->pack_kw >= 0.3f;
	if (down) pp[2].gap = q * 2;
	run_set(dial_pct, pp, known ? (down && down_px ? 3 : 2) : 1);
	align(dial_pct, LV_ALIGN_TOP_MID, 0, ri(34 * q + (36 * q - lv_obj_get_style_height(dial_pct, 0)) / 2));
	show(dial_range, !isnan(s->range));
	if (!isnan(s->range)) set_text(dial_range, "%d %s", (int)lroundf(s->range), s->kph ? "km" : "mi");
	align(dial_range, LV_ALIGN_BOTTOM_MID, 0, ri(-(18 * q - lv_font_get_line_height(lv_obj_get_style_text_font(dial_range, 0))) / 2));

	/* The charge card. */
	/* plugged in (AC at the port), charging or not: where the power goes, the wall, the climate, the battery */
	bool plug = s->wifi_live && s->plugged && !isnan(s->wall_kw);
	bool card = (charging && (s->charge_limit_pct >= 0 || s->charge_minutes >= 0)) || plug;
	float ch_h = 0;
	show(charge_box, card);
	if (card) {
		const lv_font_t *fh = lv_obj_get_style_text_font(charge_head, 0), *fb = lv_obj_get_style_text_font(charge_to, 0);
		float y = lv_font_get_line_height(fh) + PF(2), w = 0;
		/* plugged in, not charging: the limit it holds at in the header (no sentence under the table) */
		if (charging) set_text(charge_head, "CHARGE");
		else if (s->charge_limit_pct >= 0) set_text(charge_head, "PLUGGED IN · LIMIT %d %%", s->charge_limit_pct);
		else set_text(charge_head, "PLUGGED IN");
		show(charge_to, charging && s->charge_limit_pct >= 0);
		if (charging && s->charge_limit_pct >= 0) {
			set_text(charge_to, "to %d %%", s->charge_limit_pct);
			lv_obj_set_pos(charge_to, 0, ri(y));
			y += lv_font_get_line_height(fb);
		}
		show(charge_when, charging && s->charge_minutes >= 0);
		if (charging && s->charge_minutes >= 0) {
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
			if (lv_obj_is_hidden(l)) continue;
			lv_text_get_size(&sz, lv_label_get_text(l), lv_obj_get_style_text_font(l, 0),
					 lv_obj_get_style_text_letter_space(l, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			w = fmaxf(w, sz.x);
		}
		/* the flows, in the figures' columns (labels at 0, values at 104 dp) and their fonts: the card and the figures
		 * read as one table; the battery red while it gives (it covers what the wall does not), green while it takes */
		for (int i = 0; i < 3; i++) show(flow_n[i], plug), show(flow_v[i], plug);
		if (plug) {
			static const char *const nm[3] = {"WALL", "CLIMATE", "BATTERY"};
			float pk = s->pack_kw, v[3] = {s->wall_kw, s->climate_kw, pk};
			const lv_font_t *fl = lv_obj_get_style_text_font(flow_n[0], 0), *fv = lv_obj_get_style_text_font(flow_v[0], 0);
			float row_h = fmaxf(lv_font_get_line_height(fl), lv_font_get_line_height(fv)), vw = 0;
			for (int i = 0; i < 3; i++) {
				set_text(flow_n[i], "%s", nm[i]);
				if (isnan(v[i])) set_text(flow_v[i], "—");
				else if (i == 2 && fabsf(pk) >= 0.05f) set_text(flow_v[i], "%s%.1f kW", pk > 0 ? "−" : "+", fabsf(pk));
				else set_text(flow_v[i], "%.1f kW", fabsf(v[i]));
				text_color(flow_v[i], i == 2 && !isnan(pk) && pk >= 0.05f ? ALERT : i == 2 && !isnan(pk) && pk <= -0.05f ? REGEN : WHITE);
				lv_text_get_size(&sz, lv_label_get_text(flow_v[i]), fv, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
				vw = fmaxf(vw, sz.x);
				float base = y + PF(4) + i * row_h + row_h / 2 + (ascent(fv) - fv->base_line) / 2.0f;
				put_text(flow_n[i], 0, 0, base, 0);
				put_text(flow_v[i], 0, PF(104), base, 0);
			}
			y += PF(4) + 3 * row_h;
			w = fmaxf(w, PF(104) + vw);
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
		snprintf(val[rows], 24, !s->wifi_live || isnan(v) ? "—" : "%.1f kW", v); \
		rows++; \
	} while (0)
	/* the card (basic, the split; main.dart _basicRow) has them all whatever the switch says, the draws as dashes
	 * without WiFi, the charge's lines in their place while charging */
	bool in_card = G.phone && layout_key != 0, minor = s->minor_figures || in_card;
	/* (not under the plugged-in card: its table has the climate, and the wall and the battery it is weighed against) */
	if ((in_card ? !charging : s->wifi_live && s->draws) && !plug) {
		KW_ROW("CLIMATE", s->climate_kw);
		if (minor) {
			KW_ROW("12 V", s->lv_kw);
			KW_ROW("OTHER", s->other_kw);
		}
	}
	if (minor) {
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
	if (plug && card) { /* the plugged-in card and the figures one width, so one left edge: one table */
		st_w = fmaxf(st_w, lv_obj_get_style_width(charge_box, 0));
		lv_obj_set_width(charge_box, ri(st_w));
	}
	lv_obj_set_size(stats_box, ri(st_w), ri(st_h));

	/* The group in the column's middle, each piece 5 dp above and below (widgets.dart _SideColumn). */
	/* The dial level with the g-meter's circle and as big (ui/assets/sizes.json), its figures under it, centred. */
	/* The pair's top: the taller of the two blocks centred in the height. */
	float dh = 98 * q, pad = PF(5);
	if (G.phone) {
		/* the app's column (main.dart): the dial 14 dp down, the g-meter's place at the foot (kept while it has nothing
		 * to show, so the figures hold still), the figures centred between; Bluetooth alone, the figures 6 dp under
		 * the dial; charging (no g-meter), the dial, the charge card and the figures together in the middle
		 * (_SideColumn: 5 dp above and below each) */
		float top = G.mt + px(14), y;
		if (view == DASH_VIEW_PROJECT || ST.basic) { /* a card (main.dart _basicRow): the dial, 12 dp, the charge card,
							  * the figures, together in the middle */
			top = (G.H - (dh + px(12) + (card ? ch_h + px(12) : 0) + st_h)) / 2;
			y = top + dh + px(12);
			if (card) {
				lv_obj_set_pos(charge_box, ri(cx - lv_obj_get_style_width(charge_box, 0) / 2.0f), ri(y));
				y += ch_h + px(12);
			}
		} else if (charging || card) {   /* (plugged in, not charging: its card the same way) */
			top = G.mt + (G.row_b - G.mt - (dh + (card ? ch_h + 2 * pad : 0) + st_h + 4 * pad)) / 2 + pad;
			y = top + dh + 2 * pad;
			if (card) {
				lv_obj_set_pos(charge_box, ri(cx - lv_obj_get_style_width(charge_box, 0) / 2.0f), ri(y));
				y += ch_h + 2 * pad;
			}
		} else if (!s->wifi_live && s->live) {
			y = top + dh + px(6);
		} else {
			y = (top + dh + G.row_b - px(4) - PG(72)) / 2 - st_h / 2;
		}
		lv_obj_set_pos(dial, ri(cx - 52 * q), ri(top));
		lv_obj_set_pos(stats_box, ri(cx - st_w / 2), ri(y));
		G.energy_cx = cx, G.energy_foot = y + st_h;
		dial_dirty(s);
		return;
	}
	float eh = dh + pad + (card ? ch_h + 2 * pad : 0) + st_h;
	bool g_on = (s->wifi_live && !s->charging && !s->plugged) || bt_brakes(s); /* the g-meter's place, in P too (the last drive's peak); not charging (the app's charge column) */
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
	G.energy_cx = cx, G.energy_foot = y + st_h;
	dial_dirty(s);
}

static bool g_figures(const dash_state_t *s, bool card);
static void set_g(const dash_state_t *s, float cx)
{
	/* The pedal (main.dart's line along the top) as a labelled bar at the g-meter block's foot, the circle's width:
	 * not on a rear-motor car, whose ring has the pedal and the brake; not in P. With Bluetooth alone (no g-meter)
	 * it sits under the battery block instead, unless the brakes' temperatures take the g-meter's place. Braking, it is the app's foot brake instead (gauge.dart: kAlert, BRAKE APPLIED, its reach the pressure).
	 * Only the fill and the figure change, so only they redraw. */
	float side = PG(72), bar_y = G.pair_top + g_height() - PEDAL_H;
	bool bar = s->live && !s->rwd && s->gear != 'P', braking = bar && s->brake_pct > 0;
	bool bb = bt_brakes(s);
	show(brake_temps, bb);
	if (bb) {
		place(brake_temps, cx - brakes_w() / 2, G.pair_top, brakes_w(), brakes_h());
		if (CHANGED(brake_temp_c) || CHANGED(fahrenheit)) lv_obj_invalidate(brake_temps);
	}
	if (bar && !s->wifi_live && !bb) /* Bluetooth alone: no g-meter, so under the battery block's figures */
		cx = G.energy_cx, bar_y = G.energy_foot + PEDAL_GAP + lv_font_get_line_height(label_font()) + PG(3);
	float lab_y = bar_y - PG(3) - lv_font_get_line_height(label_font());
	bool tyres = braking && s->wifi_live; /* the per-wheel torque comes with WiFi's stream: the four tyres in the row's place */
	show(pedal, bar && !tyres);
	show(brake_car, tyres);
	show(pedal_name, bar && !tyres);
	show(pedal_val, bar && !tyres);
	if (tyres) {
		/* its foot just above the brake glow at the screen's foot (red on red would not read), its top under the g figures */
		float ty = fminf(lab_y - PG(2), G.H - px(45) - px(3) - TYRES_H);
		place(brake_car, cx - side / 2, ty, side, TYRES_H);
		if (CHANGED(brake_nm) || CHANGED(steer_deg) || CHANGED(car)) lv_obj_invalidate(brake_car);
	}
	if (bar && !tyres) {
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
	if (!g_figures(s, false)) return;
	/* the ball, then its figures centred under it, the block centred in the column's height */
	float h1 = lv_obj_get_style_height(g_val, 0), top = G.pair_top;
	place(gball, cx - side / 2, top, side, side);
	lv_obj_set_pos(g_val, ri(cx - lv_obj_get_style_width(g_val, 0) / 2.0f), ri(top + side + PG(6)));
	align(g_peak, LV_ALIGN_TOP_MID, ri(cx - G.W / 2), ri(top + side + PG(6) + h1));
	gball_dirty(s); /* a point was pushed: the trail's fade moves on, until all of it holds the same g */
}

/* The g-meter's figures (the reading five times a second, the peak) and the ball's trail; false while it is not
 * shown. The phone layout's are the app's full-layout sizes (widgets.dart _GBall: 20 / 14 sp). Its drive [card] shows
 * it with WiFi alone (main.dart _basicRow: charging too). */
static bool g_figures(const dash_state_t *s, bool card)
{
	bool on = card ? s->wifi_live : s->wifi_live && !s->charging && !s->plugged && s->g_shown; /* in P too: the last drive's peak; not plugged in (the card's column) */
	bool skel = card && !s->live; /* basic's drive card with no link at all: its skeleton, not an empty card (Bluetooth
				      * alone has the brakes and the pedal bar there) */
	if (skel != g_skel) g_skel = skel, lv_obj_invalidate(gball);
	show(gball, on || skel);
	show(g_val, on || skel);
	show(g_peak, on || skel);
	if (!on) trail_n = 0;
	if (skel) {
		part_t p[2] = {TXT(FS(REGULAR, G.phone ? 20 : 16, UI_S_FIG), WHITE, "\xe2\x80\x94"), /* the figures' dash */
			       TXT(FS(REGULAR, G.phone ? 14 : 12, UI_S_FIG), W70, " g")};
		run_set(g_val, p, 2);
		set_text(g_peak, "PEAK \xe2\x80\x94");
		return true;
	}
	if (!on) return false;
	float gx = isnan(s->g_lat) ? 0 : s->g_lat, gy = isnan(s->g_long) ? 0 : s->g_long;
	if (trail_n == TRAIL) memmove(trail, trail + 1, sizeof trail[0] * (TRAIL - 1)), trail_n--;
	trail[trail_n][0] = gx, trail[trail_n][1] = gy, trail_n++;
	char v[16], pk[24];
	static uint32_t g_at;
	if (lv_tick_elaps(g_at) >= 200 || !lv_obj_get_child_count(g_val)) { /* five a second: readable, not rebuilt each frame */
		g_at = lv_tick_get();
		snprintf(v, sizeof v, "%.2f", sqrtf(gx * gx + gy * gy));
		part_t p[2] = {TXT(FS(REGULAR, G.phone ? 20 : 16, UI_S_FIG), WHITE, v), /* as the dial's figures */
			       TXT(FS(REGULAR, G.phone ? 14 : 12, UI_S_FIG), W70, " g")};
		run_set(g_val, p, 2);
	}
	float peak = fmaxf(s->g_peak_brake, fmaxf(s->g_peak_accel, s->g_peak_lat));
	snprintf(pk, sizeof pk, "PEAK %.2f", peak);
	set_text(g_peak, "%s", pk);
	return true;
}

/* The pedal and, Bluetooth alone, the brakes (main.dart): the app's line along the top of the screen, from its centre
 * (not on a rear-motor car: its ring has it); with Bluetooth alone the labelled pedal bar (150 dp, not in P) and the
 * brakes' temperatures under it, centred between y0 and y1 (room: there is a place for them, else only the line).
 * The phone layout's foot of the energy column (main.dart): the g-meter (widgets.dart _GBall: the ball, 12 dp, its
 * figures beside it) at the foot; the pedal as the app's line along the top of the screen, from its centre (not on a
 * rear-motor car: its ring has it); Bluetooth alone (no g-meter), the labelled pedal bar (150 dp, not in P) and the
 * brakes' temperatures under it, centred in the room under the figures. No tyres (the Dash's).
 * [card]: the drive card's ball's side (main.dart _basicRow): Bluetooth alone the brakes over the labelled bar (as wide
 * as the ball) 22 dp under them, the pair 20 dp under the middle of y0..y1 (the brakes the dash's size, as in its full
 * layout: the app's 120 dp cross is half its card's width, the F133's card is narrower); 0: the full layout's column.
 * The line along the top: not in the split (it would run over the bar above the picture), and with the speedometer's
 * face on a rear-motor car too (the app's: that face has no pedal). */
static void pedal_brakes(const dash_state_t *s, float cx, float y0, float y1, bool room, float card)
{
	bool bt = !s->wifi_live, bb = room && bt_brakes(s);
	bool bar = s->live && (bt ? !s->rwd && room && s->gear != 'P' : (!s->rwd || speedo()) && view != DASH_VIEW_PROJECT);
	bool braking = bar && bt && s->brake_pct > 0;
	show(brake_car, false);
	static int look = -1; /* the pedal: 1 the line along the top, 0 the labelled bar */
	if (look != !bt) {
		look = !bt;
		fill(pedal, bt ? W24 : 0);
		lv_obj_set_style_radius(pedal, bt ? LV_RADIUS_CIRCLE : 0, 0);
		lv_obj_set_style_radius(pedal_fill, bt ? LV_RADIUS_CIRCLE : 0, 0);
	}
	float pct = braking ? s->brake_pct : s->pedal_pct, lh = lv_font_get_line_height(label_font());
	pct = fmaxf(0, fminf(100, isnan(pct) ? 0 : pct));
	show(pedal, bar);
	show(pedal_name, bar && bt);
	show(pedal_val, bar && bt);
	if (bar && !bt) {
		float f = G.W * pct / 100;
		place(pedal, 0, 0, G.W, px(4));
		show(pedal_fill, f >= 1);
		fill(pedal_fill, ACCENT);
		place(pedal_fill, (G.W - f) / 2, 0, f, px(4));
	}
	/* Bluetooth alone: the bar, 16 dp, the brakes, together in the middle of the room under the figures (the drive card:
	 * the brakes, 22 dp, the bar) */
	float bw = card > 0 ? card : px(150), bh = bar && bt ? lh + PG(3) + PEDAL_H : 0, kh = bb ? brakes_h() : 0;
	float rh = y1 - y0, kgap = bh && kh ? (card > 0 ? px(22) : fmaxf(px(4), fminf(px(16), rh - bh - kh))) : 0;
	float y = card > 0 ? (y0 + y1) / 2 + px(20) - (bh + kgap + kh) / 2 : y0 + fmaxf(0, (rh - bh - kgap - kh) / 2);
	float by = card > 0 ? y + kh + kgap : y; /* the bar's label's top */
	if (bar && bt) {
		place(pedal, cx - bw / 2, by + lh + PG(3), bw, PEDAL_H);
		float f = bw * pct / 100;
		show(pedal_fill, f >= 1);
		fill(pedal_fill, braking ? ALERT : ACCENT);
		place(pedal_fill, 0, 0, fmaxf(f, PEDAL_H), PEDAL_H);
		set_text(pedal_name, "%s", braking ? "BRAKE APPLIED" : "PEDAL");
		text_color(pedal_name, braking ? ALERT : W70);
		if (isnan(braking ? s->brake_pct : s->pedal_pct)) set_text(pedal_val, " —");
		else set_text(pedal_val, " %d %%", (int)lroundf(pct));
		lv_point_t a, b;
		lv_text_get_size(&a, lv_label_get_text(pedal_name), label_font(), ri(PF(0.8)), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_text_get_size(&b, lv_label_get_text(pedal_val), label_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		float x = cx - (a.x + b.x) / 2.0f;
		lv_obj_set_pos(pedal_name, ri(x), ri(by));
		lv_obj_set_pos(pedal_val, ri(x + a.x), ri(by));
		if (card <= 0) y += bh + kgap;
	}
	show(brake_temps, bb);
	if (bb) {
		place(brake_temps, cx - brakes_w() / 2, y, brakes_w(), brakes_h());
		if (CHANGED(brake_temp_c) || CHANGED(fahrenheit)) lv_obj_invalidate(brake_temps);
	}
}
static void set_g_phone(const dash_state_t *s, float cx)
{
	pedal_brakes(s, cx, G.energy_foot + px(6), G.row_b, true, 0);
	if (!g_figures(s, false)) return;
	float side = PG(72), gap = PG(12), h1 = lv_obj_get_style_height(g_val, 0), h2 = lv_font_get_line_height(lv_obj_get_style_text_font(g_peak, 0));
	lv_point_t pk;
	lv_text_get_size(&pk, lv_label_get_text(g_peak), lv_obj_get_style_text_font(g_peak, 0), lv_obj_get_style_text_letter_space(g_peak, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	float fw = fmaxf(lv_obj_get_style_width(g_val, 0), pk.x), x = cx - (side + gap + fw) / 2, top = G.row_b - px(4) - side;
	float fy = top + (side - h1 - h2) / 2;
	place(gball, x, top, side, side);
	lv_obj_set_pos(g_val, ri(x + side + gap), ri(fy));
	align(g_peak, LV_ALIGN_TOP_LEFT, ri(x + side + gap), ri(fy + h1));
	gball_dirty(s); /* a point was pushed: the trail's fade moves on, until all of it holds the same g */
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
static int edit_slot = -1;
static char pick_ssid[33];                     /* the Nearby network the editor was opened for: joined when saved */
static char shown_nearby[DASH_NEARBY][33];     /* the Nearby rows as drawn (a tap names its own, not today's list) */

static lv_obj_t *tip, *snack, *snack_wheel, *snack_text;
static lv_obj_t *project, *project_video, *pj_hint, *pj_gear, *pj_bar, *pj_bar_fill, *pj_soc, *pj_cold, *pj_range,
	*pj_belt, *pj_grip, *pj_info, *pj_exit;
static lv_obj_t *panel, *panel_msg, *settings, *set_dlg, *set_status, *set_step, *set_wheel, *set_sw[14], *set_car_btn[6], *set_slider, *set_bl_note, *set_offset, *set_offset_note, *net_list, *near_list, *net_edit, *net_ta[2],
	*net_kb, *net_err, *set_bt_line, *set_wifi_line, *set_net_line, *set_car_ask, *set_tz_btn, *set_host_l;
enum { HOST_SLOT = -2 }; /* edit_slot: the editor holds the Panda's address, not a network */
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
	lv_obj_set_press_lock(o, false); /* a finger that slides off is a lost press, not a click (a swipe from a door chip) */
	lv_obj_set_ext_click_area(o, ri(room));
	lv_obj_add_event_cb(o, cb, LV_EVENT_SHORT_CLICKED, (void *)arg);
}
static void pressed_look(lv_obj_t *o, uint32_t c) /* InkWell's highlight while held */
{
	lv_obj_set_style_bg_color(o, rgb(c), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(o, opa(c), LV_STATE_PRESSED);
}

/* Chips: a tap presses (once: the app guards and never resends); a long press names it (Tooltip). */
/* The tap after a screen closes lands on the dash beneath: Settings' X is over the Frunk chip, the car panel's Back
 * over Trunk. So for a moment after the dash (or the car panel) appears, its controls take no taps. */
static uint32_t dash_at, panel_at, edit_at;
#define FRESH(at, ms) (lv_tick_elaps(at) < (ms))
static void massage_menu(int i);
static void run_car_act(lv_obj_t *anchor, int k);
static void overlays_close(void);
static char panel_note[112], note_toast[sizeof S.toast]; /* the note, and the words up when it was set */
static lv_obj_t *column(lv_obj_t *parent, float gap);
static void chip_tap(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (FRESH(dash_at, 700)) return;
	if (slot_act[i] >= 0) run_car_act(chips[i].box, slot_act[i]);
	else if (i == CHIP_MASSAGE_L || i == CHIP_MASSAGE_R) massage_menu(i);
	else press(chip_act[i]);
}

/* A choice over its button (widgets.dart _pick, car_panel.dart _tap: Flutter's showMenu, a CheckedPopupMenuItem ticked):
 * the words on a card over [anchor], kept 8 dp inside the screen, scrolled when they are more than fit; the running one
 * ticked; a pick presses its action (the ticked one again stops it: the massage), asked first when [confirm]; a tap
 * anywhere else closes it. The massage chips' modes and the car panel's options. */
#define M3_MENU 0xFF211F26u /* surfaceContainer */
#define M3_ON_VARIANT 0xFFCAC4D0u
enum { MENU_MAX = 24 };
static lv_obj_t *mm_scrim, *mm_card;
static int mm_act[MENU_MAX];
static bool mm_confirm;
static void confirm_press(int a);
static void mm_close(lv_event_t *e)
{
	(void)e;
	show(mm_scrim, false);
}
static void mm_pick(lv_event_t *e)
{
	int a = mm_act[(intptr_t)lv_event_get_user_data(e)];
	show(mm_scrim, false);
	if (mm_confirm) confirm_press(a);
	else press(a);
}
static void menu_open(lv_obj_t *anchor, int n, const char *const *words, const int *acts, int ticked, bool confirm)
{
	float w = PC(112), row = PC(56), pad = PC(8);
	if (!mm_scrim) {
		mm_scrim = box(lv_layer_top());
		place(mm_scrim, 0, 0, G.W, G.H);
		tappable(mm_scrim, mm_close, 0, 0);
		mm_card = box(mm_scrim);
		fill(mm_card, M3_MENU);
		lv_obj_set_style_radius(mm_card, ri(PC(4)), 0);
		lv_obj_set_clickable(mm_card, true); /* a tap between the rows keeps it open */
		lv_obj_set_scroll_dir(mm_card, LV_DIR_VER);
	}
	lv_obj_clean(mm_card);
	if (n > MENU_MAX) n = MENU_MAX;
	mm_confirm = confirm;
	static lv_point_precise_t pts[3];
	pts[0] = (lv_point_precise_t){ri(PC(5)), ri(PC(12.5f))}; /* the check mark (Icons.done, 24 dp at the leading 16 dp) */
	pts[1] = (lv_point_precise_t){ri(PC(9.5f)), ri(PC(17))};
	pts[2] = (lv_point_precise_t){ri(PC(19)), ri(PC(7.5f))};
	const lv_font_t *f = FS(REGULAR, 16, UI_S_CHIP);
	for (int k = 0; k < n; k++) { /* as wide as the widest word needs, 112 dp at least */
		lv_point_t sz;
		lv_text_get_size(&sz, words[k], f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		w = fmaxf(w, PC(52) + sz.x + PC(24));
	}
	for (int k = 0; k < n; k++) {
		mm_act[k] = acts[k];
		lv_obj_t *r = box(mm_card);
		place(r, 0, pad + k * row, w, row);
		tappable(r, mm_pick, k, 0);
		pressed_look(r, 0x1FE6E0E9);
		lv_obj_t *l = label(r, f, M3_ON_SURFACE);
		lv_label_set_text(l, words[k]);
		align(l, LV_ALIGN_LEFT_MID, ri(PC(52)), 0);
		if (k != ticked) continue;
		lv_obj_t *t = lv_line_create(r);
		lv_line_set_points(t, pts, 3);
		lv_obj_set_style_line_width(t, ri(PC(2)), 0);
		lv_obj_set_style_line_color(t, rgb(M3_ON_VARIANT), 0);
		lv_obj_set_style_line_rounded(t, true, 0);
		align(t, LV_ALIGN_LEFT_MID, ri(PC(16)), 0);
	}
	float h = fminf(n * row + 2 * pad, G.H - 2 * PC(8));
	lv_obj_set_scrollable(mm_card, n * row + 2 * pad > h);
	lv_obj_set_size(mm_card, ri(w), ri(h));
	lv_obj_set_style_pad_bottom(mm_card, ri(pad), 0); /* (scrolled: room after the last row) */
	lv_area_t a;
	lv_obj_get_coords(anchor, &a);
	float x = fminf(a.x1, G.W - PC(8) - w), y = fminf(a.y1, G.H - PC(8) - h);
	if (view == DASH_VIEW_PROJECT) x = fmaxf(x, PJ_RGN_X + PJ_RGN_W + PC(8)); /* off the picture: a tap there is the phone's */
	lv_obj_set_pos(mm_card, ri(fmaxf(x, PC(8))), ri(fmaxf(y, PC(8))));
	lv_obj_scroll_to_y(mm_card, 0, LV_ANIM_OFF);
	show(mm_scrim, true);
}
/* The massage chips' modes: Soft, Pulse, Hard (3, 2, 4 as pressed), the running one ticked. */
static void massage_menu(int i)
{
	static const char *const words[3] = {"Soft", "Pulse", "Hard"};
	static const int acts[2][3] = {{ACT_MASSAGE_L_SOFT, ACT_MASSAGE_L_PULSE, ACT_MASSAGE_L_HARD},
				       {ACT_MASSAGE_R_SOFT, ACT_MASSAGE_R_PULSE, ACT_MASSAGE_R_HARD}};
	static const uint8_t mode[3] = {3, 2, 4};
	bool r = i == CHIP_MASSAGE_R;
	uint8_t m = r ? S.massage_r : S.massage_l;
	int ticked = -1;
	for (int k = 0; k < 3; k++) if (massage_mode(m) && m == mode[k]) ticked = k;
	menu_open(chips[i].box, 3, words, acts[r], ticked, false);
}

/* A dialog centred where it is no part of the picture (projecting: right of it; a tap on the picture is the phone's) */
static void off_picture(lv_obj_t *d)
{
	lv_obj_center(d);
	if (view != DASH_VIEW_PROJECT) return;
	lv_obj_update_layout(d);
	float l = PJ_RGN_X + PJ_RGN_W, w = lv_obj_get_width(d);
	lv_obj_align(d, LV_ALIGN_LEFT_MID, ri(fmaxf(l + (G.W - l - w) / 2, l)), 0);
}
/* Asked first (car_panel.dart: an AlertDialog): the press's name, "Press it on the car now?", Cancel and Press. */
static lv_obj_t *cf_scrim, *cf_title;
static int cf_act;
static void cf_answer(lv_event_t *e)
{
	show(cf_scrim, false);
	if (lv_event_get_user_data(e)) press(cf_act);
}
static lv_obj_t *text_button(lv_obj_t *parent, const char *t, lv_event_cb_t cb, intptr_t arg)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, LV_SIZE_CONTENT, ri(PB(40)));
	lv_obj_set_style_pad_hor(b, ri(PB(12)), 0);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	pressed_look(b, 0x1FD0BCFF);
	lv_obj_t *l = label(b, FS(MEDIUM, 14, UI_S_PANEL), M3_PRIMARY);
	lv_label_set_text(l, t);
	lv_obj_center(l);
	tappable(b, cb, arg, PB(4));
	return b;
}
static void confirm_press(int a)
{
	float w = PB(312);
	if (!cf_scrim) {
		cf_scrim = box(lv_layer_top());
		place(cf_scrim, 0, 0, G.W, G.H);
		fill(cf_scrim, 0x80000000);
		lv_obj_set_clickable(cf_scrim, true); /* modal: only its buttons answer */
		lv_obj_t *d = box(cf_scrim);
		lv_obj_set_size(d, ri(w), LV_SIZE_CONTENT);
		lv_obj_center(d);
		fill(d, M3_DIALOG);
		lv_obj_set_style_radius(d, ri(PB(28)), 0);
		lv_obj_set_style_pad_all(d, ri(PB(24)), 0);
		lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(d, ri(PB(16)), 0);
		cf_title = label(d, FS(REGULAR, 24, UI_S_PANEL), M3_ON_SURFACE);
		lv_obj_set_width(cf_title, LV_PCT(100));
		lv_label_set_long_mode(cf_title, LV_LABEL_LONG_WRAP);
		lv_obj_t *t = label(d, FS(REGULAR, 14, UI_S_PANEL), M3_ON_VARIANT);
		lv_label_set_text(t, "Press it on the car now?");
		lv_obj_t *r = box(d);
		lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(r, ri(PB(8)), 0);
		text_button(r, "Cancel", cf_answer, 0);
		text_button(r, "Press", cf_answer, 1);
	}
	cf_act = a;
	const cmdr_action_def *d = cmdr_action_at(a);
	lv_label_set_text(cf_title, d ? d->label : "");
	off_picture(lv_obj_get_parent(cf_title));
	show(cf_scrim, true);
}

/* A car panel action pressed (car_panel.dart runCarAction): its choice first in the menu over [anchor] (if it has one),
 * a risky one asked first. The panel's Execute and a button put in its place. */
static void run_car_act(lv_obj_t *anchor, int k)
{
	const cmdr_car_act *c = &cmdr_car_acts[k];
	if (c->n == 1) {
		if (c->confirm) confirm_press(ACT_COUNT + c->first);
		else press(ACT_COUNT + c->first);
		return;
	}
	const char *words[MENU_MAX];
	int acts[MENU_MAX];
	for (int j = 0; j < c->n && j < MENU_MAX; j++) words[j] = cmdr_car_words[c->first + j], acts[j] = ACT_COUNT + c->first + j;
	menu_open(anchor, c->n, words, acts, -1, c->confirm);
}

/* A choice in a dialog (SimpleDialog: a title, a row for each, a tap anywhere else closes it): [cb] gets the row picked.
 * The car panel's Execute / Add to home screen, and the button to put an action in place of. */
static lv_obj_t *ld_scrim, *ld_box, *ld_title, *ld_list;
static void (*ld_cb)(int);
static void ld_pick(lv_event_t *e)
{
	show(ld_scrim, false);
	if (ld_cb) ld_cb((int)(intptr_t)lv_event_get_user_data(e));
}
static void ld_close(lv_event_t *e)
{
	(void)e;
	show(ld_scrim, false);
}
static void list_dialog(const char *title, int n, const char *const *words, void (*cb)(int))
{
	if (!ld_scrim) {
		ld_scrim = box(lv_layer_top());
		place(ld_scrim, 0, 0, G.W, G.H);
		fill(ld_scrim, 0x80000000);
		tappable(ld_scrim, ld_close, 0, 0);
		ld_box = box(ld_scrim);
		lv_obj_set_width(ld_box, ri(PB(360)));
		fill(ld_box, M3_DIALOG);
		lv_obj_set_style_radius(ld_box, ri(PB(28)), 0);
		lv_obj_set_style_pad_ver(ld_box, ri(PB(24)), 0);
		lv_obj_set_clickable(ld_box, true); /* a tap on it, off its rows, keeps it open */
		lv_obj_set_flex_flow(ld_box, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(ld_box, ri(PB(12)), 0);
		ld_title = label(ld_box, FS(REGULAR, 24, UI_S_PANEL), M3_ON_SURFACE);
		lv_obj_set_style_pad_hor(ld_title, ri(PB(24)), 0);
		lv_obj_set_width(ld_title, LV_PCT(100));
		lv_label_set_long_mode(ld_title, LV_LABEL_LONG_WRAP);
		ld_list = column(ld_box, 0);
		lv_obj_set_width(ld_list, LV_PCT(100));
		lv_obj_set_scroll_dir(ld_list, LV_DIR_VER);
	}
	lv_label_set_text(ld_title, title);
	lv_obj_clean(ld_list);
	float row = PB(48);
	for (int k = 0; k < n; k++) {
		lv_obj_t *r = box(ld_list);
		lv_obj_set_size(r, LV_PCT(100), ri(row));
		tappable(r, ld_pick, k, 0);
		pressed_look(r, 0x1FE6E0E9);
		lv_obj_t *l = label(r, FS(REGULAR, 16, UI_S_PANEL), M3_ON_SURFACE);
		lv_label_set_text(l, words[k]);
		align(l, LV_ALIGN_LEFT_MID, ri(PB(24)), 0);
	}
	ld_cb = cb;
	lv_obj_update_layout(ld_title);
	float head = lv_obj_get_height(ld_title) + PB(12) + 2 * PB(24), room = G.H - 2 * PB(24) - head;
	lv_obj_set_height(ld_list, ri(fminf(n * row, room)));
	lv_obj_set_scrollable(ld_list, n * row > room);
	lv_obj_set_height(ld_box, LV_SIZE_CONTENT);
	lv_obj_center(ld_box);
	off_picture(ld_box);
	lv_obj_scroll_to_y(ld_list, 0, LV_ANIM_OFF);
	show(ld_scrim, true);
}
static void overlays_close(void)
{
	if (mm_scrim) show(mm_scrim, false);
	if (cf_scrim) show(cf_scrim, false);
	if (ld_scrim) show(ld_scrim, false);
}
static void tip_hide(lv_timer_t *t)
{
	(void)t;
	show(tip, false);
	lv_timer_pause(tip_timer);
}
/* The tooltip: [t] beside x (right of it, or left with [left]), its middle at y; stays a moment after the finger lifts. */
static void tip_at(const char *t, float x, float y, bool left)
{
	set_text(lv_obj_get_child(tip, 0), "%s", t);
	align(lv_obj_get_child(tip, 0), LV_ALIGN_CENTER, 0, 0);
	lv_obj_update_layout(tip);
	float w = lv_obj_get_width(tip), h = lv_obj_get_height(tip);
	lv_obj_set_pos(tip, ri(left ? x - w : x), ri(y - h / 2));
	show(tip, true);
	lv_timer_pause(tip_timer);
}
static bool tip_released(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	if (c != LV_EVENT_RELEASED && c != LV_EVENT_PRESS_LOST) return false;   /* lifted, or slid off first */
	lv_timer_reset(tip_timer);
	lv_timer_resume(tip_timer);
	return true;
}
static void chip_hold(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (tip_released(e)) return;
	float x, y;
	chip_geom(i, &x, &y);
	bool left = i >= CHIP_SIDE;
	if (G.phone) { /* where the layout put it: basic's card, the split's bar (towards its middle, along the row) */
		lv_area_t a;
		lv_obj_get_coords(chips[i].box, &a);
		x = a.x1, y = a.y1;
		if (view == DASH_VIEW_PROJECT) left = x + PC(26) > (G.ml + PL(60) + lv_obj_get_x(pj_exit)) / 2;
	}
	char t[96];
	if (slot_act[i] >= 0) snprintf(t, sizeof t, "%s: %s", cmdr_car_acts[slot_act[i]].group, cmdr_car_acts[slot_act[i]].name);
	else snprintf(t, sizeof t, "%s", chip_tip[i]);
	if (!left) tip_at(t, x + PC(52) + PC(8), y + PC(21), false); /* beside the chip, towards the middle */
	else tip_at(t, x - PC(8), y + PC(21), true);
}
static void ap_hold(lv_event_t *e) /* the assist disc's words (widgets.dart _ApIcon's Tooltip) */
{
	if (tip_released(e) || !S.ap_word[0] || view == DASH_VIEW_PROJECT) return;
	lv_area_t a;
	lv_obj_get_coords(ap_disc, &a);
	tip_at(S.ap_word, a.x2 + PC(8), (a.y1 + a.y2) / 2.0f, false);
}

/* ---- the car panel (car_panel.dart): its own page, an app bar, the groups of outlined buttons, a message line ---- */
/* back on the settings from the editor or pairing: for 500 ms a clear cover over them takes every touch, so the bounce
 * of the tap that came back (Save, Close) does not land on what lies under it (the X, a switch, a button) */
static uint32_t set_back_at;
static lv_obj_t *set_guard;
static void close_tap(lv_event_t *e) { (void)e, dash_ui_show(DASH_VIEW_DASH); }
/* The car panel's buttons (cmdr_car_acts, car_panel.dart _tap): Execute (pressed as run_car_act does), or Add to home
 * screen: in place of one of the dash's twelve buttons, the one picked. Its line says what was done until the next
 * press's words. */
static int pa_act;
static lv_obj_t *pa_btn;
static const char *const own_name[CHIPS] = {"Trunk", "Mirror tilt", "Front left door", "Rear left door", "Charge port",
					    "Left Seat Massage", "Frunk", "Recirculation", "Front right door", "Rear right door",
					    "Interior lights", "Right Seat Massage"}; /* car_actions.dart kHomeButtons */
static const char *slot_name(int i) { return slot_act[i] >= 0 ? cmdr_car_acts[slot_act[i]].name : own_name[i]; }
static void panel_slot(int i)
{
	const cmdr_car_act *c = &cmdr_car_acts[pa_act];
	snprintf(panel_note, sizeof panel_note, "%s on the dash in place of %s", c->name, slot_name(i));
	snprintf(note_toast, sizeof note_toast, "%s", S.toast);
	snprintf(ST.slots[i], sizeof ST.slots[i], "%s|%s", c->group, c->name);
	slots_refresh();
	save(DASH_SETTINGS_SAVE);
}
static void panel_choice(int k)
{
	if (k == 0) {
		panel_note[0] = 0;
		run_car_act(pa_btn, pa_act);
		return;
	}
	static char rows[CHIPS][80];
	static const char *words[CHIPS];
	for (int i = 0; i < CHIPS; i++)
		snprintf(rows[i], sizeof rows[i], "%s %d: %s", i < CHIP_SIDE ? "Left" : "Right", i % CHIP_SIDE + 1, slot_name(i)),
			words[i] = rows[i];
	list_dialog("In place of", CHIPS, words, panel_slot);
}
static void panel_act_tap(lv_event_t *e)
{
	if (FRESH(panel_at, 500)) return;
	pa_act = (int)(intptr_t)lv_event_get_user_data(e);
	pa_btn = lv_event_get_current_target(e);
	static char title[96];
	snprintf(title, sizeof title, "%s: %s", cmdr_car_acts[pa_act].group, cmdr_car_acts[pa_act].name);
	static const char *const words[2] = {"Execute", "Add to home screen"};
	list_dialog(title, 2, words, panel_choice);
}
static void restore_tap(lv_event_t *e) /* the dash's own twelve buttons back */
{
	(void)e;
	memset(ST.slots, 0, sizeof ST.slots);
	slots_refresh();
	save(DASH_SETTINGS_SAVE);
	snprintf(panel_note, sizeof panel_note, "The dash's own buttons are back");
	snprintf(note_toast, sizeof note_toast, "%s", S.toast);
}
static void open_tap(lv_event_t *e) { if (!FRESH(dash_at, 700)) dash_ui_show((intptr_t)lv_event_get_user_data(e)); }
static uint32_t aa_pending_at;
/* a start still running: just tapped (dash-aa takes its lock within moments), or its lock held (a start killed hard
 * leaves it: AA_START_MS caps that) */
static bool aa_start_running(void)
{
	uint32_t t = lv_tick_elaps(aa_pending_at);
	return t < 5000 || (t < AA_START_MS && access(aa_start_lock, F_OK) == 0); /* (5 s: one core, a stop just ended) */
}
/* the split's Exit: end the Android Auto session (the marker would pull the view straight back otherwise) */
static void pj_exit_tap(lv_event_t *e)
{
	(void)e;
	if (pj_leaving) return; /* tapped already: the session is going */
	if (aa_pending && aa_start_running() && access(dash_ui_aa_up, F_OK) == 0 && extra_up() != aa_pending_x)
		return; /* a switch to the other mode is running: its stop is underway */
	aa_pending = false, aa_stopping = true, aa_stop_at = lv_tick_get();
	save(DASH_AA_STOP);
	if (dash_ui_whole_frame) { /* the dash once the session has gone (dash_ui_set), whole: its last picture until then */
		pj_leaving = lv_tick_get() | 1;
		return;
	}
	pj_left = true;
	dash_ui_show(DASH_VIEW_DASH);
	dash_ui_set(&S); /* laid out at once: else one frame of the strip's ring on the dash */
}
/* the dash's Android Auto button (beside the car-panel icon): toggle the on-demand bring-up by its live state */
static void aa_tap(lv_event_t *e)
{
	(void)e;
	if (FRESH(dash_at, 700)) return;
	if (aa_pending && aa_start_running()) return; /* still starting: a second start would race it */
	if (aa_stopping && lv_tick_elaps(aa_stop_at) < AA_STOP_MS) return;   /* still stopping: a start now would find it up */
	if (access(PJ_AA_SESSION, F_OK) == 0 && !projecting) {   /* a phone on it, its map put away by AA's own Exit: */
		static uint32_t focus_at;                                 /* back (the split's Exit stops AA) */
		if (access(PJ_AA_FOCUS, F_OK) != 0) {
			close(open(PJ_AA_FOCUS, O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
			focus_at = lv_tick_get();
			return;
		}
		if (lv_tick_elaps(focus_at) < 3000) return; /* asked a moment ago: aa takes it at its next ping tick */
		unlink(PJ_AA_FOCUS); /* not taken: aa serve is gone (its session marker left behind): stop AA instead */
	}
	bool up = access(dash_ui_aa_up, F_OK) == 0 && !extra_up(); /* the second button's up: AA takes its place */
	save(up ? DASH_AA_STOP : DASH_AA_START);
	aa_pending = !up, aa_pending_x = false, aa_pending_at = lv_tick_get();
	if (up) aa_stopping = true, aa_stop_at = lv_tick_get();
}
/* the second projection button: its mode up, a stop; else its start (in AA's place if AA is up) */
static void x_tap(lv_event_t *e)
{
	(void)e;
	if (FRESH(dash_at, 700)) return;
	if (aa_pending && aa_start_running()) return;
	if (aa_stopping && lv_tick_elaps(aa_stop_at) < AA_STOP_MS) return;
	bool up = access(dash_ui_aa_up, F_OK) == 0 && extra_up();
	save(up ? DASH_AA_STOP : DASH_EXTRA_START);
	aa_pending = !up, aa_pending_x = true, aa_pending_at = lv_tick_get();
	if (up) aa_stopping = true, aa_stop_at = lv_tick_get();
}

/* ---- the phone-map split screen: the picture a rectangle up to the 2/3 line, clear of the left slant (which keeps
 * the Wi-Fi / Bluetooth status), a compact dash on the right 1/3 ---- */
static float pj_vw(void) { return PJ_RGN_X + PJ_RGN_W; } /* where the dash's side starts: the 2/3 line, or the middle */
static bool strip(void) { return view == DASH_VIEW_PROJECT && !G.phone; } /* the Dash's compact strip, not the basic ring */
void dash_ui_project_region(int *x, int *y, int *w, int *h)
{
	if (x) *x = PJ_RGN_X;
	if (y) *y = PJ_RGN_Y;
	if (w) *w = PJ_RGN_W;
	if (h) *h = PJ_RGN_H;
}
static void project_build(void)
{
	float vw = pj_vw(), sw = G.W - vw, sx = vw, pad = px(10);
	project = box(lv_screen_active()); /* a page (dash_ui_show): on the top layer the screen's black was painted under it */
	lv_obj_set_clickable(project, true); /* else a tap on the strips falls through to the hidden dash (door chips!) */
	place(project, 0, 0, G.W, G.H);
	fill(project, BLACK);
	/* the video region: black until frames arrive; a hint sits in it meanwhile */
	project_video = box(project);
	place(project_video, PJ_RGN_X, PJ_RGN_Y, PJ_RGN_W, PJ_RGN_H);
	fill(project_video, 0xFF0A0A0Au);
	/* touches on the picture: the AA receiver reads them from the touchscreen itself (all fingers); here they only
	 * have to stop at the picture, not reach anything beneath it */
	lv_obj_set_clickable(project_video, true);
	lv_obj_set_scrollable(project_video, false);
	lv_obj_set_gesture_bubble(project_video, false);
	lv_obj_set_scroll_chain(project_video, false);
	pj_hint = label(project_video, F(REGULAR, 20), W54);
	lv_label_set_text(pj_hint, "Connect the phone for maps");
	lv_obj_center(pj_hint);
	lv_obj_t *div = box(project); /* a thin divider between the map and the strip */
	place(div, sx, 0, px(1), G.H);
	fill(div, W24);

	/* The compact dash (wattline _Compact), fitted to the short strip: the gear and the battery on top, the real
	 * ring centred, the assist state and a nav / charge line below. The ring, the assist disc and the air outside are
	 * the dash's own objects, moved here while projecting (dash_ui_show). */
	pj_gear = label(project, FS(BOLD, 28, UI_S_RING), WHITE);
	/* the battery line (_BatteryLine): a thin gauge over the charge % and the range */
	pj_bar = box(project);
	lv_obj_set_style_radius(pj_bar, LV_RADIUS_CIRCLE, 0);
	fill(pj_bar, W10);
	pj_bar_fill = box(pj_bar);
	lv_obj_set_style_radius(pj_bar_fill, LV_RADIUS_CIRCLE, 0);
	pj_soc = label(project, F(MEDIUM, 20), WHITE);
	pj_cold = icon(project, &ic_snow_dial, COLD);
	pj_range = label(project, F(REGULAR, 15), W70);
	/* the top-row group, each in a fixed slot so the row does not jump: a belt alert, the air outside, the grip lamp */
	pj_belt = label(project, FS(BOLD, 15, UI_S_RING), ALERT);
	lv_label_set_text(pj_belt, "BELT");
	pj_grip = icon(project, &ic_grip, COLD);
	/* the bottom line: a one-line nav summary, or the charge going in while parked (reuses the row builder) */
	pj_info = run_new(project);

	/* The exit: a clear, finger-sized pill in the strip's bottom corner, always there to leave the phone map and
	 * return to the full dash. */
	/* the Exit pill: outlined like the dash's other buttons (W70, thin), a little smaller than a chip */
	float eh = px(36);
	pj_exit = box(project);
	lv_obj_set_height(pj_exit, ri(eh));
	lv_obj_set_width(pj_exit, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(pj_exit, LV_RADIUS_CIRCLE, 0);
	fill(pj_exit, 0x33000000u);
	border(pj_exit, W70, px(1));
	pressed_look(pj_exit, 0x33FFFFFF);
	lv_obj_set_flex_flow(pj_exit, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pj_exit, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(pj_exit, ri(px(12)), 0);
	lv_obj_set_style_pad_right(pj_exit, ri(px(14)), 0);
	lv_obj_set_style_pad_column(pj_exit, ri(px(6)), 0);
	lv_obj_t *xi = icon(pj_exit, &ic_p_close, WHITE);
	lv_image_set_scale(xi, 256 * 3 / 4); /* the panel's close glyph at 3/4: sized to the shorter pill */
	lv_obj_set_size(xi, ri(ic_p_close.header.w * 3 / 4.0f), ri(ic_p_close.header.h * 3 / 4.0f));
	lv_image_set_inner_align(xi, LV_IMAGE_ALIGN_CENTER);
	lv_obj_t *xl = label(pj_exit, FS(MEDIUM, 13, UI_S_RING), WHITE);
	lv_label_set_text(xl, "EXIT");
	tappable(pj_exit, pj_exit_tap, 0, px(12)); /* the touch target stays generous around the smaller pill */
	lv_obj_update_layout(pj_exit);
	lv_obj_set_pos(pj_exit, ri(sx + sw - pad - lv_obj_get_width(pj_exit)), ri(G.H - eh - px(6)));
	if (G.phone) { /* the bar over the picture: Exit at its right end; the compact strip's own parts unused */
		lv_obj_set_gesture_bubble(project, false); /* the swipes on the ring and the card (swipe) */
		lv_obj_add_event_cb(project, swipe, LV_EVENT_GESTURE, NULL);
		lv_obj_set_pos(pj_exit, ri(PJ_RGN_X + PJ_RGN_W - pad - lv_obj_get_width(pj_exit)), ri(PJ_RGN_Y / 2.0f - eh / 2));
		lv_obj_t *unused[] = {pj_gear, pj_bar, pj_soc, pj_cold, pj_range, pj_belt, pj_grip, pj_info};
		for (size_t i = 0; i < sizeof unused / sizeof unused[0]; i++) show(unused[i], false);
	}

	show(project, false);
}
/* centre an object (its box) on (cx, cy), resetting any alignment a full-dash frame left on a reused object */
static void pj_mid(lv_obj_t *o, float cx, float cy)
{
	lv_obj_update_layout(o);
	align(o, LV_ALIGN_TOP_LEFT, ri(cx - lv_obj_get_width(o) / 2.0f), ri(cy - lv_obj_get_height(o) / 2.0f));
}
static bool any_unbuckled(const dash_state_t *s)
{
	for (int i = 0; i < DS_SEAT_COUNT; i++)
		if (s->seat[i] == DS_SEAT_UNBUCKLED) return true;
	return false;
}
static void set_project(const dash_state_t *s)
{
	float vw = pj_vw(), sw = G.W - vw, sx = vw, pad = px(10);
	bool live = s->live;

	/* the dash's own ring, already filled by set_ring, placed where ring_geom_compact centred it */
	lv_obj_set_pos(ring, ri(G.cx - G.D / 2), ri(G.cy - G.D / 2));

	/* ---- top row: gear left, the group in the middle, the battery right ---- */
	float top_c = PJ_TOP / 2;
	set_text(pj_gear, "%s", s->gear ? (char[]){s->gear, 0} : "–");
	text_color(pj_gear, live && s->gear && strchr("PRND", s->gear) ? WHITE : W38);
	lv_obj_update_layout(pj_gear);
	align(pj_gear, LV_ALIGN_TOP_LEFT, ri(sx + pad), ri(top_c - lv_obj_get_height(pj_gear) / 2.0f));

	float gx = sx + pad + px(52);
	bool belt = s->wifi_live && any_unbuckled(s);
	show(pj_belt, belt);
	if (belt) pj_mid(pj_belt, gx + px(26), top_c);
	bool out = !isnan(s->outside_c); /* set_ring set outside_l's text and ice-blue colour; place it here */
	show(outside_l, out);
	pj_mid(outside_l, gx + px(92), top_c); /* placed hidden too: its full-dash spot is under the picture */
	show(pj_grip, s->wifi_live); /* the slot is kept on WiFi; lit only when there is a grip state */
	SET_COLOR(pj_grip, lv_obj_get_style_image_recolor, lv_obj_set_style_image_recolor, rgb(s->grip == GRIP_HELD ? WARN : COLD));
	SET_NUM(pj_grip, lv_obj_get_style_image_opa, lv_obj_set_style_image_opa, s->wifi_live && s->grip != GRIP_NONE ? LV_OPA_COVER : LV_OPA_TRANSP);
	pj_mid(pj_grip, gx + px(142), top_c);

	/* the battery line, right-aligned to the strip edge */
	const lv_font_t *sf = lv_obj_get_style_text_font(pj_soc, 0), *rf = lv_obj_get_style_text_font(pj_range, 0);
	bool bk = live && !isnan(s->soc_pct);
	char soc[8];
	snprintf(soc, sizeof soc, bk ? "%d%%" : "–", (int)lroundf(bk ? s->soc_pct : 0));
	set_text(pj_soc, "%s", soc);
	text_color(pj_soc, WHITE);
	bool cold = !isnan(s->pack_temp_c) && s->pack_temp_c <= 10;
	show(pj_cold, cold);
	bool hasr = live && !isnan(s->range);
	show(pj_range, hasr);
	if (hasr) set_text(pj_range, "%d %s", (int)lroundf(s->range), s->kph ? "km" : "mi");
	lv_point_t zs, zr = {0, 0};
	lv_text_get_size(&zs, soc, sf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	if (hasr) lv_text_get_size(&zr, lv_label_get_text(pj_range), rf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	float gap = px(5), cw = cold ? ic_snow_dial.header.w : 0;
	float row_w = zs.x + (cold ? gap + cw : 0) + (hasr ? gap + zr.x : 0), bar_w = fmaxf(row_w, px(24));
	float right = sx + sw - pad - inset(px(2)), left = right - bar_w, bar_h = px(5), gv = px(3), soc_h = lv_font_get_line_height(sf);
	float y0 = top_c - (bar_h + gv + soc_h) / 2;
	place(pj_bar, left, y0, bar_w, bar_h);
	float frac = bk ? fmaxf(0, fminf(100, s->soc_pct)) / 100 : 0, fw = frac * bar_w;
	fill(pj_bar_fill, bk && s->soc_pct < 20 ? ALERT : REGEN);
	show(pj_bar_fill, fw >= 1);
	place(pj_bar_fill, 0, 0, fmaxf(fw, bar_h), bar_h);
	float ty = y0 + bar_h + gv, x = left + zs.x;
	align(pj_soc, LV_ALIGN_TOP_LEFT, ri(left), ri(ty));
	if (cold) {
		lv_obj_set_pos(pj_cold, ri(x + gap), ri(ty + (soc_h - ic_snow_dial.header.h) / 2.0f));
		x += gap + cw;
	}
	if (hasr) align(pj_range, LV_ALIGN_TOP_LEFT, ri(x + gap), ri(ty + ascent(sf) - ascent(rf)));

	/* ---- bottom row: the assist disc (dash's own), then the nav / charge line; the exit keeps its corner ---- */
	float bot_c = G.H - pj_bot() / 2;
	bool ap_on = !lv_obj_is_hidden(ap_disc); /* set_ring showed it when there is an assist state */
	lv_obj_set_pos(ap_disc, ri(sx + pad), ri(bot_c - PR(30) / 2)); /* hidden too: never shown back in the picture */
	float info_x = sx + pad + (ap_on ? PR(30) + px(10) : 0);

	part_t p[12];
	int n = 0;
	const lv_font_t *nf = F(REGULAR, 18);
	char togo[16], clk[16], dist[24], en[16], chg[64];
	if (s->wifi_live && s->charging && s->gear == 'P') { /* _Charge oneLine: to limit %, then time */
		char to[16] = "", when[36] = "";
		if (s->charge_limit_pct >= 0) snprintf(to, sizeof to, "to %d %%", s->charge_limit_pct);
		if (s->charge_minutes >= 0) {
			int m = s->charge_minutes;
			char d[12];
			if (m < 60) snprintf(d, sizeof d, "%d min", m);
			else snprintf(d, sizeof d, "%dh %dm", m / 60, m % 60);
			snprintf(when, sizeof when, "%s   %s", d, s->charge_clock[0] ? s->charge_clock : "--:--");
		}
		snprintf(chg, sizeof chg, "%s%s%s", to, to[0] && when[0] ? "   " : "", when);
		if (chg[0]) {
			p[n] = TXT(nf, WHITE, chg);
			n++;
		}
	} else { /* _Nav oneLine: time + clock, range at arrival, battery at arrival */
		if (s->nav_minutes >= 0) {
			int m = s->nav_minutes;
			if (m < 1) snprintf(togo, sizeof togo, "<1 min");
			else if (m < 60) snprintf(togo, sizeof togo, "%d min", m);
			else snprintf(togo, sizeof togo, "%dh %dm", m / 60, m % 60);
			p[n] = IMG(&ic_hourglass, W70); n++;
			p[n] = TXT(nf, WHITE, togo); p[n].gap = px(4); n++;
			snprintf(clk, sizeof clk, "%s", s->arrive_clock[0] ? s->arrive_clock : "--:--");
			p[n] = IMG(&ic_flag, W70); p[n].gap = px(14); n++;
			p[n] = TXT(nf, WHITE, clk); p[n].gap = px(4); n++;
		}
		if (!isnan(s->arrive_range)) {
			snprintf(dist, sizeof dist, "%d %s", (int)lroundf(s->arrive_range), s->kph ? "km" : "mi");
			p[n] = IMG(&ic_ecar, W70); p[n].gap = n ? px(14) : 0; n++;
			p[n] = TXT(nf, WHITE, dist); p[n].gap = px(4); n++;
		}
		if (!isnan(s->arrive_pct) || !isnan(s->arrive_kwh)) {
			if (!isnan(s->arrive_pct)) snprintf(en, sizeof en, "%d%%", (int)lroundf(s->arrive_pct));
			else snprintf(en, sizeof en, "%.1f kWh", s->arrive_kwh);
			uint32_t ec = !isnan(s->arrive_pct) && s->arrive_pct < 10 ? ALERT : WHITE;
			p[n] = IMG(&ic_battery, W70); p[n].gap = n ? px(14) : 0; n++;
			p[n] = TXT(nf, ec, en); p[n].gap = px(4); n++;
		}
	}
	static char pj_key[640];
	char key[640] = "", *k = key;
	k += snprintf(k, sizeof key, "%d;", ri(info_x)); /* the disc coming or going moves it and changes its room */
	for (int i = 0; i < n && k < key + sizeof key - 48; i++)
		k += snprintf(k, key + sizeof key - k, "%s|%x|%p;", p[i].text ? p[i].text : "", (unsigned)p[i].color, (const void *)p[i].img);
	if (n && strcmp(key, pj_key)) {
		snprintf(pj_key, sizeof pj_key, "%s", key);
		run_set(pj_info, p, n);
		/* keep it clear of the exit pill: drop trailing stats (battery, then range) until the line fits */
		float avail = lv_obj_get_x(pj_exit) - px(10) - info_x;
		while (n > 2 && lv_obj_get_style_width(pj_info, 0) > avail) {
			n -= 2;
			run_set(pj_info, p, n);
		}
		align(pj_info, LV_ALIGN_TOP_LEFT, ri(info_x), ri(bot_c - lv_obj_get_style_height(pj_info, 0) / 2.0f));
	}
	/* shown once filled and placed, and laid out: LVGL applies a new place at its next layout pass, so shown before it the
	 * line would be invalidated at its old spot (at first LVGL's default 130 px box at 0,0: over the picture) */
	if (n > 0 && lv_obj_is_hidden(pj_info)) lv_obj_update_layout(pj_info);
	show(pj_info, n > 0);
}

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
	panel = box(lv_screen_active()); /* a page: on the screen, over all of it while shown (pages_top) */
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
	align(text_button(bar, "Restore default buttons", restore_tap, 0), LV_ALIGN_RIGHT_MID, -ri(left), 0);
	/* what the last press did, above the list it came from; the list scrolled under it */
	float top = PB(64) * 0.8f + PB(8), line = lv_font_get_line_height(FS(REGULAR, 14, UI_S_PANEL));
	panel_msg = label(panel, FS(REGULAR, 14, UI_S_PANEL), WHITE);
	lv_obj_set_pos(panel_msg, ri(left), ri(top));
	lv_obj_set_width(panel_msg, ri(G.W - 2 * left));
	lv_label_set_long_mode(panel_msg, LV_LABEL_LONG_DOT);
	lv_obj_t *body = column(panel, PB(10));
	place(body, left, top + line + PB(6), G.W - 2 * left, G.H - top - line - PB(6));
	lv_obj_set_clickable(body, true); /* a drag on its background, a title or a gap scrolls it too, not only one on a button */
	lv_obj_set_scrollable(body, true);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_pad_bottom(body, ri(PB(16)), 0);
	lv_obj_t *r = NULL;
	for (int i = 0; i < cmdr_car_act_count; i++) { /* every S3XY function, in its groups (car_panel.dart) */
		const cmdr_car_act *c = &cmdr_car_acts[i];
		if (!i || strcmp(c->group, cmdr_car_acts[i - 1].group)) {
			if (r) lv_obj_set_style_margin_bottom(r, ri(PB(6)), 0);
			char t[48];
			snprintf(t, sizeof t, "%s", c->group);
			for (char *q = t; *q; q++) *q = (char)toupper((unsigned char)*q);
			group_title(body, t);
			r = row(body);
		}
		lv_obj_t *b = outlined(r, NULL, c->name, panel_act_tap, i);
		if (c->n > 1) { /* a choice to make: the menu's arrow after the name (arrow_drop_down, drawn: no font has it) */
			static lv_point_precise_t v[3];
			v[0] = (lv_point_precise_t){0, 0}, v[1] = (lv_point_precise_t){ri(PB(4)), ri(PB(4))};
			v[2] = (lv_point_precise_t){ri(PB(8)), 0};
			lv_obj_t *l = lv_line_create(b);
			lv_line_set_points(l, v, 3);
			lv_obj_set_style_line_width(l, ri(PB(2)), 0);
			lv_obj_set_style_line_color(l, rgb(WHITE), 0);
			lv_obj_set_style_line_rounded(l, true, 0);
		}
	}
	show(panel, false);
}

/* ---- settings: the Commander, the display, the networks (an AlertDialog of the app's style) ---- */
static bool approve_asked, approve_moved; /* tapped, not yet approved; and the state has moved since the tap */
static uint32_t approve_at;
static void car_tap(lv_event_t *e)
{
	ST.car = (int)(intptr_t)lv_event_get_user_data(e);
	show(set_car_ask, false); /* answered */
	save(DASH_SETTINGS_SAVE);
	dash_ui_show(DASH_VIEW_SETTINGS); /* the choice shown */
}
static void approve_tap(lv_event_t *e) { (void)e, approve_asked = true, approve_moved = false, approve_at = lv_tick_get(), save(DASH_CMDR_APPROVE); }
static void pair_tap(lv_event_t *e)
{
	(void)e;
	save(DASH_PAIR_START);
	dash_ui_show(DASH_VIEW_PAIR);
}
/* A forget button: the first tap asks "Tap again to forget" (red), a second within 3 s does it: a stray tap must not. */
typedef struct {
	lv_obj_t *btn;
	const char *word;
	uint32_t armed;            /* the first tap's time, 0 none */
	const char *again, *done; /* the second tap's asking and its answer (NULL: forgetting's) */
} forget_t;
static forget_t fg_cmdr = {NULL, "Forget Commander", 0, NULL, NULL}, fg_aa = {NULL, "Forget phones", 0, NULL, NULL},
		fg_restart = {NULL, "Restart", 0, "Tap again to restart", "Restarting"};
static void forget_label(forget_t *f, const char *t, uint32_t c)
{
	lv_obj_t *l = lv_obj_get_child(f->btn, -1);
	set_text(l, "%s", t);
	text_color(l, c);
}
static bool forget_second(forget_t *f)
{
	if (!f->armed || lv_tick_elaps(f->armed) > 3000) {
		f->armed = lv_tick_get();
		forget_label(f, f->again ? f->again : "Tap again to forget", ALERT);
		return false;
	}
	f->armed = 0;
	forget_label(f, f->done ? f->done : "Forgotten", WHITE);
	return true;
}
static void forget_reset(forget_t *f, bool all) /* all: the settings opened; else only a second-tap window run out */
{
	if (!f->btn || (!all && !(f->armed && lv_tick_elaps(f->armed) > 3000))) return;
	f->armed = 0;
	forget_label(f, f->word, WHITE);
}
static void forget_tap(lv_event_t *e)
{
	(void)e;
	if (forget_second(&fg_cmdr)) save(DASH_CMDR_FORGET);
}
/* what a restart does, the device's own words (its profile: the F133's power is cut and restored by its MCU) */
#ifndef DASH_RESTART_NOTE
#define DASH_RESTART_NOTE "It starts again in under a minute; settings and pairings stay"
#endif
static void restart_tap(lv_event_t *e)
{
	(void)e;
	if (forget_second(&fg_restart)) save(DASH_RESTART);
}
/* The time zones a tap steps through (POSIX TZ: no tzdata on the card); another, from dash.conf, shows as it is. */
static const char *const tz_name[] = {"UTC", "UK / Ireland", "Central Europe", "Eastern Europe", "US Eastern", "US Central",
				      "US Mountain", "US Pacific", "Australia East", "Japan", "China", "India"};
static const char *const tz_posix[] = {"UTC0", "GMT0BST,M3.5.0/1,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/3",
				       "EET-2EEST,M3.5.0/3,M10.5.0/4", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0",
				       "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "AEST-10AEDT,M10.1.0,M4.1.0/3",
				       "JST-9", "CST-8", "IST-5:30"};
#define TZ_N ((int)(sizeof tz_posix / sizeof *tz_posix))
static char tz_own[64]; /* a zone from dash.conf outside the presets: kept in the cycle, after the last preset */
static int tz_preset(const char *tz)
{
	for (int i = 0; i < TZ_N; i++)
		if (!strcmp(tz, tz_posix[i])) return i;
	return -1;
}
static int tz_index(void)
{
	if (!tz_own[0]) /* dash.conf's zone (else the one shown) when no preset: it stays reachable after a reboot */
		snprintf(tz_own, sizeof tz_own, "%s", ST.tz_conf[0] && tz_preset(ST.tz_conf) < 0 ? ST.tz_conf
						     : ST.tz[0] && tz_preset(ST.tz) < 0 ? ST.tz : "");
	return tz_preset(ST.tz);
}
static void tz_look(void)
{
	int i = tz_index();
	set_text(lv_obj_get_child(set_tz_btn, 0), "%s", i >= 0 ? tz_name[i] : ST.tz[0] ? ST.tz : "UTC");
}
static void tz_tap(lv_event_t *e)
{
	(void)e;
	int i = tz_index(); /* own -> the first preset; the last preset -> own (if any) -> ... */
	const char *next = i < 0 ? tz_posix[0] : i + 1 < TZ_N ? tz_posix[i + 1] : tz_own[0] ? tz_own : tz_posix[0];
	snprintf(ST.tz, sizeof ST.tz, "%s", next);
	tz_look();
	save(DASH_SETTINGS_SAVE);
}
static void host_tap(lv_event_t *e)
{
	(void)e;
	edit_slot = HOST_SLOT;
	dash_ui_show(DASH_VIEW_NETWORK);
}
static void slider_look(void);
/* LVGL's counter in the bottom right corner, its text ours, two rows: the CPU (the SoC's temperature, from the
 * kernel's thermal zone 0 every 2 s in the dash's unit where there is one; its load) and the frames (a second; a
 * frame's ms, render | flush). The Display settings' switch, or DASH_PERF=1 (performance checks). The corner is clear
 * of the split's picture. */
#if LV_USE_PERF_MONITOR
static lv_timer_t *fps_temp_timer;
static long fps_mc = LONG_MIN; /* the SoC's temperature, m°C (LONG_MIN: none) */
static void fps_temp_cb(lv_timer_t *t)
{
	(void)t;
	FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	long mc;
	fps_mc = f && fscanf(f, "%ld", &mc) == 1 ? mc : LONG_MIN;
	if (f) fclose(f);
}
/* v right-aligned in w places, the empty ones figure spaces (U+2007: a digit's width in Roboto, whose digits are all
 * one width), onto o: the counter's numbers change in place, nothing beside them moves */
static void fps_num(char *o, size_t n, long v, int w)
{
	char t[16];
	int pad = w - snprintf(t, sizeof t, "%ld", v);
	size_t k = strlen(o);
	while (pad-- > 0 && k + 3 < n) memcpy(o + k, "\xe2\x80\x87", 3), k += 3;
	snprintf(o + k, n - k, "%s", t);
}
/* after LVGL's own observer (observers run in the order they were added), so this text is the one shown */
static void fps_text_cb(lv_observer_t *o, lv_subject_t *subject)
{
	const lv_sysmon_perf_info_t *p = lv_subject_get_pointer(subject);
	char t[160] = "CPU ";
	if (fps_mc != LONG_MIN) {
		fps_num(t, sizeof t, lroundf(shown_deg(fps_mc / 1000.0f)), S.fahrenheit ? 3 : 2); /* (2 places: up to 99 C) */
		snprintf(t + strlen(t), sizeof t - strlen(t), " \xc2\xb0%c, ", S.fahrenheit ? 'F' : 'C');
	}
	fps_num(t, sizeof t, p->calculated.cpu, 3);
	snprintf(t + strlen(t), sizeof t - strlen(t), "%%\n");
	fps_num(t, sizeof t, p->calculated.fps, 2);
	snprintf(t + strlen(t), sizeof t - strlen(t), " FPS, ");
	fps_num(t, sizeof t, p->calculated.render_avg_time + p->calculated.flush_avg_time, 3);
	snprintf(t + strlen(t), sizeof t - strlen(t), " ms (");
	fps_num(t, sizeof t, p->calculated.render_avg_time, 2); /* (a rare 100 ms or more: the row wraps for that second) */
	snprintf(t + strlen(t), sizeof t - strlen(t), " | ");
	fps_num(t, sizeof t, p->calculated.flush_avg_time, 2);
	snprintf(t + strlen(t), sizeof t - strlen(t), ")");
	lv_label_set_text(lv_observer_get_target(o), t);
}
#endif
static void fps_show(void)
{
#if LV_USE_PERF_MONITOR
	lv_display_t *d = lv_display_get_default();
	const char *perf = getenv("DASH_PERF");
	bool on = ST.show_fps || (perf && !strcmp(perf, "1"));
	if (on) lv_sysmon_show_performance(d);
	else lv_sysmon_hide_performance(d);
	if (on && !fps_temp_timer && d->perf_label && d->perf_sysmon_backend.subject) {
		lv_subject_add_observer_obj(d->perf_sysmon_backend.subject, fps_text_cb, d->perf_label, NULL);
		fps_temp_timer = lv_timer_create(fps_temp_cb, 2000, NULL);
		/* in Roboto (its digits one width: fps_num), as wide as its text with every place filled, the text left in
		 * it: right-aligned in the corner, the box would otherwise move as its text changes */
		lv_obj_t *l = d->perf_label;
		lv_point_t sz;
		lv_obj_set_style_text_font(l, font_px(REGULAR, 15), 0);
		lv_text_get_size(&sz, "CPU 888 \xc2\xb0" "F, 100%\n88 FPS, 888 ms (88 | 88)", lv_obj_get_style_text_font(l, 0),
				 lv_obj_get_style_text_letter_space(l, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_obj_set_width(l, sz.x + lv_obj_get_style_pad_left(l, 0) + lv_obj_get_style_pad_right(l, 0));
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
	}
	if (!fps_temp_timer) return;
	if (on) lv_timer_resume(fps_temp_timer), fps_temp_cb(NULL);
	else lv_timer_pause(fps_temp_timer);
#endif
}
static void switch_changed(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	if (i == 0) ST.minor_figures = on;
	else if (i == 1) ST.hour12 = !on; /* the switch says 24-hour */
	else if (i == 2) ST.dev_ssh = on;
	else if (i == 4) ST.record = on;
	else if (i == 5) ST.sparse = on;
	else if (i == 6) ST.wifi_first = on;
	else if (i == 7) ST.ota_auto = on;
	else if (i == 8) ST.aa_low_res = on;
	else if (i == 9) ST.unit_miles = on;
	else if (i == 10) ST.unit_f = on;
	else if (i == 11) ST.unit_psi = on;
	else if (i == 12) ST.show_fps = on, fps_show();
	else if (i == 13) ST.save_logs = on;
	else ST.follow_car = on, slider_look();
	save(DASH_SETTINGS_SAVE);
}
/* ---- software update (Advanced): what dash-update reports in /run/dash-update.status, and its one button ---- */
static lv_obj_t *set_ota_l, *set_ota_btn;
static void aa_forget_tap(lv_event_t *e)
{
	(void)e;
	if (forget_second(&fg_aa)) save(DASH_AA_FORGET);
}
/* A part of a settings page shown or hidden (a row's button, a row): the page it is on is laid out and scrolled back
 * within its content, and redrawn whole. LVGL left a page that had been scrolled to its end past it when a row grew
 * shorter, and its scroll bar's old end stayed drawn. */
static void page_settle(lv_obj_t *o)
{
	lv_obj_mark_layout_as_dirty(lv_obj_get_parent(o)); /* its row: a plain box is not laid out again for a child hidden */
	for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p))
		if (lv_obj_is_scrollable(p)) { /* the page: its rows' boxes do not scroll */
			lv_obj_update_layout(p), lv_obj_readjust_scroll(p, LV_ANIM_OFF), lv_obj_invalidate(p);
			break;
		}
}
static void show_in_page(lv_obj_t *o, bool on)
{
	if (on != lv_obj_is_hidden(o)) return;
	show(o, on);
	page_settle(o);
}
/* A row's words given another width (the button beside them come or gone): fewer lines make the row shorter after the
 * page was settled for the button, so it is settled again. */
static void width_in_page(lv_obj_t *l, int32_t w)
{
	if (lv_obj_get_style_width(l, 0) == w) return;
	lv_obj_set_width(l, w);
	page_settle(l);
}
static int ota_next = DASH_OTA_CHECK; /* what the button does now */
static uint32_t ota_tapped; /* when the button was tapped (and hidden): back after a minute with no news */
static void ota_tap(lv_event_t *e)
{
	(void)e;
	save(ota_next);
	set_text(set_ota_l, "%s", ota_next == DASH_OTA_RESTART ? "Restarting..." : ota_next == DASH_OTA_INSTALL ? "Installing..." : "Checking...");
	show_in_page(set_ota_btn, false); /* back when dash-update reports */
	ota_tapped = lv_tick_get();
}
static void ota_field(const char *txt, const char *key, char *out, size_t n)
{
	const char *p = strstr(txt, key);
	size_t k = 0;
	if (p) for (p += strlen(key); *p && *p != '\n' && k + 1 < n; p++) out[k++] = *p;
	out[k] = 0;
}
static void ota_refresh(bool force)
{
	static uint32_t last;
	static char prev[512];
	if (!set_ota_l || (!force && lv_tick_elaps(last) < 1000)) return;
	last = lv_tick_get();
	char txt[512] = "", st[24], cur[24], lat[24], note[96];
	FILE *f = fopen("/run/dash-update.status", "r");
	if (f) { txt[fread(txt, 1, sizeof txt - 1, f)] = 0; fclose(f); }
	struct stat sb; /* dash-update rewrites the file (rename): the same text again is still news (Check -> "current") */
	static ino_t prev_ino;
	ino_t ino = stat("/run/dash-update.status", &sb) == 0 ? sb.st_ino : 0;
	if (ota_tapped && lv_tick_elaps(ota_tapped) > 60000) force = true; /* no answer to the tap: the button again */
	if (!force && !strcmp(txt, prev) && ino == prev_ino) return;
	ota_tapped = 0;
	snprintf(prev, sizeof prev, "%s", txt);
	prev_ino = ino;
	ota_field(txt, "state=", st, sizeof st), ota_field(txt, "current=", cur, sizeof cur);
	ota_field(txt, "latest=", lat, sizeof lat), ota_field(txt, "note=", note, sizeof note);
	if (!cur[0] && (f = fopen("/etc/image-id", "r"))) { /* no status yet: the version from the image */
		if (fgets(cur, sizeof cur, f)) cur[strcspn(cur, "-\n")] = 0;
		fclose(f);
	}
	char vs[48]; /* "Version 0.3.0", or a build with no version (any release counts as newer) */
	if (strchr(cur, '.')) snprintf(vs, sizeof vs, "Version %s", cur); else snprintf(vs, sizeof vs, "A development build");
	const char *btn = "Check for updates";
	ota_next = DASH_OTA_CHECK;
	bool busy = false;
	if (!strcmp(st, "available")) set_text(set_ota_l, "Version %s is available (now: %s)", lat, vs), btn = "Install", ota_next = DASH_OTA_INSTALL;
	else if (!strcmp(st, "downloading")) set_text(set_ota_l, "Downloading version %s...", lat), busy = true;
	else if (!strcmp(st, "verifying")) set_text(set_ota_l, "Checking the signature of version %s...", lat), busy = true;
	else if (!strcmp(st, "unpacking")) set_text(set_ota_l, "Unpacking version %s...", lat), busy = true;
	else if (!strcmp(st, "ready")) set_text(set_ota_l, "Version %s is downloaded", lat), btn = "Install", ota_next = DASH_OTA_INSTALL;
	else if (!strcmp(st, "armed")) set_text(set_ota_l, "Version %s starts at the next start", lat), btn = "Restart now", ota_next = DASH_OTA_RESTART;
	else if (!strcmp(st, "current")) set_text(set_ota_l, "%s: up to date", vs);
	else if (!strcmp(st, "offline")) set_text(set_ota_l, "%s. No internet now", vs);
	else if (!strcmp(st, "skipped")) set_text(set_ota_l, "%s. On the car's WiFi: updates need internet", vs);
	else if (!strcmp(st, "ok") && note[0]) set_text(set_ota_l, "%s: %s", vs, note);
	else if (!strcmp(st, "rolled-back")) set_text(set_ota_l, "The update did not start. %s", vs);
	else if (!strcmp(st, "failed-before")) set_text(set_ota_l, "%s. Not installed again on its own: %s", vs, note); /* after a rollback */
	else if (!strcmp(st, "trial-failed")) set_text(set_ota_l, "Version %s did not start this time: the next start tries again", lat), busy = true;
	else if (!strcmp(st, "trial") && strstr(note, "confirm")) set_text(set_ota_l, "%s is being tried out", vs), busy = true;
	else if (!strcmp(st, "trial")) set_text(set_ota_l, "%s: %s", vs, note), busy = true; /* another slot's trial, or why not kept */
	else if (!strcmp(st, "error")) set_text(set_ota_l, "Update failed: %s", note);
	else set_text(set_ota_l, "%s", vs);
	set_text(lv_obj_get_child(set_ota_btn, 0), "%s", btn);
	show_in_page(set_ota_btn, !busy);
	/* the words take what the button leaves (all of the row while it is hidden), two lines at most: a long error's rest
	 * is in the log */
	lv_obj_update_layout(set_ota_btn);
	float rw = lv_obj_get_width(lv_obj_get_parent(set_ota_l)) - (busy ? 0 : lv_obj_get_width(set_ota_btn) + PB(12));
	width_in_page(set_ota_l, ri(rw));
	lv_obj_set_style_max_height(set_ota_l, 2 * lv_font_get_line_height(lv_obj_get_style_text_font(set_ota_l, 0)), 0);
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
/* A page scroll that began on a slider releases it too (nothing moved then): saved only after a drag of its own. */
static bool slider_released(lv_event_t *e)
{
	if (lv_event_get_code(e) != LV_EVENT_RELEASED) return false;
	if (!lv_indev_get_scroll_obj(lv_indev_active())) save(DASH_SETTINGS_SAVE);
	return true;
}
static void slider_changed(lv_event_t *e)
{
	if (slider_released(e)) return;   /* the takeover: only on a value it moved to */
	if (ST.follow_car) {
		ST.follow_car = false;
		lv_obj_set_state(set_sw[3], LV_STATE_CHECKED, false);
		ST.brightness_pct = lv_slider_get_value(lv_event_get_target(e));
		slider_look();
	}
	ST.brightness_pct = lv_slider_get_value(lv_event_get_target(e));
}
/* The follow-car brightness bias: %, shown signed; applied only while following (does not turn following off). */
static void offset_note(void) { set_text(set_offset_note, "%+d%%", ST.follow_offset); }
static void offset_changed(lv_event_t *e)
{
	if (slider_released(e)) return;
	ST.follow_offset = lv_slider_get_value(lv_event_get_target(e));
	offset_note();
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
	lv_obj_set_width(l, ri(w - PB(52) - PB(8)));
	align(l, LV_ALIGN_LEFT_MID, 0, 0);
	set_sw[i] = m3_switch(r, i);
	align(set_sw[i], LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_min_height(r, ri(PB(40)), 0);
	return r;
}
/* a setting's name with a note under it (Material's list item with supporting text); the row sets its width */
static lv_obj_t *text_block(lv_obj_t *parent, const char *text, const char *note)
{
	lv_obj_t *t = column(parent, PB(1));
	lv_obj_set_height(t, LV_SIZE_CONTENT);
	lv_label_set_text(label(t, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE), text);
	lv_obj_t *n = label(t, FS(REGULAR, 12, UI_S_PANEL), W54);
	lv_obj_set_width(n, LV_PCT(100));
	lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(n, note);
	return t;
}
/* a switch with its name and a note left of it */
static lv_obj_t *switch_note_row(lv_obj_t *parent, const char *text, const char *note, int i, float w)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(r, ri(PB(40)), 0);
	lv_obj_t *t = text_block(r, text, note);
	lv_obj_set_width(t, ri(w - PB(52) - PB(12)));
	align(t, LV_ALIGN_LEFT_MID, 0, 0);
	set_sw[i] = m3_switch(r, i);
	align(set_sw[i], LV_ALIGN_RIGHT_MID, 0, 0);
	return r;
}
/* words with an outlined button right of them, the words taking what the button leaves */
static lv_obj_t *button_row(lv_obj_t *parent, lv_obj_t *words, const char *btn, lv_event_cb_t cb, float w)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), LV_SIZE_CONTENT);
	/* the button's width, from a layout of the button alone: made on a screen of its own, then moved into the row. In
	 * the row at once, each of these laid out the whole dialog built so far (three quarters of its building) */
	static lv_obj_t *apart;
	if (!apart) apart = lv_obj_create(NULL);
	lv_obj_t *b = outlined(apart, NULL, btn, cb, 0);
	lv_obj_update_layout(b);
	lv_obj_set_parent(b, r);
	align(b, LV_ALIGN_RIGHT_MID, 0, 0);
	if (words) {
		lv_obj_set_parent(words, r);
		lv_obj_set_width(words, ri(w - lv_obj_get_width(b) - PB(12)));
		align(words, LV_ALIGN_LEFT_MID, 0, 0);
	}
	return b;
}
static void net_tap(lv_event_t *e) /* a network's row: replace it */
{
	edit_slot = (intptr_t)lv_event_get_user_data(e);
	dash_ui_show(DASH_VIEW_NETWORK);
}
static int del_armed = -1; /* a network's delete: the first tap's row and time, its icon red until 3 s have passed */
static uint32_t del_armed_at;
static lv_obj_t *del_armed_ic;
static void net_delete_reset(bool all) /* all: the list rebuilt (the icon gone with it) */
{
	if (del_armed < 0 || (!all && lv_tick_elaps(del_armed_at) <= 3000)) return;
	if (!all && del_armed_ic) icon_color(del_armed_ic, W70);
	del_armed = -1, del_armed_ic = NULL;
}
static void net_delete(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i >= ST.net_count) return;
	if (del_armed != i || lv_tick_elaps(del_armed_at) > 3000) { /* a stray tap must not drop the car's WiFi while driving */
		if (del_armed_ic && del_armed_ic != lv_event_get_target(e)) icon_color(del_armed_ic, W70);
		del_armed = i, del_armed_at = lv_tick_get(), del_armed_ic = lv_event_get_target(e);
		icon_color(del_armed_ic, ALERT);
		return;
	}
	del_armed = -1, del_armed_ic = NULL;
	memmove(&ST.net[i], &ST.net[i + 1], sizeof ST.net[0] * (ST.net_count - i - 1));
	ST.net_count--;
	memset(&ST.net[ST.net_count], 0, sizeof ST.net[0]);
	save(DASH_SETTINGS_SAVE);
	dash_ui_show(DASH_VIEW_SETTINGS); /* the list again */
}
static char joining[33]; /* the network Connect was tapped for: "Connecting…", not a second join, for 20 s */
static uint32_t joining_at;
static void net_connect(lv_event_t *e) /* a saved network: join it now (it goes first; the rest stay as fallback) */
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i >= ST.net_count) return;
	if (!strcmp(joining, ST.net[i].ssid) && lv_tick_elaps(joining_at) < 20000) return;
	snprintf(joining, sizeof joining, "%s", ST.net[i].ssid), joining_at = lv_tick_get();
	snprintf(ST.join, sizeof ST.join, "%s", ST.net[i].ssid);
	save(DASH_SETTINGS_SAVE);
	ST.join[0] = 0;
	set_text(lv_event_get_target(e), "Connecting…");
}
static void nearby_pick(lv_event_t *e) /* a network in range: the editor, its name filled in, the passphrase next */
{
	int k = (intptr_t)lv_event_get_user_data(e);
	if (k < 0 || k >= DASH_NEARBY || ST.net_count >= DASH_NETS) return;
	snprintf(pick_ssid, sizeof pick_ssid, "%s", shown_nearby[k]);
	edit_slot = ST.net_count;
	dash_ui_show(DASH_VIEW_NETWORK);
}
static void net_row(lv_obj_t *parent, const char *ssid, int i, float w)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), ri(PB(26)));
	lv_obj_t *ic = icon(r, &ic_p_net, W70);
	align(ic, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_t *l = label(r, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_size(l, ri(w - 2 * PB(18) - 2 * PB(12) - PB(110)), lv_font_get_line_height(lv_obj_get_style_text_font(l, 0))); /* one line: LVGL's dots need a height */
	lv_label_set_text(l, ssid);
	align(l, LV_ALIGN_LEFT_MID, ri(PB(18) + PB(12)), 0);
	pressed_look(r, 0x1FFFFFFF);
	tappable(r, net_tap, i, PB(2));
	lv_obj_t *del = icon(r, &ic_p_delete, W70);
	align(del, LV_ALIGN_RIGHT_MID, 0, 0);
	tappable(del, net_delete, i, PB(12));
	bool on = S.net_ssid[0] && !strcmp(S.net_ssid, ssid);
	bool going = !on && !strcmp(joining, ssid) && lv_tick_elaps(joining_at) < 20000;
	lv_obj_t *c = label(r, FS(REGULAR, 14, UI_S_PANEL), on || going ? W70 : 0xFFD0BCFFu); /* M3 primary: the action */
	lv_label_set_text(c, on ? "Connected" : going ? "Connecting…" : "Connect");
	align(c, LV_ALIGN_RIGHT_MID, -ri(PB(18) + PB(20)), 0);
	if (!on && !going) tappable(c, net_connect, i, PB(10));
}
static void nearby_row(lv_obj_t *parent, int k, float w, bool room)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, ri(w), ri(PB(26)));
	lv_obj_t *ic = icon(r, &ic_p_net, W54);
	align(ic, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_t *l = label(r, FS(REGULAR, 14, UI_S_PANEL), room ? M3_ON_SURFACE : W54);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_size(l, ri(w - 2 * PB(18) - 2 * PB(12)), lv_font_get_line_height(lv_obj_get_style_text_font(l, 0))); /* one line, dots */
	lv_label_set_text(l, shown_nearby[k]);
	align(l, LV_ALIGN_LEFT_MID, ri(PB(18) + PB(12)), 0);
	if (room) { pressed_look(r, 0x1FFFFFFF); tappable(r, nearby_pick, k, PB(2)); }
}
#define SET_TABS 5
static lv_obj_t *set_tab_btn[SET_TABS], *set_page[SET_TABS];
static void settings_fill_nets(void)
{
	float w = lv_obj_get_style_width(net_list, 0);
	net_delete_reset(true);
	lv_obj_invalidate(set_page[3]); /* its scroll bar: longer when the content shrinks, outside the rows' areas */
	lv_obj_clean(net_list);
	set_wifi_line = label(net_list, FS(REGULAR, 12, UI_S_PANEL), W70); /* the link's state (main.dart's connection page) */
	lv_obj_set_width(set_wifi_line, ri(w));
	lv_label_set_long_mode(set_wifi_line, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(set_wifi_line, S.wifi_line);
	set_net_line = label(net_list, FS(REGULAR, 12, UI_S_PANEL), W70); /* the joined network, its address, ssh */
	lv_obj_set_width(set_net_line, ri(w));
	lv_label_set_long_mode(set_net_line, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(set_net_line, S.net_line);
	for (int i = 0; i < ST.net_count; i++) net_row(net_list, ST.net[i].ssid, i, w); /* never the passphrase */
	bool room = ST.net_count < DASH_NETS;
	if (room) lv_obj_set_height(outlined(net_list, &ic_p_add, "Add network", net_tap, ST.net_count), ri(PB(32)));
	lv_obj_clean(near_list); /* the right column: in range and not saved, tap to add and join (OTA, SSH somewhere new) */
	float nw = lv_obj_get_style_width(near_list, 0);
	int shown = 0;
	for (int k = 0; k < S.nearby_n && k < DASH_NEARBY; k++) {
		bool saved = false;
		for (int i = 0; i < ST.net_count; i++) saved |= !strcmp(ST.net[i].ssid, S.nearby[k]);
		if (saved || !S.nearby[k][0] || !strcmp(S.nearby[k], S.net_ssid)) continue;
		if (!shown) {
			lv_obj_t *h = label(near_list, FS(REGULAR, 12, UI_S_PANEL), W70);
			lv_label_set_text(h, room ? "Nearby: tap one to join" : "Nearby: delete a saved network to join another");
		}
		snprintf(shown_nearby[shown], sizeof shown_nearby[shown], "%s", S.nearby[k]);
		nearby_row(near_list, shown++, nw, room);
	}
	if (!shown) set_text(label(near_list, FS(REGULAR, 12, UI_S_PANEL), W54), "Nearby: no other network heard yet");
}
/* the settings' categories: the rail's buttons, their pages, the one shown (kept between openings) */
static int set_tab;
static void tab_select(int i)
{
	if (i != set_tab) lv_obj_scroll_to_y(set_page[i], 0, LV_ANIM_OFF); /* another tab: from its top */
	set_tab = i;
	for (int k = 0; k < SET_TABS; k++) {
		bool on = k == i;
		fill(set_tab_btn[k], on ? 0xFF4A4458u : 0x00000000u); /* M3 secondaryContainer (dark) */
		text_color(lv_obj_get_child(set_tab_btn[k], 0), on ? 0xFFE8DEF8u : W70);
		show(set_page[k], on);
	}
}
static void tab_tap(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	tab_select(i);
	if (i == 3) save(DASH_NET_SCAN); /* the Network tab: a fresh Nearby list */
}
void dash_ui_settings_tab(int i) { if (i >= 0 && i < SET_TABS && set_page[0]) tab_select(i); }
/* The Commander page's wheel at twice its size: worked out once by LVGL's own transform, for the whole image (what it
 * gives each part of the screen redrawn, pixel for pixel: its source pixel depends on the destination pixel alone), and
 * shown as it is where LVGL would put it. Transformed again for every part redrawn, it was a quarter of the frame that
 * opens the Settings. Without the memory: scaled as before. */
static void wheel_2x(lv_obj_t *img)
{
	static lv_image_dsc_t big;
	lv_image_decoder_dsc_t dec;
	lv_image_set_src(img, &img_wheel);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
	if (lv_image_decoder_open(&dec, &img_wheel, NULL) != LV_RESULT_OK) return (void)lv_image_set_scale(img, 512);
	const lv_draw_buf_t *src = dec.decoded;
	int32_t w = src->header.w, h = src->header.h;
	lv_draw_image_dsc_t d; /* as lv_image draws it: scaled about its middle, anti-aliased */
	lv_draw_image_dsc_init(&d);
	d.src = &img_wheel, d.scale_x = d.scale_y = 512, d.pivot = (lv_point_t){w / 2, h / 2}, d.antialias = 1;
	lv_area_t a, at = {0, 0, w - 1, h - 1}, box = {0, 0, lv_obj_get_style_width(img, 0) - 1, lv_obj_get_style_height(img, 0) - 1};
	lv_image_buf_get_transformed_area(&a, w, h, 0, 512, 512, &d.pivot);
	lv_area_align(&box, &at, LV_ALIGN_CENTER, 0, 0); /* where lv_image puts the unscaled image in its box */
	int32_t bw = lv_area_get_width(&a), bh = lv_area_get_height(&a);
	bool argb = src->header.cf == LV_COLOR_FORMAT_ARGB8888; /* (any other: LVGL scales it at each draw) */
	uint8_t *px = big.data ? (uint8_t *)big.data : argb ? lv_malloc((size_t)bw * bh * 4) : NULL;
	if (px && argb) {
		lv_draw_sw_transform(&a, src->data, w, h, src->header.stride, &d, NULL, src->header.cf, px);
		big = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED, .w = bw,
						  .h = bh, .stride = bw * 4}, .data_size = bw * bh * 4, .data = px};
		lv_image_set_src(img, &big); /* (anti-aliased, LVGL's transform gives premultiplied pixels) */
		lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_LEFT);
		lv_image_set_offset_x(img, at.x1 + a.x1), lv_image_set_offset_y(img, at.y1 + a.y1);
	} else
		lv_image_set_scale(img, 512);
	lv_image_decoder_close(&dec);
}
static void settings_build(void)
{
	float top = PB(6), x0 = G.ml + inset(top) + PB(6), w = G.W - 2 * x0, h = G.H - 2 * top, pad = PB(20);
	/* on the screen, above the dash, not on the top layer: LVGL looks for an opaque cover only among the screen's own
	 * objects, so on the top layer each redraw inside the dialog (a scrolled page: every frame) drew the whole dash
	 * beneath it first. Here the dialog is that cover; the dash still shows, dimmed, round it (C906: 64 -> ~20 ms) */
	settings = box(lv_screen_active());
	place(settings, 0, 0, G.W, G.H);
	fill(settings, 0x8A000000); /* the dialog's barrier, black54 */
	lv_obj_set_clickable(settings, true);
	lv_obj_t *d = set_dlg = box(settings);
	place(d, x0, top, w, h);
	fill(d, M3_DIALOG);
	lv_obj_set_style_radius(d, ri(px(28)), 0);
	lv_obj_t *title = label(d, FS(REGULAR, 24, UI_S_PANEL * 0.8f), M3_ON_SURFACE);
	lv_label_set_text(title, "Settings");
	lv_obj_set_pos(title, ri(pad + PB(4)), ri(pad * 0.7f));
	lv_obj_t *x = icon(d, &ic_p_close, M3_ON_SURFACE);
	align(x, LV_ALIGN_TOP_RIGHT, ri(-pad), ri(pad * 0.7f));
	tappable(x, close_tap, 0, PB(16));
	/* A rail of categories on the left (M3's navigation rail: the chosen one in a pill), its page on the right */
	static const char *const names[SET_TABS] = {"Commander", "Car", "Display", "Network", "Advanced"};
	float cy = pad * 0.7f + PB(24) * 0.8f * 1.17f + PB(14), rail_w = PB(150), tab_h = PB(32);
	float xp = pad + rail_w + PB(32), cw = fminf(PB(360), w - xp - pad), ph = h - cy - PB(8);
	/* A page's rectangle stays inside the dialog's round corner. LVGL draws a scrolled page in strips and starts each
	 * at the dialog only if the dialog covers the strip's four corners; the last strip's bottom right one lay in the
	 * part the round cuts off (the F133's radius: 53 px, the page 11 and 15 px from the edges), so every frame of a
	 * scroll drew the dash beneath that strip, the barrier and the dialog again (a quarter of the frame). The page is
	 * narrowed by what poked out, its right padding and its scroll bar's margin by as much: nothing drawn moves. */
	float rr = ri(px(28)), ex = PB(6), ey = PB(8);
	int cut = ey < rr ? (int)ceilf(rr - sqrtf(rr * rr - (rr - ey) * (rr - ey)) - ex) + 1 : 0;
	cut = cut < 0 ? 0 : cut > ri(PB(4)) ? ri(PB(4)) : cut; /* (at most the bar's margin: the bar stays inside the page) */
	for (int i = 0; i < SET_TABS; i++) {
		lv_obj_t *t = set_tab_btn[i] = box(d);
		place(t, pad, cy + i * (tab_h + PB(3)), rail_w, tab_h);
		lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
		pressed_look(t, 0x1FFFFFFF);
		tappable(t, tab_tap, i, PB(3));
		lv_obj_t *l = label(t, FS(MEDIUM, 14, UI_S_PANEL), M3_ON_SURFACE);
		lv_label_set_text(l, names[i]);
		align(l, LV_ALIGN_LEFT_MID, ri(PB(20)), 0);
		/* a page longer than the dialog scrolls, its bar shown all the time there is more (in the right margin) */
		lv_obj_t *pg = set_page[i] = column(d, PB(10));
		place(pg, xp, cy, w - xp - PB(6) - cut, ph);
		lv_obj_set_style_pad_right(pg, ri(pad - PB(6)) - cut, 0);
		lv_obj_set_style_pad_bottom(pg, ri(PB(12)), 0);
		lv_obj_set_clickable(pg, true);
		lv_obj_set_scrollable(pg, true);
		lv_obj_set_scroll_dir(pg, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_style_width(pg, ri(PB(4)), LV_PART_SCROLLBAR);
		lv_obj_set_style_radius(pg, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
		lv_obj_set_style_bg_color(pg, rgb(M3_OUTLINE), LV_PART_SCROLLBAR);
		lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, LV_PART_SCROLLBAR);
		lv_obj_set_style_pad_right(pg, ri(PB(4)) - cut, LV_PART_SCROLLBAR);
	}
	/* the Commander: the steering wheel to press, and beside it the steps, the current one said as it comes */
	lv_obj_t *c = set_page[0];
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(c, ri(PB(28)), 0);
	set_wheel = lv_image_create(c);
	lv_obj_remove_style_all(set_wheel);
	lv_obj_set_size(set_wheel, ri(img_wheel.header.w * 2.0f), ri(img_wheel.header.h * 2.0f));
	wheel_2x(set_wheel); /* 135 x 105 at 2: 270 x 210 */
	float pw = w - xp - pad, sw = fminf(PB(460), pw - img_wheel.header.w * 2.0f - PB(28)); /* this page: the dialog's width */
	lv_obj_t *steps = column(c, PB(10));
	lv_obj_set_size(steps, ri(sw), LV_SIZE_CONTENT);
	set_status = label(steps, FS(MEDIUM, 14, UI_S_PANEL), WHITE);
	set_bt_line = label(steps, FS(REGULAR, 12, UI_S_PANEL), W70); /* the session in words (main.dart's connection page) */
	lv_obj_set_width(set_bt_line, ri(sw));
	lv_label_set_long_mode(set_bt_line, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_t *btns = row(steps); /* side by side: the status line's room */
	lv_obj_set_style_pad_column(btns, ri(PB(16)), 0);
	outlined(btns, NULL, "Approve this " UNIT, approve_tap, 0);
	fg_cmdr.btn = outlined(btns, NULL, "Forget Commander", forget_tap, 0);
	set_step = label(steps, FS(MEDIUM, 16, UI_S_PANEL), W70);
	lv_obj_set_width(set_step, ri(sw));
	lv_label_set_long_mode(set_step, LV_LABEL_LONG_MODE_WRAP);
	/* the car: the phone app's six, the chosen one filled (the body, its seats and its drive) */
	c = set_page[1];
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(c, ri(PB(10)), 0);
	set_car_ask = label(c, FS(MEDIUM, 16, UI_S_PANEL), M3_ON_SURFACE); /* the first boot's question (main.dart _askCar) */
	lv_obj_set_width(set_car_ask, ri(w - xp - pad));
	lv_label_set_text(set_car_ask, "Which car do you have?");
	show(set_car_ask, false);
	for (int i = 0; i < 6; i++) {
		static const char *const cars[6] = {"Model 3 RWD", "Model 3 AWD", "Model Y 5 seats RWD", "Model Y 5 seats AWD",
						    "Model Y 7 seats", "Model Y L"}; /* CAR_* order (core/state.h) */
		lv_obj_t *b = set_car_btn[i] = outlined(c, NULL, cars[i], car_tap, i);
		lv_obj_set_width(b, ri((w - xp - pad - 2 * PB(10)) / 3 - 1));
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	}
	lv_obj_t *cnote = label(c, FS(REGULAR, 12, UI_S_PANEL), W54);
	lv_obj_set_width(cnote, ri(w - xp - pad));
	lv_label_set_text(cnote, "The car's picture, its seats and belts, and a rear-motor car's ring (the pedals on the right) follow it.");
	/* the display */
	c = set_page[2]; /* one column, three groups: the screen's brightness, performance (AA's resolution, the frame counter), the dash's figures and clock */
	cw = w - xp - pad;
	group_title(c, "Screen"); /* first: the one reached for most, and at night */
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
	lv_obj_set_style_margin_top(set_slider, ri(PB(10)), 0); /* the knob (7 dp past the track) clear of the label */
	lv_slider_set_range(set_slider, 0, 100); /* 0: the lowest the panel still shows a picture at, not off */
	lv_obj_set_style_bg_color(set_slider, rgb(M3_KEY), LV_PART_MAIN);
	lv_obj_set_style_bg_color(set_slider, rgb(M3_PRIMARY), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(set_slider, rgb(M3_PRIMARY), LV_PART_KNOB);
	lv_obj_set_style_pad_all(set_slider, ri(PB(7)), LV_PART_KNOB);
	lv_obj_set_ext_click_area(set_slider, ri(PB(16)));
	lv_obj_set_adv_hittest(set_slider, true); /* only the knob grabs: a page swipe over the track scrolls */
	lv_obj_add_event_cb(set_slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(set_slider, slider_changed, LV_EVENT_RELEASED, NULL);
	lv_obj_t *ob = box(c); /* "Follow offset" and its signed value: the bias used while following the car */
	lv_obj_set_size(ob, ri(cw), LV_SIZE_CONTENT);
	lv_obj_set_style_margin_top(ob, ri(PB(14)), 0);
	lv_label_set_text(label(ob, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE), "Follow offset");
	set_offset_note = label(ob, FS(REGULAR, 12, UI_S_PANEL), W54);
	align(set_offset_note, LV_ALIGN_RIGHT_MID, 0, 0);
	set_offset = lv_slider_create(c);
	lv_obj_set_size(set_offset, ri(cw - PB(16)), ri(PB(6)));
	lv_obj_set_style_margin_left(set_offset, ri(PB(8)), 0);
	lv_obj_set_style_margin_top(set_offset, ri(PB(10)), 0);
	lv_slider_set_range(set_offset, -50, 50);
	lv_obj_set_style_bg_color(set_offset, rgb(M3_KEY), LV_PART_MAIN);
	lv_obj_set_style_bg_color(set_offset, rgb(M3_PRIMARY), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(set_offset, rgb(M3_PRIMARY), LV_PART_KNOB);
	lv_obj_set_style_pad_all(set_offset, ri(PB(7)), LV_PART_KNOB);
	lv_obj_set_ext_click_area(set_offset, ri(PB(16)));
	lv_obj_set_adv_hittest(set_offset, true);
	lv_obj_add_event_cb(set_offset, offset_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(set_offset, offset_changed, LV_EVENT_RELEASED, NULL);
	lv_obj_set_style_margin_top(group_title(c, "Performance"), ri(PB(22)), 0); /* (clear of the slider's knob above) */
	switch_note_row(c, "Android Auto at 480p", "Scaled up to fit: a softer picture, lighter on the WiFi and the phone. From the next connection", 8, cw);
	switch_row(c, "Show frames per second", 12, cw);
	lv_obj_set_style_margin_top(group_title(c, "Dash"), ri(PB(14)), 0);
	switch_row(c, "Show 12 V, other power and cell imbalance", 0, cw);
	switch_row(c, "Distance and speed in miles (mph)", 9, cw);
	switch_row(c, "Temperature in °F", 10, cw);
	switch_row(c, "Tyre pressure in psi", 11, cw);
	switch_row(c, "24-hour clock", 1, cw); /* the zone's name on a button: a tap, the next */
	set_tz_btn = button_row(c, text_block(c, "Time zone", "The arrival and charge clocks, summer time included"), "UTC", tz_tap, cw);
	/* the networks, two columns across the page: the status and the saved ones; what is in range (settings_fill_nets) */
	float nw = w - xp - pad, ncw = (nw - PB(32)) / 2;
	c = set_page[3];
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(c, ri(PB(32)), 0);
	net_list = column(c, PB(6));
	lv_obj_set_size(net_list, ri(ncw), LV_SIZE_CONTENT); /* as tall as its rows: a box's default height clips them */
	near_list = column(c, PB(6));
	lv_obj_set_size(near_list, ri(ncw), LV_SIZE_CONTENT);
	/* advanced, one column in four groups: the developer's, the links' timing (the phone app's Advanced), Android Auto's
	 * paired phones, software update */
	float aw = w - xp - pad;
	c = set_page[4];
	group_title(c, "Developer");
	switch_note_row(c, "SSH", "Needs a paired key, or one on the card", 2, aw);
	button_row(c, text_block(c, "Pair a computer", "The " UNIT " shows a page and a code: that computer adds its key, within 5 minutes"),
		   "Pair", pair_tap, aw); /* only from this tap: someone at the Dash */
	switch_note_row(c, "Record drives", "CAN and Commander data to the card, for replays at the desk", 4, aw);
	switch_note_row(c, "Save logs", "To the card, for reading a drive afterwards. Off: no logs at all, sparing the card. From the next start", 13, aw);
	set_host_l = label(c, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE); /* the Panda's address, its editor beside it */
	lv_label_set_long_mode(set_host_l, LV_LABEL_LONG_MODE_DOTS);
	button_row(c, set_host_l, "Panda host", host_tap, aw);
	lv_obj_set_height(set_host_l, lv_font_get_line_height(lv_obj_get_style_text_font(set_host_l, 0))); /* one line, dots */
	lv_obj_set_style_margin_top(group_title(c, "Links"), ri(PB(14)), 0);
	switch_note_row(c, "Sparse polling", "Steadier speed, slower doors and belts", 5, aw);
	switch_note_row(c, "Bluetooth waits for WiFi", "No Bluetooth until the WiFi stream runs, however long", 6, aw);
	lv_obj_set_style_margin_top(group_title(c, "Android Auto"), ri(PB(14)), 0);
	lv_obj_t *fw = text_block(c, "Paired phones", "Each one gets Android Auto's WiFi password whenever it is on. Forget them "
						      "all (a phone sold, a passenger's), then pair yours again from the AA button");
	fg_aa.btn = button_row(c, fw, "Forget phones", aa_forget_tap, aw);
	lv_obj_set_style_margin_top(group_title(c, "Software update"), ri(PB(14)), 0);
	switch_note_row(c, "Automatic updates", "With internet (never the car's WiFi): download, check the signature, install at the next start", 7, aw);
	set_ota_l = label(c, FS(REGULAR, 14, UI_S_PANEL), M3_ON_SURFACE); /* the version, what dash-update says */
	lv_label_set_long_mode(set_ota_l, LV_LABEL_LONG_MODE_WRAP);
	set_ota_btn = button_row(c, set_ota_l, "Check for updates", ota_tap, aw);
	ota_refresh(true);
	lv_obj_set_style_margin_top(group_title(c, "Restart"), ri(PB(14)), 0);
	lv_obj_t *rw = text_block(c, "Restart the screen", DASH_RESTART_NOTE);
	fg_restart.btn = button_row(c, rw, "Restart", restart_tap, aw);
	tab_select(set_tab);
	show(settings, false);
}

/* ---- a network's SSID and passphrase, typed on the on-screen keyboard ---- */
/* The Panda's address as typed: host[:port] (port kept when left out). false: said why in net_err. */
static bool host_save(void)
{
	char h[80], *c;
	const char *in = lv_textarea_get_text(net_ta[0]);
	while (*in == ' ') in++;
	snprintf(h, sizeof h, "%s", in);
	for (size_t k = strlen(h); k && h[k - 1] == ' ';) h[--k] = 0;
	int port = ST.panda_port;
	if ((c = strrchr(h, ':'))) {
		char *end;
		long p = strtol(c + 1, &end, 10);
		if (end == c + 1 || *end || p < 1 || p > 65535) return set_text(net_err, "The port: 1 to 65535"), false;
		*c = 0, port = (int)p;
	}
	struct in_addr a4; /* numeric only: no DNS on the car's WiFi (a lookup would hang the dash) */
	if (!h[0] || strlen(h) >= sizeof ST.panda_host || inet_pton(AF_INET, h, &a4) != 1)
		return set_text(net_err, "The host: an IPv4 address, e.g. 192.168.4.1"), false;
	snprintf(ST.panda_host, sizeof ST.panda_host, "%s", h);
	ST.panda_port = port;
	save(DASH_SETTINGS_SAVE);
	dash_ui_show(DASH_VIEW_SETTINGS);
	return true;
}
static bool net_dup(const char *ssid) /* the name is saved in another slot */
{
	for (int k = 0; k < ST.net_count; k++)
		if (k != edit_slot && !strcmp(ST.net[k].ssid, ssid)) return true;
	return false;
}
static void net_save(lv_event_t *e)
{
	(void)e;
	if (edit_slot == HOST_SLOT) {
		host_save();
		return;
	}
	if (edit_slot < 0 || edit_slot >= DASH_NETS) return;
	char name[40]; /* the name as typed, its ends' spaces dropped (the file's reader drops them too) */
	const char *in = lv_textarea_get_text(net_ta[0]), *pass = lv_textarea_get_text(net_ta[1]);
	while (*in == ' ') in++;
	snprintf(name, sizeof name, "%s", in);
	for (size_t k = strlen(name); k && name[k - 1] == ' ';) name[--k] = 0;
	const char *ssid = name;
	size_t pl = strlen(pass);
	bool replacing = edit_slot < ST.net_count;
	if (!ssid[0] || strlen(ssid) > 32) set_text(net_err, "The name: 1 to 32 characters");
	else if (replacing && !pl && strcmp(ssid, ST.net[edit_slot].ssid)) set_text(net_err, "A new name: type its passphrase too");
	else if ((pl || !replacing) && (pl < 8 || pl > 63)) set_text(net_err, "The passphrase: 8 to 63 characters");
	else if (strchr(pass, '"')) set_text(net_err, "The passphrase: no \" (WiFi's config cannot hold it)");
	else if (net_dup(ssid)) set_text(net_err, "Already saved: tap that one to change it");
	else {
		dash_net_t *n = &ST.net[edit_slot];
		snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
		snprintf(n->pass, sizeof n->pass, "%s", pass); /* "" when replacing: keep the one it has */
		if (!replacing) ST.net_count++;
		if (pick_ssid[0] && !strcmp(pick_ssid, ssid)) snprintf(ST.join, sizeof ST.join, "%s", ssid); /* picked: join now */
		pick_ssid[0] = 0;
		save(DASH_SETTINGS_SAVE);
		ST.join[0] = 0;
		memset(n->pass, 0, sizeof n->pass); /* handed over; not kept here */
		lv_textarea_set_text(net_ta[1], "");
		dash_ui_show(DASH_VIEW_SETTINGS);
	}
}
static void net_cancel(lv_event_t *e)
{
	(void)e;
	pick_ssid[0] = 0;
	lv_textarea_set_text(net_ta[1], "");
	dash_ui_show(DASH_VIEW_SETTINGS);
}
/* The keyboard: Gboard's rows (q-p; a-l half a key in; shift z-m backspace; ?123 , space . enter) and, the screen
 * being long, a number pad on the right. Shift: one tap = the next letter capital, a second = caps lock, a third = off.
 * ?# swaps the letters for every other printable ASCII character (passphrases may use any). Our own handler: no
 * LVGL keyboard modes, so nothing types a key's label. */
#define KB_SHIFT LV_SYMBOL_UP
#define KB_CAPS LV_SYMBOL_UPLOAD
#define KB_BS LV_SYMBOL_BACKSPACE
#define KB_ENTER LV_SYMBOL_NEW_LINE
#define KB_PAD "\n"
static lv_obj_t *kb_ta;
static int kb_mode; /* 0 lower, 1 shift (one letter), 2 caps lock, 3 symbols */
static const char *const kb_lower[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", KB_PAD,
	" ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", KB_PAD,
	KB_SHIFT, "z", "x", "c", "v", "b", "n", "m", KB_BS, KB_PAD,
	"?#", ",", " ", ".", KB_ENTER, ""};
static const char *const kb_upper[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", KB_PAD,
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", KB_PAD,
	KB_SHIFT, "Z", "X", "C", "V", "B", "N", "M", KB_BS, KB_PAD,
	"?#", ",", " ", ".", KB_ENTER, ""};
static const char *const kb_caps[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", KB_PAD,
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", KB_PAD,
	KB_CAPS, "Z", "X", "C", "V", "B", "N", "M", KB_BS, KB_PAD,
	"?#", ",", " ", ".", KB_ENTER, ""};
static const char *const kb_sym[] = {
	"@", "#", "$", "%", "&", "*", "(", ")", "+", "/", KB_PAD,
	"!", "\"", "'", ":", ";", "=", "?", "<", ">", "^", KB_PAD,
	" ", "[", "]", "{", "}", "\\", "|", "~", "`", KB_BS, KB_PAD,
	"ABC", ",", " ", ".", KB_ENTER, ""};
/* widths in a 20-unit row; control keys styled as checked. The number pad is a grid of its own, beside it. */
static const char *const kb_pad[] = {"1", "2", "3", KB_PAD, "4", "5", "6", KB_PAD, "7", "8", "9", KB_PAD, "-", "0", "_", ""};
#define K(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_WIDTH_##w | LV_BUTTONMATRIX_CTRL_NO_REPEAT))
#define KC(w) ((lv_buttonmatrix_ctrl_t)(K(w) | LV_BUTTONMATRIX_CTRL_CHECKED))
#define KH(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_WIDTH_##w | LV_BUTTONMATRIX_CTRL_HIDDEN))
#define KBS(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_WIDTH_##w | LV_BUTTONMATRIX_CTRL_CHECKED)) /* repeats */
static const lv_buttonmatrix_ctrl_t kb_ctrl_text[] = {
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	KH(1), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KH(1),
	KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KBS(3),
	KC(3), K(2), K(10), K(2), KC(3)};
static const lv_buttonmatrix_ctrl_t kb_ctrl_sym[] = {
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	KH(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KBS(2),
	KC(3), K(2), K(10), K(2), KC(3)};
static void kb_set_mode(int m)
{
	kb_mode = m;
	lv_buttonmatrix_set_map(net_kb, m == 3 ? kb_sym : m == 2 ? kb_caps : m == 1 ? kb_upper : kb_lower);
	lv_buttonmatrix_set_ctrl_map(net_kb, m == 3 ? kb_ctrl_sym : kb_ctrl_text);
}
static void kb_key(lv_event_t *e)
{
	lv_obj_t *m = lv_event_get_target(e);
	uint32_t id = lv_buttonmatrix_get_selected_button(m);
	const char *t = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(m, id);
	if (!t || !kb_ta || FRESH(edit_at, 500)) return; /* keys act on press: a bounce of the opening tap types nothing */
	if (!strcmp(t, KB_SHIFT) || !strcmp(t, KB_CAPS)) kb_set_mode(kb_mode == 0 ? 1 : kb_mode == 1 ? 2 : 0);
	else if (!strcmp(t, "?#")) kb_set_mode(3);
	else if (!strcmp(t, "ABC")) kb_set_mode(0);
	else if (!strcmp(t, KB_BS)) lv_textarea_delete_char(kb_ta);
	else if (!strcmp(t, KB_ENTER)) {
		if (kb_ta == net_ta[0] && edit_slot != HOST_SLOT) lv_obj_send_event(net_ta[1], LV_EVENT_CLICKED, NULL); /* next: the passphrase */
		else net_save(e);
	} else {
		lv_textarea_add_text(kb_ta, t);
		if (kb_mode == 1 && t[1] == 0 && t[0] >= 'A' && t[0] <= 'Z') kb_set_mode(0); /* one capital, then back */
	}
}
/* A key down or up: the keyboard's pressed state set here first, with the display's invalidation off. LVGL sets it on
 * the whole keyboard, and a pressed look among its keys' styles made that redraw every key (most of the screen), down
 * and up again, for each key typed; the keyboard itself then redraws the key under the finger (and the field what it
 * types), so only those change. */
static void kb_press(lv_event_t *e)
{
	lv_obj_t *m = lv_event_get_current_target(e);
	lv_display_t *d = lv_obj_get_display(m);
	bool on = lv_display_is_invalidation_enabled(d);
	lv_display_enable_invalidation(d, false);
	lv_obj_set_state(m, LV_STATE_PRESSED, lv_event_get_code(e) == LV_EVENT_PRESSED);
	lv_display_enable_invalidation(d, on);
}
static void ta_focus(lv_event_t *e)
{
	kb_ta = lv_event_get_target(e);
	for (int i = 0; i < 2; i++) lv_obj_set_state(net_ta[i], LV_STATE_FOCUSED, net_ta[i] == kb_ta);
}
static void net_edit_build(void)
{
	float top = PB(6), x0 = G.ml + inset(top) + PB(6), w = G.W - 2 * x0, pad = PB(16);
	net_edit = box(lv_screen_active()); /* a page (pages_top) */
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
	lv_obj_set_click_focusable(b, false);
	lv_obj_set_pos(b, ri(x0 + 2 * (fw + pad)), ri(top + pad));
	b = outlined(net_edit, NULL, "Save", (lv_event_cb_t)net_save, 0);
	lv_obj_set_click_focusable(b, false);
	lv_obj_set_pos(b, ri(x0 + 2 * (fw + pad) + PB(110) + pad / 2), ri(top + pad));
	net_err = label(net_edit, FS(REGULAR, 12, UI_S_PANEL), 0xFFF2B8B5); /* M3 dark error */
	lv_obj_set_pos(net_err, ri(x0), ri(top + pad + PB(48)));
	/* the letters (20 units) and, a unit's gap to their right, the number pad (6 units): one key size throughout */
	float ky = top + pad + PB(48) + PB(18), u = w / 27;
	((lv_font_t *)FS(REGULAR, 16, UI_S_PANEL))->fallback = &lv_font_montserrat_28; /* the keys' symbols */
	for (int i = 0; i < 2; i++) {
		lv_obj_t *m = lv_buttonmatrix_create(net_edit);
		if (i == 0) net_kb = m, kb_set_mode(0);
		else {
			lv_buttonmatrix_set_map(m, kb_pad);
			lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
		}
		lv_obj_set_size(m, ri(i ? 6 * u : 20 * u), ri(G.H - ky - top));
		align(m, LV_ALIGN_TOP_LEFT, ri(x0 + (i ? 21 * u : 0)), ri(ky));
		lv_obj_set_style_pad_all(m, ri(PB(3)), 0);
		lv_obj_set_style_pad_gap(m, ri(PB(5)), 0);
		lv_obj_set_style_border_width(m, 0, 0);
		lv_obj_set_style_bg_color(m, rgb(M3_DIALOG), 0);
		lv_obj_set_style_radius(m, ri(PB(6)), LV_PART_ITEMS);
		lv_obj_set_style_bg_color(m, rgb(M3_KEY), LV_PART_ITEMS);
		lv_obj_set_style_bg_color(m, rgb(M3_OUTLINE), LV_PART_ITEMS | LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(m, rgb(0xFF332D41), LV_PART_ITEMS | LV_STATE_CHECKED); /* the control keys */
		lv_obj_set_style_text_color(m, rgb(M3_ON_SURFACE), LV_PART_ITEMS | LV_STATE_CHECKED);
		lv_obj_set_style_text_color(m, rgb(M3_ON_SURFACE), LV_PART_ITEMS);
		lv_obj_set_style_text_font(m, FS(REGULAR, 16, UI_S_PANEL), LV_PART_ITEMS);
		lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
		lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
		lv_obj_add_event_cb(m, kb_key, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(m, kb_press, LV_EVENT_PRESSED | LV_EVENT_PREPROCESS, NULL);
		lv_obj_add_event_cb(m, kb_press, LV_EVENT_RELEASED | LV_EVENT_PREPROCESS, NULL);
		lv_obj_add_event_cb(m, kb_press, LV_EVENT_PRESS_LOST | LV_EVENT_PREPROCESS, NULL);
		lv_obj_set_click_focusable(m, false); /* a key press keeps the field's focus look */
	}
	show(net_edit, false);
}

void dash_ui_settings(const dash_settings_t *s)
{
	ST = *s;
	slots_refresh();
	for (int i = 0; i < DASH_NETS; i++) memset(ST.net[i].pass, 0, sizeof ST.net[i].pass); /* never kept for show */
	ST.join[0] = 0; /* a one-off: never sent again with the next save */
	fps_show();
	if (view == DASH_VIEW_SETTINGS) dash_ui_show(DASH_VIEW_SETTINGS);
}

void dash_ui_ask_car(void)
{
	show(set_car_ask, true);
	dash_ui_settings_tab(1);
	dash_ui_show(DASH_VIEW_SETTINGS);
}

/* The phone layout's split: the cards' parts, the buttons and their tooltip go onto its cover with it, hidden and moved
 * into the right half (one card shows; LVGL repaints a part where it was when it shows it, and the left half's lower
 * 3/4 under the cover is the decoder's picture); the path lines' box (the whole screen on the dash) cut to it too.
 * Back on the dash after, each in its place among the dash's parts (the frame and the path lines under the car and the
 * chips): the reverse of the way out, before the ring and the links go back. */
static void pj_parts(bool to_pj)
{
	static int at[64]; /* each one's place among the dash's, as it left */
	lv_obj_t *o[64];
	int n = 0;
	o[n++] = cab.frame, o[n++] = steer, o[n++] = pedal, o[n++] = pedal_name, o[n++] = pedal_val, o[n++] = brake_car;
	for (int i = 0; i < CHIPS; i++) o[n++] = chips[i].box;
	o[n++] = lights_row, o[n++] = cab.grp;
	for (int c = 0; c < DASH_CARDS; c++) o[n++] = dots[SPLIT_SLOT][c];
	o[n++] = dest_card, o[n++] = no_route, o[n++] = dial, o[n++] = charge_box, o[n++] = stats_box;
	o[n++] = gball, o[n++] = g_val, o[n++] = g_peak, o[n++] = brake_temps, o[n++] = pdial, o[n++] = tip; /* the dash's order */
	if (to_pj)
		for (int i = 0; i < n; i++) { /* parked clear of the picture by more than any part draws past its box (the g dot's) */
			at[i] = lv_obj_get_index(o[i]);
			lv_obj_set_parent(o[i], project);
			show(o[i], false);
			lv_obj_set_x(o[i], ri(pj_vw() + px(16)));
		}
	else
		for (int i = n - 1; i >= 0; i--) lv_obj_set_parent(o[i], root), lv_obj_move_to_index(o[i], at[i]);
	place(steer, to_pj ? pj_vw() : 0, 0, to_pj ? G.W - pj_vw() : G.W, G.H);
	strcpy(lights_shown, "-"); /* moved: placed again when next shown */
}

/* The settings coming up: the screen redrawn in parts, those the dialog covers on their own. LVGL starts each part at the
 * topmost object that covers all of it: in one part, the whole screen, that was the screen itself, so the frame that
 * opened the settings drew the whole dash and the dim barrier over it beneath the dialog (two thirds of its work). The
 * dialog's rectangle less its round corners, in three parts, starts at the dialog; the barrier's frame round it and the
 * four corners draw what they did. The parts make up the whole screen: whatever else came or went is drawn too. */
static void settings_reveal(void)
{
	lv_area_t d;
	lv_obj_get_coords(set_dlg, &d);
	int32_t r = lv_obj_get_style_radius(set_dlg, 0), w = G.W - 1, h = G.H - 1;
	const lv_area_t part[] = {
		{d.x1 + r, d.y1, d.x2 - r, d.y2}, {d.x1, d.y1 + r, d.x1 + r - 1, d.y2 - r}, {d.x2 - r + 1, d.y1 + r, d.x2, d.y2 - r},
		{0, 0, w, d.y1 - 1}, {0, d.y2 + 1, w, h}, {0, d.y1, d.x1 - 1, d.y2}, {d.x2 + 1, d.y1, w, d.y2},
		{d.x1, d.y1, d.x1 + r - 1, d.y1 + r - 1}, {d.x2 - r + 1, d.y1, d.x2, d.y1 + r - 1},
		{d.x1, d.y2 - r + 1, d.x1 + r - 1, d.y2}, {d.x2 - r + 1, d.y2 - r + 1, d.x2, d.y2}};
	for (size_t i = 0; i < sizeof part / sizeof part[0]; i++)
		if (part[i].x1 <= part[i].x2 && part[i].y1 <= part[i].y2) lv_obj_invalidate_area(settings, &part[i]);
}

/* The units' switches: the owner's pick, or (none yet) what is shown: the car's unit, °F going with miles. Again each
 * frame while the settings are open: miles switched there takes °F with it, and the car's own unit can arrive. */
static void units_sw(const dash_state_t *s)
{
	lv_obj_set_state(set_sw[9], LV_STATE_CHECKED, ST.unit_miles >= 0 ? ST.unit_miles : !s->kph);
	lv_obj_set_state(set_sw[10], LV_STATE_CHECKED, ST.unit_f >= 0 ? ST.unit_f : s->fahrenheit);
	lv_obj_set_state(set_sw[11], LV_STATE_CHECKED, ST.unit_psi >= 0 ? ST.unit_psi : s->psi);
}
void dash_ui_show(int v)
{
	if (v == DASH_VIEW_SETTINGS) save(DASH_SETTINGS_OPEN);
	bool was_pj = view == DASH_VIEW_PROJECT, now_pj = v == DASH_VIEW_PROJECT;
	if (v == DASH_VIEW_DASH && view != DASH_VIEW_DASH) dash_at = lv_tick_get();
	/* the phone layout's split has the buttons in its bar, and it comes on its own: a tap meant for the dash presses none */
	if (G.phone && v == DASH_VIEW_PROJECT && view != DASH_VIEW_PROJECT) dash_at = lv_tick_get();
	if (v == DASH_VIEW_CAR_PANEL && view != DASH_VIEW_CAR_PANEL) panel_at = lv_tick_get(), panel_note[0] = 0;
	if (v != view) overlays_close(); /* a menu or a dialog belongs to the view it was opened on (Android Auto comes on its own) */
	if (v == DASH_VIEW_NETWORK && view != DASH_VIEW_NETWORK) edit_at = lv_tick_get(); /* the tap that opened it: no key */
	if (v == DASH_VIEW_SETTINGS && (view == DASH_VIEW_NETWORK || view == DASH_VIEW_PAIR)) {
		set_back_at = lv_tick_get();
		if (set_guard) lv_obj_set_clickable(set_guard, true);
	}
	view = v;
	/* The real ring (and the assist disc and the air outside it carries in the full dash) is reused in the strip:
	 * it lives over the full-screen project overlay while projecting, and back on the dash otherwise. */
	if (now_pj != was_pj) {
		lv_obj_t *to = now_pj ? project : root;
		if (G.phone && !now_pj) pj_parts(false);
		lv_obj_set_parent(ring, to);
		lv_obj_set_parent(ap_disc, to);
		lv_obj_set_parent(outside_l, to);
		lv_obj_set_parent(link_w.box, to); /* the Wi-Fi / Bluetooth status: kept in the split, in the left strip */
		lv_obj_set_parent(link_b.box, to);
		/* the edge glows (blind spot, indicators) too, under everything. The left one: the decoder draws the picture, so
		 * nothing of LVGL's may be repainted over it. The Dash: at the dash's side's left edge, beside the picture (the
		 * sliver left of the picture is under the housing's slant). The phone layout, whose picture reaches the screen's
		 * left edge: still on that edge: LVGL's over the bar above the picture, the decoder's over the picture itself
		 * (pj_glow) */
		lv_obj_set_parent(glow_l, to), lv_obj_set_parent(glow_r, to);
		lv_obj_move_to_index(glow_r, 0), lv_obj_move_to_index(glow_l, 0);
		if (G.phone) {
			if (now_pj) lv_obj_set_size(glow_l, glow_dsc[0].header.w, PJ_RGN_Y); /* (cut, its bright edge kept: top left) */
			else lv_obj_set_size(glow_l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		} else {
			lv_obj_set_x(glow_l, now_pj ? ri(pj_vw()) : 0);
		}
		if (G.phone && now_pj) pj_parts(true); /* the cards' parts */
		if (!now_pj) lv_obj_move_foreground(tip), lv_obj_move_foreground(snack); /* toasts stay above the moved ring */
		/* the decoder draws straight to the framebuffer: LVGL would leave its last picture on screen, so repaint all
		 * (whole, where the port can and the session's decoder has gone: its memory is what the whole frame takes) */
		if (!now_pj && !projecting && dash_ui_whole_frame) dash_ui_whole_frame();
		lv_obj_invalidate(lv_screen_active());
		lv_obj_invalidate(lv_layer_top());
		if (!now_pj) lv_timer_create(pj_repaint_cb, 600, NULL);
		if (now_pj) close(open(PJ_VISIBLE, O_CREAT | O_WRONLY | O_CLOEXEC, 0644)); /* the AA receiver sends touches */
		else unlink(PJ_VISIBLE);
	}
	bool reveal = v == DASH_VIEW_SETTINGS && lv_obj_is_hidden(settings); /* the views' own redraws: settings_reveal's */
	if (reveal) lv_display_enable_invalidation(lv_obj_get_display(settings), false);
	show(panel, v == DASH_VIEW_CAR_PANEL);
	show(settings, v == DASH_VIEW_SETTINGS);
	if (v == DASH_VIEW_SETTINGS) lv_obj_move_foreground(settings); /* above all the dash's own (the screen's) */
	/* The opaque pages live on the screen too, the one shown its topmost child: LVGL starts a redraw at the topmost of
	 * the screen's objects that covers it, so a page there is drawn alone, where on the top layer the screen beneath
	 * was painted first (as much again as the page, for every redraw: opening it, each key typed) */
	lv_obj_t *page = v == DASH_VIEW_CAR_PANEL ? panel : v == DASH_VIEW_NETWORK ? net_edit : v == DASH_VIEW_PAIR ? pair_view : NULL;
	if (v == DASH_VIEW_PROJECT) page = project;
	if (page) lv_obj_move_foreground(page);
	show(net_edit, v == DASH_VIEW_NETWORK);
	show(pair_view, v == DASH_VIEW_PAIR);
	show(project, v == DASH_VIEW_PROJECT);
	/* under the split and the opaque pages the dash is hidden: else each change in the strip or the page draws the whole
	 * dash beneath it too (the settings' barrier is see-through: drawn there) */
	show(root, v == DASH_VIEW_DASH || v == DASH_VIEW_SETTINGS);
	if (reveal) lv_display_enable_invalidation(lv_obj_get_display(settings), true), settings_reveal();
	show(tip, false);
	lv_obj_t *sp = v == DASH_VIEW_SETTINGS ? lv_layer_top() : root; /* the snackbar (a failed save) over the settings too */
	if (now_pj) sp = project; /* the split: on the dash's side, off the picture (set_snack) */
	if (lv_obj_get_parent(snack) != sp) lv_obj_set_parent(snack, sp);
	if (sp != root || !now_pj) lv_obj_move_foreground(snack);
	if (v == DASH_VIEW_SETTINGS) {
		lv_obj_set_state(set_sw[0], LV_STATE_CHECKED, ST.minor_figures);
		lv_obj_set_state(set_sw[1], LV_STATE_CHECKED, !ST.hour12);
		lv_obj_set_state(set_sw[2], LV_STATE_CHECKED, ST.dev_ssh);
		lv_obj_set_state(set_sw[3], LV_STATE_CHECKED, ST.follow_car);
		if (set_sw[4]) lv_obj_set_state(set_sw[4], LV_STATE_CHECKED, ST.record);
		lv_obj_set_state(set_sw[5], LV_STATE_CHECKED, ST.sparse);
		lv_obj_set_state(set_sw[6], LV_STATE_CHECKED, ST.wifi_first);
		lv_obj_set_state(set_sw[7], LV_STATE_CHECKED, ST.ota_auto);
		lv_obj_set_state(set_sw[8], LV_STATE_CHECKED, ST.aa_low_res);
		lv_obj_set_state(set_sw[12], LV_STATE_CHECKED, ST.show_fps);
		lv_obj_set_state(set_sw[13], LV_STATE_CHECKED, ST.save_logs);
		/* the units as shown now, until switched (panda.dart's toggles) */
		units_sw(&S);
		forget_reset(&fg_cmdr, true), forget_reset(&fg_aa, true), forget_reset(&fg_restart, true);
		for (int i = 0; i < 6; i++) { /* the chosen car filled, as a selected chip */
			fill(set_car_btn[i], i == ST.car ? 0xFF4A4458u : 0x00000000u);
			border(set_car_btn[i], i == ST.car ? 0xFFD0BCFFu : W70, PB(1));
		}
		lv_slider_set_value(set_slider, ST.follow_car && S.backlight_pct >= 0 ? S.backlight_pct : ST.brightness_pct, LV_ANIM_OFF);
		lv_slider_set_value(set_offset, ST.follow_offset, LV_ANIM_OFF);
		offset_note();
		slider_look();
		settings_fill_nets();
		if (set_tab == 3) save(DASH_NET_SCAN); /* opened on the Network tab: a fresh Nearby list */
		tz_look();
		set_text(set_host_l, "Panda: %s:%d", ST.panda_host, ST.panda_port);
	}
	if (v == DASH_VIEW_NETWORK) {
		bool host = edit_slot == HOST_SLOT; /* one field: the Panda's address */
		show(net_ta[1], !host);
		lv_textarea_set_max_length(net_ta[0], host ? 70 : 32);
		lv_textarea_set_placeholder_text(net_ta[0], host ? "Panda host[:port]" : "Network name (SSID)");
		if (host) {
			char hp[80];
			snprintf(hp, sizeof hp, "%s:%d", ST.panda_host, ST.panda_port);
			lv_textarea_set_text(net_ta[0], hp);
			set_text(net_err, "");
			lv_obj_send_event(net_ta[0], LV_EVENT_CLICKED, NULL);
			kb_set_mode(0);
			return;
		}
		if (edit_slot < 0 || edit_slot > ST.net_count || edit_slot >= DASH_NETS) edit_slot = ST.net_count;
		bool pick = edit_slot == ST.net_count && pick_ssid[0];
		lv_textarea_set_text(net_ta[0], edit_slot < ST.net_count ? ST.net[edit_slot].ssid : pick ? pick_ssid : "");
		lv_textarea_set_text(net_ta[1], "");
		lv_textarea_set_placeholder_text(net_ta[1], edit_slot < ST.net_count ? "Passphrase (empty: keep)" : "Passphrase");
		set_text(net_err, "");
		lv_obj_send_event(net_ta[pick ? 1 : 0], LV_EVENT_CLICKED, NULL); /* the name, or for a pick its passphrase */
		kb_set_mode(0);
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
	pair_view = box(lv_screen_active()); /* a page (pages_top) */
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
static float side_l, side_r; /* the Dash's layout: the car's side, from the left chips to the ring (the toasts' place) */
static void set_snack(const dash_state_t *s)
{
	bool approving = s->bt == BT_APPROVING; /* the one step that needs a hand: said until it is done */
	/* (the car panel says both in its own line, and the settings' Commander tab the step, where the prompt hid its foot) */
	approving &= !(view == DASH_VIEW_SETTINGS && set_tab == 0);
	bool say = (approving || s->toast[0]) && view != DASH_VIEW_CAR_PANEL;
	if (!say) {
		show(snack, false);
		return;
	}
	/* a tap on a toast stops there (the app's SnackBar); the prompt, up for a minute, lets it through: in basic the
	 * links and the buttons of the foot are under it */
	lv_obj_set_clickable(snack, !approving);
	bool wheel = approving && !strip(); /* (the strip has no room for the picture) */
	const char *t = !approving ? s->toast : strip() ? "Double-press the right scroll wheel"
			: "Approve this " UNIT " for the Commander: in the car, double-press the right scroll wheel";
	/* the strip's is one line across its foot, a long text cut short: the ring's own foot (the cruise line) is just above */
	lv_label_long_mode_t lm = strip() ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP;
	bool reshape = wheel == lv_obj_is_hidden(snack_wheel) || lv_label_get_long_mode(snack_text) != lm;
	if (lv_label_get_long_mode(snack_text) != lm) lv_label_set_long_mode(snack_text, lm); /* (set, it redraws the label) */
	reshape |= set_text(snack_text, "%s", t);
	/* out of sight while it takes its new shape and place: laid out where it stood, with the wheel or a longer text,
	 * it reached over the picture in a split, and that was painted */
	if (reshape) show(snack, false);
	show(snack_wheel, wheel);
	/* Its place, at the foot: the middle; on the Dash the car's side, clear of the ring's foot (the gear, the cruise
	 * line); in a split the dash's side, off the picture (the strip: left of Exit) */
	float lo = 0, hi = G.W, max = G.W * 0.42f;
	if (view == DASH_VIEW_PROJECT) lo = pj_vw() + PB(G.phone ? 12 : 6), hi = G.phone ? G.W - PB(12) : lv_obj_get_x(pj_exit) - PB(6);
	else if (view == DASH_VIEW_DASH && !G.phone && side_r > side_l) lo = side_l, hi = side_r;
	if (lo > 0) {
		lv_obj_update_layout(snack);
		max = fminf(max, hi - lo - (lv_obj_get_width(snack) - lv_obj_get_width(snack_text))); /* less the wheel and the padding */
	}
	lv_point_t sz;
	const lv_font_t *f = lv_obj_get_style_text_font(snack_text, 0);
	lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_height(snack_text, strip() ? lv_font_get_line_height(f) : LV_SIZE_CONTENT);
	lv_obj_set_width(snack_text, ri(strip() ? max : fminf(sz.x + 1, max)));
	lv_obj_update_layout(snack);
	align(snack, LV_ALIGN_BOTTOM_MID, ri((lo + hi - G.W) / 2), ri(-PB(6)));
	lv_obj_update_layout(snack);
	show(snack, true); /* once in its place: where it was may be the picture (a split) */
}

/* ---- the car's path either side, dotted (widgets.dart _CarLights, steerLines): under the car ---- */
static bool overlap(const lv_area_t *a, const lv_area_t *b)
{
	return a->x1 <= b->x2 && a->x2 >= b->x1 && a->y1 <= b->y2 && a->y2 >= b->y1;
}
/* The path lines' dots as last worked out, and what from: a box and an opacity each (a dot over a chip or the route card,
 * outlined, no fill when unlit, left out, as if passing under it), each line's bounding box (one box round both took in
 * the car's picture and its lights between them, redrawn each time the wheel turned: a quarter of a driving frame's
 * work; a line with no dot shown has an empty box) and a checksum of the dots (a dot that came out from under a chip or
 * the card changes it, not always a box). steer_draw runs for every part of the screen redrawn (its box is the whole
 * screen) and steer_dirty once a frame, all on the same dots. */
static struct {
	struct { bool on; char gear; int car; float deg, x, y, w, h; lv_area_t under[CHIPS + 1]; } key;
	struct { lv_area_t a; lv_opa_t opa; } dot[80]; /* (33 a line at most) */
	int n;
	lv_area_t box[2];
	uint32_t sum;
} steer_m;
/* The dots for [s] into steer_m (worked out again only when what they come from changed); false when there are none. */
static bool steer_dots(const dash_state_t *s)
{
	typeof(steer_m.key) k;
	memset(&k, 0, sizeof k); /* its padding too: compared whole */
	k.on = s->live && s->wifi_live && s->steer_lines && !isnan(s->steer_deg) && cab.h > 0; /* (a car drawn at least once) */
	k.gear = s->gear, k.car = s->car, k.deg = s->steer_deg, k.x = cab.x, k.y = cab.y, k.w = cab.w, k.h = cab.h;
	for (int i = 0; i <= CHIPS; i++) {
		lv_obj_t *b = i < CHIPS ? chips[i].box : dest_card;
		k.under[i] = (lv_area_t){INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN}; /* none: a box nothing overlaps */
		if (b && !lv_obj_is_hidden(b)) lv_obj_get_coords(b, &k.under[i]);
	}
	if (!memcmp(&k, &steer_m.key, sizeof k)) return k.on;
	steer_m.key = k, steer_m.n = 0, steer_m.sum = 0;
	if (!k.on) return false;
	const ui_car_pics_t *p = &ui_car_pics[s->car];
	float w = cab.w, h = cab.h, ox = cab.x, oy = cab.y;
	float road = fmaxf(-max_road[s->car], fminf(max_road[s->car], s->steer_deg / steer_ratio[s->car])) * (float)M_PI / 180;
	float wheelbase = h * (p->axles[1] - p->axles[0]);
	float r = wheelbase / tanf(fmaxf(fabsf(road), 1e-4f)), side = road > 0 ? -1 : 1;
	float cx = w * p->centre, ccx = cx + side * r, ccy = h * p->axles[1];
	bool rev = s->gear == 'R';
	float step = h * 0.022f, len = h * (rev ? 0.53f : 0.7f), dot = cb(1.6f);
	for (int sd = -1; sd <= 1; sd += 2) {
		lv_area_t bb = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
		float px0 = cx + sd * w * p->path_from[1], py0 = h * p->path_from[0];
		float rad = hypotf(px0 - ccx, py0 - ccy), a0 = atan2f(py0 - ccy, px0 - ccx);
#define AT(d, X, Y) do { float a_ = a0 + side * (d) / rad; X = ccx + cosf(a_) * rad, Y = ccy + sinf(a_) * rad; } while (0)
		float q0x, q0y, q1x, q1y, mx, my;
		AT(0, q0x, q0y);
		AT(len, q1x, q1y);
		AT(len / 2, mx, my);
		float chx = (q0x + q1x) / 2, chy = (q0y + q1y) / 2, ctx = chx + (mx - chx) * 6, cty = chy + (my - chy) * 6;
		for (float d = 0; d <= len && steer_m.n < (int)(sizeof steer_m.dot / sizeof steer_m.dot[0]); d += step) {
			float t = d / len, qx = q0x * (1 - t) * (1 - t) + ctx * 2 * t * (1 - t) + q1x * t * t,
			      qy = q0y * (1 - t) * (1 - t) + cty * 2 * t * (1 - t) + q1y * t * t;
			float x = ox + qx, y = oy + (rev ? h - qy : qy);
			lv_area_t a = {ri(x - dot), ri(y - dot), ri(x + dot) - 1, ri(y + dot) - 1};
			bool under = false;
			for (int i = 0; i <= CHIPS; i++) under |= overlap(&a, &k.under[i]);
			if (under) continue;
			steer_m.sum = steer_m.sum * 31 + (uint32_t)(a.x1 * 2048 + a.y1);
			steer_m.dot[steer_m.n].a = a, steer_m.dot[steer_m.n++].opa = (lv_opa_t)(0.8f * 255 * (1 - d / len));
			bb.x1 = LV_MIN(bb.x1, a.x1), bb.y1 = LV_MIN(bb.y1, a.y1), bb.x2 = LV_MAX(bb.x2, a.x2), bb.y2 = LV_MAX(bb.y2, a.y2);
		}
		steer_m.box[sd > 0] = bb;
	}
	return true;
}
static lv_area_t steer_drawn[2]; /* the dots as last drawn (steer_on: any), whatever came between (the split, a page) */
static uint32_t steer_sum;
static bool steer_on;
static void steer_draw(lv_event_t *e) /* only the dots in the part being redrawn */
{
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!(steer_on = steer_dots(&S))) return;
	memcpy(steer_drawn, steer_m.box, sizeof steer_drawn), steer_sum = steer_m.sum;
	lv_draw_rect_dsc_t rd;
	lv_draw_rect_dsc_init(&rd);
	rd.radius = LV_RADIUS_CIRCLE;
	rd.bg_color = rgb(WHITE);
	for (int i = 0; i < steer_m.n; i++)
		if (overlap(&steer_m.dot[i].a, &layer->_clip_area)) rd.bg_opa = steer_m.dot[i].opa, lv_draw_rect(layer, &rd, &steer_m.dot[i].a);
}
/* Each frame: where the dots are drawn and where they go now are repainted when they differ (or CHANGED says so), each
 * line on its own. */
static void steer_dirty(const dash_state_t *s, bool changed)
{
	bool has = steer_dots(s);
	const lv_area_t *a = steer_m.box;
	if (!changed && has == steer_on && (!has || (!memcmp(a, steer_drawn, sizeof steer_drawn) && steer_m.sum == steer_sum))) return;
	for (int i = 0; i < 2; i++) {
		if (steer_on && steer_drawn[i].x1 <= steer_drawn[i].x2) lv_obj_invalidate_area(steer, &steer_drawn[i]);
		if (has && a[i].x1 <= a[i].x2) lv_obj_invalidate_area(steer, &a[i]);
	}
}

/* The link icons (main.dart _btStatus): WiFi, its settings; Bluetooth, try the link now (as Approve does), then its
 * settings, which say what it is doing. Nothing while the approval prompt is up: it already says it all. */
static bool bt_ready(dash_bt_t b);
/* The phone layout: the gear's tap switches between the full and basic layouts (main.dart: "Tap the gear under the speed
 * to switch"); in basic a swipe up or down on a slot brings its next or previous card (_Cards), passing over those the
 * other slots show (and so does one to the left or the right that begins on the slot), and in the split on its one
 * card the next or previous of five (the buttons are in its bar); on the ring (basic's, the split's) its other face
 * (_Pages); all kept in the settings. */
static void gear_tap(lv_event_t *e)
{
	(void)e;
	if (!G.phone || view != DASH_VIEW_DASH) return;
	ST.basic = !ST.basic;
	save(DASH_SETTINGS_SAVE);
}
/* LVGL hands a swipe to the first object up from the finger that does not pass it on: the dash's root (basic) and the
 * split's cover stop it (the picture's own box keeps those that start on it: the phone's). Where the finger is decides
 * (a vertical swipe keeps its column). Any swipe lets go of the press it began with: a swipe that starts on a chip (a
 * card's, the split's bar's) and ends on it never presses it, nor one from the gear the layout. */
/* Where the finger went down. LVGL's own sum of a gesture starts again whenever a read moves under 3 px, so a swipe
 * that begins slowly was taken to begin where it sped up: one from basic's bar, drifting up onto the ring, turned
 * the ring's face. Kept from the panel's press itself (every pointer device gets the handler, whenever it appears). */
static lv_point_t press_at;
static bool press_known;
static void press_cb(lv_event_t *e)
{
	(void)e;
	lv_indev_t *in = lv_indev_active();
	if (in) lv_indev_get_point(in, &press_at), press_known = true;
}
static void press_hook(void)
{
	static lv_indev_t *hooked[4];
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		int k = 0;
		while (k < 4 && hooked[k] && hooked[k] != i) k++;
		if (k == 4 || hooked[k] || lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) continue;
		hooked[k] = i;
		lv_indev_add_event_cb(i, press_cb, LV_EVENT_PRESSED, NULL);
	}
}
static int split_card(void);
static void swipe(lv_event_t *e)
{
	(void)e;
	bool pj = view == DASH_VIEW_PROJECT;
	if (!G.phone || !(pj || (ST.basic && view == DASH_VIEW_DASH))) return;
	lv_indev_t *in = lv_indev_active();
	lv_dir_t d = lv_indev_get_gesture_dir(in);
	lv_point_t p;
	lv_indev_get_point(in, &p);
	lv_indev_wait_release(in);
	bool across = d == LV_DIR_LEFT || d == LV_DIR_RIGHT; /* basic's cards only: the slot it began on, not where it has got to */
	lv_point_t from = press_known ? press_at : (lv_point_t){p.x - in->pointer.gesture_sum.x, p.y - in->pointer.gesture_sum.y};
	if (across) p.x = from.x;
	bool ring = pj ? p.x < split_x : p.x >= G.cx - G.D / 2 && p.x < G.cx + G.D / 2;
	/* basic's bar under the ring (the links, the car panel's button) is no part of the ring's pages (main.dart): a swipe
	 * that began there flips nothing (the cards either side run the screen's whole height) */
	if (ring && !pj && from.y >= G.row_b) return;
	if (across ? pj || ring : d != LV_DIR_TOP && d != LV_DIR_BOTTOM) return;
	if (pj && p.x < pj_vw()) return; /* not on the bar over the picture */
	int step = d == LV_DIR_TOP || d == LV_DIR_LEFT ? 1 : -1;
	if (ring) { /* its other face */
		ST.ring_page = ((ST.ring_page + step) % DASH_RING_PAGES + DASH_RING_PAGES) % DASH_RING_PAGES;
	} else if (pj) { /* the split's card: the next or the previous of its five in turn */
		int c = split_card();
		do c = (c + step + DASH_CARDS) % DASH_CARDS;
		while (c == DASH_CARD_BUTTONS);
		ST.split_card = c;
	} else { /* a slot of basic's: passing over the cards the other slots show */
		int i = 0;
		while (i < 4 && !(p.x >= slot_x[i] && p.x < slot_x[i + 1])) i++;
		if (i == 1 && p.x >= G.cx) i = 2; /* slot 1 runs on under the ring to slot 2's card: right of the ring is slot 2's */
		if (i == 4) return;
		int c = ST.cards[i];
		for (int k = 0; k < DASH_CARDS; k++) {
			c = (c + step + DASH_CARDS) % DASH_CARDS;
			bool used = false;
			for (int j = 0; j < 4; j++) used |= j != i && ST.cards[j] == c;
			if (!used) break;
		}
		ST.cards[i] = c;
	}
	save(DASH_SETTINGS_SAVE);
}
static void wifi_tap(lv_event_t *e)
{
	(void)e;
	if (view == DASH_VIEW_PROJECT) return; /* in the split the box is status only: settings would sit under the video */
	dash_ui_settings_tab(3);
	dash_ui_show(DASH_VIEW_SETTINGS);
}
static void bt_tap(lv_event_t *e)
{
	if (view == DASH_VIEW_PROJECT || S.bt == BT_APPROVING) return; /* status only in the split */
	if (!bt_ready(S.bt)) approve_tap(e);
	dash_ui_settings_tab(0);
	dash_ui_show(DASH_VIEW_SETTINGS);
}

static void touch_init(void)
{
	for (int i = 0; i < CHIPS; i++) {
		tappable(chips[i].box, chip_tap, i, (PC(48) - PC(42)) / 2);
		lv_obj_add_event_cb(chips[i].box, chip_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(chips[i].box, chip_hold, LV_EVENT_RELEASED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(chips[i].box, chip_hold, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
		pressed_look(chips[i].box, 0x4DFFFFFF);
	}
	tappable(link_w.box, wifi_tap, 0, PL(4)); /* the app's link icons open its settings */
	tappable(link_b.box, bt_tap, 0, PL(4));
	lv_obj_set_clickable(ap_disc, true);
	lv_obj_add_event_cb(ap_disc, ap_hold, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_add_event_cb(ap_disc, ap_hold, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(ap_disc, ap_hold, LV_EVENT_PRESS_LOST, NULL);
	tappable(car_panel, open_tap, DASH_VIEW_CAR_PANEL, PL(12));
	if (G.phone) {
		tappable(gear_l, gear_tap, 0, PR(16));
		lv_obj_set_clickable(root, true); /* a swipe that starts on no button reaches it */
		lv_obj_set_gesture_bubble(root, false); /* and stops here (passed on, LVGL gives it to the screen) */
		lv_obj_add_event_cb(root, swipe, LV_EVENT_GESTURE, NULL);
	}
	tappable(aa_btn, aa_tap, 0, PL(8)); /* half the gap to the car icon: AA sits on top, so it must not reach past it */
	if (x_btn) tappable(x_btn, x_tap, 0, PL(8)); /* (and half the gap to AA) */
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
	project_build();
	panel_build();
	settings_build();
	set_guard = box(settings); /* over everything in the settings (their last child), drawing nothing: it takes */
	place(set_guard, 0, 0, G.W, G.H); /* the touches only while clickable (see set_back_at): no repaint to turn it */
	lv_obj_set_clickable(set_guard, false);
	net_edit_build();
	pair_build();
}

/* The approval, step by step (the one thing that needs a hand in the car): what to do now, said when it is time */
static bool bt_ready(dash_bt_t b) { return b == BT_BUTTONS || b == BT_LEAN || b == BT_FULL || b == BT_BORROWED; }
/* The tap's bookkeeping, every frame (the settings shut or not): done once linked; a refusal counts only once the
 * state has moved since the tap (the one shown at the tap is the last attempt's); given up after 30 s unasked. */
static void approval_track(dash_bt_t b)
{
	if (!approve_asked) return;
	if (b != BT_REFUSED) approve_moved = true;
	if (bt_ready(b) || (b == BT_REFUSED && approve_moved) || (b != BT_APPROVING && lv_tick_elaps(approve_at) > 30000))
		approve_asked = false;
}
static void set_approval(dash_bt_t b)
{
	const char *t;
	uint32_t c = W70;
	bool ready = bt_ready(b), press = b == BT_APPROVING;
	if (ready) t = "Approved: this " UNIT " is linked to the Commander.", c = REGEN;
	else if (press) t = "Now, in the car: double-press the right scroll wheel.", c = WARN;
	else if (approve_asked && b == BT_PAUSED) t = "Waiting for the car's WiFi stream first (Advanced: Bluetooth waits for WiFi).", c = ACCENT;
	else if (approve_asked && b == BT_AWAY) t = "Looking for the Commander: is the car awake? A phone's S3XY app may hold it: close it.", c = ACCENT;
	else if (approve_asked) t = "Connecting to the Commander… keep the car awake.\nNext: the scroll wheel, when this says so.", c = ACCENT;
	else if (b == BT_REFUSED) t = "Not approved (refused, or no press in time). Tap Approve to try again.", c = ALERT;
	else if (b == BT_OFF) t = "Bluetooth is off on this " UNIT ".", c = ALERT;
	else if (b == BT_AWAY) t = "The Commander is not in reach (car asleep, or a phone's S3XY app holds it).";
	else t = "1. Tap Approve this " UNIT ".\n2. When this says so, double-press the right scroll wheel.";
	set_text(set_step, "%s", t);
	text_color(set_step, c);
	SET_NUM(set_wheel, lv_obj_get_style_image_opa, lv_obj_set_style_image_opa, ready ? LV_OPA_40 : LV_OPA_COVER); /* lit until linked */
}

static void set_touch(const dash_state_t *s)
{
	approval_track(s->bt);
	set_snack(s);
	set_pair(s);
	if (view == DASH_VIEW_CAR_PANEL) { /* its message line: '<label>…' while out, then what came of it */
		int a = s->press_pending;
		bool waiting = strstr(s->toast, "waiting for") != NULL; /* a second tap dropped behind it: say that */
		if (a >= 0 || (s->toast[0] && strcmp(s->toast, note_toast))) panel_note[0] = 0; /* a later press or newer words */
		if (s->bt == BT_APPROVING) set_text(panel_msg, "To approve this " UNIT ": in the car, double-press the right scroll wheel");
		else if (!waiting && a >= ACT_COUNT && cmdr_action_at(a)) set_text(panel_msg, "%s…", cmdr_action_at(a)->label);
		else set_text(panel_msg, "%s", s->toast[0] ? s->toast : panel_note);
	}
	if (view == DASH_VIEW_SETTINGS && ST.follow_car && s->backlight_pct >= 0 && CHANGED(backlight_pct))
		lv_slider_set_value(set_slider, s->backlight_pct, LV_ANIM_OFF); /* the live value, greyed */
	if (view == DASH_VIEW_SETTINGS) {
		units_sw(s);
		ota_refresh(false);
		uint32_t c;
		const char *w = bt_word(s->bt, &c, NULL);
		set_text(set_status, "Commander: %s", w);
		text_color(set_status, c == W54 ? W70 : c);
		set_text(set_bt_line, "%s", s->bt_line);
		if (set_wifi_line) set_text(set_wifi_line, "%s", s->wifi_line);
		/* a second tap's window over: the label again */
		forget_reset(&fg_cmdr, false), forget_reset(&fg_aa, false), forget_reset(&fg_restart, false);
		if (set_guard && lv_obj_is_clickable(set_guard) && !FRESH(set_back_at, 500)) lv_obj_set_clickable(set_guard, false);
		net_delete_reset(false);
		bool join_over = joining[0] && lv_tick_elaps(joining_at) >= 20000; /* a join that did not take: Connect again */
		if (join_over) joining[0] = 0;
		if (net_list && view == DASH_VIEW_SETTINGS && (join_over || CHANGED(nearby_n) || CHANGED(nearby) || CHANGED(net_ssid)))
			settings_fill_nets(); /* a network came or went, or the joined one changed */
		if (set_net_line) set_text(set_net_line, "%s", s->net_line);
		set_approval(s->bt);
	}
}

/* ---------- the phone layout's basic layout (main.dart _basicRow): the ring between cards ----------
 * The F133's wide screen takes two cards each side, each slot half its side wide, the screen's height; the
 * ring as tall as the row, the bar under it as wide as it (the links, the car panel and AA buttons). A card is the
 * dash's own parts in its slot: the battery (the dial over the figures), the drive card (the lamps over the g-meter's
 * ball, as big as fits, its figures under it), the car (the cabin, the lamps over it while no drive card has them),
 * the buttons (the ten chips in two columns), the route (the Dash's column), the power (the pack's power round a
 * circle). A part shows in one place at most; the full layout gets them all back. The split shows one card. */
static void cards_parts(bool basic)
{
	bool on[DASH_CARDS];
	for (int c = 0; c < DASH_CARDS; c++) on[c] = !basic || card_slot[c] >= 0;
	show(dial, on[DASH_CARD_ENERGY]), show(stats_box, on[DASH_CARD_ENERGY]);
	if (!on[DASH_CARD_ENERGY]) show(charge_box, false);
	show(cab.grp, on[DASH_CARD_CAR]), show(steer, on[DASH_CARD_CAR]);
	if (!on[DASH_CARD_CAR]) show(cab.frame, false);
	show(lights_row, on[DASH_CARD_CAR] || on[DASH_CARD_G]);
	if (!on[DASH_CARD_ROUTE]) show(dest_card, false);
	show(no_route, false); /* (basic's route card shows it again: card_set) */
	if (!on[DASH_CARD_G]) {
		lv_obj_t *g[] = {gball, g_val, g_peak, brake_temps, pedal, pedal_name, pedal_val, brake_car};
		for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) show(g[i], false);
	}
	show(pdial, basic && on[DASH_CARD_POWER]); /* not the full layout's */
	for (int i = 0; i < CHIPS; i++) {
		float x, y;
		show(chips[i].box, on[DASH_CARD_BUTTONS]);
		if (!basic) chip_geom(i, &x, &y), lv_obj_set_pos(chips[i].box, ri(x), ri(y));
	}
	for (int i = 0; i < 4; i++) /* each slot's dots; the split's card the right one's, the buttons (in its bar) not one */
		for (int c = 0; c < DASH_CARDS; c++)
			show(dots[i][c], basic && (view != DASH_VIEW_PROJECT || (i == SPLIT_SLOT && c != DASH_CARD_BUTTONS)));
	if (!basic) { /* the buttons back in the bar's corner */
		lv_obj_set_pos(car_panel, car_at[0], car_at[1]);
		aa_place(car_at[0], car_at[1]);
	}
	lv_obj_invalidate(root);
}
/* A slot's row of dots at its foot, [cx] its middle, card [lit] lit, card [skip] not in its round (-1 none: all are).
 * The app's two stacks (widgets.dart _Cards) have theirs in a column at the card's outer edge; the four slots side by
 * side here have no edge of their own, hence the row under each. */
static void dots_place(int i, float cx, int lit, int skip)
{
	int n = DASH_CARDS - (skip >= 0);
	for (int c = 0, k = 0; c < DASH_CARDS; c++) {
		if (c == skip) continue;
		float d = px(6), x = cx + (k++ - (n - 1) / 2.0f) * px(14) - d / 2;
		place(dots[i][c], x, G.H - G.mb / 2 - d / 2, d, d);
		fill(dots[i][c], c == lit ? W70 : W24);
	}
}
/* The power card's figure (_PowerDial): the pack's own power over WiFi, else the motors' (Bluetooth alone); NAN none. */
static float pack_now(const dash_state_t *s)
{
	float kw = s->wifi_live && !isnan(s->pack_kw) ? s->pack_kw
		 : s->live && !isnan(s->rear_kw) ? (isnan(s->front_kw) ? 0 : s->front_kw) + s->rear_kw : NAN;
	return isnan(kw) || fabsf(kw) >= 0.05f ? kw : 0; /* never -0.0 */
}
/* The power card: the dial as big as the card (its circle 6 dp in), the figure (at a 1.0 line) over its unit in the
 * middle, the range's foot 0.86 of the radius down; the marks redrawn only where they moved (the power over the top,
 * in half degrees; the battery round the foot). */
static void pdial_set(const dash_state_t *s, float cx, float w)
{
	float side = fminf(w - px(16), G.H - G.mt - G.mb), r = side / 2 - px(6), x0 = cx - side / 2, top = G.H / 2 - side / 2;
	place(pdial, x0, top, side, side);
	/* basic's dial is small (two cards a side): its figure 1.3x, its words no smaller than the cards' small print */
	bool basic = ST.basic && view != DASH_VIEW_PROJECT;
	float kw = pack_now(s), tk = basic ? 1.3f : 1, wmin = basic ? px(14) : 0;
	const lv_font_t *fk = font_px(BOLD, ri(r * 0.32f * tk)), *fu = font_px(REGULAR, ri(fmaxf(r * 0.11f, wmin))),
			*fr = font_px(REGULAR, ri(fmaxf(r * 0.12f, wmin + px(2))));
	figure_font(pdial_kw, fk);
	if (lv_obj_get_style_text_font(pdial_unit, 0) != fu) lv_obj_set_style_text_font(pdial_unit, fu, 0);
	if (lv_obj_get_style_text_font(pdial_range, 0) != fr) lv_obj_set_style_text_font(pdial_range, fr, 0);
	if (isnan(kw)) set_text(pdial_kw, "—");
	else set_text(pdial_kw, "%.1f", kw);
	text_color(pdial_kw, isnan(kw) ? W38 : kw < 0 ? REGEN : WHITE);
	float kh = ri(r * 0.32f * tk), y = side / 2 - (kh + lv_font_get_line_height(fu)) / 2;
	put_text(pdial_kw, 1, side / 2, y + baseline_in(fk, 1), side);
	put_text(pdial_unit, 1, side / 2, y + kh + ascent(fu), side);
	if (!s->live || isnan(s->range)) set_text(pdial_range, "—");
	else set_text(pdial_range, "%d %s", (int)lroundf(s->range), s->kph ? "km" : "mi");
	put_text(pdial_range, 1, side / 2, side / 2 + r * 0.86f - px(basic ? 5 : 0) - lv_font_get_line_height(fr) + ascent(fr), side);
	float p = isnan(kw) ? NAN : roundf(fmaxf(-1, fminf(1, kw / PD_ENDS)) * PD_REACH * 2) / (PD_REACH * 2);
	int soc = s->live && !isnan(s->soc_pct) ? (int)lroundf(fmaxf(0, fminf(100, s->soc_pct))) : -1;
	if (isnan(p) ? !isnan(pd_p) : p != pd_p) {
		lv_area_t a = {ri(x0) - 2, ri(top) - 2, ri(x0 + side) + 2, ri(top + side / 2 + r * 0.62f) + 2};
		/* The power moved: only the degrees between where its end was and is (the fill's end, the dash with its
		 * shadow and tip), as the ring's and the battery dial's; a step is 1.25 kW, so most driving frames, and the
		 * whole upper dial was redrawn for each. Known from unknown, or back: all of it (the track's box, down to 30°
		 * under the level). */
		if (!isnan(p) && !isnan(pd_p)) {
			float a0 = 270 + PD_REACH * fminf(p, pd_p), a1 = 270 + PD_REACH * fmaxf(p, pd_p);
			float m = px(9) / (r * 0.74f) * 57.3f + 1; /* the dash's half width and its shadow, as an angle at its inner end */
			arc_box(ri(x0 + side / 2), ri(top + side / 2), r * 0.74f - px(4), r * 1.02f + px(6), (int)floorf(a0 - m), (int)ceilf(a1 + m), &a);
		}
		pd_p = p;
		lv_obj_invalidate_area(pdial, &a);
	}
	if (soc != pd_soc) { /* the battery's, from 40° under the level */
		pd_soc = soc;
		lv_area_t a = {ri(x0) - 2, ri(top + side / 2 + r * 0.55f), ri(x0 + side) + 2, ri(top + side) + 2};
		lv_obj_invalidate_area(pdial, &a);
	}
}
/* Card [c] in its place: [cx] its middle, [w] its width, the screen's height (basic's slots, the split's card). */
static void card_set(const dash_state_t *s, int c, float cx, float w)
{
	switch (c) {
	case DASH_CARD_ENERGY:
		set_energy(s, cx);
		break;
	case DASH_CARD_CAR: { /* the lights 5 dp down (unless the drive card has them, as the app's: its car card is the car
			       * alone), the cabin fitted to the rest, a little lower (the full layout's) */
		float top = G.mt + px(5), lh = PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS));
		if (card_slot[DASH_CARD_G] < 0) set_lights(s, cx, top, w + (view == DASH_VIEW_PROJECT ? 0 : 2 * px(20))); /* basic: the slot's whole width */
		float bt = top + lh + PLI(4), bb = G.H - G.mb - px(5), ch = cb(8) + ui_car_pics[s->car].car->header.h + cb(10);
		set_cabin(s, cx, (bt + bb - ch) / 2 + 0.07f * ch);
		break;
	}
	case DASH_CARD_BUTTONS: /* the two columns, 16 dp apart, the rows spread over the height */
		for (int k = 0; k < CHIPS; k++) {
			float gap = (G.H - G.mt - G.mb - CHIP_SIDE * PC(42)) / (CHIP_SIDE + 1);
			float x = cx + (k < CHIP_SIDE ? -PC(8) - PC(52) : PC(8)), y = G.mt + gap + (k % CHIP_SIDE) * (PC(42) + gap);
			lv_obj_set_pos(chips[k].box, ri(x), ri(y));
		}
		break;
	case DASH_CARD_ROUTE: /* no route: the app's NO ROUTE in the card's middle, not an empty card */
		show(dest_card, s->route);
		show(no_route, !s->route);
		if (s->route) set_route(s, cx - G.card_w / 2, G.H / 2);
		else place(no_route, cx - w / 2, G.H / 2 - lv_font_get_line_height(lv_obj_get_style_text_font(no_route, 0)) / 2.0f, w,
			   lv_font_get_line_height(lv_obj_get_style_text_font(no_route, 0)));
		break;
	case DASH_CARD_G: { /* the app's drive card: the ball's square as big as the card leaves, the lamps 4 dp over it, the
			     * ball 5 dp in it (its circle the power card's), the figures 6 dp under it; Bluetooth alone the
			     * brakes and the pedal bar in its place (pedal_brakes) */
		float lh = PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS)), fh = PG(48);
		float side = fminf(w - px(16), G.H - G.mt - G.mb - 2 * (lh + px(8)) - fh), top = G.H / 2 - side / 2;
		set_lights(s, cx, top - lh - px(4), w + (view == DASH_VIEW_PROJECT ? 0 : 2 * px(20)));
		pedal_brakes(s, cx, 0, G.H, true, side);
		if (g_figures(s, true)) {
			place(gball, cx - side / 2 + px(5), top + px(5), side - px(10), side - px(10));
			lv_point_t pk;
			const lv_font_t *pf = lv_obj_get_style_text_font(g_peak, 0);
			lv_text_get_size(&pk, lv_label_get_text(g_peak), pf, lv_obj_get_style_text_letter_space(g_peak, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			float vw = lv_obj_get_style_width(g_val, 0), vh = lv_obj_get_style_height(g_val, 0), x = cx - (vw + PG(12) + pk.x) / 2;
			float y = top + side + px(6);
			lv_obj_set_pos(g_val, ri(x), ri(y));
			align(g_peak, LV_ALIGN_TOP_LEFT, ri(x + vw + PG(12)), ri(y + vh - ascent(pf) - pf->base_line + (vh - lv_font_get_line_height(pf)) * 0));
			gball_dirty(s);
		}
		break;
	}
	case DASH_CARD_POWER:
		pdial_set(s, cx, w);
		break;
	}
}
/* Each basic card's widest so far in this layout (its visible parts' extent, as drawn last frame): each side's two cards
 * are spread by it, the screen's edge, the gap between them and the ring as far apart, so a card as wide as the
 * battery's has the same room as its neighbour, and a figure growing moves nothing after it once has (reset with the
 * layout). */
static float card_wide[DASH_CARDS];
static float card_extent(int c)
{
	lv_obj_t *p[10] = {0};
	switch (c) {
	case DASH_CARD_ENERGY: p[0] = dial, p[1] = stats_box, p[2] = charge_box; break;
	case DASH_CARD_G: p[0] = gball, p[1] = g_val, p[2] = g_peak, p[3] = brake_temps, p[5] = pedal_name,
			  p[6] = pedal_val, p[7] = brake_car, p[8] = lights_row;
		if (lv_obj_get_width(pedal) < ri(G.W)) p[4] = pedal; /* (not as the line along the screen's top) */
		break;
	case DASH_CARD_CAR: /* the lights row too, when no drive card has it */
		p[0] = cab.grp, p[1] = cab.frame, p[2] = card_slot[DASH_CARD_G] < 0 ? lights_row : NULL; break;
	case DASH_CARD_BUTTONS: return 2 * PC(52) + PC(16); /* the two columns, 16 dp apart */
	case DASH_CARD_ROUTE: p[0] = dest_card, p[1] = no_route; break;
	case DASH_CARD_POWER: p[0] = pdial, p[1] = pdial_kw, p[2] = pdial_unit, p[3] = pdial_range; break;
	}
	int32_t x1 = INT32_MAX, x2 = INT32_MIN;
	for (int i = 0; i < 10; i++) {
		if (!p[i] || lv_obj_is_hidden(p[i])) continue;
		lv_area_t a;
		lv_obj_get_coords(p[i], &a);
		int32_t sc = lv_obj_get_style_transform_scale_x(p[i], 0), m = (a.x2 - a.x1) * (256 - sc) / 512; /* as drawn */
		a.x1 += m, a.x2 -= m;
		x1 = a.x1 < x1 ? a.x1 : x1, x2 = a.x2 > x2 ? a.x2 : x2;
	}
	return x2 > x1 ? (float)(x2 - x1 + 1) : 0;
}

static void basic_layout(const dash_state_t *s)
{
	int key = 1;
	for (int c = 0; c < DASH_CARDS; c++) card_slot[c] = -1;
	for (int i = 0; i < 4; i++) { /* a card in two slots shows in the first: the dash has one set of its parts */
		int c = ST.cards[i];
		if (c < 0 || c >= DASH_CARDS || card_slot[c] >= 0) c = -1;
		else card_slot[c] = i;
		key = key * 8 + c + 1;
	}
	if (key != layout_key) cards_parts(true), layout_key = key, memset(card_wide, 0, sizeof card_wide);
	/* the ring a tenth smaller than the row: two cards a side (main.dart's one) want the room */
	G.D *= 0.9f, G.r = G.D / 2 - px(6), G.aw = G.r * 0.085f;
	float rl = G.cx - G.D / 2 - px(8), rr = G.cx + G.D / 2 + px(8), lw = (rl - G.ml) / 2, rw = (G.W - G.ml - rr) / 2;
	float x0[4] = {G.ml, G.ml + lw, rr, rr + rw}, w[4] = {lw, lw, rw, rw}, mid[4];
	for (int pass = 0; pass < 2; pass++) { /* laid out, measured, and laid out again at once if a card grew */
		for (int side = 0; side < 2; side++) { /* the edge, the gap between the two and the ring as far apart */
			int a = 2 * side, b = a + 1;
			float wa = ST.cards[a] >= 0 && ST.cards[a] < DASH_CARDS && card_slot[ST.cards[a]] == a ? card_wide[ST.cards[a]] : 0;
			float wb = ST.cards[b] >= 0 && ST.cards[b] < DASH_CARDS && card_slot[ST.cards[b]] == b ? card_wide[ST.cards[b]] : 0;
			float l = x0[a], span = 2 * w[a], gap = (span - wa - wb) / 3;
			slot_x[a] = l, slot_x[b] = x0[b]; /* the swipes' split: the halves', or the gap's middle */
			if (!wa || !wb || gap < 0) mid[a] = l + w[a] / 2, mid[b] = l + 1.5f * w[a]; /* not yet measured, or no room: halves */
			else mid[a] = l + gap + wa / 2, mid[b] = l + 2 * gap + wa + wb / 2, slot_x[b] = l + 1.5f * gap + wa;
		}
		for (int i = 0; i < 4; i++) dots_place(i, mid[i], ST.cards[i], -1);
		slot_x[4] = G.W - G.ml;
		ring_place();
		set_chips(s);
		set_links(s);
		/* the car panel and AA under the ring's right end, as the links under its left */
		int car_x = ri(G.cx + G.D / 2 - PL(20) - PL(12));
		lv_obj_set_pos(car_panel, car_x, car_at[1]);
		aa_place(car_x, car_at[1]);
		set_ring(s);
		ring_dirty(s);
		for (int c = 0; c < DASH_CARDS; c++)
			if (card_slot[c] >= 0) card_set(s, c, mid[card_slot[c]], w[card_slot[c]] - 2 * px(20)); /* (its room: 20 dp in from each side) */
		lv_obj_update_layout(root);
		bool grew = false;
		for (int c = 0; c < DASH_CARDS; c++) {
			float e = card_slot[c] >= 0 ? card_extent(c) : 0;
			if (e > card_wide[c] + 0.5f) card_wide[c] = e, grew = true;
		}
		if (!grew) break;
	}
	if (card_slot[DASH_CARD_G] < 0) pedal_brakes(s, G.cx, 0, 0, false, 0); /* the line along the top, still */
}

/* The split's card: the one saved, the battery's if that is none of the round (the buttons are in the bar). */
static int split_card(void)
{
	int c = ST.split_card;
	return c >= 0 && c < DASH_CARDS && c != DASH_CARD_BUTTONS ? c : DASH_CARD_ENERGY;
}
/* The split's bar over the picture: the chips in two rows (a side's each) between the links (stacked at its left
 * end) and Exit, spread evenly; each row a chip's 48 dp finger room tall, the strip's half. The full layout's columns
 * become the rows: its left one's chips on top. */
static void bar_chips(void)
{
	float x0 = G.ml + PL(60), gap = (lv_obj_get_x(pj_exit) - x0 - CHIP_SIDE * PC(52)) / (CHIP_SIDE + 1);
	for (int k = 0; k < CHIPS; k++)
		lv_obj_set_pos(chips[k].box, ri(x0 + gap + (k % CHIP_SIDE) * (PC(52) + gap)),
			       ri(PJ_RGN_Y * (k / CHIP_SIDE * 2 + 1) / 4.0f - PC(21)));
}
/* The phone layout's split: the picture in the left half's lower 3/4; over it a bar, the links, the buttons and
 * Exit; the right half the basic layout's ring (as big as fits, the gear in its foot, swiped to its other face) and one
 * card right of it, swiped up or down to the next or previous of five (all but the buttons: nothing else shows one
 * here; the route is one, the bar has no room for its pill): the battery's at first. Every part on the split's cover
 * stays off the picture (the right half, the bar): the picture is the decoder's, never repainted (pj_parts). */
static void split_phone(const dash_state_t *s)
{
	float l = pj_vw() + px(8), r = G.W - G.ml - px(170); /* the card: 170 dp, the figures' widest row */
	G.D = fminf(fminf(G.H - G.mt - G.mb, r - l), G.H * 2 / 3); /* the ring smaller beside the picture */
	l += (r - l - G.D) / 2; /* centred in the room it leaves */
	G.r = G.D / 2 - px(6);
	G.aw = G.r * 0.085f;
	G.cx = l + G.D / 2;
	G.cy = G.mt + (G.H - G.mt - G.mb) / 2;
	ring_place();
	set_ring(s);
	ring_dirty(s);
	int c = split_card();
	for (int k = 0; k < DASH_CARDS; k++) card_slot[k] = k == c ? SPLIT_SLOT : k == DASH_CARD_BUTTONS ? BAR_SLOT : -1;
	if (-1 - c != layout_key) cards_parts(true), layout_key = -1 - c;
	bar_chips();
	set_chips(s);
	float x0 = G.cx + G.D / 2, cx = (x0 + G.W - G.ml) / 2;
	split_x = x0;
	dots_place(SPLIT_SLOT, cx, c, DASH_CARD_BUTTONS);
	card_set(s, c, cx, G.W - G.ml - x0);
}

/* The phone app's full layout (main.dart build): the chips' columns at the sides, the car's column (its lights over
 * it), the ring, the energy column (the dial, the figures, the g-meter), the two columns centred between the chips
 * and the ring as the app's two Expanded; the bar under them: the links, the route's pill, the buttons. */
static void phone_layout(const dash_state_t *s)
{
	if (layout_key) cards_parts(false), layout_key = 0;
	float cl = G.ml + PC(52), cr = G.W - G.ml - PC(52); /* the chips' inner edges */
	float car_cx = (cl + G.cx - G.D / 2) / 2, col_cx = (G.cx + G.D / 2 + cr) / 2;
	ring_place();
	set_chips(s);
	set_links(s);
	bool bt_set = s->bt == BT_LEAN || s->bt == BT_FULL; /* settled links shrink to their icons (set_link) */
	G.bar_l = G.ml + (s->wifi == WIFI_LIVE ? PL(36) : PL(60)) + (bt_set ? PL(36) : PL(60));
	G.bar_r = car_bar_r, G.bar_cy = G.H - G.mb - PL(22);
	bool route = s->route && !(s->wifi_live && s->charging); /* charging takes the route's place (the app's) */
	show(dest_card, route);
	if (route) set_route(s, 0, 0);
	/* the car's column: 5 dp in, the lights, 4 dp, the car fitted to the rest, then 7 % of its box lower */
	float top = G.mt + px(5), lights_h = PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS));
	set_lights(s, car_cx, top, G.cx - G.D / 2 - cl); /* the car's column, the chips to the ring */
	float box_t = top + lights_h + PLI(4), box_b = G.row_b - px(5);
	float ch = cb(8) + ui_car_pics[s->car].car->header.h + cb(10);
	set_cabin(s, car_cx, (box_t + box_b - ch) / 2 + 0.07f * ch);
	set_ring(s);
	ring_dirty(s);
	set_energy(s, col_cx);
	set_g_phone(s, col_cx);
}

void dash_ui_set(const dash_state_t *s)
{
	S = *s;
	press_hook();
	/* The AA/projection layer raises /run/dash-project when a phone's video is ready and drops it to leave. Toggle
	 * only between the dash and the strip, so it never yanks the user out of settings/pairing. */
	bool want_pj = projecting = access(dash_ui_pj_marker, F_OK) == 0;
	if (!want_pj || view != DASH_VIEW_PROJECT) pj_leaving = 0;
	if (pj_leaving && lv_tick_elaps(pj_leaving) > 1500) { /* the session has not gone: the dash anyway, as without */
		pj_leaving = 0, pj_left = true;
		dash_ui_show(DASH_VIEW_DASH);
	}
	if (!want_pj && pj_left) { /* after Exit the session is gone now: repaint whatever its decoder left on the dash */
		pj_left = false;
		lv_obj_invalidate(lv_screen_active());
		lv_obj_invalidate(lv_layer_top());
	}
	if (want_pj && !pj_left && view == DASH_VIEW_DASH) dash_ui_show(DASH_VIEW_PROJECT);
	else if (!want_pj && view == DASH_VIEW_PROJECT) dash_ui_show(DASH_VIEW_DASH);
	/* glows: indicators green in step with the flasher, blind spot red (also in the split); brake red at the foot */
	bool gl = s->live && (s->blind_l || s->ind_l), gr = s->live && (s->blind_r || s->ind_r);
	show(glow_l, gl);
	show(glow_r, gr);
	icon_color(glow_l, s->blind_l ? ALERT : REGEN);
	icon_color(glow_r, s->blind_r ? ALERT : REGEN);
	pj_glow(G.phone && view == DASH_VIEW_PROJECT && gl ? (s->blind_l ? ALERT : REGEN) : 0);
	if (view == DASH_VIEW_PROJECT) { /* the phone-map split: only the compact strip (the ring reused in it) */
		if (G.phone) {
			split_phone(s);
		} else {
			ring_geom_compact();
			set_ring(s);
			ring_dirty(s);
			set_project(s);
		}
		set_links(s);
		set_snack(s); /* a button's press: what came of it; the approval's step */
		if (G.phone && card_slot[DASH_CARD_CAR] >= 0) { /* the car's card: its path lines, as on the dash */
			lv_obj_update_layout(project);
			steer_dirty(s, CHANGED(steer_deg) || CHANGED(car) || CHANGED(gear));
		}
		P = *s;
		first = false;
		return;
	}
	show(glow_b, s->live && s->braking); /* not in the split: it would run under the picture */

	/* Android Auto button: only with a head-unit cert in place (else the phone rejects the handshake); green while the
	 * on-demand bring-up is up, so a tap tears it down rather than leaving the Dash advertising all the time. */
	bool aa_cert = access(dash_ui_aa_cert, F_OK) == 0 && access(PJ_AA_UNUSABLE, F_OK) != 0;
	show(aa_btn, aa_cert);
	if (x_btn) show(x_btn, true);
	if (aa_cert || x_btn) {   /* on: AP up (in its mode), a tap pending, or projecting */
		bool up = access(dash_ui_aa_up, F_OK) == 0, xup = up && extra_up();
		if ((up && xup == aa_pending_x) || (aa_pending && !aa_start_running())) aa_pending = false;
		/* (not while a start still holds its lock: Exit in a start's last moments finds no mark yet, and the start
		 * then keeps one until that stop has ended: the button showed on, and a tap sent a second stop) */
		if (aa_stopping && ((!up && access(aa_start_lock, F_OK) != 0) || lv_tick_elaps(aa_stop_at) > AA_STOP_MS)) aa_stopping = false;
		/* off: white, as the other icons; on: Android Auto's blue. Off at once on the tap that stops it (dash-aa stop
		 * takes a few seconds to take the AP and its processes down) */
		bool aa_on = aa_pending ? !aa_pending_x : !xup && (projecting || up); /* (a pending start: its button alone) */
		bool x_on = aa_pending ? aa_pending_x : xup;
		icon_color(aa_btn, !aa_stopping && aa_on ? AA_BLUE : WHITE);
		if (x_btn) icon_color(x_btn, !aa_stopping && x_on ? AA_BLUE : WHITE);
	}

	/* Under the settings' dialog the dash's middle (the ring, the car, the figures, the route, the g-meter) is left as
	 * it was drawn: each part of it that changed had the dialog over it drawn again, the same pixels, three quarters
	 * of a dash frame's work at every frame on the move. What shows beside and under the dialog goes on (the glows
	 * and the Android Auto button above, the chips, the links, the F133's pedal line), and so does the touch layer (the settings' own lines,
	 * the snackbar); the first frame after the dialog sets the dash all again. */
	static bool held;
	if (view == DASH_VIEW_SETTINGS && !first) {
		set_chips(s);
		set_links(s);
		if (G.phone) { /* the pedal's line along the top is above the dialog (Bluetooth's bar is under it: not kept) */
			if (s->wifi_live) pedal_brakes(s, G.cx, 0, 0, false, 0);
			else show(pedal, false);
		}
		set_touch(s);
		P = *s;
		held = true;
		return;
	}
	if (held) first = true, held = false, trail_n = 0; /* (no streak from the g reading the dialog opened on) */
	/* The blocks: the car and the route (only with one) left of the ring, the energy figures and the g-meter right of
	 * it. Widths are fixed per block (the widest it gets), so nothing moves as the figures change. A route coming or
	 * going re-spreads the left side only; losing WiFi drops the g-meter's place (with WiFi it is kept, in P too, even
	 * with nothing to show, as the app keeps it). */
	bool route = s->route;
	ring_geom_full(); /* the full-dash ring geometry first (the last frame may have been the split's small ring) */
	G.cx = G.W / 2;
	if (G.phone && ST.basic) {
		basic_layout(s);
	} else if (G.phone) {
		phone_layout(s);
	} else {
		float q = dial_q(), w[5], sum = 0;
		lv_point_t t1, t2;
		w[0] = ui_car_pics[DASH_CAR_Y7].car->header.w + 2 * cb(6);
		w[1] = route ? G.card_w : 0;
		w[2] = 2 * (G.r + G.aw / 2);
		lv_text_get_size(&t1, "88.8 kW", FS(REGULAR, 16, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_text_get_size(&t2, "35 min   12:51 AM", FS(REGULAR, 18, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		w[3] = fmaxf(104 * q, fmaxf(PF(104) + t1.x, t2.x));
		lv_text_get_size(&t1, "0.88 g", FS(REGULAR, 16, UI_S_FIG), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_text_get_size(&t2, "PEAK 0.88", FS(REGULAR, 12, UI_S_FIG), ri(PF(0.8)), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		w[4] = bt_brakes(s) ? brakes_w() /* Bluetooth alone: the brakes in the ball's place */
		       : s->wifi_live && !s->charging ? fmaxf(PG(72), fmaxf(t1.x, t2.x)) : 0; /* the ball over its figures, in P too */
		for (int i = 0; i < 5; i++) sum += w[i];
		/* The chips step in towards the top: bound by those level with the car picture (the first sits over it, beside
		 * the lights row) and with the g-meter's ball. */
		float car_top = G.mt + PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS)) + PLI(4) + cb(8);
		float chips_r = 0, chips_l = G.W;
		for (int i = 0; i < CHIPS; i++) {
			float cx, cy;
			chip_geom(i, &cx, &cy);
			if (i < CHIP_SIDE && cy + PC(42) > car_top) chips_r = fmaxf(chips_r, cx + PC(52));
			if (i == 0) chips_r = fmaxf(chips_r, G.ml + inset(G.link_top + PL(22)) + 2 * PL(60)); /* the links' row, widest */
			if (i >= CHIP_SIDE && cy < G.H / 2 + g_height() / 2 && cy + PC(42) > G.H / 2 - g_height() / 2) chips_l = fminf(chips_l, cx);
		}
		/* The ring fixed at the centre; each side's blocks spread evenly between it and the chips. */
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
		side_l = chips_r, side_r = G.cx - w[2] / 2;
		ring_place();

		set_chips(s);
		set_links(s);
		show(dest_card, route);
		if (route) set_route(s, at[1], G.H / 2);
		/* the lights row at the top, the car under it down to the foot */
		float car_cx = at[0] + w[0] / 2, c0x, c0y;
		chip_geom(0, &c0x, &c0y); /* the row's room: from the first chip, beside it, to the route card or the ring */
		set_lights(s, car_cx, G.mt, 2 * fminf(car_cx - (c0x + PC(52)), (route ? at[1] : G.cx - w[2] / 2) - car_cx) - px(8));
		set_cabin(s, car_cx, G.mt + PLI(20 + 2) + lv_font_get_line_height(FS(REGULAR, 12, UI_S_LIGHTS)) + PLI(4));

		set_ring(s);
		ring_dirty(s);
		set_energy(s, at[3] + w[3] / 2);
		set_g(s, at[4] + w[4] / 2);
	}
	set_touch(s);
	/* last, on this frame's layout (the car placed, the chips and the route card where the draw will find them) */
	lv_obj_update_layout(root);
	steer_dirty(s, CHANGED(steer_deg) || CHANGED(car) || CHANGED(gear));
	P = *s;
	first = false;
}

void dash_state_blank(dash_state_t *s)
{
	memset(s, 0, sizeof *s);
	s->kph = true;
	s->speed_hz = -1;
	s->speed = s->front_kw = s->rear_kw = s->pack_kw = s->pedal_pct = s->brake_pct = s->regen_cap_kw = NAN;
	for (int i = 0; i < 4; i++) s->brake_nm[i] = NAN;
	s->speed_limit = s->cruise_set = s->outside_c = s->charge_kw = s->charge_max_kw = s->wall_kw = s->wall_max_kw = NAN;
	s->follow_gap = s->charge_limit_pct = -1;
	s->charge_minutes = s->nav_minutes = s->co2_ppm = -1;
	s->soc_pct = s->range = s->pack_temp_c = s->precond_c = s->climate_kw = s->degradation_pct = NAN;
	s->minor_figures = true; /* the default on the Dash */
	s->lv_kw = s->other_kw = s->imbalance_mv = NAN;
	s->steer_deg = NAN;
	s->press_pending = -1;
	s->backlight_pct = -1;
	s->g_lat = s->g_long = NAN;
	for (int i = 0; i < 4; i++) s->tyre_psi[i] = NAN;
	s->psi = true;
	for (int i = 0; i < 3; i++) s->cabin_c[i] = NAN;
	for (int i = 0; i < 4; i++) s->brake_temp_c[i] = NAN;
	s->route_len = s->route_kwh = s->arrive_range = s->range_drop = s->arrive_pct = s->pct_drop = s->arrive_kwh = NAN;
}
