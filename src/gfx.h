#pragma once
#include "sys.h"

#include <stdbool.h>

#define SCREEN_W 960
#define SCREEN_H 544

// Colors are 0xAABBGGRR, which is what vita2d draws with.
#define RGBA(r, g, b, a) ((u32)(((a) & 0xFF) << 24 | ((b) & 0xFF) << 16 | ((g) & 0xFF) << 8 | ((r) & 0xFF)))
#define RGB(r, g, b) RGBA(r, g, b, 0xFF)
// The same color with another alpha.
#define FADE(c, a) (((c) & 0x00FFFFFF) | ((u32)((a) & 0xFF) << 24))

// iOS dark palette
#define C_BG        RGB(0x00, 0x00, 0x00)
#define C_CARD      RGB(0x1C, 0x1C, 0x1E)
#define C_CARD_HI   RGB(0x3A, 0x3A, 0x3C)
#define C_LINE      RGB(0x38, 0x38, 0x3A)
#define C_TEXT      RGB(0xFF, 0xFF, 0xFF)
#define C_TEXT2     RGB(0x98, 0x98, 0x9F)
#define C_TEXT3     RGB(0x63, 0x63, 0x66)
#define C_BLUE      RGB(0x0A, 0x84, 0xFF)
#define C_GREEN     RGB(0x30, 0xD1, 0x58)
#define C_ORANGE    RGB(0xFF, 0x9F, 0x0A)
#define C_RED       RGB(0xFF, 0x45, 0x3A)
#define C_PURPLE    RGB(0xBF, 0x5A, 0xF2)
#define C_YELLOW    RGB(0xFF, 0xD6, 0x0A)
#define C_TEAL      RGB(0x64, 0xD2, 0xFF)
#define C_PINK      RGB(0xFF, 0x37, 0x5F)
#define C_GRAY      RGB(0x8E, 0x8E, 0x93)
#define C_INDIGO    RGB(0x5E, 0x5C, 0xE6)

void gfx_init(void);
void gfx_exit(void);
void gfx_begin(void);  // starts a frame on a black screen
void gfx_end(void);    // finishes it (the keyboard, if open, draws over it) and shows it
// Draws the shapes collected so far, before something drawn with vita2d directly goes on top.
void gfx_flush(void);
// Shifts everything drawn after it dx px to the right (for pages sliding past); 0 to stop.
void gfx_offset(float dx);

void rect(float x, float y, float w, float h, u32 color);
void tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 color);
void line(float x1, float y1, float x2, float y2, float t, u32 color);
void disc(float cx, float cy, float r, u32 color);
void ellipse(float x, float y, float w, float h, u32 color);
void rrect(float x, float y, float w, float h, float r, u32 color);
// Outlines, t px thick, drawn inside the shape.
void ring(float cx, float cy, float r, float t, u32 color);
void stroke_arc(float cx, float cy, float rx, float ry, float from, float to, float t, u32 color);
void stroke_rrect(float x, float y, float w, float h, float r, float t, u32 color);

// ---- text ---------------------------------------------------------------------

typedef enum { W_REGULAR, W_BOLD } Weight;
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

// Text sitting on the baseline y, starting at x (or centered on it, or ending at it).
// size is the font size in px.
void text(float x, float y, int size, Weight w, u32 color, int align, const char *str);
// Same, but cut short with "…" to fit maxw.
void text_fit(float x, float y, int size, Weight w, u32 color, int align, float maxw, const char *str);
float text_width(const char *str, int size, Weight w);
// How far capitals rise above the baseline: for centering a line on y, put the baseline at y + cap / 2.
float cap_height(int size);

// ---- glyphs -------------------------------------------------------------------

// Little drawn icons, centered on (cx, cy), about size px tall. bg is what's behind the icon,
// for the few that have gaps (a keyhole, the front window of a stack).
typedef enum {
	GLYPH_NONE,
	// media
	GLYPH_PLAY, GLYPH_PAUSE, GLYPH_PLAYPAUSE, GLYPH_NEXT, GLYPH_PREV,
	GLYPH_VOL_UP, GLYPH_VOL_DOWN, GLYPH_MUTE,
	// system
	GLYPH_SMILE, GLYPH_SEARCH, GLYPH_CAMERA, GLYPH_CROP, GLYPH_MONITOR, GLYPH_GRID, GLYPH_WINDOWS, GLYPH_LOCK,
	// windows
	GLYPH_MINIMIZE, GLYPH_EXPAND, GLYPH_CLOSE, GLYPH_REFRESH, GLYPH_MINUS_CIRCLE, GLYPH_POWER,
	// browser
	GLYPH_TAB, GLYPH_UNDO, GLYPH_REDO, GLYPH_ARROW_RIGHT, GLYPH_ARROW_LEFT, GLYPH_ADDRESS,
	// editing
	GLYPH_COPY, GLYPH_PASTE, GLYPH_CUT, GLYPH_SELECT_ALL, GLYPH_SAVE,
	// key kinds + UI
	GLYPH_BOLT, GLYPH_GLOBE, GLYPH_KEYS, GLYPH_SHORTCUT, GLYPH_APP,
	GLYPH_PLUS, GLYPH_CHECK, GLYPH_CHEVRON_R, GLYPH_CHEVRON_L, GLYPH_CLOCK, GLYPH_LAPTOP, GLYPH_PENCIL, GLYPH_TRASH,
	GLYPH_MENU, GLYPH_HOME,
	// the controller, typing, the Mac's switches
	GLYPH_GAMEPAD, GLYPH_MOUSE, GLYPH_CLICK_LEFT, GLYPH_CLICK_RIGHT, GLYPH_COFFEE, GLYPH_ARROW_UP, GLYPH_ARROW_DOWN,
	// accessibility and more system actions
	GLYPH_PERSON, GLYPH_MIC, GLYPH_SUN, GLYPH_SUN_LOW, GLYPH_MOON, GLYPH_ZOOM_IN, GLYPH_ZOOM_OUT, GLYPH_INVERT,
	GLYPH_WIFI, GLYPH_BLUETOOTH, GLYPH_EJECT, GLYPH_EYE, GLYPH_STAGE,
	// where a window goes: a screen with the part it fills
	GLYPH_WIN_LEFT, GLYPH_WIN_RIGHT, GLYPH_WIN_TOP, GLYPH_WIN_BOTTOM,
	GLYPH_WIN_TL, GLYPH_WIN_TR, GLYPH_WIN_BL, GLYPH_WIN_BR, GLYPH_WIN_CENTER, GLYPH_WIN_FILL,
} Glyph;
void glyph(Glyph g, float cx, float cy, float size, u32 color, u32 bg);

// ---- pictures -----------------------------------------------------------------

// A texture made from RGBA8 pixels.
typedef struct {
	void *tex;  // vita2d_texture
	int w, h;
} Picture;

bool picture_load(Picture *pic, const u8 *rgba, int w, int h);
// Frees it once the GPU is done with the frames that drew it.
void picture_free(Picture *pic);
void picture_draw(const Picture *pic, float x, float y, float w, float h);
// The same, with its corners rounded to r by drawing bg over them.
void picture_draw_rounded(const Picture *pic, float x, float y, float w, float h, float r, u32 bg);
