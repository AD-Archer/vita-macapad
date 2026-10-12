#include "gfx.h"

#include <math.h>
#include <psp2/pvf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- frames and the triangle batch ----------------------------------------------

// Shapes are triangles collected in GPU memory from vita2d's per-frame pool and drawn in one go
// whenever something else (text, a picture) has to go on top, or the frame ends. The pool is
// reset each frame, so nothing here is freed.
#define BATCH_VERTS (3 * 4096)
static vita2d_color_vertex *s_batch;
static int s_batchN, s_batchDrawn;

static void flush(void) {
	if (s_batch && s_batchN > s_batchDrawn)
		vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, s_batch + s_batchDrawn, s_batchN - s_batchDrawn);
	s_batchDrawn = s_batchN;
}

void gfx_flush(void) { flush(); }

static float s_ox;  // gfx_offset
void gfx_offset(float dx) { s_ox = dx; }

static vita2d_color_vertex *batch_room(int n) {
	if (!s_batch || s_batchN + n > BATCH_VERTS) {
		flush();
		s_batch = vita2d_pool_malloc(BATCH_VERTS * sizeof *s_batch);
		s_batchN = s_batchDrawn = 0;
		if (!s_batch) return NULL;
	}
	vita2d_color_vertex *v = s_batch + s_batchN;
	s_batchN += n;
	return v;
}

void tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c) {
	vita2d_color_vertex *v = batch_room(3);
	if (!v) return;
	v[0] = (vita2d_color_vertex){x1 + s_ox, y1, 0.5f, c};
	v[1] = (vita2d_color_vertex){x2 + s_ox, y2, 0.5f, c};
	v[2] = (vita2d_color_vertex){x3 + s_ox, y3, 0.5f, c};
}

void rect(float x, float y, float w, float h, u32 c) {
	if (w <= 0 || h <= 0) return;
	tri(x, y, x + w, y, x, y + h, c);
	tri(x + w, y, x + w, y + h, x, y + h, c);
}

void line(float x1, float y1, float x2, float y2, float t, u32 c) {
	float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
	if (len <= 0) return;
	float nx = -dy / len * t / 2, ny = dx / len * t / 2;
	tri(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x1 - nx, y1 - ny, c);
	tri(x2 + nx, y2 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, c);
}

// Pictures freed this frame wait until the GPU has finished with it.
#define DEAD_MAX 64
static vita2d_texture *s_dead[DEAD_MAX];
static int s_deadCount;

// ---- fonts ------------------------------------------------------------------------

// vita2d keeps one glyph cache per font, filled at the first size each glyph is drawn at, so
// each size gets its own font (they share the file in memory).
typedef struct {
	int size;
	Weight weight;
	vita2d_font *font;
} SizedFont;
#define FONTS_MAX 40
static SizedFont s_fonts[FONTS_MAX];
static int s_fontCount;
static void *s_fontData[2];
static unsigned s_fontLen[2];

// Song titles and names in Japanese, Korean or Chinese: the system's own fonts, made on first use.
static vita2d_pvf *s_pvf;
static bool s_pvfTried;
static float s_pvfLine;  // its line height at scale 1

static void *read_file(const char *path, unsigned *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *data = n > 0 ? malloc(n) : NULL;
	if (data && fread(data, 1, n, f) != (size_t)n) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*len = data ? (unsigned)n : 0;
	return data;
}

static vita2d_font *font_for(int size, Weight w) {
	for (int i = 0; i < s_fontCount; i++)
		if (s_fonts[i].size == size && s_fonts[i].weight == w) return s_fonts[i].font;
	if (!s_fontData[w]) return NULL;
	vita2d_font *f = vita2d_load_font_mem(s_fontData[w], s_fontLen[w]);
	if (!f) return NULL;
	if (s_fontCount == FONTS_MAX) {
		// (never expected: the app uses a couple dozen sizes) reuse the oldest
		flush();
		vita2d_wait_rendering_done();
		vita2d_free_font(s_fonts[0].font);
		memmove(s_fonts, s_fonts + 1, (FONTS_MAX - 1) * sizeof *s_fonts);
		s_fontCount--;
	}
	s_fonts[s_fontCount++] = (SizedFont){size, w, f};
	return f;
}

static int is_hangul(unsigned c) {
	return (c >= 0x1100 && c < 0x1200) || (c >= 0x3130 && c < 0x3190) || (c >= 0xAC00 && c < 0xD7B0);
}

static vita2d_pvf *pvf(void) {
	if (!s_pvfTried) {
		s_pvfTried = true;
		vita2d_system_pvf_config configs[] = {
			{SCE_PVF_LANGUAGE_K, is_hangul},
			{SCE_PVF_LANGUAGE_J, NULL},
		};
		s_pvf = vita2d_load_system_pvf(2, configs);
		if (s_pvf) s_pvfLine = vita2d_pvf_text_height(s_pvf, 1.0f, "A");
		if (s_pvfLine <= 0) s_pvfLine = 18;
	}
	return s_pvf;
}

// Whether the string has characters Inter doesn't: CJK, kana, Hangul, or anything past the BMP.
static bool needs_system_font(const char *s) {
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p >= 0xF0) return true;
		if (*p >= 0xE0 && p[1]) {
			unsigned cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			if (cp >= 0x2E80 && cp < 0xFB00) return true;
		}
	}
	return false;
}

static float pvf_scale(int size) { return size * 1.21f / s_pvfLine; }

float cap_height(int size) { return size * 0.727f; }

float text_width(const char *str, int size, Weight w) {
	if (!str || !*str) return 0;
	if (needs_system_font(str) && pvf()) return vita2d_pvf_text_width(s_pvf, pvf_scale(size), str);
	vita2d_font *f = font_for(size, w);
	return f ? vita2d_font_text_width(f, size, str) : 0;
}

void text(float x, float y, int size, Weight w, u32 color, int align, const char *str) {
	if (!str || !*str) return;
	flush();
	bool system = needs_system_font(str) && pvf();
	float width = align != ALIGN_LEFT ? text_width(str, size, w) : 0;
	if (align == ALIGN_CENTER) x -= width / 2;
	else if (align == ALIGN_RIGHT) x -= width;
	x += s_ox;
	if (x > SCREEN_W || x + (width ? width : SCREEN_W) < 0) return;  // off screen
	if (system) {
		vita2d_pvf_draw_text(s_pvf, (int)lroundf(x), (int)lroundf(y), color, pvf_scale(size), str);
		return;
	}
	vita2d_font *f = font_for(size, w);
	if (f) vita2d_font_draw_text(f, (int)lroundf(x), (int)lroundf(y), color, size, str);
}

static int utf8_len(unsigned char c) {
	return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

void text_fit(float x, float y, int size, Weight w, u32 color, int align, float maxw, const char *str) {
	if (!str || !*str) return;
	if (text_width(str, size, w) <= maxw) {
		text(x, y, size, w, color, align, str);
		return;
	}
	// the longest start of it that fits with "…" after it, found by halving
	char out[256];
	int ends[256], n = 0;
	for (int i = 0; str[i] && n < 255; ) {
		i += utf8_len((unsigned char)str[i]);
		if (i >= (int)sizeof out - 4) break;
		ends[n++] = i;
	}
	int lo = 0, hi = n - 1, best = 0;  // ends[k] bytes; best = 0 means just "…"
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		memcpy(out, str, ends[mid]);
		memcpy(out + ends[mid], "\xE2\x80\xA6", 4);
		if (text_width(out, size, w) <= maxw) {
			best = ends[mid];
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	while (best > 0 && str[best - 1] == ' ') best--;
	memcpy(out, str, best);
	memcpy(out + best, "\xE2\x80\xA6", 4);
	text(x, y, size, w, color, align, out);
}

// ---- setup --------------------------------------------------------------------------

void gfx_init(void) {
	vita2d_init_advanced_with_msaa(8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
	vita2d_set_clear_color(C_BG);
	vita2d_set_vblank_wait(1);
	s_fontData[W_REGULAR] = read_file("app0:assets/fonts/Inter-Regular.ttf", &s_fontLen[W_REGULAR]);
	s_fontData[W_BOLD] = read_file("app0:assets/fonts/Inter-SemiBold.ttf", &s_fontLen[W_BOLD]);
}

void gfx_exit(void) {
	vita2d_wait_rendering_done();
	for (int i = 0; i < s_deadCount; i++) vita2d_free_texture(s_dead[i]);
	s_deadCount = 0;
	for (int i = 0; i < s_fontCount; i++) vita2d_free_font(s_fonts[i].font);
	s_fontCount = 0;
	if (s_pvf) vita2d_free_pvf(s_pvf);
	s_pvf = NULL;
	free(s_fontData[0]);
	free(s_fontData[1]);
	vita2d_fini();
}

void gfx_begin(void) {
	if (s_deadCount) {
		vita2d_wait_rendering_done();
		for (int i = 0; i < s_deadCount; i++) vita2d_free_texture(s_dead[i]);
		s_deadCount = 0;
	}
	vita2d_start_drawing();  // (resets the pool)
	s_batch = NULL;
	s_batchN = s_batchDrawn = 0;
	vita2d_clear_screen();
}

void gfx_end(void) {
	flush();
	vita2d_end_drawing();
	vita2d_common_dialog_update();
	vita2d_swap_buffers();
}

// ---- shapes -------------------------------------------------------------------------

static void arc(float cx, float cy, float rx, float ry, float from, float to, u32 c) {
	int n = (int)((rx > ry ? rx : ry) * (to - from) / 3.0f) + 4;
	if (n > 64) n = 64;
	float px = cx + rx * cosf(from), py = cy + ry * sinf(from);
	for (int i = 1; i <= n; i++) {
		float a = from + (to - from) * i / n;
		float qx = cx + rx * cosf(a), qy = cy + ry * sinf(a);
		tri(cx, cy, px, py, qx, qy, c);
		px = qx;
		py = qy;
	}
}

void disc(float cx, float cy, float r, u32 c) { arc(cx, cy, r, r, 0, 2 * M_PI, c); }

void ellipse(float x, float y, float w, float h, u32 c) { arc(x + w / 2, y + h / 2, w / 2, h / 2, 0, 2 * M_PI, c); }

void rrect(float x, float y, float w, float h, float r, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if (r < 0.5f) {
		rect(x, y, w, h, c);
		return;
	}
	rect(x + r, y, w - 2 * r, h, c);
	rect(x, y + r, r, h - 2 * r, c);
	rect(x + w - r, y + r, r, h - 2 * r, c);
	arc(x + r, y + r, r, r, M_PI, 1.5f * M_PI, c);
	arc(x + w - r, y + r, r, r, 1.5f * M_PI, 2 * M_PI, c);
	arc(x + w - r, y + h - r, r, r, 0, 0.5f * M_PI, c);
	arc(x + r, y + h - r, r, r, 0.5f * M_PI, M_PI, c);
}

void ring(float cx, float cy, float r, float t, u32 c) { stroke_arc(cx, cy, r, r, 0, 2 * M_PI, t, c); }

// A band between radius (rx, ry) and (rx - t, ry - t), as a strip of triangles.
void stroke_arc(float cx, float cy, float rx, float ry, float from, float to, float t, u32 c) {
	int n = (int)((rx > ry ? rx : ry) * (to - from) / 2.5f) + 4;
	if (n > 96) n = 96;
	float irx = rx - t, iry = ry - t;
	if (irx < 0) irx = 0;
	if (iry < 0) iry = 0;
	float ox = cx + rx * cosf(from), oy = cy + ry * sinf(from);
	float ix = cx + irx * cosf(from), iy = cy + iry * sinf(from);
	for (int i = 1; i <= n; i++) {
		float a = from + (to - from) * i / n;
		float qox = cx + rx * cosf(a), qoy = cy + ry * sinf(a);
		float qix = cx + irx * cosf(a), qiy = cy + iry * sinf(a);
		tri(ox, oy, ix, iy, qox, qoy, c);
		tri(ix, iy, qix, qiy, qox, qoy, c);
		ox = qox; oy = qoy; ix = qix; iy = qiy;
	}
}

void stroke_rrect(float x, float y, float w, float h, float r, float t, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	rect(x + r, y, w - 2 * r, t, c);
	rect(x + r, y + h - t, w - 2 * r, t, c);
	rect(x, y + r, t, h - 2 * r, c);
	rect(x + w - t, y + r, t, h - 2 * r, c);
	stroke_arc(x + r, y + r, r, r, M_PI, 1.5f * M_PI, t, c);
	stroke_arc(x + w - r, y + r, r, r, 1.5f * M_PI, 2 * M_PI, t, c);
	stroke_arc(x + w - r, y + h - r, r, r, 0, 0.5f * M_PI, t, c);
	stroke_arc(x + r, y + h - r, r, r, 0.5f * M_PI, M_PI, t, c);
}

// ---- glyphs ----------------------------------------------------------------------

static void tri_right(float x, float cy, float s, u32 c) { tri(x, cy - s / 2, x, cy + s / 2, x + s * 0.8f, cy, c); }

static void tri_left(float x, float cy, float s, u32 c) { tri(x + s * 0.8f, cy - s / 2, x + s * 0.8f, cy + s / 2, x, cy, c); }

static void speaker(float cx, float cy, float s, u32 c) {
	float x = cx - s * 0.55f;
	rect(x, cy - s * 0.18f, s * 0.22f, s * 0.36f, c);
	tri(x + s * 0.1f, cy, x + s * 0.5f, cy - s * 0.45f, x + s * 0.5f, cy + s * 0.45f, c);
}

static void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u32 c) {
	tri(x1, y1, x2, y2, x3, y3, c);
	tri(x1, y1, x3, y3, x4, y4, c);
}

// An arrowhead with its tip at (x, y), pointing along (dx, dy) (a unit vector), s px long.
static void arrow_head(float x, float y, float dx, float dy, float s, u32 c) {
	float bx = x - dx * s, by = y - dy * s;  // middle of the base
	float nx = -dy * s * 0.6f, ny = dx * s * 0.6f;
	tri(x, y, bx + nx, by + ny, bx - nx, by - ny, c);
}

// A right-pointing arrow with a shaft, cy across the middle, from x0 to x1 (x1 is the tip).
static void arrow(float x0, float x1, float cy, float s, float t, u32 c) {
	float dir = x1 > x0 ? 1 : -1;
	line(x0, cy, x1 - dir * s * 0.3f, cy, t, c);
	arrow_head(x1, cy, dir, 0, s * 0.42f, c);
}

// The sun's disc with rays from r1 to r2 (fractions of s).
static void sun_rays(float cx, float cy, float s, float r0, float r1, float r2, float t, u32 c) {
	disc(cx, cy, s * r0, c);
	for (int i = 0; i < 8; i++) {
		float a = i * (float)M_PI / 4, ca = cosf(a), sa = sinf(a);
		line(cx + ca * s * r1, cy + sa * s * r1, cx + ca * s * r2, cy + sa * s * r2, t, c);
	}
}

// A magnifier, for zooming.
static void magnifier(float cx, float cy, float s, float t, u32 c) {
	ring(cx - s * 0.1f, cy - s * 0.1f, s * 0.36f, t * 0.85f, c);
	line(cx + s * 0.16f, cy + s * 0.16f, cx + s * 0.46f, cy + s * 0.46f, t * 1.2f, c);
	rect(cx - s * 0.24f, cy - s * 0.1f - t * 0.35f, s * 0.28f, t * 0.7f, c);
}

// A mouse: its outline and the line between the buttons. side -1 / 1 fills the left / right button.
static void mouse(float cx, float cy, float s, float t, int side, u32 c) {
	float w = s * 0.62f, h = s * 0.96f, r = w / 2, x = cx - w / 2, y = cy - h / 2, split = y + h * 0.42f;
	stroke_rrect(x, y, w, h, r, t * 0.75f, c);
	rect(x, split - t * 0.35f, w, t * 0.7f, c);
	rect(cx - t * 0.35f, y, t * 0.7f, split - y, c);
	if (side < 0) stroke_arc(x + r, y + r, r, r, M_PI, 1.5f * M_PI, r, c);
	if (side > 0) stroke_arc(x + r, y + r, r, r, 1.5f * M_PI, 2 * M_PI, r, c);
	if (side) rect(side < 0 ? x : cx, y + r - 0.5f, r, split - (y + r) + 0.5f, c);
}

// A screen with the part a window fills: fx, fy, fw, fh are fractions of it.
static void screen_part(float cx, float cy, float s, float t, float fx, float fy, float fw, float fh, u32 c) {
	float w = s * 1.1f, h = s * 0.8f, x = cx - w / 2, y = cy - h / 2, gap = t * 0.75f + s * 0.06f;
	stroke_rrect(x, y, w, h, s * 0.1f, t * 0.7f, c);
	float iw = w - 2 * gap, ih = h - 2 * gap;
	rrect(x + gap + iw * fx, y + gap + ih * fy, iw * fw, ih * fh, s * 0.05f, c);
}

// A window: a rounded rect with a title bar, drawn solid in c, with a bg border so it reads on a stack.
static void window(float x, float y, float w, float h, float s, u32 c, u32 bg, bool border) {
	float b = s * 0.06f;
	if (border) rrect(x - b, y - b, w + 2 * b, h + 2 * b, s * 0.14f, bg);
	rrect(x, y, w, h, s * 0.1f, c);
	rect(x + b, y + h * 0.3f, w - 2 * b, s * 0.05f, bg);
}

void glyph(Glyph g, float cx, float cy, float s, u32 c, u32 bg) {
	float bar = s * 0.12f, t = s * 0.13f, b = s * 0.06f;
	switch (g) {
	case GLYPH_NONE:
		break;
	case GLYPH_PLAY:
		tri_right(cx - s * 0.32f, cy, s, c);
		break;
	case GLYPH_PAUSE:
		rrect(cx - s * 0.32f, cy - s / 2, s * 0.22f, s, s * 0.06f, c);
		rrect(cx + s * 0.10f, cy - s / 2, s * 0.22f, s, s * 0.06f, c);
		break;
	case GLYPH_PLAYPAUSE:
		tri_right(cx - s * 0.55f, cy, s * 0.8f, c);
		rect(cx + s * 0.18f, cy - s * 0.4f, bar * 1.3f, s * 0.8f, c);
		rect(cx + s * 0.42f, cy - s * 0.4f, bar * 1.3f, s * 0.8f, c);
		break;
	case GLYPH_NEXT:
		tri_right(cx - s * 0.6f, cy, s * 0.75f, c);
		tri_right(cx - s * 0.05f, cy, s * 0.75f, c);
		rect(cx + s * 0.55f, cy - s * 0.375f, bar, s * 0.75f, c);
		break;
	case GLYPH_PREV:
		rect(cx - s * 0.55f - bar, cy - s * 0.375f, bar, s * 0.75f, c);
		tri_left(cx - s * 0.55f, cy, s * 0.75f, c);
		tri_left(cx, cy, s * 0.75f, c);
		break;
	case GLYPH_VOL_UP:
		speaker(cx - s * 0.1f, cy, s, c);
		rect(cx + s * 0.2f, cy - bar / 2, s * 0.4f, bar, c);
		rect(cx + s * 0.4f - bar / 2, cy - s * 0.2f, bar, s * 0.4f, c);
		break;
	case GLYPH_VOL_DOWN:
		speaker(cx - s * 0.1f, cy, s, c);
		rect(cx + s * 0.2f, cy - bar / 2, s * 0.4f, bar, c);
		break;
	case GLYPH_MUTE: {
		speaker(cx - s * 0.1f, cy, s, c);
		float x = cx + s * 0.4f, d = s * 0.18f;
		line(x - d, cy - d, x + d, cy + d, bar, c);
		line(x - d, cy + d, x + d, cy - d, bar, c);
		break;
	}

	// ---- system ----
	case GLYPH_SMILE:
		ring(cx, cy, s / 2, t, c);
		disc(cx - s * 0.17f, cy - s * 0.12f, s * 0.07f, c);
		disc(cx + s * 0.17f, cy - s * 0.12f, s * 0.07f, c);
		stroke_arc(cx, cy + s * 0.02f, s * 0.28f, s * 0.26f, 0.2f * M_PI, 0.8f * M_PI, t * 0.8f, c);
		break;
	case GLYPH_SEARCH:
		ring(cx - s * 0.1f, cy - s * 0.1f, s * 0.32f, t, c);
		line(cx + s * 0.14f, cy + s * 0.14f, cx + s * 0.44f, cy + s * 0.44f, t * 1.2f, c);
		break;
	case GLYPH_CAMERA:
		rect(cx - s * 0.18f, cy - s * 0.42f, s * 0.36f, s * 0.16f, c);
		stroke_rrect(cx - s * 0.5f, cy - s * 0.3f, s, s * 0.7f, s * 0.12f, t * 0.8f, c);
		ring(cx, cy + s * 0.05f, s * 0.18f, t * 0.8f, c);
		break;
	case GLYPH_CROP: {
		float a = s * 0.45f, l = s * 0.26f, w = t * 0.85f;
		for (int i = 0; i < 4; i++) {
			float sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1;
			float x = cx + sx * a, y = cy + sy * a;
			rect(sx > 0 ? x - l : x, y - (sy > 0 ? w : 0), l, w, c);
			rect(x - (sx > 0 ? w : 0), sy > 0 ? y - l : y, w, l, c);
		}
		disc(cx, cy, s * 0.1f, c);
		break;
	}
	case GLYPH_MONITOR:
		stroke_rrect(cx - s * 0.5f, cy - s * 0.42f, s, s * 0.66f, s * 0.08f, t * 0.8f, c);
		rect(cx - t / 2, cy + s * 0.24f, t, s * 0.14f, c);
		rrect(cx - s * 0.24f, cy + s * 0.36f, s * 0.48f, t * 0.8f, b, c);
		break;
	case GLYPH_GRID: {
		float sq = s * 0.42f, gap = s * 0.14f, x0 = cx - sq - gap / 2, y0 = cy - sq - gap / 2;
		for (int i = 0; i < 4; i++) rrect(x0 + (i & 1) * (sq + gap), y0 + (i >> 1) * (sq + gap), sq, sq, s * 0.08f, c);
		break;
	}
	case GLYPH_WINDOWS:
		window(cx - s * 0.5f, cy - s * 0.45f, s * 0.78f, s * 0.62f, s, c, bg, false);
		window(cx - s * 0.26f, cy - s * 0.15f, s * 0.78f, s * 0.62f, s, c, bg, true);
		break;
	case GLYPH_LOCK:
		stroke_arc(cx, cy - s * 0.14f, s * 0.24f, s * 0.26f, M_PI, 2 * M_PI, t * 0.85f, c);
		rect(cx - s * 0.24f, cy - s * 0.14f, t * 0.85f, s * 0.1f, c);
		rect(cx + s * 0.24f - t * 0.85f, cy - s * 0.14f, t * 0.85f, s * 0.1f, c);
		rrect(cx - s * 0.4f, cy - s * 0.06f, s * 0.8f, s * 0.56f, s * 0.1f, c);
		disc(cx, cy + s * 0.2f, s * 0.07f, bg);
		break;

	// ---- windows ----
	case GLYPH_MINIMIZE:
		line(cx, cy - s * 0.48f, cx, cy + s * 0.02f, t, c);
		arrow_head(cx, cy + s * 0.2f, 0, 1, s * 0.4f, c);
		rrect(cx - s * 0.42f, cy + s * 0.36f, s * 0.84f, t, b, c);
		break;
	case GLYPH_EXPAND: {
		float a = s * 0.42f, l = s * 0.3f, w = t * 0.9f;
		// top-right and bottom-left corners, with a diagonal between
		rect(cx + a - l, cy - a, l, w, c);
		rect(cx + a - w, cy - a, w, l, c);
		rect(cx - a, cy + a - w, l, w, c);
		rect(cx - a, cy + a - l, w, l, c);
		line(cx - a + w, cy + a - w, cx + a - w, cy - a + w, w, c);
		break;
	}
	case GLYPH_CLOSE: {
		ring(cx, cy, s / 2, t, c);
		float d = s * 0.17f;
		line(cx - d, cy - d, cx + d, cy + d, t * 0.85f, c);
		line(cx - d, cy + d, cx + d, cy - d, t * 0.85f, c);
		break;
	}
	case GLYPH_REFRESH: {
		float r = s * 0.4f, to = 4.3f;
		stroke_arc(cx, cy, r, r, -0.9f, to, t, c);
		float ex = cx + (r - t / 2) * cosf(to), ey = cy + (r - t / 2) * sinf(to);
		arrow_head(ex - sinf(to) * s * 0.1f, ey + cosf(to) * s * 0.1f, -sinf(to), cosf(to), s * 0.36f, c);
		break;
	}
	case GLYPH_MINUS_CIRCLE:
		ring(cx, cy, s / 2, t, c);
		rrect(cx - s * 0.24f, cy - t * 0.42f, s * 0.48f, t * 0.85f, b * 0.6f, c);
		break;
	case GLYPH_POWER:
		stroke_arc(cx, cy + s * 0.04f, s * 0.42f, s * 0.42f, -M_PI / 2 + 0.7f, 1.5f * M_PI - 0.7f, t, c);
		rrect(cx - t / 2, cy - s * 0.5f, t, s * 0.48f, t / 2, c);
		break;

	// ---- browser ----
	case GLYPH_TAB:
		rrect(cx - s * 0.5f, cy - s * 0.46f, s * 0.44f, s * 0.3f, s * 0.08f, c);
		rrect(cx - s * 0.5f, cy - s * 0.28f, s, s * 0.74f, s * 0.1f, c);
		rect(cx - s * 0.2f, cy + s * 0.08f - bar * 0.4f, s * 0.4f, bar * 0.8f, bg);
		rect(cx - bar * 0.4f, cy + s * 0.08f - s * 0.2f, bar * 0.8f, s * 0.4f, bg);
		break;
	case GLYPH_UNDO:
	case GLYPH_REDO: {
		float m = g == GLYPH_UNDO ? 1 : -1, r = s * 0.3f;
		float ax = cx + m * s * 0.1f, ay = cy + s * 0.08f;
		// the curve on the far side, then a straight run back toward the arrow
		if (m > 0) stroke_arc(ax, ay, r, r, -M_PI / 2, M_PI / 2, t, c);
		else stroke_arc(ax, ay, r, r, M_PI / 2, 1.5f * M_PI, t, c);
		line(ax, ay - r + t / 2, ax - m * s * 0.3f, ay - r + t / 2, t, c);
		line(ax, ay + r - t / 2, ax - m * s * 0.22f, ay + r - t / 2, t, c);
		arrow_head(ax - m * s * 0.5f, ay - r + t / 2, -m, 0, s * 0.36f, c);
		break;
	}
	case GLYPH_ARROW_RIGHT:
		arrow(cx - s * 0.45f, cx + s * 0.48f, cy, s, t, c);
		break;
	case GLYPH_ARROW_LEFT:
		arrow(cx + s * 0.45f, cx - s * 0.48f, cy, s, t, c);
		break;
	case GLYPH_ADDRESS:
		stroke_rrect(cx - s * 0.62f, cy - s * 0.26f, s * 1.24f, s * 0.52f, s * 0.26f, t * 0.75f, c);
		disc(cx - s * 0.34f, cy, s * 0.07f, c);
		rrect(cx - s * 0.18f, cy - t * 0.3f, s * 0.5f, t * 0.6f, b * 0.5f, c);
		break;

	// ---- editing ----
	case GLYPH_COPY:
		stroke_rrect(cx - s * 0.46f, cy - s * 0.5f, s * 0.64f, s * 0.74f, s * 0.08f, t * 0.75f, c);
		rrect(cx - s * 0.2f - b, cy - s * 0.26f - b, s * 0.64f + 2 * b, s * 0.74f + 2 * b, s * 0.1f, bg);
		rrect(cx - s * 0.2f, cy - s * 0.26f, s * 0.64f, s * 0.74f, s * 0.08f, c);
		break;
	case GLYPH_PASTE:
		rrect(cx - s * 0.4f, cy - s * 0.38f, s * 0.8f, s * 0.88f, s * 0.08f, c);
		rrect(cx - s * 0.2f, cy - s * 0.5f, s * 0.4f, s * 0.22f, s * 0.05f, c);
		rect(cx - s * 0.24f, cy - s * 0.02f, s * 0.48f, t * 0.55f, bg);
		rect(cx - s * 0.24f, cy + s * 0.18f, s * 0.32f, t * 0.55f, bg);
		break;
	case GLYPH_CUT:
		ring(cx - s * 0.26f, cy + s * 0.3f, s * 0.17f, t * 0.8f, c);
		ring(cx + s * 0.26f, cy + s * 0.3f, s * 0.17f, t * 0.8f, c);
		line(cx - s * 0.14f, cy + s * 0.16f, cx + s * 0.32f, cy - s * 0.5f, t * 0.85f, c);
		line(cx + s * 0.14f, cy + s * 0.16f, cx - s * 0.32f, cy - s * 0.5f, t * 0.85f, c);
		break;
	case GLYPH_SELECT_ALL: {
		float a = s * 0.44f, d = s * 0.22f, w = t * 0.7f;
		for (int i = 0; i < 3; i++) {
			float o = -a + i * (2 * a - d) / 2;
			rect(cx + o, cy - a, d, w, c);
			rect(cx + o, cy + a - w, d, w, c);
			rect(cx - a, cy + o, w, d, c);
			rect(cx + a - w, cy + o, w, d, c);
		}
		rrect(cx - s * 0.18f, cy - s * 0.18f, s * 0.36f, s * 0.36f, b, c);
		break;
	}
	case GLYPH_SAVE:
		rrect(cx - s * 0.44f, cy - s * 0.44f, s * 0.88f, s * 0.88f, s * 0.1f, c);
		rect(cx - s * 0.24f, cy - s * 0.44f, s * 0.48f, s * 0.26f, bg);
		rect(cx + s * 0.06f, cy - s * 0.4f, s * 0.1f, s * 0.16f, c);
		rrect(cx - s * 0.26f, cy + s * 0.1f, s * 0.52f, s * 0.26f, s * 0.04f, bg);
		break;

	// ---- key kinds + UI ----
	case GLYPH_BOLT:
		quad(cx + s * 0.2f, cy - s * 0.5f, cx - s * 0.3f, cy + s * 0.05f, cx, cy + s * 0.05f, cx + s * 0.1f, cy - s * 0.08f, c);
		quad(cx, cy + s * 0.05f, cx - s * 0.15f, cy + s * 0.5f, cx + s * 0.3f, cy - s * 0.08f, cx + s * 0.1f, cy - s * 0.08f, c);
		break;
	case GLYPH_GLOBE: {
		float r = s / 2, w = t * 0.7f;
		ring(cx, cy, r, w, c);
		stroke_arc(cx, cy, r * 0.45f, r, 0, 2 * M_PI, w, c);
		rect(cx - r, cy - w / 2, r * 2, w, c);
		break;
	}
	case GLYPH_KEYS:
		stroke_rrect(cx - s * 0.6f, cy - s * 0.38f, s * 1.2f, s * 0.76f, s * 0.12f, t * 0.7f, c);
		for (int row = 0; row < 2; row++)
			for (int col = 0; col < 4; col++)
				rect(cx - s * 0.44f + col * s * 0.24f, cy - s * 0.22f + row * s * 0.18f, s * 0.14f, s * 0.11f, c);
		rect(cx - s * 0.3f, cy + s * 0.14f, s * 0.6f, s * 0.1f, c);
		break;
	case GLYPH_SHORTCUT:
		rrect(cx - s * 0.5f, cy - s * 0.5f, s * 0.74f, s * 0.74f, s * 0.18f, c);
		rrect(cx - s * 0.24f - b, cy - s * 0.24f - b, s * 0.74f + 2 * b, s * 0.74f + 2 * b, s * 0.2f, bg);
		rrect(cx - s * 0.24f, cy - s * 0.24f, s * 0.74f, s * 0.74f, s * 0.18f, c);
		break;
	case GLYPH_APP:
		rrect(cx - s * 0.5f, cy - s * 0.5f, s, s, s * 0.22f, c);
		disc(cx - s * 0.2f, cy - s * 0.2f, s * 0.1f, bg);
		disc(cx + s * 0.2f, cy - s * 0.2f, s * 0.1f, bg);
		disc(cx - s * 0.2f, cy + s * 0.2f, s * 0.1f, bg);
		disc(cx + s * 0.2f, cy + s * 0.2f, s * 0.1f, bg);
		break;
	case GLYPH_PLUS:
		rrect(cx - s / 2, cy - bar / 2, s, bar, bar / 2, c);
		rrect(cx - bar / 2, cy - s / 2, bar, s, bar / 2, c);
		break;
	case GLYPH_CHECK:
		line(cx - s * 0.42f, cy + s * 0.02f, cx - s * 0.14f, cy + s * 0.3f, t * 1.1f, c);
		line(cx - s * 0.14f, cy + s * 0.3f, cx + s * 0.44f, cy - s * 0.3f, t * 1.1f, c);
		disc(cx - s * 0.14f, cy + s * 0.3f, t * 0.55f, c);  // (fills the notch where the strokes meet)
		break;
	case GLYPH_CHEVRON_R:
	case GLYPH_CHEVRON_L: {
		float m = g == GLYPH_CHEVRON_R ? 1 : -1;
		line(cx - m * s * 0.14f, cy - s * 0.36f, cx + m * s * 0.16f, cy, t * 0.9f, c);
		line(cx + m * s * 0.16f, cy, cx - m * s * 0.14f, cy + s * 0.36f, t * 0.9f, c);
		disc(cx + m * s * 0.16f, cy, t * 0.45f, c);
		break;
	}
	case GLYPH_CLOCK:
		ring(cx, cy, s / 2, t, c);
		rrect(cx - t * 0.4f, cy - s * 0.3f, t * 0.8f, s * 0.3f + t * 0.4f, t * 0.3f, c);
		rrect(cx - t * 0.4f, cy - t * 0.4f, s * 0.24f, t * 0.8f, t * 0.3f, c);
		break;
	case GLYPH_LAPTOP:
		stroke_rrect(cx - s * 0.4f, cy - s * 0.42f, s * 0.8f, s * 0.56f, s * 0.06f, t * 0.8f, c);
		rrect(cx - s * 0.58f, cy + s * 0.2f, s * 1.16f, t, t / 2, c);
		break;
	case GLYPH_PENCIL:
		line(cx - s * 0.22f, cy + s * 0.22f, cx + s * 0.4f, cy - s * 0.4f, s * 0.24f, c);
		tri(cx - s * 0.46f, cy + s * 0.46f, cx - s * 0.38f, cy + s * 0.16f, cx - s * 0.16f, cy + s * 0.38f, c);
		break;
	case GLYPH_TRASH:
		rrect(cx - s * 0.46f, cy - s * 0.4f, s * 0.92f, t * 0.8f, b, c);
		rect(cx - s * 0.16f, cy - s * 0.5f, s * 0.32f, t * 0.8f, c);
		rrect(cx - s * 0.36f, cy - s * 0.26f, s * 0.72f, s * 0.76f, s * 0.08f, c);
		rect(cx - s * 0.14f, cy - s * 0.12f, t * 0.5f, s * 0.46f, bg);
		rect(cx + s * 0.07f, cy - s * 0.12f, t * 0.5f, s * 0.46f, bg);
		break;
	case GLYPH_MENU:
		for (int i = -1; i <= 1; i++) rrect(cx - s * 0.45f, cy + i * s * 0.32f - t / 2, s * 0.9f, t, t / 2, c);
		break;
	case GLYPH_HOME:
		tri(cx, cy - s * 0.5f, cx - s * 0.52f, cy - s * 0.02f, cx + s * 0.52f, cy - s * 0.02f, c);
		rect(cx - s * 0.36f, cy - s * 0.04f, s * 0.72f, s * 0.52f, c);
		rect(cx - s * 0.1f, cy + s * 0.14f, s * 0.2f, s * 0.34f, bg);
		break;

	// ---- the controller, typing, the Mac's switches ----
	case GLYPH_GAMEPAD: {
		rrect(cx - s * 0.62f, cy - s * 0.3f, s * 1.24f, s * 0.56f, s * 0.26f, c);
		disc(cx - s * 0.4f, cy + s * 0.2f, s * 0.2f, c);  // the grips
		disc(cx + s * 0.4f, cy + s * 0.2f, s * 0.2f, c);
		float dx = cx - s * 0.36f, dy = cy - s * 0.04f, arm = s * 0.12f, w = s * 0.075f;
		rect(dx - arm, dy - w / 2, 2 * arm, w, bg);
		rect(dx - w / 2, dy - arm, w, 2 * arm, bg);
		disc(cx + s * 0.28f, cy - s * 0.08f, s * 0.055f, bg);
		disc(cx + s * 0.42f, cy + s * 0.02f, s * 0.055f, bg);
		break;
	}
	case GLYPH_MOUSE:
		mouse(cx, cy, s, t, 0, c);
		break;
	case GLYPH_CLICK_LEFT:
	case GLYPH_CLICK_RIGHT:
		mouse(cx, cy, s, t, g == GLYPH_CLICK_LEFT ? -1 : 1, c);
		break;
	case GLYPH_COFFEE:
		rrect(cx - s * 0.42f, cy - s * 0.12f, s * 0.64f, s * 0.54f, s * 0.14f, c);
		ring(cx + s * 0.24f, cy + s * 0.1f, s * 0.17f, t * 0.75f, c);
		rrect(cx - s * 0.5f, cy + s * 0.44f, s * 0.86f, t * 0.6f, t * 0.3f, c);
		for (int i = 0; i < 2; i++) {
			float x = cx - s * 0.22f + i * s * 0.22f;
			line(x, cy - s * 0.48f, x - s * 0.04f, cy - s * 0.34f, t * 0.6f, c);
			line(x - s * 0.04f, cy - s * 0.34f, x, cy - s * 0.2f, t * 0.6f, c);
		}
		break;
	case GLYPH_ARROW_UP:
	case GLYPH_ARROW_DOWN: {
		float m = g == GLYPH_ARROW_UP ? -1 : 1;
		line(cx, cy - m * s * 0.45f, cx, cy + m * s * 0.2f, t, c);
		arrow_head(cx, cy + m * s * 0.48f, 0, m, s * 0.42f, c);
		break;
	}

	// ---- accessibility and more system actions ----
	case GLYPH_PERSON:
		disc(cx, cy - s * 0.36f, s * 0.12f, c);
		rrect(cx - s * 0.44f, cy - s * 0.18f, s * 0.88f, t * 0.9f, t * 0.45f, c);
		rect(cx - t * 0.6f, cy - s * 0.18f, t * 1.2f, s * 0.34f, c);
		line(cx, cy + s * 0.12f, cx - s * 0.2f, cy + s * 0.5f, t * 0.95f, c);
		line(cx, cy + s * 0.12f, cx + s * 0.2f, cy + s * 0.5f, t * 0.95f, c);
		break;
	case GLYPH_MIC:
		rrect(cx - s * 0.16f, cy - s * 0.5f, s * 0.32f, s * 0.58f, s * 0.16f, c);
		stroke_arc(cx, cy - s * 0.1f, s * 0.3f, s * 0.3f, 0, M_PI, t * 0.75f, c);
		rect(cx - t * 0.375f, cy + s * 0.2f, t * 0.75f, s * 0.2f, c);
		rrect(cx - s * 0.2f, cy + s * 0.4f, s * 0.4f, t * 0.75f, t * 0.35f, c);
		break;
	case GLYPH_SUN:
		sun_rays(cx, cy, s, 0.22f, 0.34f, 0.5f, s * 0.08f, c);
		break;
	case GLYPH_SUN_LOW:
		sun_rays(cx, cy, s, 0.2f, 0.34f, 0.39f, s * 0.09f, c);
		break;
	case GLYPH_MOON:
		disc(cx, cy, s * 0.44f, c);
		disc(cx + s * 0.22f, cy - s * 0.16f, s * 0.36f, bg);
		break;
	case GLYPH_ZOOM_IN:
		magnifier(cx, cy, s, t, c);
		rect(cx - s * 0.1f - t * 0.35f, cy - s * 0.24f, t * 0.7f, s * 0.28f, c);
		break;
	case GLYPH_ZOOM_OUT:
		magnifier(cx, cy, s, t, c);
		break;
	case GLYPH_INVERT:
		ring(cx, cy, s / 2, t, c);
		stroke_arc(cx, cy, s / 2, s / 2, -M_PI / 2, M_PI / 2, s / 2, c);
		break;
	case GLYPH_WIFI: {
		float by = cy + s * 0.36f;
		disc(cx, by, s * 0.09f, c);
		for (int i = 1; i <= 3; i++) stroke_arc(cx, by, s * (0.06f + 0.24f * i), s * (0.06f + 0.24f * i), -0.75f * M_PI, -0.25f * M_PI, t * 0.85f, c);
		break;
	}
	case GLYPH_BLUETOOTH: {
		// the rune: a stem, with arrowheads crossing it to the right
		float top = cy - s * 0.48f, bot = cy + s * 0.48f, x = cx + s * 0.26f, w = t * 0.8f;
		line(cx - s * 0.04f, top, cx - s * 0.04f, bot, w, c);
		line(cx - s * 0.04f, top, x, cy - s * 0.22f, w, c);
		line(x, cy - s * 0.22f, cx - s * 0.3f, cy + s * 0.24f, w, c);
		line(cx - s * 0.04f, bot, x, cy + s * 0.22f, w, c);
		line(x, cy + s * 0.22f, cx - s * 0.3f, cy - s * 0.24f, w, c);
		disc(x, cy - s * 0.22f, w / 2, c);
		disc(x, cy + s * 0.22f, w / 2, c);
		break;
	}
	case GLYPH_EJECT:
		tri(cx, cy - s * 0.44f, cx - s * 0.44f, cy + s * 0.08f, cx + s * 0.44f, cy + s * 0.08f, c);
		rrect(cx - s * 0.44f, cy + s * 0.24f, s * 0.88f, t * 1.1f, t * 0.3f, c);
		break;
	case GLYPH_EYE:
		ellipse(cx - s * 0.56f, cy - s * 0.32f, s * 1.12f, s * 0.64f, c);
		ellipse(cx - s * 0.56f + t, cy - s * 0.32f + t, s * 1.12f - 2 * t, s * 0.64f - 2 * t, bg);
		disc(cx, cy, s * 0.19f, c);
		break;
	case GLYPH_STAGE:  // Stage Manager: the window in front, the others in a strip at the side
		stroke_rrect(cx - s * 0.2f, cy - s * 0.36f, s * 0.72f, s * 0.72f, s * 0.08f, t * 0.75f, c);
		for (int i = -1; i <= 1; i++) rrect(cx - s * 0.56f, cy + i * s * 0.26f - s * 0.09f, s * 0.24f, s * 0.18f, s * 0.04f, c);
		break;

	// ---- where a window goes ----
	case GLYPH_WIN_LEFT: screen_part(cx, cy, s, t, 0, 0, 0.5f, 1, c); break;
	case GLYPH_WIN_RIGHT: screen_part(cx, cy, s, t, 0.5f, 0, 0.5f, 1, c); break;
	case GLYPH_WIN_TOP: screen_part(cx, cy, s, t, 0, 0, 1, 0.5f, c); break;
	case GLYPH_WIN_BOTTOM: screen_part(cx, cy, s, t, 0, 0.5f, 1, 0.5f, c); break;
	case GLYPH_WIN_TL: screen_part(cx, cy, s, t, 0, 0, 0.5f, 0.5f, c); break;
	case GLYPH_WIN_TR: screen_part(cx, cy, s, t, 0.5f, 0, 0.5f, 0.5f, c); break;
	case GLYPH_WIN_BL: screen_part(cx, cy, s, t, 0, 0.5f, 0.5f, 0.5f, c); break;
	case GLYPH_WIN_BR: screen_part(cx, cy, s, t, 0.5f, 0.5f, 0.5f, 0.5f, c); break;
	case GLYPH_WIN_CENTER: screen_part(cx, cy, s, t, 0.22f, 0.18f, 0.56f, 0.64f, c); break;
	case GLYPH_WIN_FILL: screen_part(cx, cy, s, t, 0, 0, 1, 1, c); break;
	}
}

// ---- pictures ----------------------------------------------------------------------

bool picture_load(Picture *pic, const u8 *rgba, int w, int h) {
	picture_free(pic);
	if (!rgba || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
	vita2d_texture *tex = vita2d_create_empty_texture(w, h);  // RGBA bytes, like the pixels
	if (!tex) return false;
	u8 *dst = vita2d_texture_get_datap(tex);
	unsigned stride = vita2d_texture_get_stride(tex);
	for (int y = 0; y < h; y++) memcpy(dst + y * stride, rgba + y * w * 4, w * 4);
	vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	pic->tex = tex;
	pic->w = w;
	pic->h = h;
	return true;
}

void picture_free(Picture *pic) {
	if (!pic->tex) return;
	if (s_deadCount < DEAD_MAX) {
		s_dead[s_deadCount++] = pic->tex;
	} else {
		vita2d_wait_rendering_done();
		vita2d_free_texture(pic->tex);
	}
	pic->tex = NULL;
}

void picture_draw(const Picture *pic, float x, float y, float w, float h) {
	if (!pic->tex) return;
	if (x + s_ox > SCREEN_W || x + s_ox + w < 0) return;
	flush();
	vita2d_draw_texture_scale(pic->tex, x + s_ox, y, w / pic->w, h / pic->h);
}

// Fills the bit of a corner outside its quarter circle.
static void corner(float px, float py, float cx, float cy, float r, float from, u32 bg) {
	int n = (int)(r * 1.57f / 3.0f) + 4;
	float ax = cx + r * cosf(from), ay = cy + r * sinf(from);
	for (int i = 1; i <= n; i++) {
		float a = from + (float)M_PI / 2 * i / n;
		float bx = cx + r * cosf(a), by = cy + r * sinf(a);
		tri(px, py, ax, ay, bx, by, bg);
		ax = bx;
		ay = by;
	}
}

void picture_draw_rounded(const Picture *pic, float x, float y, float w, float h, float r, u32 bg) {
	picture_draw(pic, x, y, w, h);
	if (!pic->tex) return;
	corner(x, y, x + r, y + r, r, M_PI, bg);
	corner(x + w, y, x + w - r, y + r, r, 1.5f * M_PI, bg);
	corner(x + w, y + h, x + w - r, y + h - r, r, 0, bg);
	corner(x, y + h, x + r, y + h - r, r, 0.5f * M_PI, bg);
}
