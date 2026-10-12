// Macapad for PS Vita: a dashboard of the time, the weather, what's playing and how the Mac is
// doing, then pages of keys that run things on the Mac. Swipe (or L / R) between them. From the
// menu, the Vita can also be a controller (and trackpad) for the Mac, or type on it.
#include <ctype.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>

#include "controller.h"
#include "gfx.h"
#include "http.h"
#include "input.h"
#include "macapad.h"
#include "qrscan.h"
#include "sys.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Room for the app's own allocations (catalogs, pictures being decoded); the rest stays free.
int _newlib_heap_size_user = 96 * 1024 * 1024;

#define LONG_PRESS_MS 500
#define DRAG_PX 14

// the title bar of lists and the find / pair screens
#define BAR_H 64

// lists: a centered column of inset cards
#define LIST_X 150
#define LIST_W 660
#define ROW_H 60
#define ROW2_H 74       // a row with a second line
#define HEAD_H 46       // a section title above a card
#define BREAK_H 18      // a gap between cards
#define TILE 36         // the icon square at the left of a row

// the pad: 5x3 keys under a bar with the page's name, dots for the pages under them
#define PAD_BAR_H 74
#define GRID_X 24
#define GRID_Y 80
#define GAP 14
#define DOTS_Y 528
#define KEY_W ((SCREEN_W - 2 * GRID_X - (COLS - 1) * GAP) / (float)COLS)
#define KEY_H ((DOTS_Y - 16 - GRID_Y - (ROWS - 1) * GAP) / (float)ROWS)
#define KEY_R 22

// swiping between pages
#define SWIPE_COMMIT 150  // px: farther than this and the page changes
#define FLICK_PX 40       // or a quick flick of at least this far
#define FLICK_MS 260

typedef enum { SCR_FIND, SCR_QR, SCR_PAIR, SCR_MAIN, SCR_LIST, SCR_KEYS, SCR_PAD, SCR_TYPE } Screen;

// What the list on screen is for, so a pick knows where to go.
typedef enum {
	LIST_KIND,       // "What should this key do?"
	LIST_ACTIONS,
	LIST_APPS,
	LIST_SHORTCUTS,
	LIST_KEYNAME,    // the key in a keyboard shortcut
	LIST_MENU,       // START menu
	LIST_SEARCH,     // actions, apps and Shortcuts that match what was typed
	LIST_LOOK,       // "How it looks": a key's name, icon and color, after picking what it does
	LIST_GLYPH,      // the icon for it
	LIST_COLOR,      // the color for it
	LIST_PAD_SETUP,  // the controller: its preset, and the way to change buttons
	LIST_PAD_CONTROLS,  // "Change a button": which one
	LIST_PAD_KEY,    // what that button does
	LIST_PAD_STICK,  // what a stick does
} ListPurpose;

typedef enum {
	ROW_PLAIN,
	ROW_HEADER,   // a section title (not pickable); an empty one is just a gap
	ROW_TOGGLE,   // a switch at the right
	ROW_CHEVRON,  // leads somewhere: "›" at the right, with detail text before it
	ROW_CHECK,    // one of several: a check at the right when it's the one
	ROW_SEARCH,   // the search field: a pill with what was typed, or a prompt
} RowStyle;

typedef struct {
	RowStyle style;
	char label[64];
	char sub[64];       // a second line under the label
	char detail[48];    // small text at the right
	char value[192];    // what picking it means
	int id;
	Glyph icon;         // GLYPH_NONE: no tile (unless tag is set)
	char tag[4];        // short text on the tile instead of a glyph: "1", "Aa"
	u32 color;          // the tile
	bool on;            // the switch is on / the check shows
	bool danger;        // red label
	bool swatch;        // a plain tile of color, with nothing on it
	char category[16];  // actions: for the key
	char iconName[24];  // actions: for the key
	int control;        // the controller: a button (or stick) drawn on the tile, PC_ value + 1; 0 = none
	float y, h;         // from list_layout
	bool first, last;   // ends of its card
} Item;

typedef struct {
	ListPurpose purpose;
	char title[48];
	Item *items;
	int count;
	int sel;
	float scroll, height, vel;
	bool loading;
	char empty[96];
	char filter[48];  // catalogs: only rows whose name contains this
	int results;      // catalogs: rows that matched
	int refreshRow;   // the "Refresh from your Mac" row, whose text follows the cache; -1 = none
	float x, w;       // the column the rows are in (to the right of the preview in "How it looks")
} List;

// What the on-screen keyboard was opened for, so its answer knows where to go.
typedef enum { ASK_NONE, ASK_WEB_URL, ASK_KEY_NAME, ASK_PAGE_NAME, ASK_SEARCH, ASK_ADDRESS, ASK_TYPE } Ask;

// ---- state -------------------------------------------------------------------

static Screen s_screen;
static Scan s_scan;
static Pairing s_pair;
static Catalog s_cats[3];  // one per CatalogKind; the ones a list asked for stay until it closes
static int s_wantCats;      // bit per CatalogKind
static List s_list;
static Status s_st;
static Ask s_ask;

// The pages, side by side: 0 is the dashboard, 1..count the pads.
static int s_view;
static float s_slide;      // px the current page is shifted while swiping or sliding into place
static bool s_swiping;     // this touch is a sideways drag
static bool s_swipeAxis;   // this touch has picked an axis
static bool s_editing;
static int s_editPage, s_editSlot = -1;  // the key being changed
static int s_flashSlot = -1;
static u64 s_flashUntil;
static char s_findMessage[128];
static int s_findSel;
static bool s_quit;

// buttons: the focus ring on the pad, moved with the D-pad (for a PS TV, or anyone who'd rather)
static int s_focus;
static bool s_navShown;      // the D-pad was used last, so show where it is
static bool s_okHeld, s_okFired;
static u64 s_okAt;

// keyboard shortcut builder (for a key on the pad, or a controller button when s_keysForPad)
static bool s_mods[4];  // cmd, shift, opt, ctrl
static char s_keyName[24], s_keyLabel[24];
static int s_keysSel;
static bool s_keysForPad;

// the controller
static int s_padControl;     // the button being changed
static bool s_setupFromMenu;  // its setup was opened from the menu (so Back goes there)
static SceTouchData s_frontTouch;

// touch tracking
static bool s_touching, s_dragging, s_longFired;
static int s_touchX, s_touchY, s_startX, s_startY, s_lastY;
static u64 s_touchAt;
static float s_scrollStart;

// artwork + app and website icons
static Picture s_art;
#define ICON_SLOTS (MAX_PAGES * SLOTS + 8)  // every key on every page, plus a few
typedef struct {
	char id[128];
	int state;     // 1 asked, 2 ready, 3 none, 4 the Mac couldn't be reached: asked again after retryAt
	u64 retryAt;
	Picture pic;
} Icon;
static Icon s_icons[ICON_SLOTS];
static bool s_wasOnline;

// ---- input -------------------------------------------------------------------

enum {
	B_UP = 1 << 0, B_DOWN = 1 << 1, B_LEFT = 1 << 2, B_RIGHT = 1 << 3,
	B_OK = 1 << 4, B_BACK = 1 << 5, B_SQUARE = 1 << 6, B_TRIANGLE = 1 << 7,
	B_L = 1 << 8, B_R = 1 << 9, B_START = 1 << 10, B_SELECT = 1 << 11,
};
#define B_DIRS (B_UP | B_DOWN | B_LEFT | B_RIGHT)

typedef struct {
	u32 down, held, up;
	int ly, ry;  // sticks, -128..127, 0 in the middle (with a dead zone)
	u32 raw;     // the SCE_CTRL_ bits as they are, ✕ always ✕ (for the controller)
	u8 lx, rawLy, rx, rawRy;  // the sticks as they are, 0..255
} Input;

static u32 s_prevButtons;
static u64 s_repeatAt;

static void input_read(Input *in) {
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	sceCtrlPeekBufferPositive(0, &pad, 1);
	bool circleOk = enter_is_circle();
	u32 b = 0, raw = pad.buttons;
	if (raw & SCE_CTRL_UP) b |= B_UP;
	if (raw & SCE_CTRL_DOWN) b |= B_DOWN;
	if (raw & SCE_CTRL_LEFT) b |= B_LEFT;
	if (raw & SCE_CTRL_RIGHT) b |= B_RIGHT;
	if (raw & SCE_CTRL_CROSS) b |= circleOk ? B_BACK : B_OK;
	if (raw & SCE_CTRL_CIRCLE) b |= circleOk ? B_OK : B_BACK;
	if (raw & SCE_CTRL_SQUARE) b |= B_SQUARE;
	if (raw & SCE_CTRL_TRIANGLE) b |= B_TRIANGLE;
	if (raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) b |= B_L;
	if (raw & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) b |= B_R;
	if (raw & SCE_CTRL_START) b |= B_START;
	if (raw & SCE_CTRL_SELECT) b |= B_SELECT;
	in->held = b;
	in->down = b & ~s_prevButtons;
	in->up = s_prevButtons & ~b;
	// a held direction repeats, for running down a list
	u64 now = now_ms();
	if (in->down & B_DIRS) s_repeatAt = now + 380;
	else if ((b & B_DIRS) && now >= s_repeatAt) {
		in->down |= b & B_DIRS;
		s_repeatAt = now + 70;
	}
	s_prevButtons = b;
	int ly = pad.ly - 128, ry = pad.ry - 128;
	in->ly = abs(ly) > 30 ? ly : 0;
	in->ry = abs(ry) > 30 ? ry : 0;
	in->raw = raw;
	in->lx = pad.lx;
	in->rawLy = pad.ly;
	in->rx = pad.rx;
	in->rawRy = pad.ry;
}

// ---- helpers -------------------------------------------------------------------

static bool hit(int tx, int ty, float x, float y, float w, float h) {
	return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void copy(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

// ASCII upper case, for section titles.
static void upper(char *dst, size_t size, const char *src) {
	size_t i = 0;
	for (; src[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)src[i]);
	dst[i] = 0;
}

static u32 category_color(const char *cat) {
	if (!strcmp(cat, "media")) return C_GREEN;
	if (!strcmp(cat, "system")) return C_TEAL;
	if (!strcmp(cat, "windows")) return C_YELLOW;
	if (!strcmp(cat, "browser")) return C_BLUE;
	if (!strcmp(cat, "editing")) return C_ORANGE;
	if (!strcmp(cat, "accessibility")) return C_INDIGO;
	if (!strcmp(cat, "mouse")) return C_PINK;
	return C_PURPLE;
}

static int views(void) { return g_pads.count + 1; }
static int pad_index(void) { return s_view - 1; }  // -1 on the dashboard

// The slot for an icon id, or a free one for it (NULL when they're all taken).
static Icon *icon_slot(const char *id) {
	Icon *free_slot = NULL;
	for (int i = 0; i < ICON_SLOTS; i++) {
		if (s_icons[i].state && !strcmp(s_icons[i].id, id)) return &s_icons[i];
		if (!s_icons[i].state && !free_slot) free_slot = &s_icons[i];
	}
	if (free_slot) copy(free_slot->id, sizeof free_slot->id, id);
	return free_slot;
}

// The icon for a key, asked for the first time it's wanted.
static Icon *icon_for(KeyKind kind, const char *ref) {
	char id[128];
	icon_id(kind, ref, id, sizeof id);
	if (!id[0]) return NULL;
	Icon *icon = icon_slot(id);
	if (icon && (!icon->state || (icon->state == 4 && now_ms() >= icon->retryAt))) {
		icon->state = 1;
		icon_request(id, false);
	}
	return icon;
}

static void icons_collect(void) {
	char id[128];
	u8 *px;
	int w, h;
	bool retry;
	while (icon_take(id, sizeof id, &px, &w, &h, &retry)) {
		Icon *icon = icon_slot(id);
		if (icon) {
			if (px) {
				icon->state = picture_load(&icon->pic, px, w, h) ? 2 : 3;
			} else if (icon->state != 2) {  // (a newer one that didn't come keeps the old one showing)
				icon->state = retry ? 4 : 3;
				icon->retryAt = now_ms() + 15000;
			}
		}
		free(px);
	}
}

// Asks for every app and website icon on the pad, so they're there before a page is shown. The
// current page's go last: the icon thread serves the newest request first. Cheap once they're in.
// fresh fetches them all again from the Mac (the menu's Refresh).
static void icons_prefetch(bool fresh) {
	int cur = pad_index() < 0 ? 0 : pad_index();
	for (int n = g_pads.count - 1; n >= 0; n--) {
		PadPage *p = &g_pads.pages[(cur + n) % g_pads.count];
		for (int slot = 0; slot < SLOTS; slot++) {
			PadKey *k = &p->keys[slot];
			if (k->kind != KEY_APP && k->kind != KEY_WEB) continue;
			Icon *icon = icon_for(k->kind, k->ref);
			if (icon && fresh && icon->state != 1) icon_request(icon->id, true);
		}
	}
}

// Icons that couldn't be fetched get another go once the Mac is back.
static void icons_retry(void) {
	for (int i = 0; i < ICON_SLOTS; i++) if (s_icons[i].state >= 3) s_icons[i].state = 0;
	icons_prefetch(false);
}

// Opens the system keyboard; its answer comes back to ask_done.
static void ask(Ask purpose, const char *title, const char *initial, ImeType type, int maxLen) {
	if (ime_open(title, initial, type, maxLen)) s_ask = purpose;
}

// Whether hay contains needle, ignoring case.
static bool contains_ci(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	if (!n) return true;
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
		if (i == n) return true;
	}
	return false;
}

// " text " -> "text", in place.
static char *trim(char *s) {
	while (*s == ' ') s++;
	for (char *end = s + strlen(s); end > s && end[-1] == ' '; ) *--end = 0;
	return s;
}

static PadKey *edit_key(void) { return &g_pads.pages[s_editPage].keys[s_editSlot]; }

// ---- which icon an action gets -----------------------------------------------------

// The built-in actions, by id.
static const struct { const char *id; Glyph g; } s_actionGlyphs[] = {
	{"next-track", GLYPH_NEXT}, {"previous-track", GLYPH_PREV},
	{"volume-up", GLYPH_VOL_UP}, {"volume-down", GLYPH_VOL_DOWN}, {"mute", GLYPH_MUTE},
	{"emoji", GLYPH_SMILE}, {"spotlight", GLYPH_SEARCH}, {"screenshot-ui", GLYPH_CAMERA},
	{"screenshot-area", GLYPH_CROP}, {"screenshot-screen", GLYPH_MONITOR}, {"mission-control", GLYPH_GRID},
	{"app-windows", GLYPH_WINDOWS}, {"lock-screen", GLYPH_LOCK},
	{"minimize", GLYPH_MINIMIZE}, {"fullscreen", GLYPH_EXPAND}, {"close-window", GLYPH_CLOSE},
	{"next-window", GLYPH_WINDOWS}, {"hide-app", GLYPH_MINUS_CIRCLE}, {"quit-app", GLYPH_POWER},
	{"reload", GLYPH_REFRESH}, {"hard-reload", GLYPH_REFRESH}, {"new-tab", GLYPH_TAB}, {"close-tab", GLYPH_CLOSE},
	{"reopen-tab", GLYPH_UNDO}, {"next-tab", GLYPH_ARROW_RIGHT}, {"previous-tab", GLYPH_ARROW_LEFT},
	{"address-bar", GLYPH_ADDRESS},
	{"copy", GLYPH_COPY}, {"paste", GLYPH_PASTE}, {"cut", GLYPH_CUT}, {"undo", GLYPH_UNDO}, {"redo", GLYPH_REDO},
	{"select-all", GLYPH_SELECT_ALL}, {"find", GLYPH_SEARCH}, {"save", GLYPH_SAVE},
	{"window-left", GLYPH_WIN_LEFT}, {"window-right", GLYPH_WIN_RIGHT}, {"window-top", GLYPH_WIN_TOP},
	{"window-bottom", GLYPH_WIN_BOTTOM}, {"window-top-left", GLYPH_WIN_TL}, {"window-top-right", GLYPH_WIN_TR},
	{"window-bottom-left", GLYPH_WIN_BL}, {"window-bottom-right", GLYPH_WIN_BR}, {"window-maximize", GLYPH_WIN_FILL},
	{"window-center", GLYPH_WIN_CENTER}, {"window-next-display", GLYPH_MONITOR}, {"window-restore", GLYPH_UNDO},
	{"keep-awake", GLYPH_COFFEE}, {"jiggler", GLYPH_MOUSE}, {"siri", GLYPH_MIC},
	{"brightness-up", GLYPH_SUN}, {"brightness-down", GLYPH_SUN_LOW}, {"dark-mode", GLYPH_MOON},
	{"sleep-display", GLYPH_MONITOR}, {"sleep", GLYPH_MOON}, {"screen-saver", GLYPH_MONITOR},
	{"show-desktop", GLYPH_WINDOWS}, {"force-quit", GLYPH_CLOSE},
	{"accessibility-shortcuts", GLYPH_PERSON}, {"voiceover", GLYPH_PERSON}, {"zoom", GLYPH_ZOOM_IN},
	{"zoom-in", GLYPH_ZOOM_IN}, {"zoom-out", GLYPH_ZOOM_OUT}, {"invert-colors", GLYPH_INVERT},
	{"speak-selection", GLYPH_VOL_UP}, {"left-click", GLYPH_CLICK_LEFT}, {"right-click", GLYPH_CLICK_RIGHT},
	{"wifi", GLYPH_WIFI}, {"bluetooth", GLYPH_BLUETOOTH}, {"stage-manager", GLYPH_STAGE}, {"hidden-files", GLYPH_EYE},
	{"empty-trash", GLYPH_TRASH}, {"eject-disks", GLYPH_EJECT}, {"restart", GLYPH_REFRESH}, {"shut-down", GLYPH_POWER},
};

// Material Symbols names the Mac suggests, for actions made there.
static const struct { const char *name; Glyph g; } s_iconGlyphs[] = {
	{"play_pause", GLYPH_PLAYPAUSE}, {"play_arrow", GLYPH_PLAY}, {"pause", GLYPH_PAUSE},
	{"skip_next", GLYPH_NEXT}, {"skip_previous", GLYPH_PREV},
	{"volume_up", GLYPH_VOL_UP}, {"volume_down", GLYPH_VOL_DOWN}, {"volume_off", GLYPH_MUTE}, {"volume_mute", GLYPH_MUTE},
	{"mood", GLYPH_SMILE}, {"sentiment_satisfied", GLYPH_SMILE}, {"search", GLYPH_SEARCH},
	{"photo_camera", GLYPH_CAMERA}, {"camera", GLYPH_CAMERA}, {"screenshot", GLYPH_CROP}, {"crop", GLYPH_CROP},
	{"screenshot_monitor", GLYPH_MONITOR}, {"desktop_mac", GLYPH_MONITOR}, {"monitor", GLYPH_MONITOR},
	{"grid_view", GLYPH_GRID}, {"apps", GLYPH_GRID}, {"dashboard", GLYPH_GRID}, {"web_asset", GLYPH_WINDOWS},
	{"lock", GLYPH_LOCK}, {"lock_open", GLYPH_LOCK},
	{"arrow_downward", GLYPH_MINIMIZE}, {"open_in_new", GLYPH_EXPAND}, {"fullscreen", GLYPH_EXPAND},
	{"close", GLYPH_CLOSE}, {"cancel", GLYPH_CLOSE}, {"refresh", GLYPH_REFRESH}, {"sync", GLYPH_REFRESH},
	{"remove_circle_outline", GLYPH_MINUS_CIRCLE}, {"remove_circle", GLYPH_MINUS_CIRCLE}, {"visibility_off", GLYPH_MINUS_CIRCLE},
	{"power_settings_new", GLYPH_POWER}, {"power", GLYPH_POWER}, {"logout", GLYPH_POWER},
	{"tab", GLYPH_TAB}, {"add_box", GLYPH_TAB}, {"folder", GLYPH_TAB},
	{"undo", GLYPH_UNDO}, {"redo", GLYPH_REDO}, {"arrow_forward", GLYPH_ARROW_RIGHT}, {"arrow_back", GLYPH_ARROW_LEFT},
	{"link", GLYPH_ADDRESS}, {"language", GLYPH_GLOBE}, {"public", GLYPH_GLOBE},
	{"content_copy", GLYPH_COPY}, {"content_paste", GLYPH_PASTE}, {"content_cut", GLYPH_CUT},
	{"select_all", GLYPH_SELECT_ALL}, {"save_alt", GLYPH_SAVE}, {"save", GLYPH_SAVE},
	{"keyboard", GLYPH_KEYS}, {"schedule", GLYPH_CLOCK}, {"laptop_mac", GLYPH_LAPTOP}, {"edit", GLYPH_PENCIL},
	{"delete", GLYPH_TRASH}, {"add", GLYPH_PLUS}, {"check", GLYPH_CHECK}, {"bolt", GLYPH_BOLT}, {"flash_on", GLYPH_BOLT},
	{"terminal", GLYPH_KEYS}, {"shortcut", GLYPH_SHORTCUT}, {"app", GLYPH_APP}, {"home", GLYPH_HOME}, {"menu", GLYPH_MENU},
	{"splitscreen_left", GLYPH_WIN_LEFT}, {"splitscreen_right", GLYPH_WIN_RIGHT}, {"splitscreen_top", GLYPH_WIN_TOP},
	{"splitscreen_bottom", GLYPH_WIN_BOTTOM}, {"north_west", GLYPH_WIN_TL}, {"north_east", GLYPH_WIN_TR},
	{"south_west", GLYPH_WIN_BL}, {"south_east", GLYPH_WIN_BR}, {"fit_screen", GLYPH_WIN_FILL},
	{"filter_center_focus", GLYPH_WIN_CENTER}, {"restore", GLYPH_UNDO}, {"history", GLYPH_UNDO},
	{"coffee", GLYPH_COFFEE}, {"local_cafe", GLYPH_COFFEE}, {"mouse", GLYPH_MOUSE}, {"mic", GLYPH_MIC},
	{"brightness_high", GLYPH_SUN}, {"brightness_low", GLYPH_SUN_LOW}, {"light_mode", GLYPH_SUN},
	{"dark_mode", GLYPH_MOON}, {"bedtime", GLYPH_MOON}, {"nightlight", GLYPH_MOON},
	{"desktop_access_disabled", GLYPH_MONITOR}, {"wallpaper", GLYPH_MONITOR}, {"desktop_windows", GLYPH_WINDOWS},
	{"accessibility_new", GLYPH_PERSON}, {"accessibility", GLYPH_PERSON}, {"record_voice_over", GLYPH_PERSON},
	{"zoom_in", GLYPH_ZOOM_IN}, {"zoom_out", GLYPH_ZOOM_OUT}, {"invert_colors", GLYPH_INVERT},
	{"text_to_speech", GLYPH_VOL_UP}, {"left_click", GLYPH_CLICK_LEFT}, {"right_click", GLYPH_CLICK_RIGHT},
	{"sports_esports", GLYPH_GAMEPAD}, {"gamepad", GLYPH_GAMEPAD},
	{"wifi", GLYPH_WIFI}, {"bluetooth", GLYPH_BLUETOOTH}, {"eject", GLYPH_EJECT}, {"visibility", GLYPH_EYE},
	{"restart_alt", GLYPH_REFRESH}, {"view_sidebar", GLYPH_STAGE},
};

static Glyph glyph_for_icon(const char *name) {
	for (size_t i = 0; i < sizeof s_iconGlyphs / sizeof *s_iconGlyphs; i++)
		if (!strcmp(name, s_iconGlyphs[i].name)) return s_iconGlyphs[i].g;
	return GLYPH_NONE;
}

static Glyph action_glyph(const char *id, const char *icon) {
	if (!strcmp(id, "play-pause")) return s_st.hasTrack && s_st.playing ? GLYPH_PAUSE : GLYPH_PLAY;
	for (size_t i = 0; i < sizeof s_actionGlyphs / sizeof *s_actionGlyphs; i++)
		if (!strcmp(id, s_actionGlyphs[i].id)) return s_actionGlyphs[i].g;
	Glyph g = icon ? glyph_for_icon(icon) : GLYPH_NONE;
	return g != GLYPH_NONE ? g : GLYPH_BOLT;
}

// ---- how a key looks ---------------------------------------------------------------

// The icons a key can be given in "How it looks": the name saved in pads.json, the list's label, the group.
typedef struct { const char *id, *label; Glyph g; int group; } GlyphChoice;
static const GlyphChoice s_glyphChoices[] = {
	{"play_arrow", "Play", GLYPH_PLAY, 0}, {"pause", "Pause", GLYPH_PAUSE, 0}, {"play_pause", "Play / Pause", GLYPH_PLAYPAUSE, 0},
	{"skip_next", "Next", GLYPH_NEXT, 0}, {"skip_previous", "Previous", GLYPH_PREV, 0},
	{"volume_up", "Volume up", GLYPH_VOL_UP, 0}, {"volume_down", "Volume down", GLYPH_VOL_DOWN, 0}, {"volume_off", "Mute", GLYPH_MUTE, 0},
	{"mood", "Smile", GLYPH_SMILE, 1}, {"search", "Search", GLYPH_SEARCH, 1}, {"photo_camera", "Camera", GLYPH_CAMERA, 1},
	{"crop", "Crop", GLYPH_CROP, 1}, {"monitor", "Screen", GLYPH_MONITOR, 1}, {"grid_view", "Grid", GLYPH_GRID, 1},
	{"web_asset", "Windows", GLYPH_WINDOWS, 1}, {"lock", "Lock", GLYPH_LOCK, 1}, {"power", "Power", GLYPH_POWER, 1},
	{"laptop_mac", "Laptop", GLYPH_LAPTOP, 1}, {"schedule", "Clock", GLYPH_CLOCK, 1}, {"keyboard", "Keyboard", GLYPH_KEYS, 1},
	{"app", "App", GLYPH_APP, 1}, {"home", "Home", GLYPH_HOME, 1},
	{"mic", "Microphone", GLYPH_MIC, 1}, {"brightness_high", "Sun", GLYPH_SUN, 1}, {"dark_mode", "Moon", GLYPH_MOON, 1},
	{"coffee", "Coffee", GLYPH_COFFEE, 1}, {"mouse", "Mouse", GLYPH_MOUSE, 1}, {"accessibility_new", "Person", GLYPH_PERSON, 1},
	{"zoom_in", "Zoom in", GLYPH_ZOOM_IN, 1}, {"sports_esports", "Controller", GLYPH_GAMEPAD, 1},
	{"wifi", "Wi-Fi", GLYPH_WIFI, 1}, {"bluetooth", "Bluetooth", GLYPH_BLUETOOTH, 1}, {"eject", "Eject", GLYPH_EJECT, 1},
	{"visibility", "Eye", GLYPH_EYE, 1}, {"view_sidebar", "Stage Manager", GLYPH_STAGE, 1},
	{"arrow_downward", "Minimize", GLYPH_MINIMIZE, 2}, {"fullscreen", "Expand", GLYPH_EXPAND, 2}, {"close", "Close", GLYPH_CLOSE, 2},
	{"refresh", "Refresh", GLYPH_REFRESH, 2}, {"remove_circle", "Minus", GLYPH_MINUS_CIRCLE, 2}, {"tab", "Tab", GLYPH_TAB, 2},
	{"arrow_back", "Arrow left", GLYPH_ARROW_LEFT, 2}, {"arrow_forward", "Arrow right", GLYPH_ARROW_RIGHT, 2},
	{"link", "Address bar", GLYPH_ADDRESS, 2}, {"language", "Globe", GLYPH_GLOBE, 2},
	{"splitscreen_left", "Left half", GLYPH_WIN_LEFT, 2}, {"splitscreen_right", "Right half", GLYPH_WIN_RIGHT, 2},
	{"fit_screen", "Fill screen", GLYPH_WIN_FILL, 2}, {"filter_center_focus", "Centered", GLYPH_WIN_CENTER, 2},
	{"content_copy", "Copy", GLYPH_COPY, 3}, {"content_paste", "Paste", GLYPH_PASTE, 3}, {"content_cut", "Cut", GLYPH_CUT, 3},
	{"select_all", "Select all", GLYPH_SELECT_ALL, 3}, {"undo", "Undo", GLYPH_UNDO, 3}, {"redo", "Redo", GLYPH_REDO, 3},
	{"save", "Save", GLYPH_SAVE, 3}, {"edit", "Pencil", GLYPH_PENCIL, 3}, {"delete", "Trash", GLYPH_TRASH, 3},
	{"add", "Plus", GLYPH_PLUS, 3}, {"check", "Check", GLYPH_CHECK, 3}, {"bolt", "Bolt", GLYPH_BOLT, 3},
	{"shortcut", "Shortcut", GLYPH_SHORTCUT, 3}, {"menu", "Menu", GLYPH_MENU, 3},
};
static const char *s_glyphGroups[] = {"Media", "System", "Windows and browser", "Editing and more"};

static const struct { const char *id, *label; } s_colorChoices[] = {
	{"blue", "Blue"}, {"green", "Green"}, {"orange", "Orange"}, {"red", "Red"}, {"purple", "Purple"},
	{"yellow", "Yellow"}, {"teal", "Teal"}, {"pink", "Pink"}, {"gray", "Gray"},
};

static const GlyphChoice *glyph_choice(const char *id) {
	if (!id[0]) return NULL;
	for (size_t i = 0; i < sizeof s_glyphChoices / sizeof *s_glyphChoices; i++)
		if (!strcmp(id, s_glyphChoices[i].id)) return &s_glyphChoices[i];
	return NULL;
}

static const char *glyph_choice_label(const char *id) {
	const GlyphChoice *c = glyph_choice(id);
	return c ? c->label : "Automatic";
}

// 0 for a name that isn't one of the choices.
static u32 color_from(const char *id) {
	if (!strcmp(id, "blue")) return C_BLUE;
	if (!strcmp(id, "green")) return C_GREEN;
	if (!strcmp(id, "orange")) return C_ORANGE;
	if (!strcmp(id, "red")) return C_RED;
	if (!strcmp(id, "purple")) return C_PURPLE;
	if (!strcmp(id, "yellow")) return C_YELLOW;
	if (!strcmp(id, "teal")) return C_TEAL;
	if (!strcmp(id, "pink")) return C_PINK;
	if (!strcmp(id, "gray")) return C_GRAY;
	return 0;
}

static const char *color_choice_label(const char *id) {
	for (size_t i = 0; i < sizeof s_colorChoices / sizeof *s_colorChoices; i++)
		if (!strcmp(id, s_colorChoices[i].id)) return s_colorChoices[i].label;
	return "Automatic";
}

// The key's color: the one picked, else one for what it does.
static u32 key_accent(const PadKey *k) {
	u32 c = color_from(k->color);
	if (c) return c;
	switch (k->kind) {
	case KEY_ACTION: return category_color(k->category);
	case KEY_APP: return C_BLUE;
	case KEY_WEB: return C_TEAL;
	case KEY_KEYS: return C_ORANGE;
	case KEY_SHORTCUT: return C_PURPLE;
	default: return C_GRAY;
	}
}

// The glyph a key shows on its own (apps and websites show their own icon instead when they have one).
static Glyph kind_glyph(const PadKey *k) {
	switch (k->kind) {
	case KEY_ACTION: return action_glyph(k->ref, k->icon);
	case KEY_APP: return GLYPH_APP;
	case KEY_WEB: return GLYPH_GLOBE;
	case KEY_KEYS: return GLYPH_KEYS;
	case KEY_SHORTCUT: return GLYPH_SHORTCUT;
	default: return GLYPH_NONE;
	}
}

static const char *s_kindTitles[] = {"", "Mac action", "App", "Website", "Keyboard shortcut", "Shortcut"};

static void key_tile(const PadKey *k, float x, float y, bool flash);

// ---- lists -----------------------------------------------------------------------

static void list_open(ListPurpose purpose, const char *title) {
	free(s_list.items);
	memset(&s_list, 0, sizeof s_list);
	s_list.purpose = purpose;
	s_list.refreshRow = -1;
	s_list.x = LIST_X;
	s_list.w = LIST_W;
	copy(s_list.title, sizeof s_list.title, title);
	s_screen = SCR_LIST;
}

static Item *list_add(RowStyle style, const char *label, const char *value, int id) {
	Item *grown = realloc(s_list.items, (s_list.count + 1) * sizeof(Item));
	if (!grown) {  // out of memory: a row that's never shown, rather than a crash
		static Item spare;
		memset(&spare, 0, sizeof spare);
		return &spare;
	}
	s_list.items = grown;
	Item *it = &s_list.items[s_list.count++];
	memset(it, 0, sizeof *it);
	it->style = style;
	copy(it->label, sizeof it->label, label);
	copy(it->value, sizeof it->value, value);
	it->id = id;
	return it;
}

// A section title, in capitals. An empty one is a gap between cards.
static void list_header(const char *title) {
	char up[64];
	upper(up, sizeof up, title);
	list_add(ROW_HEADER, up, "", 0);
}

static bool pickable(int i) { return i >= 0 && i < s_list.count && s_list.items[i].style != ROW_HEADER; }

static float list_view(void) { return SCREEN_H - BAR_H; }

static float list_max_scroll(void) {
	float h = s_list.height - list_view();
	return h > 0 ? h : 0;
}

static void list_clamp(void) {
	if (s_list.scroll < 0) s_list.scroll = 0;
	if (s_list.scroll > list_max_scroll()) s_list.scroll = list_max_scroll();
}

// Works out where every row sits, and settles the selection on a row that can be picked.
static void list_layout(void) {
	float y = 14;
	bool inCard = false;
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (it->style == ROW_HEADER) {
			it->y = y;
			it->h = it->label[0] ? HEAD_H + (inCard ? 6 : 0) : BREAK_H;
			y += it->h;
			inCard = false;
			continue;
		}
		if (it->style == ROW_SEARCH) {  // a pill of its own
			it->first = it->last = true;
			it->y = y;
			it->h = ROW_H;
			y += it->h;
			inCard = false;
			continue;
		}
		it->first = !inCard;
		it->last = true;
		if (inCard) s_list.items[i - 1].last = false;
		it->y = y;
		it->h = it->sub[0] ? ROW2_H : ROW_H;
		y += it->h;
		inCard = true;
	}
	s_list.height = y + 24;
	list_clamp();
	if (!pickable(s_list.sel)) {
		// the next row that can be picked, else the first
		int i = s_list.sel < 0 ? 0 : s_list.sel;
		while (i < s_list.count && !pickable(i)) i++;
		if (i >= s_list.count) for (i = 0; i < s_list.count && !pickable(i); i++) {}
		s_list.sel = i;
	}
}

static void list_show_sel(void) {
	if (!pickable(s_list.sel)) return;
	Item *it = &s_list.items[s_list.sel];
	float top = it->y - (it->first ? HEAD_H : 0) - 6, bottom = it->y + it->h + 14;
	if (top < s_list.scroll) s_list.scroll = top;
	if (bottom > s_list.scroll + list_view()) s_list.scroll = bottom - list_view();
	list_clamp();
}

// Moves the selection by step rows that can be picked.
static void list_move(int step) {
	int dir = step < 0 ? -1 : 1, i = s_list.sel;
	for (int n = abs(step); n > 0; n--) {
		int j = i + dir;
		while (j >= 0 && j < s_list.count && !pickable(j)) j += dir;
		if (j < 0 || j >= s_list.count) break;
		i = j;
	}
	s_list.sel = i;
	s_list.vel = 0;
	list_show_sel();
}

static void menu_build(void);
static void menu_switches_update(void);
static void clock_text(char *out, size_t size, const struct tm *tm);
static void list_search(void);
static void start_controller(void);
static void resume_controller(void);
static void start_type(void);
static void open_pad_setup(bool fromMenu);
static void open_pad_controls(int select);
static void open_pad_key_list(int control);

static bool is_catalog(void) {
	return s_list.purpose == LIST_ACTIONS || s_list.purpose == LIST_APPS || s_list.purpose == LIST_SHORTCUTS;
}

// Lists with a search field and the magnifier in the title bar.
static bool searchable(void) { return is_catalog() || s_list.purpose == LIST_KIND || s_list.purpose == LIST_SEARCH; }

static void catalogs_finish(void) {
	for (int k = 0; k < 3; k++) catalog_finish(&s_cats[k]);
	s_wantCats = 0;
}

// The field at the top of a list. Shows the query when there is one, else what it searches.
static void add_search_row(const char *prompt) {
	Item *it = list_add(ROW_SEARCH, s_list.filter[0] ? s_list.filter : prompt, "search", -1);
	it->on = s_list.filter[0] != 0;
	list_header("");
}

// The editor's first question.
static void open_kind_menu(void) {
	PadKey *k = edit_key();
	list_open(LIST_KIND, k->kind ? k->label : "New key");
	add_search_row("Search actions, apps, Shortcuts");
	Item *it;
	it = list_add(ROW_CHEVRON, "Mac action", "", KEY_ACTION);
	copy(it->sub, sizeof it->sub, "Media, windows, accessibility and more");
	it->icon = GLYPH_BOLT;
	it->color = C_GREEN;
	it = list_add(ROW_CHEVRON, "Open an app", "", KEY_APP);
	copy(it->sub, sizeof it->sub, "Any app on your Mac, with its icon");
	it->icon = GLYPH_APP;
	it->color = C_BLUE;
	it = list_add(ROW_CHEVRON, "Open a website", "", KEY_WEB);
	copy(it->sub, sizeof it->sub, "In your Mac's browser, with the site's icon");
	it->icon = GLYPH_GLOBE;
	it->color = C_TEAL;
	it = list_add(ROW_CHEVRON, "Keyboard shortcut", "", KEY_KEYS);
	copy(it->sub, sizeof it->sub, "Like Cmd + Shift + T");
	it->icon = GLYPH_KEYS;
	it->color = C_ORANGE;
	it = list_add(ROW_CHEVRON, "Run a Shortcut", "", KEY_SHORTCUT);
	copy(it->sub, sizeof it->sub, "From the Shortcuts app");
	it->icon = GLYPH_SHORTCUT;
	it->color = C_PURPLE;
	if (k->kind) {
		list_header("This key");
		it = list_add(ROW_CHEVRON, "How it looks", "", 100);
		copy(it->detail, sizeof it->detail, k->label);
		it->icon = GLYPH_PENCIL;
		it->color = C_GRAY;
		it = list_add(ROW_PLAIN, "Remove this key", "", 101);
		it->icon = GLYPH_TRASH;
		it->color = C_RED;
		it->danger = true;
	}
	list_layout();
}

static void list_fill_catalog(void);

// "How it looks": the key as it will show (at the left), then its name, icon and color. A key
// lands here right after what it does is picked, and from "This key" in the long-press menu.
static void open_look(void) {
	PadKey *k = edit_key();
	list_open(LIST_LOOK, "How it looks");
	s_list.x = 360;
	s_list.w = SCREEN_W - 360 - 48;
	Item *it;
	it = list_add(ROW_CHEVRON, "Name", "name", 0);
	copy(it->detail, sizeof it->detail, k->label);
	it->icon = GLYPH_PENCIL;
	it->color = C_GRAY;
	it = list_add(ROW_CHEVRON, "Icon", "icon", 0);
	copy(it->detail, sizeof it->detail, glyph_choice_label(k->glyph));
	const GlyphChoice *chosen = glyph_choice(k->glyph);
	it->icon = chosen ? chosen->g : kind_glyph(k);
	it->color = key_accent(k);
	it = list_add(ROW_CHEVRON, "Color", "color", 0);
	copy(it->detail, sizeof it->detail, color_choice_label(k->color));
	it->swatch = true;
	it->color = key_accent(k);
	list_header("");
	it = list_add(ROW_CHEVRON, "What it does", "kind", 0);
	copy(it->detail, sizeof it->detail, s_kindTitles[k->kind]);
	it->icon = GLYPH_BOLT;
	it->color = C_GREEN;
	it = list_add(ROW_PLAIN, "Remove this key", "remove", 0);
	it->icon = GLYPH_TRASH;
	it->color = C_RED;
	it->danger = true;
	list_header("");
	it = list_add(ROW_PLAIN, "Done", "done", 0);
	it->icon = GLYPH_CHECK;
	it->color = C_BLUE;
	list_layout();
}

// Starts on the row that's checked.
static void list_select_checked(void) {
	for (int i = 0; i < s_list.count; i++) if (s_list.items[i].style == ROW_CHECK && s_list.items[i].on) { s_list.sel = i; break; }
	list_show_sel();
}

static void open_glyph_list(void) {
	PadKey *k = edit_key();
	u32 accent = key_accent(k);
	list_open(LIST_GLYPH, "Icon");
	Item *it = list_add(ROW_CHECK, "Automatic", "", 0);
	copy(it->sub, sizeof it->sub, "The app's or site's own, or one for the action");
	it->icon = kind_glyph(k);
	it->color = accent;
	it->on = !k->glyph[0];
	for (int g = 0; g < 4; g++) {
		list_header(s_glyphGroups[g]);
		for (size_t i = 0; i < sizeof s_glyphChoices / sizeof *s_glyphChoices; i++) {
			const GlyphChoice *c = &s_glyphChoices[i];
			if (c->group != g) continue;
			it = list_add(ROW_CHECK, c->label, c->id, 0);
			it->icon = c->g;
			it->color = accent;
			it->on = !strcmp(k->glyph, c->id);
		}
	}
	list_layout();
	list_select_checked();
}

static void open_color_list(void) {
	PadKey *k = edit_key();
	PadKey plain = *k;
	plain.color[0] = 0;
	list_open(LIST_COLOR, "Color");
	Item *it = list_add(ROW_CHECK, "Automatic", "", 0);
	copy(it->sub, sizeof it->sub, "One for what the key does");
	it->swatch = true;
	it->color = key_accent(&plain);
	it->on = !k->color[0];
	list_header("");
	for (size_t i = 0; i < sizeof s_colorChoices / sizeof *s_colorChoices; i++) {
		it = list_add(ROW_CHECK, s_colorChoices[i].label, s_colorChoices[i].id, 0);
		it->swatch = true;
		it->color = color_from(s_colorChoices[i].id);
		it->on = !strcmp(k->color, s_colorChoices[i].id);
	}
	list_layout();
	list_select_checked();
}

// Which catalog a list shows, or -1 for all three.
static int list_catalog(void) {
	return s_list.purpose == LIST_ACTIONS ? CATALOG_ACTIONS : s_list.purpose == LIST_APPS ? CATALOG_APPS
	     : s_list.purpose == LIST_SHORTCUTS ? CATALOG_SHORTCUTS : -1;
}

// The rows once every catalog the list wants is in; until then, "Asking your Mac…".
static void list_catalogs_ready(void) {
	bool done = true;
	for (int k = 0; k < 3; k++) if ((s_wantCats >> k & 1) && !s_cats[k].done) done = false;
	s_list.loading = !done;
	if (done) list_fill_catalog();
}

// Usually from the cache, so the rows are there at once.
static void open_catalog(ListPurpose purpose, CatalogKind kind, const char *title) {
	list_open(purpose, title);
	s_wantCats = 1 << kind;
	catalog_start(&s_cats[kind], kind);
	list_catalogs_ready();
}

// Everything that matches query, from all three catalogs.
static void open_search(const char *query) {
	list_open(LIST_SEARCH, "Search");
	copy(s_list.filter, sizeof s_list.filter, query);
	s_wantCats = 7;
	for (int k = 0; k < 3; k++) catalog_start(&s_cats[k], k);
	list_catalogs_ready();
}

// "Updated 3 min ago", "Refreshing…", or what went wrong, for one catalog or all three (kind -1).
static void refresh_detail(int kind, char *out, size_t size) {
	bool fetching = false, failed = false;
	s64 age = -1;
	char error[96] = "";
	for (int k = 0; k < 3; k++) {
		if (kind >= 0 && k != kind) continue;
		CatalogInfo info;
		catalog_info(k, &info);
		fetching |= info.fetching;
		if (info.failed && !failed) {
			failed = true;
			copy(error, sizeof error, info.error);
		}
		if (info.ageSec > age) age = info.ageSec;  // the oldest of them
	}
	if (fetching) copy(out, size, "Refreshing\xE2\x80\xA6");
	else if (failed) copy(out, size, error);
	else if (age < 0) copy(out, size, "Not yet");
	else if (age < 60) copy(out, size, "Updated just now");
	else if (age < 3600) snprintf(out, size, "Updated %d min ago", (int)(age / 60));
	else if (age < 86400) snprintf(out, size, "Updated %d h ago", (int)(age / 3600));
	else snprintf(out, size, "Updated %d days ago", (int)(age / 86400));
}

// Keeps the Refresh row's text up to date while a list is open.
static void refresh_row_update(void) {
	if (s_list.refreshRow < 0 || s_list.refreshRow >= s_list.count) return;
	Item *it = &s_list.items[s_list.refreshRow];
	if (s_list.purpose == LIST_MENU) {
		char detail[48];
		refresh_detail(-1, detail, sizeof detail);
		snprintf(it->sub, sizeof it->sub, "Lists and icons \xC2\xB7 %s", detail);
	} else {
		refresh_detail(list_catalog(), it->detail, sizeof it->detail);
	}
}

// Asks the Mac for this list's catalogs again (and, from the menu, the pad's icons).
static void refresh_picked(void) {
	catalog_refresh(list_catalog());
	if (s_list.purpose == LIST_MENU) icons_prefetch(true);
	refresh_row_update();
}

static void open_menu(void) {
	list_open(LIST_MENU, "Menu");
	menu_build();
}

// The rows for the Mac's switches follow what the Mac says (a tap shows at once, too).
static void menu_switches_update(void) {
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		bool awake = !strcmp(it->value, "macawake");
		if (!awake && strcmp(it->value, "jiggler")) continue;
		it->on = awake ? s_st.macAwake : s_st.jiggler;
		if (s_st.live && !s_st.hasSwitches) {
			copy(it->sub, sizeof it->sub, "Needs a newer Macapad on your Mac");
		} else if (awake && s_st.macAwake && s_st.macAwakeUntil) {
			time_t until = s_st.macAwakeUntil / 1000 + s_st.utcOffset;
			struct tm tm;
			gmtime_r(&until, &tm);
			char clock[16];
			clock_text(clock, sizeof clock, &tm);
			snprintf(it->sub, sizeof it->sub, "On until %s%s", clock, g_config.clock24 ? "" : tm.tm_hour < 12 ? " AM" : " PM");
		} else {
			copy(it->sub, sizeof it->sub, awake ? "Its screen won't go to sleep" : "Nudges the pointer now and then, so it never looks away");
		}
	}
}

// The START menu. Built again in place when something in it changes, so the scroll stays put.
static void menu_build(void) {
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = 0;
	Item *it;

	list_header("Pages");
	it = list_add(ROW_CHECK, "Dashboard", "page", 0);
	copy(it->sub, sizeof it->sub, "Time, weather, what's playing, your Mac");
	it->icon = GLYPH_HOME;
	it->color = s_view == 0 ? C_BLUE : C_GRAY;
	it->on = s_view == 0;
	for (int i = 0; i < g_pads.count; i++) {
		it = list_add(ROW_CHECK, g_pads.pages[i].name, "page", i + 1);
		snprintf(it->tag, sizeof it->tag, "%d", i + 1);
		it->color = s_view == i + 1 ? C_BLUE : C_GRAY;
		it->on = s_view == i + 1;
	}
	if (g_pads.count < MAX_PAGES) {
		it = list_add(ROW_PLAIN, "Add a page", "addpage", 0);
		it->icon = GLYPH_PLUS;
		it->color = C_GREEN;
	}

	if (pad_index() >= 0) {
		list_header(g_pads.pages[pad_index()].name);
		it = list_add(ROW_TOGGLE, "Edit keys", "edit", 0);
		it->icon = GLYPH_PENCIL;
		it->color = C_ORANGE;
		it->on = s_editing;
		it = list_add(ROW_CHEVRON, "Rename page", "renamepage", 0);
		copy(it->detail, sizeof it->detail, g_pads.pages[pad_index()].name);
		copy(it->tag, sizeof it->tag, "Aa");
		it->color = C_PURPLE;
		if (g_pads.count > 1) {
			if (pad_index() > 0) {
				it = list_add(ROW_PLAIN, "Move page left", "moveleft", 0);
				it->icon = GLYPH_ARROW_LEFT;
				it->color = C_GRAY;
			}
			if (pad_index() < g_pads.count - 1) {
				it = list_add(ROW_PLAIN, "Move page right", "moveright", 0);
				it->icon = GLYPH_ARROW_RIGHT;
				it->color = C_GRAY;
			}
			it = list_add(ROW_PLAIN, "Remove page", "removepage", 0);
			it->icon = GLYPH_TRASH;
			it->color = C_RED;
			it->danger = true;
		}
	}

	list_header("Control your Mac");
	it = list_add(ROW_CHEVRON, "Use as a controller", "controller", 0);
	copy(it->sub, sizeof it->sub, "Buttons, sticks and a trackpad for your Mac");
	copy(it->detail, sizeof it->detail, preset_name(g_controller.preset));
	it->icon = GLYPH_GAMEPAD;
	it->color = C_BLUE;
	it = list_add(ROW_CHEVRON, "Change the controller's buttons", "padsetup", 0);
	it->icon = GLYPH_PENCIL;
	it->color = C_GRAY;
	it = list_add(ROW_CHEVRON, "Type on your Mac", "type", 0);
	copy(it->sub, sizeof it->sub, "With the PS Vita's keyboard, plus Return, arrows and more");
	it->icon = GLYPH_KEYS;
	it->color = C_ORANGE;

	list_header("Your Mac");
	it = list_add(ROW_TOGGLE, "Keep Mac awake", "macawake", 0);
	it->icon = GLYPH_COFFEE;
	it->color = C_YELLOW;
	it = list_add(ROW_TOGGLE, "Mouse jiggler", "jiggler", 0);
	it->icon = GLYPH_MOUSE;
	it->color = C_PINK;
	menu_switches_update();
	it = list_add(ROW_PLAIN, "Forget this Mac", "forget", 0);
	{
		char host[64], sub[128];
		int port;
		if (url_split(g_config.url, host, sizeof host, &port)) snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", g_config.macName, host);
		else copy(sub, sizeof sub, g_config.macName);
		copy(it->sub, sizeof it->sub, sub);
	}
	it->icon = GLYPH_LAPTOP;
	it->color = C_RED;
	it->danger = true;
	it = list_add(ROW_PLAIN, "Refresh from your Mac", "refresh", -2);
	it->icon = GLYPH_REFRESH;
	it->color = C_GRAY;
	s_list.refreshRow = s_list.count - 1;
	refresh_row_update();

	list_header("This PS Vita");
	it = list_add(ROW_TOGGLE, "24-hour time", "clock", 0);
	it->icon = GLYPH_CLOCK;
	it->color = C_BLUE;
	it->on = g_config.clock24;
	it = list_add(ROW_TOGGLE, "Keep this Vita awake", "awake", 0);
	copy(it->sub, sizeof it->sub, "Its screen stays on and it won't sleep while Macapad is open");
	it->icon = GLYPH_MONITOR;
	it->color = C_TEAL;
	it->on = g_config.keepAwake;

	list_header("");
	it = list_add(ROW_PLAIN, "Quit Macapad", "quit", 0);
	it->icon = GLYPH_POWER;
	it->color = C_GRAY;

	list_layout();
}

static const char *s_keyNames[][2] = {
	{"return", "Return"}, {"space", "Space"}, {"tab", "Tab"}, {"delete", "Delete"}, {"escape", "Escape"},
	{"left", "Left arrow"}, {"right", "Right arrow"}, {"up", "Up arrow"}, {"down", "Down arrow"},
	{"home", "Home"}, {"end", "End"}, {"pageup", "Page Up"}, {"pagedown", "Page Down"},
	{"forwarddelete", "Forward Delete"},
	{"-", "-"}, {"=", "="}, {"[", "["}, {"]", "]"}, {"\\", "\\"}, {";", ";"}, {"'", "'"}, {",", ","},
	{".", "."}, {"/", "/"}, {"`", "`"},
};

// Every key, in sections; the one called current shows as picked (for ROW_CHECK rows).
static void add_keyname_rows(RowStyle style, const char *current) {
	char v[4], l[4];
	list_header("Letters");
	for (char c = 'a'; c <= 'z'; c++) {
		snprintf(v, sizeof v, "%c", c);
		snprintf(l, sizeof l, "%c", c - 32);
		list_add(style, l, v, 0)->on = !strcmp(v, current);
	}
	list_header("Numbers");
	for (char c = '0'; c <= '9'; c++) {
		snprintf(v, sizeof v, "%c", c);
		list_add(style, v, v, 0)->on = !strcmp(v, current);
	}
	list_header("Other keys");
	for (size_t i = 0; i < sizeof s_keyNames / sizeof *s_keyNames; i++)
		list_add(style, s_keyNames[i][1], s_keyNames[i][0], 0)->on = !strcmp(s_keyNames[i][0], current);
	list_header("Function keys");
	for (int f = 1; f <= 12; f++) {
		char fv[8], fl[8];
		snprintf(fv, sizeof fv, "f%d", f);
		snprintf(fl, sizeof fl, "F%d", f);
		list_add(style, fl, fv, 0)->on = !strcmp(fv, current);
	}
}

static void open_keyname_list(void) {
	list_open(LIST_KEYNAME, "Which key?");
	add_keyname_rows(ROW_PLAIN, "");
	list_layout();
}

// ---- the controller's setup ---------------------------------------------------------

static const Glyph s_presetGlyphs[PRESETS] = {GLYPH_MOUSE, GLYPH_GAMEPAD, GLYPH_KEYS};

// Built again in place when something changes, so the scroll stays put.
static void pad_setup_build(void) {
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = 0;
	Item *it;
	list_header("Buttons and sticks");
	for (int p = 0; p < PRESETS; p++) {
		it = list_add(ROW_CHECK, preset_name(p), "preset", p);
		copy(it->sub, sizeof it->sub, preset_about(p));
		it->icon = s_presetGlyphs[p];
		it->color = p == g_controller.preset ? C_BLUE : C_GRAY;
		it->on = p == g_controller.preset;
	}
	list_header("");
	it = list_add(ROW_CHEVRON, "Change a button", "controls", 0);
	if (controls_changed()) snprintf(it->detail, sizeof it->detail, "%d changed", controls_changed());
	it->icon = GLYPH_PENCIL;
	it->color = C_ORANGE;
	if (controls_changed()) {
		it = list_add(ROW_PLAIN, "Back to preset", "reset", 0);
		snprintf(it->sub, sizeof it->sub, "Undo your changes to %s", preset_name(g_controller.preset));
		it->icon = GLYPH_UNDO;
		it->color = C_GRAY;
	}
	list_header("");
	it = list_add(ROW_TOGGLE, "Back touchpad scrolls", "rear", 0);
	copy(it->sub, sizeof it->sub, "Drag a finger on the back of the PS Vita");
	it->icon = GLYPH_ARROW_DOWN;
	it->color = C_TEAL;
	it->on = g_controller.backScroll;
	if (s_setupFromMenu) {
		list_header("");
		it = list_add(ROW_PLAIN, "Use as a controller", "start", 0);
		it->icon = GLYPH_GAMEPAD;
		it->color = C_BLUE;
	}
	list_layout();
}

static void open_pad_setup(bool fromMenu) {
	s_setupFromMenu = fromMenu;
	list_open(LIST_PAD_SETUP, "Controller setup");
	pad_setup_build();
	list_select_checked();
}

// "Change a button": every button and stick, with what it does now.
static void open_pad_controls(int select) {
	static const int order[] = {PC_CROSS, PC_CIRCLE, PC_SQUARE, PC_TRIANGLE, PC_UP, PC_DOWN, PC_LEFT, PC_RIGHT,
		PC_L, PC_R, PC_START, PC_SELECT, PC_LSTICK, PC_RSTICK};
	list_open(LIST_PAD_CONTROLS, "Change a button");
	list_header(preset_name(g_controller.preset));
	for (size_t i = 0; i < sizeof order / sizeof *order; i++) {
		int c = order[i];
		if (c == PC_LSTICK) list_header("Sticks");
		Item *it = list_add(ROW_CHEVRON, control_name(c), "", c);
		it->control = c + 1;
		control_label(c, it->detail, sizeof it->detail);
		if (c == select) s_list.sel = s_list.count - 1;
	}
	list_layout();
	list_show_sel();
}

// What one button does: nothing, a click, keys held together, or any one key.
static void open_pad_key_list(int control) {
	static const struct { const char *value, *label; Glyph g; } clicks[] = {
		{"mouse:left", "Click", GLYPH_CLICK_LEFT}, {"mouse:right", "Right click", GLYPH_CLICK_RIGHT},
		{"mouse:middle", "Middle click", GLYPH_MOUSE},
	};
	static const struct { const char *value, *label; } mods[] = {
		{"cmd", "Command"}, {"shift", "Shift"}, {"opt", "Option"}, {"ctrl", "Control"},
	};
	s_padControl = control;
	char title[48];
	snprintf(title, sizeof title, "What should %s do?", control_name(control));
	list_open(LIST_PAD_KEY, title);
	const char *now = control_value(control);
	Item *it = list_add(ROW_CHECK, "Nothing", "none", 0);
	it->on = !strcmp(now, "none");
	list_header("Mouse");
	for (size_t i = 0; i < sizeof clicks / sizeof *clicks; i++) {
		it = list_add(ROW_CHECK, clicks[i].label, clicks[i].value, 0);
		it->icon = clicks[i].g;
		it->color = C_PINK;
		it->on = !strcmp(now, clicks[i].value);
	}
	list_header("Keys held together");
	it = list_add(ROW_CHEVRON, "A shortcut, like Cmd + Tab", "combo", 0);
	if (strchr(now, '+')) value_label(now, it->detail, sizeof it->detail);
	it->icon = GLYPH_KEYS;
	it->color = C_ORANGE;
	list_header("Modifier keys");
	for (size_t i = 0; i < sizeof mods / sizeof *mods; i++) list_add(ROW_CHECK, mods[i].label, mods[i].value, 0)->on = !strcmp(now, mods[i].value);
	add_keyname_rows(ROW_CHECK, now);
	list_layout();
	list_select_checked();
}

static const char *s_stickAbout[STICK_MODES] = {
	"", "Slow near the middle, quick at the edge", "Up, down and sideways", "Up, down, left and right",
	"For walking in PC games", "A quicker pointer, for turning in games",
};

static void open_pad_stick_list(int control) {
	s_padControl = control;
	list_open(LIST_PAD_STICK, control == PC_LSTICK ? "What should the left stick do?" : "What should the right stick do?");
	for (int m = 0; m < STICK_MODES; m++) {
		Item *it = list_add(ROW_CHECK, stick_mode_name(m), stick_mode_id(m), m);
		copy(it->sub, sizeof it->sub, s_stickAbout[m]);
		it->on = !strcmp(control_value(control), stick_mode_id(m));
	}
	list_layout();
	list_select_checked();
}

static void set_key(KeyKind kind, const char *label, const char *ref, const char *category, const char *icon) {
	PadKey *k = edit_key();
	k->kind = kind;
	copy(k->label, sizeof k->label, label);
	copy(k->ref, sizeof k->ref, ref);
	copy(k->category, sizeof k->category, category);
	copy(k->icon, sizeof k->icon, icon);
	k->glyph[0] = k->color[0] = 0;  // a new job, a fresh look
	pads_save();
	icons_prefetch(false);
	open_look();
}

// "https://www.github.com/x" -> "github.com"
static void url_label(const char *url, char *out, size_t size) {
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	if (!strncmp(p, "www.", 4)) p += 4;
	size_t n = strcspn(p, "/?#");
	if (n >= size) n = size - 1;
	memcpy(out, p, n);
	out[n] = 0;
}

static void start_main(void);
static void start_find(const char *message);
static void start_qr(void);
static void go_view(int view);

static void back_to_pad(void) {
	s_screen = SCR_MAIN;
	go_view(s_editPage + 1);
	s_slide = 0;
}

static void list_picked(int i) {
	Item *it = &s_list.items[i];
	if (it->id == -1 && searchable()) {
		list_search();
		return;
	}
	if (it->id == -2) {
		refresh_picked();
		return;
	}
	if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		// a result: the row's id says which kind of key it makes
		Item picked = *it;  // (the rows go when the catalogs do)
		catalogs_finish();
		switch (picked.id) {
		case KEY_ACTION: set_key(KEY_ACTION, picked.label, picked.value, picked.category, picked.iconName); break;
		case KEY_APP: set_key(KEY_APP, picked.label, picked.value, "", ""); break;
		case KEY_SHORTCUT: set_key(KEY_SHORTCUT, picked.label, picked.value, "", ""); break;
		}
		return;
	}
	switch (s_list.purpose) {
	case LIST_KIND:
		switch (it->id) {
		case KEY_ACTION: open_catalog(LIST_ACTIONS, CATALOG_ACTIONS, "Pick a Mac action"); break;
		case KEY_APP: open_catalog(LIST_APPS, CATALOG_APPS, "Pick an app"); break;
		case KEY_SHORTCUT: open_catalog(LIST_SHORTCUTS, CATALOG_SHORTCUTS, "Pick a Shortcut"); break;
		case KEY_WEB: {
			PadKey *k = edit_key();
			ask(ASK_WEB_URL, "Website address", k->kind == KEY_WEB ? k->ref : "https://", IME_URL, 190);
			break;
		}
		case KEY_KEYS:
			memset(s_mods, 0, sizeof s_mods);
			s_keyName[0] = s_keyLabel[0] = 0;
			s_keysSel = 0;
			s_keysForPad = false;
			s_screen = SCR_KEYS;
			break;
		case 100:
			open_look();
			break;
		case 101:
			memset(edit_key(), 0, sizeof(PadKey));
			pads_save();
			back_to_pad();
			break;
		}
		break;
	case LIST_ACTIONS: case LIST_APPS: case LIST_SHORTCUTS: case LIST_SEARCH: break;  // above
	case LIST_LOOK: {
		char id[16];
		copy(id, sizeof id, it->value);  // (the rows go away when the list is built again)
		PadKey *k = edit_key();
		if (!strcmp(id, "name")) {
			ask(ASK_KEY_NAME, "Name for this key", k->label, IME_TEXT, 46);
		} else if (!strcmp(id, "icon")) {
			open_glyph_list();
		} else if (!strcmp(id, "color")) {
			open_color_list();
		} else if (!strcmp(id, "kind")) {
			open_kind_menu();
		} else if (!strcmp(id, "remove")) {
			memset(k, 0, sizeof *k);
			pads_save();
			back_to_pad();
		} else {
			back_to_pad();
		}
		break;
	}
	case LIST_GLYPH:
		copy(edit_key()->glyph, sizeof edit_key()->glyph, it->value);
		pads_save();
		open_look();
		break;
	case LIST_COLOR:
		copy(edit_key()->color, sizeof edit_key()->color, it->value);
		pads_save();
		open_look();
		break;
	case LIST_KEYNAME:
		copy(s_keyName, sizeof s_keyName, it->value);
		copy(s_keyLabel, sizeof s_keyLabel, it->label);
		s_keysSel = 5;  // straight to Save
		s_screen = SCR_KEYS;
		break;
	case LIST_MENU: {
		char id[16];
		copy(id, sizeof id, it->value);
		int pi = pad_index();
		if (!strcmp(id, "page")) {
			go_view(it->id);
			s_slide = 0;
			menu_build();
		} else if (!strcmp(id, "edit")) {
			s_editing = !s_editing;
			menu_build();
		} else if (!strcmp(id, "addpage")) {
			PadPage *p = &g_pads.pages[g_pads.count];
			memset(p, 0, sizeof *p);
			snprintf(p->name, sizeof p->name, "Page %d", g_pads.count + 1);
			g_pads.count++;
			go_view(g_pads.count);
			s_slide = 0;
			s_editing = true;
			pads_save();
			s_screen = SCR_MAIN;
		} else if (!strcmp(id, "renamepage") && pi >= 0) {
			ask(ASK_PAGE_NAME, "Page name", g_pads.pages[pi].name, IME_TEXT, 30);
		} else if ((!strcmp(id, "moveleft") || !strcmp(id, "moveright")) && pi >= 0) {
			int to = pi + (!strcmp(id, "moveleft") ? -1 : 1);
			PadPage tmp = g_pads.pages[pi];
			g_pads.pages[pi] = g_pads.pages[to];
			g_pads.pages[to] = tmp;
			s_view = to + 1;
			pads_save();
			menu_build();
		} else if (!strcmp(id, "removepage") && pi >= 0) {
			memmove(&g_pads.pages[pi], &g_pads.pages[pi + 1], (g_pads.count - pi - 1) * sizeof(PadPage));
			g_pads.count--;
			if (s_view > g_pads.count) s_view = g_pads.count;
			pads_save();
			menu_build();
		} else if (!strcmp(id, "clock")) {
			g_config.clock24 = !g_config.clock24;
			config_save();
			menu_build();
		} else if (!strcmp(id, "awake")) {
			g_config.keepAwake = !g_config.keepAwake;
			config_save();
			menu_build();
		} else if (!strcmp(id, "controller")) {
			start_controller();
		} else if (!strcmp(id, "padsetup")) {
			open_pad_setup(true);
		} else if (!strcmp(id, "type")) {
			start_type();
		} else if (!strcmp(id, "macawake") || !strcmp(id, "jiggler")) {
			bool awake = !strcmp(id, "macawake");
			mac_switch(awake ? SWITCH_KEEP_AWAKE : SWITCH_JIGGLER, !(awake ? s_st.macAwake : s_st.jiggler));
			status_get(&s_st);
			menu_switches_update();
		} else if (!strcmp(id, "forget")) {
			macapad_stop();
			cache_clear();
			g_config.token[0] = 0;
			config_save();
			start_find(NULL);
		} else if (!strcmp(id, "quit")) {
			s_quit = true;
		}
		break;
	}
	case LIST_PAD_SETUP: {
		char id[16];
		copy(id, sizeof id, it->value);
		if (!strcmp(id, "preset")) {
			g_controller.preset = it->id;
			controller_save();
			pad_setup_build();
		} else if (!strcmp(id, "controls")) {
			open_pad_controls(PC_CROSS);
		} else if (!strcmp(id, "reset")) {
			controls_reset();
			pad_setup_build();
		} else if (!strcmp(id, "rear")) {
			g_controller.backScroll = !g_controller.backScroll;
			controller_save();
			pad_setup_build();
		} else if (!strcmp(id, "start")) {
			start_controller();
		}
		break;
	}
	case LIST_PAD_CONTROLS:
		if (it->id < PC_BUTTONS) open_pad_key_list(it->id);
		else open_pad_stick_list(it->id);
		break;
	case LIST_PAD_KEY:
		if (!strcmp(it->value, "combo")) {
			memset(s_mods, 0, sizeof s_mods);
			s_keyName[0] = s_keyLabel[0] = 0;
			s_keysSel = 0;
			s_keysForPad = true;
			s_screen = SCR_KEYS;
			break;
		}
		control_set(s_padControl, it->value);
		open_pad_controls(s_padControl);
		break;
	case LIST_PAD_STICK:
		control_set(s_padControl, it->value);
		open_pad_controls(s_padControl);
		break;
	}
}

static void list_back(void) {
	if (s_list.purpose == LIST_KEYNAME) {
		s_screen = SCR_KEYS;
	} else if (s_list.purpose == LIST_GLYPH || s_list.purpose == LIST_COLOR) {
		open_look();
	} else if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		catalogs_finish();
		open_kind_menu();
	} else if (s_list.purpose == LIST_MENU) {
		s_screen = SCR_MAIN;
	} else if (s_list.purpose == LIST_PAD_SETUP) {
		if (s_setupFromMenu) open_menu();
		else resume_controller();
	} else if (s_list.purpose == LIST_PAD_CONTROLS) {
		open_pad_setup(s_setupFromMenu);
	} else if (s_list.purpose == LIST_PAD_KEY || s_list.purpose == LIST_PAD_STICK) {
		open_pad_controls(s_padControl);
	} else {
		back_to_pad();
	}
}

// Rows for the items of one catalog that match the filter. For actions, a title wherever the
// category changes. Each row's id is the kind of key it makes.
static int add_catalog_rows(CatalogKind kind, bool categoryTitles) {
	Catalog *cat = &s_cats[kind];
	char lastCat[16] = "";
	int n = 0;
	for (int i = 0; i < cat->count; i++) {
		CatalogItem *c = &cat->items[i];
		if (!contains_ci(c->label, s_list.filter)) continue;
		n++;
		Item *it;
		switch (kind) {
		case CATALOG_ACTIONS:
			if (categoryTitles && strcmp(c->sub, lastCat)) {
				list_header(c->sub[0] ? c->sub : "Other");
				copy(lastCat, sizeof lastCat, c->sub);
			}
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_ACTION);
			it->icon = action_glyph(c->value, c->icon);
			it->color = category_color(c->sub);
			copy(it->category, sizeof it->category, c->sub);
			copy(it->iconName, sizeof it->iconName, c->icon);
			break;
		case CATALOG_APPS:
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_APP);
			it->tag[0] = (char)toupper((unsigned char)c->label[0]);
			it->color = C_BLUE;
			break;
		case CATALOG_SHORTCUTS:
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_SHORTCUT);
			it->icon = GLYPH_SHORTCUT;
			it->color = C_PURPLE;
			break;
		}
	}
	return n;
}

static int matches_in(CatalogKind kind) {
	int n = 0;
	for (int i = 0; i < s_cats[kind].count; i++) n += contains_ci(s_cats[kind].items[i].label, s_list.filter);
	return n;
}

// The rows of a catalog list: the search field, then what matches. Built again whenever the
// filter changes; the catalogs stay loaded until the list closes.
static void list_fill_catalog(void) {
	static const char *prompts[] = {"Search actions", "Search apps", "Search Shortcuts"};
	static const char *titles[] = {"Mac actions", "Apps", "Shortcuts"};
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = s_list.results = 0;
	s_list.scroll = 0;

	bool all = s_list.purpose == LIST_SEARCH;
	CatalogKind one = s_list.purpose == LIST_ACTIONS ? CATALOG_ACTIONS : s_list.purpose == LIST_APPS ? CATALOG_APPS : CATALOG_SHORTCUTS;
	add_search_row(all ? "Search actions, apps, Shortcuts" : prompts[one]);
	if (all) {
		for (int k = 0; k < 3; k++) {
			if (!matches_in(k)) continue;
			list_header(titles[k]);
			s_list.results += add_catalog_rows(k, false);
		}
	} else {
		s_list.results = add_catalog_rows(one, true);
	}

	if (s_list.filter[0]) snprintf(s_list.items[0].detail, sizeof s_list.items[0].detail, "%d found", s_list.results);
	s_list.empty[0] = 0;
	if (s_list.results == 0) {
		bool anyOk = false;
		const char *error = NULL;
		for (int k = 0; k < 3; k++) {
			if (!(s_wantCats >> k & 1)) continue;
			if (s_cats[k].ok) anyOk = true;
			else if (!error) error = s_cats[k].error;
		}
		if (!anyOk && error) copy(s_list.empty, sizeof s_list.empty, error);
		else if (s_list.filter[0]) snprintf(s_list.empty, sizeof s_list.empty, "Nothing called \xE2\x80\x9C%s\xE2\x80\x9D", s_list.filter);
		else copy(s_list.empty, sizeof s_list.empty, s_list.purpose == LIST_SHORTCUTS ? "No shortcuts in the Shortcuts app yet." : "Nothing here.");
	}
	s_list.sel = s_list.results ? 2 : 0;  // the first match, past the field and the gap

	list_header("");
	Item *it = list_add(ROW_PLAIN, "Refresh from your Mac", "refresh", -2);
	it->icon = GLYPH_REFRESH;
	it->color = C_GRAY;
	s_list.refreshRow = s_list.count - 1;
	refresh_row_update();
	list_layout();
}

// Asks what to look for; an empty answer shows everything again.
static void list_search(void) {
	static const char *hints[] = {"Search Mac actions", "Search apps", "Search Shortcuts"};
	const char *hint = s_list.purpose == LIST_ACTIONS ? hints[0] : s_list.purpose == LIST_APPS ? hints[1]
	                 : s_list.purpose == LIST_SHORTCUTS ? hints[2] : "Search actions, apps and Shortcuts";
	ask(ASK_SEARCH, hint, s_list.filter, IME_TEXT, 46);
}

static void search_answered(char *q) {
	char *start = trim(q);
	if (s_list.purpose == LIST_KIND) {
		if (*start) open_search(start);  // from the first question: look everywhere
		return;
	}
	copy(s_list.filter, sizeof s_list.filter, start);
	if (s_list.loading) return;  // applied once the Mac answers
	list_fill_catalog();
}

static void list_update(const Input *in) {
	if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		bool changed = false;
		for (int k = 0; k < 3; k++) if ((s_wantCats >> k & 1) && catalog_sync(&s_cats[k])) changed = true;
		if (s_list.loading) {
			list_catalogs_ready();
		} else if (changed) {
			// a refresh landed while the list was open: the rows again, where they were
			float scroll = s_list.scroll;
			int sel = s_list.sel;
			list_fill_catalog();
			s_list.scroll = scroll;
			s_list.sel = sel;
			list_layout();
		}
	}
	refresh_row_update();
	if (s_list.purpose == LIST_MENU) menu_switches_update();

	u32 down = in->down;
	if (down & (B_DIRS | B_OK | B_L | B_R)) s_navShown = true;
	if (down & B_BACK) { list_back(); return; }
	if ((down & B_TRIANGLE) && searchable()) { list_search(); return; }
	if ((down & B_SQUARE) && s_list.refreshRow >= 0) { refresh_picked(); return; }
	if ((down & B_START) && s_list.purpose == LIST_MENU) { list_back(); return; }
	if (down & (B_DOWN | B_UP | B_L | B_R) && s_list.count)
		list_move((down & B_DOWN) ? 1 : (down & B_UP) ? -1 : (down & B_R) ? 6 : -6);
	if ((down & B_OK) && pickable(s_list.sel)) { list_picked(s_list.sel); return; }

	int stick = in->ly ? in->ly : in->ry;
	if (stick) {
		s_list.scroll += stick / 10.0f;
		s_list.vel = 0;
		list_clamp();
	}

	// touch: a drag scrolls (and carries on a little after letting go); a tap picks
	if (s_touching && s_dragging && !s_swiping && s_startY >= BAR_H) {
		s_list.vel = s_lastY - s_touchY;
		s_list.scroll = s_scrollStart - (s_touchY - s_startY);
		list_clamp();
	} else if (!s_touching && fabsf(s_list.vel) > 0.5f) {
		s_list.scroll += s_list.vel;
		s_list.vel *= 0.93f;
		list_clamp();
		if (s_list.scroll <= 0 || s_list.scroll >= list_max_scroll()) s_list.vel = 0;
	}
}

static void list_tap(int tx, int ty) {
	if (ty < BAR_H) {
		if (tx < 200) list_back();
		else if (searchable() && tx >= SCREEN_W - 100) list_search();
		return;
	}
	if (s_list.loading || tx < s_list.x || tx >= s_list.x + s_list.w) return;
	float y = ty - BAR_H + s_list.scroll;
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (y < it->y || y >= it->y + it->h) continue;
		if (!pickable(i)) return;
		s_list.sel = i;
		list_picked(i);
		return;
	}
}

// The bar at the top: "‹ Back" (or none), the title, and a magnifier when the screen can search.
static void title_bar(const char *title, const char *back, bool search) {
	rect(0, 0, SCREEN_W, BAR_H, C_BG);
	rect(0, BAR_H - 1, SCREEN_W, 1, C_LINE);
	if (back) text(28, 40, 22, W_REGULAR, C_BLUE, ALIGN_LEFT, back);
	if (search) glyph(GLYPH_SEARCH, SCREEN_W - 46, 32, 24, C_BLUE, C_BG);
	text_fit(SCREEN_W / 2, 41, 24, W_BOLD, C_TEXT, ALIGN_CENTER, 520, title);
}

// The square at the left of a row, with a glyph or a couple of letters on it.
static void tile(float x, float y, u32 color, Glyph g, const char *tag) {
	rrect(x, y, TILE, TILE, 9, color);
	if (g != GLYPH_NONE) glyph(g, x + TILE / 2, y + TILE / 2, 20, C_TEXT, color);
	else if (tag[0]) text(x + TILE / 2, y + TILE / 2 + cap_height(18) / 2, strlen(tag) > 1 ? 16 : 18, W_BOLD, C_TEXT, ALIGN_CENTER, tag);
}

// The PlayStation shapes, in their colors.
#define C_PS_GREEN RGB(0x3F, 0xD9, 0xB0)
#define C_PS_RED   RGB(0xFF, 0x63, 0x63)
#define C_PS_BLUE  RGB(0x7C, 0xB2, 0xF0)
#define C_PS_PINK  RGB(0xF2, 0x8F, 0xE8)

// A controller button's mark, centered on (cx, cy), about s px: ✕ ○ □ △, an arrow for each way
// on the D-pad, the names of the others, a stick.
static void button_icon(int control, float cx, float cy, float s) {
	float t = s * 0.15f, r = s * 0.5f;
	switch (control) {
	case PC_CROSS:
		line(cx - r * 0.75f, cy - r * 0.75f, cx + r * 0.75f, cy + r * 0.75f, t, C_PS_BLUE);
		line(cx - r * 0.75f, cy + r * 0.75f, cx + r * 0.75f, cy - r * 0.75f, t, C_PS_BLUE);
		break;
	case PC_CIRCLE:
		ring(cx, cy, r * 0.9f, t, C_PS_RED);
		break;
	case PC_SQUARE:
		stroke_rrect(cx - r * 0.78f, cy - r * 0.78f, r * 1.56f, r * 1.56f, t * 0.4f, t, C_PS_PINK);
		break;
	case PC_TRIANGLE: {
		float ax = cx, ay = cy - r * 0.86f, bx = cx - r * 0.95f, by = cy + r * 0.7f, qx = cx + r * 0.95f, qy = by;
		line(ax, ay, bx, by, t, C_PS_GREEN);
		line(bx, by, qx, qy, t, C_PS_GREEN);
		line(qx, qy, ax, ay, t, C_PS_GREEN);
		disc(ax, ay, t / 2, C_PS_GREEN);
		disc(bx, by, t / 2, C_PS_GREEN);
		disc(qx, qy, t / 2, C_PS_GREEN);
		break;
	}
	case PC_UP: tri(cx, cy - r * 0.8f, cx - r * 0.8f, cy + r * 0.5f, cx + r * 0.8f, cy + r * 0.5f, C_TEXT); break;
	case PC_DOWN: tri(cx, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.5f, cx - r * 0.8f, cy - r * 0.5f, C_TEXT); break;
	case PC_LEFT: tri(cx - r * 0.8f, cy, cx + r * 0.5f, cy + r * 0.8f, cx + r * 0.5f, cy - r * 0.8f, C_TEXT); break;
	case PC_RIGHT: tri(cx + r * 0.8f, cy, cx - r * 0.5f, cy - r * 0.8f, cx - r * 0.5f, cy + r * 0.8f, C_TEXT); break;
	case PC_L:
	case PC_R:
		text(cx, cy + cap_height((int)s) / 2, (int)s, W_BOLD, C_TEXT, ALIGN_CENTER, control == PC_L ? "L" : "R");
		break;
	case PC_START:
	case PC_SELECT: {
		int size = s < 24 ? 10 : 12;
		text(cx, cy + cap_height(size) / 2, size, W_BOLD, C_TEXT, ALIGN_CENTER, control == PC_START ? "START" : "SELECT");
		break;
	}
	default:  // a stick
		ring(cx, cy, r, t * 0.8f, C_TEXT2);
		disc(cx, cy, r * 0.55f, C_TEXT);
		break;
	}
}

static void toggle(float rx, float cy, bool on) {
	rrect(rx - 58, cy - 17, 58, 34, 17, on ? C_GREEN : C_CARD_HI);
	disc(on ? rx - 17 : rx - 41, cy, 14, C_TEXT);
}

// The row under a finger that's down and hasn't moved, so it lights up like a button.
static int list_pressed_row(void) {
	if (!s_touching || s_dragging || s_startY < BAR_H || s_startX < s_list.x || s_startX >= s_list.x + s_list.w) return -1;
	float y = s_startY - BAR_H + s_list.scroll;
	for (int i = 0; i < s_list.count; i++)
		if (pickable(i) && y >= s_list.items[i].y && y < s_list.items[i].y + s_list.items[i].h) return i;
	return -1;
}

static void list_draw(void) {
	float cx = s_list.x, cw = s_list.w;
	// the cards first, whole, so one whose first row has scrolled off still has a background
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (it->style == ROW_HEADER || it->style == ROW_SEARCH || !it->first) continue;
		int j = i;
		while (!s_list.items[j].last) j++;
		float y = BAR_H + it->y - s_list.scroll, h = s_list.items[j].y + s_list.items[j].h - it->y;
		if (y + h >= BAR_H && y <= SCREEN_H) rrect(cx, y, cw, h, 16, C_CARD);
	}
	int pressed = list_pressed_row();
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		float y = BAR_H + it->y - s_list.scroll;
		if (y + it->h < BAR_H || y > SCREEN_H) continue;
		float mid = y + it->h / 2;
		if (it->style == ROW_HEADER) {
			if (it->label[0]) text(cx + 18, y + it->h - 12, 16, W_REGULAR, C_TEXT2, ALIGN_LEFT, it->label);
			continue;
		}
		bool lit = i == pressed || (s_navShown && i == s_list.sel);
		if (it->style == ROW_SEARCH) {
			float right = cx + cw - 22;
			rrect(cx, y, cw, it->h, it->h / 2, lit ? C_CARD_HI : C_CARD);
			glyph(GLYPH_SEARCH, cx + 30, mid, 20, C_TEXT2, C_CARD);
			float w = it->detail[0] ? text_width(it->detail, 19, W_REGULAR) + 14 : 0;
			if (it->detail[0]) text(right, mid + cap_height(19) / 2, 19, W_REGULAR, C_TEXT2, ALIGN_RIGHT, it->detail);
			text_fit(cx + 54, mid + cap_height(22) / 2, 22, W_REGULAR, it->on ? C_TEXT : C_TEXT3, ALIGN_LEFT, right - w - (cx + 54), it->label);
			continue;
		}
		if (lit) {
			float r = 12, top = it->first ? 4 : 2, bot = it->last ? 4 : 2;
			rrect(cx + 4, y + top, cw - 8, it->h - top - bot, r, C_CARD_HI);
		}

		float x = cx + 18, right = cx + cw - 18;
		bool hasTile = it->icon != GLYPH_NONE || it->tag[0] || it->swatch || it->control;
		if (it->control) {
			rrect(x, mid - TILE / 2, TILE, TILE, 9, C_CARD_HI);
			button_icon(it->control - 1, x + TILE / 2, mid, 20);
			x += TILE + 16;
		} else if (hasTile) {
			tile(x, mid - TILE / 2, it->color, it->icon, it->tag);
			x += TILE + 16;
		}
		float textRight = right;
		switch (it->style) {
		case ROW_TOGGLE:
			toggle(right, mid, it->on);
			textRight = right - 70;
			break;
		case ROW_CHEVRON:
			glyph(GLYPH_CHEVRON_R, right - 6, mid, 18, C_TEXT3, C_CARD);
			textRight = right - 24;
			if (it->detail[0]) {
				text_fit(textRight, mid + cap_height(19) / 2, 19, W_REGULAR, C_TEXT2, ALIGN_RIGHT, 220, it->detail);
				float w = text_width(it->detail, 19, W_REGULAR);
				textRight -= (w > 220 ? 220 : w) + 14;
			}
			break;
		case ROW_CHECK:
			if (it->on) glyph(GLYPH_CHECK, right - 12, mid, 20, C_BLUE, C_CARD);
			textRight = right - 34;
			break;
		default:
			if (it->detail[0]) {
				text_fit(right, mid + cap_height(19) / 2, 19, W_REGULAR, C_TEXT2, ALIGN_RIGHT, 220, it->detail);
				float w = text_width(it->detail, 19, W_REGULAR);
				textRight = right - (w > 220 ? 220 : w) - 14;
			}
			break;
		}
		u32 color = it->danger ? C_RED : C_TEXT;
		if (it->sub[0]) {
			text_fit(x, y + 32, 22, W_REGULAR, color, ALIGN_LEFT, textRight - x, it->label);
			text_fit(x, y + 57, 17, W_REGULAR, C_TEXT2, ALIGN_LEFT, textRight - x, it->sub);
		} else {
			text_fit(x, mid + cap_height(22) / 2, 22, W_REGULAR, color, ALIGN_LEFT, textRight - x, it->label);
		}
		if (!it->last) rect(x, y + it->h - 1, right - x + 18, 1, C_LINE);
	}
	if (s_list.purpose == LIST_LOOK) {
		// the key, as it will look on the pad, to the left of the rows
		float px = (s_list.x - KEY_W) / 2, py = BAR_H + 70;
		key_tile(edit_key(), px, py, false);
		text(s_list.x / 2, py + KEY_H + 40, 18, W_REGULAR, C_TEXT3, ALIGN_CENTER, "How it will look");
	}
	float colMid = s_list.x + s_list.w / 2;
	if (s_list.loading) text(colMid, 260, 22, W_REGULAR, C_TEXT2, ALIGN_CENTER, "Asking your Mac\xE2\x80\xA6");
	else if (s_list.empty[0] && !s_list.results) {
		// under the search field and the Refresh row
		float y = BAR_H + s_list.height - s_list.scroll + 30;
		text_fit(colMid, y, 20, W_REGULAR, C_TEXT2, ALIGN_CENTER, s_list.w, s_list.empty);
	}
	bool back = s_list.purpose != LIST_MENU && s_list.purpose != LIST_KIND && s_list.purpose != LIST_LOOK;
	title_bar(s_list.title, back ? "\xE2\x80\xB9 Back" : "\xE2\x80\xB9 Done", searchable());
}

// ---- keyboard shortcut builder -------------------------------------------------------

static const char *s_modNames[] = {"cmd", "shift", "opt", "ctrl"};
static const char *s_modLabels[] = {"Command", "Shift", "Option", "Control"};
static const char *s_modShort[] = {"Cmd", "Shift", "Opt", "Ctrl"};

static void keys_combo(char *keys, size_t ksize, char *label, size_t lsize) {
	keys[0] = label[0] = 0;
	// macOS order: ctrl, opt, shift, cmd
	static const int order[] = {3, 2, 1, 0};
	for (int i = 0; i < 4; i++) {
		int m = order[i];
		if (!s_mods[m]) continue;
		snprintf(keys + strlen(keys), ksize - strlen(keys), "%s+", s_modNames[m]);
		snprintf(label + strlen(label), lsize - strlen(label), "%s+", s_modShort[m]);
	}
	snprintf(keys + strlen(keys), ksize - strlen(keys), "%s", s_keyName);
	snprintf(label + strlen(label), lsize - strlen(label), "%s", s_keyLabel);
}

#define KEYS_X LIST_X
#define KEYS_W LIST_W
#define MOD_Y 150
#define MOD_H 76
#define MOD_W ((KEYS_W - 3 * 12) / 4.0f)
#define PICK_Y 262
#define SAVE_Y 372
#define KEYS_ROW_H 76

// Selection with buttons: 0-3 the modifiers, 4 the key, 5 Save.
static void keys_activate(int which) {
	if (which < 4) {
		s_mods[which] = !s_mods[which];
	} else if (which == 4) {
		open_keyname_list();
	} else if (s_keyName[0]) {
		char keys[64], label[48];
		keys_combo(keys, sizeof keys, label, sizeof label);
		if (s_keysForPad) {
			// a controller button holds these down together
			s_keysForPad = false;
			control_set(s_padControl, keys);
			open_pad_controls(s_padControl);
		} else {
			set_key(KEY_KEYS, label, keys, "", "");
		}
	}
}

static void keys_back(void) {
	if (s_keysForPad) {
		s_keysForPad = false;
		open_pad_key_list(s_padControl);
	} else {
		open_kind_menu();
	}
}

static void keys_update(const Input *in) {
	u32 down = in->down;
	if (down & (B_DIRS | B_OK)) s_navShown = true;
	if (down & B_BACK) { keys_back(); return; }
	if (down & B_LEFT && s_keysSel > 0 && s_keysSel < 4) s_keysSel--;
	if (down & B_RIGHT && s_keysSel < 3) s_keysSel++;
	if (down & B_DOWN) s_keysSel = s_keysSel < 4 ? 4 : s_keyName[0] ? 5 : 4;
	if (down & B_UP) s_keysSel = s_keysSel == 5 ? 4 : 0;
	if (down & B_OK) keys_activate(s_keysSel);
}

static void keys_tap(int tx, int ty) {
	if (ty < BAR_H && tx < 200) { keys_back(); return; }
	for (int i = 0; i < 4; i++)
		if (hit(tx, ty, KEYS_X + i * (MOD_W + 12), MOD_Y, MOD_W, MOD_H)) keys_activate(i);
	if (hit(tx, ty, KEYS_X, PICK_Y, KEYS_W, KEYS_ROW_H)) keys_activate(4);
	if (s_keyName[0] && hit(tx, ty, KEYS_X, SAVE_Y, KEYS_W, KEYS_ROW_H)) keys_activate(5);
}

static void focus_ring(float x, float y, float w, float h, float r) {
	stroke_rrect(x - 5, y - 5, w + 10, h + 10, r + 5, 3, C_BLUE);
}

static void keys_draw(void) {
	title_bar(s_keysForPad ? "Keys held together" : "Keyboard shortcut", "\xE2\x80\xB9 Back", false);
	text(SCREEN_W / 2, 124, 20, W_REGULAR, C_TEXT2, ALIGN_CENTER, "Hold down\xE2\x80\xA6");
	for (int i = 0; i < 4; i++) {
		float x = KEYS_X + i * (MOD_W + 12);
		rrect(x, MOD_Y, MOD_W, MOD_H, 16, s_mods[i] ? C_ORANGE : C_CARD);
		text(x + MOD_W / 2, MOD_Y + MOD_H / 2 + cap_height(22) / 2, 22, W_BOLD, s_mods[i] ? C_BG : C_TEXT, ALIGN_CENTER, s_modLabels[i]);
		if (s_navShown && s_keysSel == i) focus_ring(x, MOD_Y, MOD_W, MOD_H, 16);
	}
	float mid = PICK_Y + KEYS_ROW_H / 2 + cap_height(22) / 2;
	rrect(KEYS_X, PICK_Y, KEYS_W, KEYS_ROW_H, 16, C_CARD);
	text(KEYS_X + 24, mid, 22, W_REGULAR, C_TEXT2, ALIGN_LEFT, "and press");
	text(KEYS_X + KEYS_W - 24, mid, 22, s_keyName[0] ? W_BOLD : W_REGULAR, s_keyName[0] ? C_TEXT : C_BLUE, ALIGN_RIGHT,
	     s_keyName[0] ? s_keyLabel : "Pick a key \xE2\x80\xBA");
	if (s_navShown && s_keysSel == 4) focus_ring(KEYS_X, PICK_Y, KEYS_W, KEYS_ROW_H, 16);
	if (s_keyName[0]) {
		char keys[64], label[48], save[80];
		keys_combo(keys, sizeof keys, label, sizeof label);
		snprintf(save, sizeof save, "Save  %s", label);
		rrect(KEYS_X, SAVE_Y, KEYS_W, KEYS_ROW_H, 16, C_BLUE);
		text_fit(SCREEN_W / 2, SAVE_Y + KEYS_ROW_H / 2 + cap_height(22) / 2, 22, W_BOLD, C_TEXT, ALIGN_CENTER, KEYS_W - 40, save);
		if (s_navShown && s_keysSel == 5) focus_ring(KEYS_X, SAVE_Y, KEYS_W, KEYS_ROW_H, 16);
	}
}

// ---- pad pages ------------------------------------------------------------------------

static float slot_x(int s) { return GRID_X + (s % COLS) * (KEY_W + GAP); }
static float slot_y(int s) { return GRID_Y + (s / COLS) * (KEY_H + GAP); }

static int slot_at(int tx, int ty) {
	for (int s = 0; s < SLOTS; s++)
		if (hit(tx, ty, slot_x(s), slot_y(s), KEY_W, KEY_H)) return s;
	return -1;
}

static void edit_slot(int slot) {
	if (pad_index() < 0 || slot < 0) return;
	s_editPage = pad_index();
	s_editSlot = slot;
	open_kind_menu();
}

static void pad_press(int slot) {
	if (slot < 0 || pad_index() < 0) return;
	PadKey *k = &g_pads.pages[pad_index()].keys[slot];
	if (s_editing || k->kind == KEY_NONE) {
		edit_slot(slot);
		return;
	}
	s_flashSlot = slot;
	s_flashUntil = now_ms() + 180;
	press_key(k);
}

// To a page, without sliding; the icons for it come first.
static void go_view(int view) {
	int n = views();
	s_view = ((view % n) + n) % n;
	if (s_view == 0) s_editing = false;
}

// To the next (+1) or previous (-1) page; it slides into place over the next few frames.
static void go_page(int dir) {
	go_view(s_view + dir);
	s_slide += dir * SCREEN_W;
	if (s_slide > SCREEN_W) s_slide = SCREEN_W;
	if (s_slide < -SCREEN_W) s_slide = -SCREEN_W;
}

// One key as it looks on the pad, at (x, y): the icon picked in "How it looks", else the app's or
// website's own, else one for what it does; its name under that.
static void key_tile(const PadKey *k, float x, float y, bool flash) {
	float cx = x + KEY_W / 2, iconY = y + KEY_H * 0.42f;
	if (k->kind == KEY_NONE) {
		u32 bg = FADE(C_CARD, 0x80);
		rrect(x, y, KEY_W, KEY_H, KEY_R, bg);
		glyph(GLYPH_PLUS, cx, y + KEY_H / 2, 26, s_editing ? C_ORANGE : C_TEXT3, bg);
		return;
	}
	u32 bg = flash ? C_CARD_HI : C_CARD, accent = key_accent(k);
	rrect(x, y, KEY_W, KEY_H, KEY_R, bg);
	const GlyphChoice *chosen = glyph_choice(k->glyph);
	if (chosen) {
		glyph(chosen->g, cx, iconY, 46, accent, bg);
	} else {
		switch (k->kind) {
		case KEY_ACTION:
			glyph(action_glyph(k->ref, k->icon), cx, iconY, 44, accent, bg);
			break;
		case KEY_APP: case KEY_WEB: {
			Icon *icon = icon_for(k->kind, k->ref);
			if (icon && icon->state == 2) {
				if (k->kind == KEY_APP) picture_draw(&icon->pic, cx - 34, iconY - 34, 68, 68);
				else picture_draw_rounded(&icon->pic, cx - 28, iconY - 28, 56, 56, 12, bg);
			}
			else if (k->kind == KEY_APP) glyph(GLYPH_APP, cx, iconY, 54, k->color[0] ? accent : C_CARD_HI, bg);
			else glyph(GLYPH_GLOBE, cx, iconY, 48, accent, bg);
			break;
		}
		case KEY_KEYS: glyph(GLYPH_KEYS, cx, iconY, 46, accent, bg); break;
		case KEY_SHORTCUT:
			rrect(cx - 26, iconY - 26, 38, 38, 11, k->color[0] ? accent : C_PINK);
			rrect(cx - 16.5f, iconY - 16.5f, 45, 45, 13, bg);
			rrect(cx - 13, iconY - 13, 38, 38, 11, accent);
			break;
		default: break;
		}
	}
	text_fit(cx, y + KEY_H - 20, 20, W_REGULAR, C_TEXT, ALIGN_CENTER, KEY_W - 20, k->label);
}

static void key_draw(int page, int slot, bool pressed, bool current) {
	PadKey *k = &g_pads.pages[page].keys[slot];
	float x = slot_x(slot), y = slot_y(slot);
	bool flash = pressed || (current && slot == s_flashSlot && now_ms() < s_flashUntil);
	key_tile(k, x, y, flash);
	if (current && s_navShown && slot == s_focus) focus_ring(x, y, KEY_W, KEY_H, KEY_R);
	if (k->kind == KEY_NONE) return;
	if (s_editing) disc(x + KEY_W - 16, y + 16, 6, C_ORANGE);
	if (s_st.off) rrect(x, y, KEY_W, KEY_H, KEY_R, RGBA(0, 0, 0, 0x90));
}

static void draw_mini_clock(float x, float base);

// A round button in the bar above the pad.
#define BAR_BTN 48
static float bar_btn_x(int i) { return SCREEN_W - GRID_X - BAR_BTN - i * (BAR_BTN + 12); }  // 0 = menu, 1 = edit

// The bar above a page of keys: the clock and the song at the left, the page's name, and
// buttons for editing and the menu. While editing, a banner that ends it.
static void pad_bar(int page, bool current) {
	if (current && s_editing) {
		rrect(GRID_X, 12, SCREEN_W - 2 * GRID_X, 54, 16, C_ORANGE);
		text(SCREEN_W / 2, 39 + cap_height(22) / 2, 22, W_BOLD, C_BG, ALIGN_CENTER, "Tap a key to change it \xC2\xB7 tap here when done");
		return;
	}
	draw_mini_clock(GRID_X + 4, 48);
	text_fit(SCREEN_W / 2, 49, 28, W_BOLD, C_TEXT, ALIGN_CENTER, 320, g_pads.pages[page].name);
	for (int i = 0; i < 2; i++) {
		float x = bar_btn_x(i);
		disc(x + BAR_BTN / 2, 39, BAR_BTN / 2, C_CARD);
		glyph(i == 0 ? GLYPH_MENU : GLYPH_PENCIL, x + BAR_BTN / 2, 39, 20, C_TEXT2, C_CARD);
	}
}

static void pad_page_draw(int page, int pressedSlot, bool current) {
	pad_bar(page, current);
	for (int s = 0; s < SLOTS; s++) key_draw(page, s, s == pressedSlot, current);
}

// A tap on a pad page: a key, or the bar above them.
static void pad_tap(int tx, int ty) {
	if (ty >= GRID_Y) {
		pad_press(slot_at(tx, ty));
		return;
	}
	if (s_editing) {
		s_editing = false;
	} else if (hit(tx, ty, bar_btn_x(0) - 6, 0, BAR_BTN + 12, PAD_BAR_H)) {
		open_menu();
	} else if (hit(tx, ty, bar_btn_x(1) - 6, 0, BAR_BTN + 12, PAD_BAR_H)) {
		s_editing = true;
	}
}

// ---- the dashboard -------------------------------------------------------------------

static const char *s_days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *s_months[] = {"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"};

static void fmt_time(char *out, size_t size, int ms) {
	int s = ms / 1000;
	snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void fmt_bytes(char *out, size_t size, double bytes) {
	double gb = bytes / 1e9;
	if (gb >= 100) snprintf(out, size, "%.0f GB", gb);
	else snprintf(out, size, "%.1f GB", gb);
}

// The Mac's local time: this Vita's clock plus the difference measured at the last answer, which
// is kept on the memory card, so it's right from the first frame and stays while the Mac is away.
static struct tm mac_time(void) {
	time_t now = time(NULL) + s_st.clockDelta;
	struct tm tm;
	gmtime_r(&now, &tm);
	return tm;
}

static void clock_text(char *out, size_t size, const struct tm *tm) {
	int hour = tm->tm_hour;
	if (!g_config.clock24) hour = hour % 12 ? hour % 12 : 12;
	snprintf(out, size, g_config.clock24 ? "%02d:%02d" : "%d:%02d", hour, tm->tm_min);
}

// The pad pages' clock, small, with the song beside it.
static void draw_mini_clock(float x, float base) {
	struct tm tm = mac_time();
	char clock[16];
	clock_text(clock, sizeof clock, &tm);
	text(x, base, 28, W_BOLD, C_TEXT, ALIGN_LEFT, clock);
	x += text_width(clock, 28, W_BOLD) + 16;
	if (!s_st.live || !s_st.hasTrack) return;
	char line[260];
	snprintf(line, sizeof line, "%s%s%s", s_st.title, s_st.artist[0] ? " \xC2\xB7 " : "", s_st.artist);
	if (s_st.playing) {
		float t = now_ms() / 1000.0f;
		for (int i = 0; i < 3; i++) {
			float h = 6 + 6 * (0.5f + 0.5f * sinf(t * (5.5f + i * 1.3f) + i * 1.9f));
			rrect(x + i * 6, base - 2 - h, 4, h, 1.5f, C_GREEN);
		}
	} else {
		glyph(GLYPH_PAUSE, x + 7, base - 9, 12, C_TEXT3, C_BG);
	}
	text_fit(x + 26, base - 2, 18, W_REGULAR, C_TEXT2, ALIGN_LEFT, SCREEN_W / 2 - 170 - (x + 26), line);
}

// The header: time, date and the Mac on the left; the weather on the right.
// Big and small text share a baseline so "PM" and "°" sit level with the digits.
#define HEAD_BASE 118
#define HEAD_LINE2 158
#define HEAD_LINE3 192
#define DASH_X 36

static void draw_clock(void) {
	struct tm tm = mac_time();
	char clock[16], date[48];
	clock_text(clock, sizeof clock, &tm);
	text(DASH_X - 4, HEAD_BASE, 112, W_BOLD, C_TEXT, ALIGN_LEFT, clock);
	if (!g_config.clock24)
		text(DASH_X + 6 + text_width(clock, 112, W_BOLD), HEAD_BASE, 30, W_BOLD, C_TEXT2, ALIGN_LEFT, tm.tm_hour < 12 ? "AM" : "PM");
	snprintf(date, sizeof date, "%s, %s %d", s_days[tm.tm_wday], s_months[tm.tm_mon], tm.tm_mday);
	text(DASH_X, HEAD_LINE2, 26, W_REGULAR, C_TEXT2, ALIGN_LEFT, date);
}

static void draw_mac_line(void) {
	const char *name = s_st.macName[0] ? s_st.macName : g_config.macName;
	u32 dot = !s_st.live ? C_TEXT3 : s_st.online ? C_GREEN : C_RED;
	disc(DASH_X + 6, HEAD_LINE3 - 7, 5, dot);
	if (s_st.live && !s_st.online) {
		text(DASH_X + 20, HEAD_LINE3, 20, W_REGULAR, C_RED, ALIGN_LEFT, "Can't reach your Mac");
		return;
	}
	char line[160];
	snprintf(line, sizeof line, "%s%s%s", name, s_st.frontApp[0] ? " \xC2\xB7 " : "", s_st.frontApp);
	text_fit(DASH_X + 20, HEAD_LINE3, 20, W_REGULAR, C_TEXT3, ALIGN_LEFT, 420, line);
}

// ---- weather icons, drawn from shapes, centered on (cx, cy), about s px across ----

#define C_CLOUD RGB(0xC7, 0xC7, 0xCC)

static void sun(float cx, float cy, float s, u32 c) {
	disc(cx, cy, s * 0.24f, c);
	for (int i = 0; i < 8; i++) {
		float a = i * (float)M_PI / 4, ca = cosf(a), sa = sinf(a);
		line(cx + ca * s * 0.34f, cy + sa * s * 0.34f, cx + ca * s * 0.47f, cy + sa * s * 0.47f, s * 0.07f, c);
	}
}

static void moon(float cx, float cy, float s, u32 c) {
	disc(cx, cy, s * 0.32f, c);
	disc(cx + s * 0.16f, cy - s * 0.1f, s * 0.27f, C_BG);
}

static void cloud(float cx, float cy, float s, u32 c) {
	rrect(cx - s * 0.46f, cy, s * 0.92f, s * 0.3f, s * 0.15f, c);
	disc(cx - s * 0.2f, cy + s * 0.04f, s * 0.2f, c);
	disc(cx + s * 0.08f, cy - s * 0.04f, s * 0.27f, c);
}

static void weather_icon(const char *symbol, bool day, float cx, float cy, float s) {
	if (!strcmp(symbol, "clear")) {
		if (day) sun(cx, cy, s, C_YELLOW);
		else moon(cx, cy, s, C_CLOUD);
	} else if (!strcmp(symbol, "partly-cloudy")) {
		if (day) sun(cx - s * 0.15f, cy - s * 0.15f, s * 0.8f, C_YELLOW);
		else moon(cx - s * 0.12f, cy - s * 0.15f, s * 0.8f, C_CLOUD);
		cloud(cx + s * 0.08f, cy + s * 0.08f, s * 0.8f, C_CLOUD);
	} else if (!strcmp(symbol, "fog")) {
		for (int i = 0; i < 3; i++)
			rrect(cx - s * (i == 1 ? 0.38f : 0.46f), cy - s * 0.26f + i * s * 0.22f, s * (i == 1 ? 0.84f : 0.76f), s * 0.1f, s * 0.05f, C_CLOUD);
	} else if (!strcmp(symbol, "rain") || !strcmp(symbol, "drizzle") || !strcmp(symbol, "snow") || !strcmp(symbol, "storm")) {
		cloud(cx, cy - s * 0.18f, s, C_CLOUD);
		float y = cy + s * 0.2f;
		if (!strcmp(symbol, "storm")) {
			tri(cx + s * 0.06f, y - s * 0.04f, cx - s * 0.14f, y + s * 0.16f, cx + s * 0.02f, y + s * 0.16f, C_YELLOW);
			tri(cx + s * 0.06f, y + s * 0.12f, cx - s * 0.1f, y + s * 0.34f, cx - s * 0.02f, y + s * 0.12f, C_YELLOW);
			return;
		}
		for (int i = -1; i <= 1; i++) {
			float x = cx + i * s * 0.22f;
			if (!strcmp(symbol, "rain")) line(x + s * 0.05f, y, x - s * 0.03f, y + s * 0.2f, s * 0.06f, C_TEAL);
			else if (!strcmp(symbol, "drizzle")) disc(x, y + s * 0.1f, s * 0.04f, C_TEAL);
			else disc(x, y + s * 0.1f, s * 0.055f, C_TEXT);
		}
	} else {
		cloud(cx, cy, s, C_CLOUD);
	}
}

static void draw_weather(void) {
	if (!s_st.hasWeather) return;
	char temp[16], line[96];
	float right = SCREEN_W - DASH_X;
	snprintf(temp, sizeof temp, "%d\xC2\xB0", s_st.temp);
	text(right, HEAD_BASE, 112, W_BOLD, C_TEXT, ALIGN_RIGHT, temp);
	weather_icon(s_st.symbol, s_st.isDay, right - text_width(temp, 112, W_BOLD) - 56, HEAD_BASE - 40, 84);
	snprintf(line, sizeof line, "%s \xC2\xB7 H %d\xC2\xB0 L %d\xC2\xB0", s_st.condition, s_st.high, s_st.low);
	text_fit(right, HEAD_LINE2, 26, W_REGULAR, C_TEXT2, ALIGN_RIGHT, 420, line);
	if (s_st.place[0]) text_fit(right, HEAD_LINE3, 20, W_REGULAR, C_TEXT3, ALIGN_RIGHT, 420, s_st.place);
}

static void draw_music_note(float x, float y, float s, u32 c) {
	ellipse(x, y + s * 0.65f, s * 0.4f, s * 0.3f, c);
	rect(x + s * 0.3f, y, s * 0.1f, s * 0.8f, c);
	tri(x + s * 0.3f, y, x + s * 0.7f, y + s * 0.15f, x + s * 0.3f, y + s * 0.3f, c);
}

#define CARD_Y 214
#define CARD_H 192
#define CARD_X DASH_X
#define CARD_W (SCREEN_W - 2 * DASH_X)
#define ART 160

// The transport buttons on the card, right to left: next, play/pause, previous.
static void transport_pos(int i, float *cx, float *cy, float *r) {
	static const float radius[] = {30, 40, 30};
	float x = CARD_X + CARD_W - 28 - radius[0];
	for (int k = 0; k < i; k++) x -= radius[k] + 16 + radius[k + 1];
	*cx = x;
	*cy = CARD_Y + CARD_H / 2 + 6;
	*r = radius[i];
}

static int s_transportPressed = -1;  // lit while a finger is on it

static void draw_transport(void) {
	static const Glyph g[] = {GLYPH_NEXT, GLYPH_PLAY, GLYPH_PREV};
	for (int i = 0; i < 3; i++) {
		float cx, cy, r;
		transport_pos(i, &cx, &cy, &r);
		bool lit = s_transportPressed == i;
		if (i == 1) {
			disc(cx, cy, r, lit ? C_TEXT2 : C_TEXT);
			glyph(s_st.playing ? GLYPH_PAUSE : GLYPH_PLAY, cx + (s_st.playing ? 0 : 3), cy, 30, C_BG, C_TEXT);
		} else {
			disc(cx, cy, r, lit ? C_CARD_HI : FADE(C_CARD_HI, 0x90));
			glyph(g[i], cx, cy, 24, C_TEXT, C_CARD_HI);
		}
	}
}

// What the music is doing: bars that bounce while it plays.
static void draw_play_state(float cx, float cy) {
	if (!s_st.playing) return;
	float t = now_ms() / 1000.0f;
	for (int i = 0; i < 4; i++) {
		float h = 9 + 8 * (0.5f + 0.5f * sinf(t * (5.5f + i * 1.3f) + i * 1.9f));
		rrect(cx - 14 + i * 8, cy + 9 - h, 5, h, 2, C_GREEN);
	}
}

static void draw_now_playing(void) {
	rrect(CARD_X, CARD_Y, CARD_W, CARD_H, 28, C_CARD);
	float mid = CARD_Y + CARD_H / 2;
	if (s_st.outdated && !s_st.live) {
		text(SCREEN_W / 2, mid - 8, 26, W_BOLD, C_ORANGE, ALIGN_CENTER, "Update Macapad on your Mac");
		text(SCREEN_W / 2, mid + 28, 20, W_REGULAR, C_TEXT2, ALIGN_CENTER, "Keys work, but it can't show the song or stats.");
		return;
	}
	if (!s_st.live) {  // the song is never shown from the snapshot: it has to be current
		char msg[96];
		snprintf(msg, sizeof msg, "Connecting to %s\xE2\x80\xA6", g_config.macName[0] ? g_config.macName : "your Mac");
		text_fit(SCREEN_W / 2, mid + 8, 24, W_REGULAR, C_TEXT2, ALIGN_CENTER, CARD_W - 60, msg);
		return;
	}
	if (s_st.off) {
		text(SCREEN_W / 2, mid - 8, 26, W_BOLD, C_ORANGE, ALIGN_CENTER, "Keys are off right now");
		text_fit(SCREEN_W / 2, mid + 28, 20, W_REGULAR, C_TEXT2, ALIGN_CENTER, CARD_W - 60, s_st.offMessage);
		return;
	}
	if (!s_st.hasTrack) {
		draw_music_note(CARD_X + 48, mid - 40, 80, C_TEXT3);
		text(CARD_X + 160, mid - 6, 28, W_BOLD, C_TEXT2, ALIGN_LEFT, "Nothing playing");
		text(CARD_X + 160, mid + 28, 20, W_REGULAR, C_TEXT3, ALIGN_LEFT, "Play something on your Mac.");
		return;
	}
	float ax = CARD_X + 16, ay = CARD_Y + 16;
	if (s_art.tex) {
		picture_draw_rounded(&s_art, ax, ay, ART, ART, 16, C_CARD);
	} else {
		rrect(ax, ay, ART, ART, 16, C_CARD_HI);
		draw_music_note(ax + 44, ay + 40, 80, C_TEXT3);
	}
	float px, py, pr;
	transport_pos(2, &px, &py, &pr);
	float tx = ax + ART + 24, tw = px - pr - 28 - tx;
	text_fit(tx, CARD_Y + 54, 32, W_BOLD, C_TEXT, ALIGN_LEFT, tw, s_st.title);
	text_fit(tx, CARD_Y + 88, 24, W_REGULAR, C_TEXT2, ALIGN_LEFT, tw, s_st.artist);
	char sub[200];
	snprintf(sub, sizeof sub, "%s%s%s", s_st.album, s_st.album[0] ? " \xC2\xB7 " : "", s_st.app);
	text_fit(tx, CARD_Y + 118, 19, W_REGULAR, C_TEXT3, ALIGN_LEFT, tw, sub);

	int pos = s_st.position;
	if (s_st.playing) pos += (int)(now_ms() - s_st.receivedAt);
	if (s_st.duration && pos > s_st.duration) pos = s_st.duration;
	if (s_st.duration > 0) {
		float by = CARD_Y + 142;
		rrect(tx, by, tw, 6, 3, C_CARD_HI);
		rrect(tx, by, tw * pos / (float)s_st.duration, 6, 3, C_TEXT);
		char a[16], b[16];
		fmt_time(a, sizeof a, pos);
		fmt_time(b, sizeof b, s_st.duration);
		text(tx, by + 30, 17, W_REGULAR, C_TEXT3, ALIGN_LEFT, a);
		text(tx + tw, by + 30, 17, W_REGULAR, C_TEXT3, ALIGN_RIGHT, b);
	}
	float nx, ny, nr;
	transport_pos(0, &nx, &ny, &nr);
	draw_play_state(nx, CARD_Y + 30);
	draw_transport();
}

#define STATS_Y 422
#define STATS_H 92

static void stat_tile(int i, const char *label, const char *value, const char *note, float frac, u32 color) {
	float w = (CARD_W - 3 * 16) / 4.0f, x = CARD_X + i * (w + 16), y = STATS_Y;
	rrect(x, y, w, STATS_H, 20, C_CARD);
	text(x + 18, y + 30, 18, W_REGULAR, C_TEXT2, ALIGN_LEFT, label);
	text_fit(x + 18, y + 66, 30, W_BOLD, C_TEXT, ALIGN_LEFT, w - 30, value);
	if (note) text(x + w - 16, y + 30, 16, W_REGULAR, color, ALIGN_RIGHT, note);
	if (frac >= 0) {
		if (frac > 1) frac = 1;
		rrect(x + 18, y + 76, w - 36, 6, 3, C_CARD_HI);
		rrect(x + 18, y + 76, (w - 36) * frac, 6, 3, color);
	}
}

static u32 load_color(double frac) { return frac > 0.85 ? C_RED : frac > 0.6 ? C_YELLOW : C_GREEN; }

static void draw_stats(void) {
	char v[32];
	if (!s_st.have) {
		static const char *labels[] = {"CPU", "Memory", "Battery", "Disk free"};
		for (int i = 0; i < 4; i++) stat_tile(i, labels[i], "\xE2\x80\x93", NULL, -1, C_TEXT);
		return;
	}
	snprintf(v, sizeof v, "%.0f%%", s_st.cpu);
	stat_tile(0, "CPU", v, NULL, s_st.cpu / 100, load_color(s_st.cpu / 100));

	double mem = s_st.memTotal ? s_st.memUsed / s_st.memTotal : 0;
	fmt_bytes(v, sizeof v, s_st.memUsed);
	stat_tile(1, "Memory", v, NULL, mem, load_color(mem));

	if (s_st.battery >= 0) {
		snprintf(v, sizeof v, "%d%%", s_st.battery);
		const char *note = s_st.charging ? "charging" : s_st.pluggedIn ? "on power" : NULL;
		stat_tile(2, "Battery", v, note, s_st.battery / 100.0, s_st.battery <= 20 ? C_RED : C_GREEN);
	} else {
		int h = s_st.uptime / 3600;
		if (h >= 48) snprintf(v, sizeof v, "%d days", h / 24);
		else snprintf(v, sizeof v, "%dh %dm", h, s_st.uptime / 60 % 60);
		stat_tile(2, "Up for", v, NULL, -1, C_TEXT);
	}

	fmt_bytes(v, sizeof v, s_st.diskFree);
	double used = s_st.diskTotal ? 1 - s_st.diskFree / s_st.diskTotal : 0;
	stat_tile(3, "Disk free", v, NULL, used, load_color(used));
}

static void draw_dashboard(void) {
	draw_clock();
	draw_mac_line();
	draw_weather();
	draw_now_playing();
	draw_stats();
}

// Which transport button is at (tx, ty), or -1.
static int transport_at(int tx, int ty) {
	if (!s_st.live || !s_st.hasTrack || s_st.off) return -1;
	for (int i = 0; i < 3; i++) {
		float cx, cy, r;
		transport_pos(i, &cx, &cy, &r);
		float dx = tx - cx, dy = ty - cy;
		if (dx * dx + dy * dy <= (r + 8) * (r + 8)) return i;
	}
	return -1;
}

static void dashboard_tap(int tx, int ty) {
	static const char *actions[] = {"next-track", "play-pause", "previous-track"};
	int i = transport_at(tx, ty);
	if (i >= 0) press_action(actions[i]);
}

// ---- the main screen: pages side by side -------------------------------------------------

// The last notice (or what went wrong), in a pill whose top is at y.
static void draw_toast(float y) {
	char msg[160];
	bool error;
	if (!toast_get(msg, sizeof msg, &error)) return;
	float w = text_width(msg, 20, W_REGULAR) + 48;
	if (w > SCREEN_W - 80) w = SCREEN_W - 80;
	rrect(SCREEN_W / 2 - w / 2, y, w, 46, 23, error ? C_RED : C_CARD_HI);
	text_fit(SCREEN_W / 2, y + 23 + cap_height(20) / 2, 20, W_REGULAR, C_TEXT, ALIGN_CENTER, w - 40, msg);
}

// Dots along the bottom, one per page; the dashboard's is a little house.
static void draw_dots(void) {
	int n = views();
	float gap = 20, x0 = SCREEN_W / 2 - (n - 1) * gap / 2;
	for (int i = 0; i < n; i++) {
		u32 c = i == s_view ? C_TEXT : C_TEXT3;
		if (i == 0) glyph(GLYPH_HOME, x0, DOTS_Y, 12, c, C_BG);
		else disc(x0 + i * gap, DOTS_Y, 4, c);
	}
}

static void view_draw(int view, float dx, bool current) {
	gfx_offset(dx);
	if (view == 0) {
		draw_dashboard();
	} else {
		int pressed = current && s_touching && !s_dragging && !s_longFired ? slot_at(s_startX, s_startY) : -1;
		if (current && s_okHeld && !s_okFired) pressed = s_focus;
		pad_page_draw(view - 1, pressed, current);
	}
	gfx_offset(0);
}

static void main_draw(void) {
	int n = views();
	s_transportPressed = s_view == 0 && s_touching && !s_dragging ? transport_at(s_startX, s_startY) : -1;
	view_draw(s_view, s_slide, true);
	if (s_slide > 0.5f) view_draw((s_view + n - 1) % n, s_slide - SCREEN_W, false);
	else if (s_slide < -0.5f) view_draw((s_view + 1) % n, s_slide + SCREEN_W, false);
	draw_dots();
	draw_toast(DOTS_Y - 66);
}

// The D-pad on a page of keys: moves the focus ring, and past the side, to the next page.
static void pad_nav(u32 down) {
	if (!s_navShown) {
		if (down & B_DIRS) s_navShown = true;  // the first press just shows where it is
		return;
	}
	int col = s_focus % COLS, row = s_focus / COLS;
	if (down & B_UP && row > 0) row--;
	if (down & B_DOWN && row < ROWS - 1) row++;
	if (down & B_LEFT) {
		if (col > 0) col--;
		else { go_page(-1); col = COLS - 1; }
	}
	if (down & B_RIGHT) {
		if (col < COLS - 1) col++;
		else { go_page(+1); col = 0; }
	}
	s_focus = row * COLS + col;
}

static void main_update(const Input *in) {
	if (s_st.forgotten) {
		macapad_stop();
		g_config.token[0] = 0;
		config_save();
		start_find("Your Mac forgot this PS Vita. Pair again.");
		return;
	}
	u32 down = in->down;
	if (down & B_START) { open_menu(); return; }
	if ((down & B_SELECT) && s_view > 0) s_editing = !s_editing;
	if (down & B_L) go_page(-1);
	if (down & B_R) go_page(+1);
	if (down & B_SQUARE) press_action("play-pause");
	if (down & B_TRIANGLE) press_action("mute");
	if (s_view == 0) {
		// the dashboard has nothing to point at, so the D-pad is the media keys
		if (down & B_LEFT) press_action("previous-track");
		if (down & B_RIGHT) press_action("next-track");
		if (down & B_UP) press_action("volume-up");
		if (down & B_DOWN) press_action("volume-down");
		if (down & B_OK) press_action("play-pause");
		return;
	}
	if (down & B_BACK && s_editing) s_editing = false;
	if ((down & B_OK) && !s_navShown) {
		s_navShown = true;  // like the D-pad: the first press shows where the focus is
		return;
	}
	pad_nav(down);
	// the confirm button works like a finger: press, or hold to change the key
	if ((down & B_OK) && s_navShown) {
		s_okHeld = true;
		s_okFired = false;
		s_okAt = now_ms();
	}
	if (s_okHeld && !(in->held & B_OK)) {
		s_okHeld = false;
		if (!s_okFired) pad_press(s_focus);
	} else if (s_okHeld && !s_okFired && now_ms() - s_okAt > LONG_PRESS_MS) {
		s_okFired = true;
		s_okHeld = false;
		edit_slot(s_focus);
	}
}

// A sideways drag moves the pages with the finger; letting go past the mark (or a quick
// flick) turns the page, otherwise it settles back. Up-and-down drags are left alone.
static void swipe_update(bool released) {
	if (s_touching && s_dragging) {
		int dx = s_touchX - s_startX, dy = s_touchY - s_startY;
		if (!s_swipeAxis) {
			s_swipeAxis = true;
			s_swiping = abs(dx) >= abs(dy);
		}
		if (s_swiping) s_slide = views() > 1 ? dx : dx * 0.25f;
	}
	if (released && s_swiping) {
		s_swiping = false;
		int dx = s_touchX - s_startX;
		bool flick = now_ms() - s_touchAt < FLICK_MS && abs(dx) >= FLICK_PX;
		if (abs(dx) >= SWIPE_COMMIT || flick) go_page(dx < 0 ? +1 : -1);
	}
	if (!s_swiping && s_slide != 0) {
		s_slide *= 0.72f;
		if (fabsf(s_slide) < 0.5f) s_slide = 0;
	}
}

// ---- finding + pairing --------------------------------------------------------------

static void start_find(const char *message) {
	copy(s_findMessage, sizeof s_findMessage, message);
	s_screen = SCR_FIND;
	s_findSel = 0;
	scan_finish(&s_scan);
	scan_start(&s_scan);
}

static void start_pair(const FoundMac *mac) {
	scan_finish(&s_scan);
	s_pair.mac = *mac;
	pair_start(&s_pair);
	s_screen = SCR_PAIR;
}

static void start_main(void) {
	s_screen = SCR_MAIN;
	s_view = 0;
	s_slide = 0;
	memset(&s_st, 0, sizeof s_st);
	s_wasOnline = false;
	macapad_start();
	icons_prefetch(false);
}

static void type_address(void) {
	ask(ASK_ADDRESS, "Your Mac's address, like 192.168.1.20", "", IME_URL, 60);
}

static void address_answered(char *addr) {
	// forgive "http://", a trailing "/" and stray spaces
	char *start = trim(addr);
	char host[64];
	int port;
	struct in_addr ip;
	if (!url_split(start, host, sizeof host, &port) || !inet_aton(host, &ip)) {
		snprintf(s_findMessage, sizeof s_findMessage, "\"%s\" isn't an IP address, like 192.168.1.20.", start);
		return;
	}
	FoundMac mac;
	if (probe_mac(host, port, &mac)) start_pair(&mac);
	else snprintf(s_findMessage, sizeof s_findMessage, "Nothing answered at %s:%d. Check the address and Wi-Fi.", host, port);
}

// the right half: the Macs that answered, then three buttons
#define FIND_X 500
#define FIND_W (SCREEN_W - FIND_X - 36)
#define FIND_ROW_Y 128
#define FIND_ROW_H 76
#define FIND_BTN_Y 452
#define FIND_BTN_H 60
#define FIND_BTN_W ((FIND_W - 2 * 12) / 3.0f)
#define FIND_ROWS 4

static int find_rows(void) { return s_scan.count < FIND_ROWS ? s_scan.count : FIND_ROWS; }

static void find_activate(int sel) {
	int rows = find_rows();
	if (sel < rows) start_pair(&s_scan.macs[sel]);
	else if (sel == rows && !s_scan.running) start_find(NULL);
	else if (sel == rows + 1) type_address();
	else if (sel == rows + 2) start_qr();
}

static void find_update(const Input *in) {
	int last = find_rows() + 2;
	if (in->down & (B_DIRS | B_OK)) s_navShown = true;
	if (in->down & (B_DOWN | B_RIGHT)) s_findSel = s_findSel < last ? s_findSel + 1 : last;
	if (in->down & (B_UP | B_LEFT)) s_findSel = s_findSel > 0 ? s_findSel - 1 : 0;
	if (s_findSel > last) s_findSel = last;
	if (in->down & B_OK) find_activate(s_findSel);
	else if (in->down & B_SQUARE) start_qr();
}

static void find_tap(int tx, int ty) {
	int rows = find_rows();
	for (int i = 0; i < rows; i++)
		if (hit(tx, ty, FIND_X, FIND_ROW_Y + i * (FIND_ROW_H + 10), FIND_W, FIND_ROW_H)) { find_activate(i); return; }
	for (int b = 0; b < 3; b++)
		if (hit(tx, ty, FIND_X + b * (FIND_BTN_W + 12), FIND_BTN_Y, FIND_BTN_W, FIND_BTN_H)) { find_activate(rows + b); return; }
}

// The Macapad website as a QR code, for anyone who doesn't have Macapad on their Mac yet.
// 29x29 modules, a row per u32 (bit 28 = leftmost), made with
//   qrencode -l M -m 0 -t ASCII "https://ad-archer.github.io/Macapad/"
static const u32 s_siteQr[29] = {
	0x1FCBD27F, 0x10567E41, 0x1744E15D, 0x1744C45D,
	0x1756765D, 0x104BA241, 0x1FD5557F, 0x0000FB00,
	0x15405C12, 0x06895149, 0x155E8877, 0x191A2AE2,
	0x04C51BCB, 0x133C91C9, 0x1960547B, 0x0293ED5A,
	0x14C349CB, 0x0F2951ED, 0x17CC8913, 0x0A052D8A,
	0x105B1FF0, 0x00149317, 0x1FC34B5B, 0x104CCF1A,
	0x175C51F3, 0x174B5337, 0x175B4139, 0x1046AD02,
	0x1FD99BD3
};

// A QR code at (x, y), m px per module, on white with a 2-module margin.
static void draw_qr(const u32 *rows, int n, float x, float y, float m) {
	float margin = 2 * m;
	rrect(x, y, n * m + 2 * margin, n * m + 2 * margin, 8, C_TEXT);
	for (int r = 0; r < n; r++) {
		// a run of dark modules is one rectangle
		for (int c = 0; c < n; ) {
			if (!(rows[r] >> (n - 1 - c) & 1)) { c++; continue; }
			int start = c;
			while (c < n && rows[r] >> (n - 1 - c) & 1) c++;
			rect(x + margin + start * m, y + margin + r * m, (c - start) * m, m, C_BG);
		}
	}
}

// Whether the Vita is on Wi-Fi, asked once a second.
static bool on_wifi(void) {
	static u64 at;
	static bool on;
	if (!at || now_ms() - at > 1000) {
		on = net_ip() != 0;
		at = now_ms();
	}
	return on;
}

static void find_draw(void) {
	// left: what to do, and the website for anyone without Macapad
	text(36, 84, 40, W_BOLD, C_TEXT, ALIGN_LEFT, "Find your Mac");
	text(36, 130, 22, W_REGULAR, C_TEXT2, ALIGN_LEFT, "Open Macapad on your Mac, and keep");
	text(36, 160, 22, W_REGULAR, C_TEXT2, ALIGN_LEFT, "both on the same Wi-Fi.");
	text(36, 200, 20, W_REGULAR, C_TEXT3, ALIGN_LEFT, "Or choose Scan code to scan the");
	text(36, 228, 20, W_REGULAR, C_TEXT3, ALIGN_LEFT, "QR code Macapad shows.");

	float m = 4, qs = 29 * m + 4 * m, qx = 36, qy = 300;
	draw_qr(s_siteQr, 29, qx, qy, m);
	text(qx + qs + 22, qy + 52, 22, W_BOLD, C_TEXT, ALIGN_LEFT, "No Macapad yet?");
	text(qx + qs + 22, qy + 82, 18, W_REGULAR, C_TEXT2, ALIGN_LEFT, "ad-archer.github.io/Macapad");

	if (s_findMessage[0]) text_fit(36, 270, 20, W_REGULAR, C_ORANGE, ALIGN_LEFT, FIND_X - 60, s_findMessage);
	if (!on_wifi()) text(36, 500, 22, W_BOLD, C_RED, ALIGN_LEFT, "This PS Vita isn't on Wi-Fi.");

	// right: the Macs
	const char *title = s_scan.running ? "Looking\xE2\x80\xA6" : s_scan.count ? "Pick your Mac" : "No Mac found";
	text(FIND_X, 84, 26, W_BOLD, C_TEXT, ALIGN_LEFT, title);
	if (s_scan.running) {
		rrect(FIND_X, 104, FIND_W, 6, 3, C_CARD);
		rrect(FIND_X, 104, FIND_W * s_scan.progress / 254.0f, 6, 3, C_BLUE);
	}
	int rows = find_rows();
	for (int i = 0; i < rows; i++) {
		float y = FIND_ROW_Y + i * (FIND_ROW_H + 10);
		FoundMac *mac = &s_scan.macs[i];
		rrect(FIND_X, y, FIND_W, FIND_ROW_H, 18, C_CARD);
		glyph(GLYPH_LAPTOP, FIND_X + 36, y + FIND_ROW_H / 2, 30, C_TEXT2, C_CARD);
		text_fit(FIND_X + 70, y + 33, 22, W_BOLD, C_TEXT, ALIGN_LEFT, FIND_W - 110, mac->name);
		text(FIND_X + 70, y + 58, 17, W_REGULAR, mac->canPair ? C_TEXT3 : C_ORANGE, ALIGN_LEFT,
		     mac->canPair ? mac->host : "Turn on pairing in Macapad's Settings");
		glyph(GLYPH_CHEVRON_R, FIND_X + FIND_W - 26, y + FIND_ROW_H / 2, 18, C_TEXT3, C_CARD);
		if (s_navShown && s_findSel == i) focus_ring(FIND_X, y, FIND_W, FIND_ROW_H, 18);
	}
	if (!s_scan.running && !s_scan.count)
		text(FIND_X + FIND_W / 2, 240, 22, W_REGULAR, C_TEXT2, ALIGN_CENTER, "Is Macapad open on your Mac?");
	static const char *buttons[] = {"Look again", "Type address", "Scan code"};
	for (int b = 0; b < 3; b++) {
		float x = FIND_X + b * (FIND_BTN_W + 12);
		bool dim = b == 0 && s_scan.running;
		rrect(x, FIND_BTN_Y, FIND_BTN_W, FIND_BTN_H, 16, C_CARD);
		text_fit(x + FIND_BTN_W / 2, FIND_BTN_Y + FIND_BTN_H / 2 + cap_height(18) / 2, 18, W_REGULAR, dim ? C_TEXT3 : C_BLUE,
		         ALIGN_CENTER, FIND_BTN_W - 10, buttons[b]);
		if (s_navShown && s_findSel == rows + b) focus_ring(x, FIND_BTN_Y, FIND_BTN_W, FIND_BTN_H, 16);
	}
}

// ---- scanning the Mac's QR code ----------------------------------------------------

static char s_qrMessage[96];
#define QR_PREVIEW_X 36
#define QR_PREVIEW_Y 84
#define QR_PREVIEW_W 512
#define QR_PREVIEW_H 384
#define QR_CANCEL_Y 452

static void start_qr(void) {
	scan_finish(&s_scan);
	pair_finish(&s_pair);
	s_qrMessage[0] = 0;
	if (!qrscan_start()) {
		start_find("Can't start the camera.");
		return;
	}
	s_screen = SCR_QR;
}

static void qr_update(const Input *in, int tx, int ty, bool tapped) {
	float bx = QR_PREVIEW_X + QR_PREVIEW_W + 36;
	if ((in->down & B_BACK) || (in->down & B_OK) || (tapped && (hit(tx, ty, bx, QR_CANCEL_Y, SCREEN_W - 36 - bx, 60) || (ty < BAR_H && tx < 200)))) {
		qrscan_stop();
		start_find(NULL);
		return;
	}
	char link[1024];
	if (!qrscan_take(link, sizeof link)) return;
	if (pair_qr_start(&s_pair, link)) {
		qrscan_stop();
		s_screen = SCR_PAIR;
	} else {
		copy(s_qrMessage, sizeof s_qrMessage, "That's not a Macapad pairing code.");
	}
}

static void qr_draw(void) {
	title_bar("Scan code", "\xE2\x80\xB9 Back", false);
	float x = QR_PREVIEW_X, y = QR_PREVIEW_Y, w = QR_PREVIEW_W, h = QR_PREVIEW_H;
	rrect(x, y, w, h, 18, C_CARD);
	qrscan_draw(x, y, w, h);
	// corner marks to aim with
	float l = 34, th = 4, bx = x + 110, by = y + 50, bw = w - 220, bh = h - 100;
	rect(bx, by, l, th, C_TEXT);
	rect(bx, by, th, l, C_TEXT);
	rect(bx + bw - l, by, l, th, C_TEXT);
	rect(bx + bw - th, by, th, l, C_TEXT);
	rect(bx, by + bh - th, l, th, C_TEXT);
	rect(bx, by + bh - l, th, l, C_TEXT);
	rect(bx + bw - l, by + bh - th, l, th, C_TEXT);
	rect(bx + bw - th, by + bh - l, th, l, C_TEXT);
	if (s_qrMessage[0]) {
		float tw = text_width(s_qrMessage, 20, W_REGULAR) + 40;
		rrect(x + w / 2 - tw / 2, y + h - 64, tw, 44, 22, C_RED);
		text(x + w / 2, y + h - 42 + cap_height(20) / 2, 20, W_REGULAR, C_TEXT, ALIGN_CENTER, s_qrMessage);
	}

	float tx = x + w + 36, tw = SCREEN_W - 36 - tx;
	text(tx, 124, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "1. On your Mac, click");
	text(tx, 152, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "    Macapad in the menu bar.");
	text(tx, 194, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "2. Choose Pair with QR");
	text(tx, 222, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "    Code\xE2\x80\xA6");
	text(tx, 264, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "3. Point the camera on the");
	text(tx, 292, 22, W_REGULAR, C_TEXT, ALIGN_LEFT, "    back of this PS Vita at it.");
	text(tx, 344, 18, W_REGULAR, C_TEXT3, ALIGN_LEFT, "Hold it about a foot away,");
	text(tx, 368, 18, W_REGULAR, C_TEXT3, ALIGN_LEFT, "straight on.");
	rrect(tx, QR_CANCEL_Y, tw, 60, 16, C_CARD);
	text(tx + tw / 2, QR_CANCEL_Y + 30 + cap_height(22) / 2, 22, W_REGULAR, C_RED, ALIGN_CENTER, "Cancel");
}

#define PAIR_BTN_W 380
#define PAIR_BTN_Y 444

static void pair_update(const Input *in, int tx, int ty, bool tapped) {
	PairState st = s_pair.state;
	if (st == PAIR_APPROVED) {
		pair_finish(&s_pair);
		start_main();
		return;
	}
	bool finished = st == PAIR_DENIED || st == PAIR_EXPIRED || st == PAIR_FAILED;
	bool button = (in->down & B_OK) || (tapped && hit(tx, ty, SCREEN_W / 2 - PAIR_BTN_W / 2, PAIR_BTN_Y, PAIR_BTN_W, 64));
	if ((in->down & B_BACK) || button) {
		pair_finish(&s_pair);
		if (finished && button) {
			// "Try again"
			if (s_pair.viaQr) {
				start_qr();
			} else {
				FoundMac mac = s_pair.mac;
				start_pair(&mac);
			}
		} else {
			start_find(NULL);
		}
	}
}

static void pair_draw(void) {
	char line[160];
	snprintf(line, sizeof line, "Pairing with %s", s_pair.mac.name);
	text_fit(SCREEN_W / 2, 84, 28, W_BOLD, C_TEXT2, ALIGN_CENTER, SCREEN_W - 80, line);
	PairState st = s_pair.state;
	switch (st) {
	case PAIR_STARTING:
		text(SCREEN_W / 2, 250, 28, W_REGULAR, C_TEXT2, ALIGN_CENTER, "Asking your Mac\xE2\x80\xA6");
		break;
	case PAIR_WAITING: {
		float w = 120, gap = 22, x = SCREEN_W / 2 - (4 * w + 3 * gap) / 2;
		for (int i = 0; i < 4 && s_pair.code[i]; i++) {
			char d[2] = {s_pair.code[i], 0};
			rrect(x + i * (w + gap), 128, w, 150, 24, C_CARD);
			text(x + i * (w + gap) + w / 2, 128 + 75 + cap_height(96) / 2, 96, W_BOLD, C_TEXT, ALIGN_CENTER, d);
		}
		text(SCREEN_W / 2, 336, 24, W_REGULAR, C_TEXT, ALIGN_CENTER, "Your Mac is asking if it can trust this PS Vita.");
		text(SCREEN_W / 2, 372, 24, W_REGULAR, C_TEXT2, ALIGN_CENTER, "If it shows this code, click Allow.");
		break;
	}
	case PAIR_DENIED:
		text(SCREEN_W / 2, 250, 30, W_BOLD, C_ORANGE, ALIGN_CENTER, "Your Mac said no.");
		break;
	case PAIR_EXPIRED:
		text(SCREEN_W / 2, 250, 30, W_BOLD, C_ORANGE, ALIGN_CENTER, "That took too long.");
		break;
	case PAIR_FAILED: {
		// up to two lines, split at a newline: what went wrong, then what to do about it
		char err[160];
		copy(err, sizeof err, s_pair.error);
		char *nl = strchr(err, '\n');
		if (nl) *nl = 0;
		text_fit(SCREEN_W / 2, nl ? 236 : 250, 28, W_BOLD, C_ORANGE, ALIGN_CENTER, SCREEN_W - 80, err);
		if (nl) text_fit(SCREEN_W / 2, 276, 22, W_REGULAR, C_TEXT2, ALIGN_CENTER, SCREEN_W - 80, nl + 1);
		break;
	}
	case PAIR_APPROVED:
		break;
	}
	bool finished = st == PAIR_DENIED || st == PAIR_EXPIRED || st == PAIR_FAILED;
	text(SCREEN_W / 2, 420, 20, W_REGULAR, C_TEXT3, ALIGN_CENTER, finished ? "Want to try again?" : "Look at your Mac's screen.");
	float bx = SCREEN_W / 2 - PAIR_BTN_W / 2;
	rrect(bx, PAIR_BTN_Y, PAIR_BTN_W, 64, 18, C_CARD);
	text(SCREEN_W / 2, PAIR_BTN_Y + 32 + cap_height(22) / 2, 22, W_REGULAR, finished ? C_BLUE : C_RED, ALIGN_CENTER,
	     finished ? "Try again" : "Cancel");
}

// ---- the controller --------------------------------------------------------------------

// What each button does, down both sides like the Vita's own buttons; the middle says whether
// it's getting through. All of the screen below the bar is a trackpad.
#define PAD_ROW_Y 84
#define PAD_ROW_H 56
#define PAD_CHIP_W 54
#define PAD_CHIP_H 36
#define PAD_SIDE 24
#define PAD_MID_W 300

static const int s_padLeft[] = {PC_L, PC_UP, PC_DOWN, PC_LEFT, PC_RIGHT, PC_LSTICK, PC_SELECT};
static const int s_padRight[] = {PC_R, PC_TRIANGLE, PC_CIRCLE, PC_CROSS, PC_SQUARE, PC_RSTICK, PC_START};
static Input s_padIn;          // this frame's buttons, to light them up
static float s_leaveProgress;  // START + SELECT held: 0..1

static void start_controller(void) {
	live_open();
	controller_release();  // (the button that picked it doesn't count until it's let go)
	s_leaveProgress = 0;
	s_screen = SCR_PAD;
}

// Back from its setup, with the session still open.
static void resume_controller(void) {
	controller_release();
	s_leaveProgress = 0;
	s_screen = SCR_PAD;
}

static void leave_controller(void) {
	controller_release();
	live_close();
	s_prevButtons = ~0u;  // START and SELECT, still down, don't count again on the pad
	s_navShown = false;
	s_screen = SCR_MAIN;
}

// The controller screen, its setup, and the shortcut builder for a button: the Vita stays awake.
static bool controller_open(void) {
	if (s_screen == SCR_PAD) return true;
	if (s_screen == SCR_KEYS) return s_keysForPad;
	return s_screen == SCR_LIST && !s_setupFromMenu && (s_list.purpose == LIST_PAD_SETUP ||
		s_list.purpose == LIST_PAD_CONTROLS || s_list.purpose == LIST_PAD_KEY || s_list.purpose == LIST_PAD_STICK);
}

static void pad_update(const Input *in, int tx, int ty, bool tapped) {
	s_padIn = *in;
	if (tapped && ty < BAR_H) {
		if (tx < 200) { leave_controller(); return; }
		if (tx >= SCREEN_W - 200) {
			controller_release();
			open_pad_setup(false);
			return;
		}
	}
	ControllerInput ci;
	memset(&ci, 0, sizeof ci);
	ci.buttons = in->raw;
	ci.lx = in->lx;
	ci.ly = in->rawLy;
	ci.rx = in->rx;
	ci.ry = in->rawRy;
	// the fingers on the trackpad: a touch that began on the bar is for the bar
	if (s_touching && s_startY >= BAR_H) {
		for (int i = 0; i < (int)s_frontTouch.reportNum && ci.front.count < 4; i++) {
			int n = ci.front.count++;
			ci.front.id[n] = s_frontTouch.report[i].id;
			ci.front.x[n] = s_frontTouch.report[i].x / 2.0f;
			ci.front.y[n] = s_frontTouch.report[i].y / 2.0f;
		}
	}
	if (g_controller.backScroll) {
		SceTouchData back;
		memset(&back, 0, sizeof back);
		sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
		for (int i = 0; i < (int)back.reportNum && ci.back.count < 4; i++) {
			int n = ci.back.count++;
			ci.back.id[n] = back.report[i].id;
			ci.back.x[n] = back.report[i].x / 2.0f;
			ci.back.y[n] = back.report[i].y / 2.0f;
		}
	}
	if (controller_leaving(&ci, &s_leaveProgress)) {
		leave_controller();
		return;
	}
	controller_frame(&ci);
}

static bool control_held(int c) {
	if (c == PC_LSTICK) return abs(s_padIn.lx - 128) > 30 || abs(s_padIn.rawLy - 128) > 30;
	if (c == PC_RSTICK) return abs(s_padIn.rx - 128) > 30 || abs(s_padIn.rawRy - 128) > 30;
	return (s_padIn.raw & control_bit(c)) != 0;
}

// One button and what it does; on the right side, the other way round.
static void pad_legend(int c, float y, bool right) {
	bool lit = control_held(c);
	float chipX = right ? SCREEN_W - PAD_SIDE - PAD_CHIP_W : PAD_SIDE, mid = y + PAD_ROW_H / 2;
	rrect(chipX, mid - PAD_CHIP_H / 2, PAD_CHIP_W, PAD_CHIP_H, 12, lit ? C_CARD_HI : C_CARD);
	button_icon(c, chipX + PAD_CHIP_W / 2, mid, 22);
	char label[64];
	control_label(c, label, sizeof label);
	float maxw = (SCREEN_W - PAD_MID_W) / 2 - PAD_SIDE - PAD_CHIP_W - 30;
	text_fit(right ? chipX - 14 : chipX + PAD_CHIP_W + 14, mid + cap_height(19) / 2, 19, W_REGULAR, lit ? C_TEXT : C_TEXT2,
	         right ? ALIGN_RIGHT : ALIGN_LEFT, maxw, label);
}

// "First sentence. The rest." -> two lines
static void split_message(const char *msg, char *a, size_t asize, char *b, size_t bsize) {
	const char *dot = strstr(msg, ". ");
	if (!dot) {
		copy(a, asize, msg);
		b[0] = 0;
		return;
	}
	snprintf(a, asize, "%.*s", (int)(dot - msg), msg);
	copy(b, bsize, dot + 2);
}

static void pad_draw(void) {
	title_bar("Controller", "\xE2\x80\xB9 Done", false);
	text(SCREEN_W - 28, 40, 22, W_REGULAR, C_BLUE, ALIGN_RIGHT, "Set up");

	// the fingers on the trackpad
	if (s_touching && s_startY >= BAR_H)
		for (int i = 0; i < (int)s_frontTouch.reportNum && i < 4; i++) {
			float x = s_frontTouch.report[i].x / 2.0f, y = s_frontTouch.report[i].y / 2.0f;
			disc(x, y, 36, FADE(C_TEXT, 0x14));
			ring(x, y, 36, 2, FADE(C_TEXT, 0x40));
		}

	for (int i = 0; i < 7; i++) {
		pad_legend(s_padLeft[i], PAD_ROW_Y + i * PAD_ROW_H, false);
		pad_legend(s_padRight[i], PAD_ROW_Y + i * PAD_ROW_H, true);
	}

	// whether it's getting through
	char msg[160], line1[96], line2[160];
	LiveState st = live_state(msg, sizeof msg);
	u32 color = C_ORANGE;
	const char *mac = g_config.macName[0] ? g_config.macName : "your Mac";
	if (st != LIVE_OUTDATED && s_st.off) {
		st = LIVE_OFF;
		copy(msg, sizeof msg, s_st.offMessage);
	}
	switch (st) {
	case LIVE_WORKING:
		color = C_GREEN;
		copy(line1, sizeof line1, "Connected");
		copy(line2, sizeof line2, mac);
		break;
	case LIVE_CONNECTING:
		color = C_TEXT2;
		copy(line1, sizeof line1, "Connecting\xE2\x80\xA6");
		snprintf(line2, sizeof line2, "to %s", mac);
		break;
	case LIVE_OFF:
		copy(line1, sizeof line1, "Paused on your Mac");
		copy(line2, sizeof line2, msg);
		break;
	case LIVE_OUTDATED:
		copy(line1, sizeof line1, "Update Macapad on your Mac");
		copy(line2, sizeof line2, "The controller needs Macapad 1.0 or later.");
		break;
	case LIVE_FAILED:
		color = C_RED;
		// fall through
	default:
		split_message(msg, line1, sizeof line1, line2, sizeof line2);
		break;
	}
	float mx = SCREEN_W / 2;
	glyph(GLYPH_GAMEPAD, mx, 142, 84, st == LIVE_WORKING ? C_TEXT : C_TEXT3, C_BG);
	text_fit(mx, 226, 26, W_BOLD, C_TEXT, ALIGN_CENTER, PAD_MID_W, preset_name(g_controller.preset));
	float w = text_width(line1, 20, W_BOLD);
	if (w > PAD_MID_W - 20) w = PAD_MID_W - 20;
	disc(mx - w / 2 - 6, 262 - cap_height(20) / 2, 5, color);
	text_fit(mx + 8, 262, 20, W_BOLD, color, ALIGN_CENTER, PAD_MID_W - 20, line1);
	text_fit(mx, 290, 17, W_REGULAR, C_TEXT2, ALIGN_CENTER, PAD_MID_W, line2);

	text(mx, 368, 19, W_BOLD, C_TEXT2, ALIGN_CENTER, "The screen is a trackpad");
	text(mx, 396, 17, W_REGULAR, C_TEXT3, ALIGN_CENTER, "Drag to move, tap to click");
	text(mx, 420, 17, W_REGULAR, C_TEXT3, ALIGN_CENTER, "Two fingers to scroll");
	text(mx, 444, 17, W_REGULAR, C_TEXT3, ALIGN_CENTER, "Tap with two to right-click");

	if (s_leaveProgress > 0) {
		float p = s_leaveProgress > 1 ? 1 : s_leaveProgress;
		rrect(mx - 110, 490, 220, 6, 3, C_CARD_HI);
		rrect(mx - 110, 490, 220 * p, 6, 3, C_BLUE);
	}
	text(mx, 522, 17, W_BOLD, s_leaveProgress > 0 ? C_TEXT : C_TEXT2, ALIGN_CENTER, "Hold START + SELECT to leave");
}

// ---- typing on the Mac ------------------------------------------------------------------

// "Type something…" opens the system keyboard and types the text on the Mac. Under it, the
// modifiers (they stay down for the next key) and keys that work as they're held, repeating.
#define TYPE_X 36
#define TYPE_W (SCREEN_W - 2 * TYPE_X)
#define TYPE_GAP 12
#define TYPE_BIG_Y 84
#define TYPE_BIG_H 92
#define TYPE_SENT_Y 204
#define TYPE_MOD_Y 220
#define TYPE_MOD_H 60
#define TYPE_ROW1_Y 296
#define TYPE_ROW2_Y 384
#define TYPE_KEY_H 76
#define TYPE_KEYS 9
#define TYPE_MIN_MS 60  // a tap holds its key at least this long, so it always reaches the Mac

static const struct { const char *key, *label; Glyph g; } s_typeKeys[TYPE_KEYS] = {
	{"escape", "Esc", GLYPH_NONE}, {"tab", "Tab", GLYPH_NONE}, {"space", "Space", GLYPH_NONE},
	{"delete", "Delete", GLYPH_NONE}, {"return", "Return", GLYPH_NONE},
	{"left", "", GLYPH_ARROW_LEFT}, {"up", "", GLYPH_ARROW_UP}, {"down", "", GLYPH_ARROW_DOWN}, {"right", "", GLYPH_ARROW_RIGHT},
};

// With buttons: 0 the big button, 1..4 the modifiers, 5.. the keys.
static int s_typeSel;
static int s_typeHeld = -1;  // the key down, or -1
static u64 s_typeHeldAt;
static bool s_typeLetGo;     // let go, but held until TYPE_MIN_MS
static bool s_typeByButton;  // held with the confirm button, not a finger
static bool s_typeTouchUsed; // this touch already pressed a key
static char s_typed[96];     // the last text sent, to show

static void type_key_rect(int i, float *x, float *y, float *w, float *h) {
	*h = TYPE_KEY_H;
	if (i < 5) {
		// Esc, Tab, a double-width Space, Delete, Return
		float unit = (TYPE_W - 5 * TYPE_GAP) / 6.0f;
		*y = TYPE_ROW1_Y;
		*x = TYPE_X;
		for (int k = 0; k < i; k++) *x += (k == 2 ? 2 * unit + TYPE_GAP : unit) + TYPE_GAP;
		*w = i == 2 ? 2 * unit + TYPE_GAP : unit;
	} else {
		float unit = (TYPE_W - 3 * TYPE_GAP) / 4.0f;
		*y = TYPE_ROW2_Y;
		*x = TYPE_X + (i - 5) * (unit + TYPE_GAP);
		*w = unit;
	}
}

static float type_mod_w(void) { return (TYPE_W - 3 * TYPE_GAP) / 4.0f; }

static void start_type(void) {
	live_open();
	memset(s_mods, 0, sizeof s_mods);
	s_typeHeld = -1;
	s_typeLetGo = s_typeByButton = false;
	s_typeTouchUsed = s_touching;  // (the finger that opened it isn't a press)
	s_typeSel = 0;
	s_typed[0] = 0;
	s_screen = SCR_TYPE;
}

static void type_release_now(void) {
	LiveHeld none;
	memset(&none, 0, sizeof none);
	live_hold(&none);
	if (s_typeHeld >= 0) memset(s_mods, 0, sizeof s_mods);
	s_typeHeld = -1;
	s_typeLetGo = false;
}

static void leave_type(void) {
	type_release_now();
	live_close();
	s_prevButtons = ~0u;
	s_navShown = false;
	s_screen = SCR_MAIN;
}

static void type_ask(void) { ask(ASK_TYPE, "Type on your Mac", "", IME_TEXT, 256); }

// A key goes down, with the modifiers that are on. Held over live input when it's getting
// through, so it repeats; otherwise pressed once the usual way.
static void type_key_down(int i) {
	if (s_typeHeld >= 0) type_release_now();
	static const int order[] = {3, 2, 1, 0};  // ctrl, opt, shift, cmd
	static const char *names[] = {"cmd", "shift", "opt", "ctrl"};
	char msg[8];
	if (live_state(msg, sizeof msg) == LIVE_WORKING) {
		LiveHeld held;
		memset(&held, 0, sizeof held);
		for (int m = 0; m < 4; m++) if (s_mods[order[m]]) live_held_add(&held, names[order[m]]);
		live_held_add(&held, s_typeKeys[i].key);
		live_hold(&held);
	} else {
		char combo[64] = "";
		for (int m = 0; m < 4; m++)
			if (s_mods[order[m]]) snprintf(combo + strlen(combo), sizeof combo - strlen(combo), "%s+", names[order[m]]);
		snprintf(combo + strlen(combo), sizeof combo - strlen(combo), "%s", s_typeKeys[i].key);
		press_combo(combo);
	}
	s_typeHeld = i;
	s_typeHeldAt = now_ms();
	s_typeLetGo = false;
}

static int type_key_at(int tx, int ty) {
	for (int i = 0; i < TYPE_KEYS; i++) {
		float x, y, w, h;
		type_key_rect(i, &x, &y, &w, &h);
		if (hit(tx, ty, x, y, w, h)) return i;
	}
	return -1;
}

// The D-pad's rows: the big button, the modifiers, then the two rows of keys.
static int type_row(int sel) { return sel == 0 ? 0 : sel < 5 ? 1 : sel < 10 ? 2 : 3; }

static float type_sel_x(int sel) {
	float x, y, w, h;
	if (sel == 0) return SCREEN_W / 2;
	if (sel < 5) return TYPE_X + (sel - 1) * (type_mod_w() + TYPE_GAP) + type_mod_w() / 2;
	type_key_rect(sel - 5, &x, &y, &w, &h);
	return x + w / 2;
}

static void type_nav(u32 down) {
	static const int first[] = {0, 1, 5, 10}, last[] = {0, 4, 9, 13};
	int row = type_row(s_typeSel);
	if (down & B_LEFT && s_typeSel > first[row]) s_typeSel--;
	if (down & B_RIGHT && s_typeSel < last[row]) s_typeSel++;
	int to = (down & B_UP) ? row - 1 : (down & B_DOWN) ? row + 1 : row;
	if (to != row && to >= 0 && to <= 3) {
		// the nearest one in that row
		float x = type_sel_x(s_typeSel), best = 1e9f;
		int pick = first[to];
		for (int i = first[to]; i <= last[to]; i++) {
			float d = fabsf(type_sel_x(i) - x);
			if (d < best) { best = d; pick = i; }
		}
		s_typeSel = pick;
	}
}

static void type_update(const Input *in, int tx, int ty, bool tapped) {
	u32 down = in->down;
	if (down & (B_DIRS | B_OK)) s_navShown = true;
	if (down & B_BACK) { leave_type(); return; }
	if (down & B_TRIANGLE) { type_ask(); return; }
	if (s_typeHeld < 0) type_nav(down);
	if (down & B_OK) {
		if (s_typeSel == 0) { type_ask(); return; }
		if (s_typeSel < 5) s_mods[s_typeSel - 1] = !s_mods[s_typeSel - 1];
		else {
			type_key_down(s_typeSel - 5);
			s_typeByButton = true;
		}
	}
	if (s_typeByButton && s_typeHeld >= 0 && !(in->held & B_OK)) {
		s_typeLetGo = true;
		s_typeByButton = false;
	}

	// a finger on a key holds it down
	if (s_touching && !s_typeTouchUsed && !s_typeByButton) {
		int k = type_key_at(s_startX, s_startY);
		if (k >= 0) type_key_down(k);
		s_typeTouchUsed = true;
	}
	if (!s_touching) {
		s_typeTouchUsed = false;
		if (s_typeHeld >= 0 && !s_typeByButton) s_typeLetGo = true;
	}
	if (s_typeHeld >= 0 && s_typeLetGo && now_ms() - s_typeHeldAt >= TYPE_MIN_MS) type_release_now();

	if (!tapped) return;
	if (ty < BAR_H && tx < 200) { leave_type(); return; }
	if (hit(tx, ty, TYPE_X, TYPE_BIG_Y, TYPE_W, TYPE_BIG_H)) { type_ask(); return; }
	for (int m = 0; m < 4; m++)
		if (hit(tx, ty, TYPE_X + m * (type_mod_w() + TYPE_GAP), TYPE_MOD_Y, type_mod_w(), TYPE_MOD_H)) s_mods[m] = !s_mods[m];
}

static void type_draw(void) {
	title_bar("Type on your Mac", "\xE2\x80\xB9 Done", false);

	bool pressed = s_touching && !s_dragging && hit(s_startX, s_startY, TYPE_X, TYPE_BIG_Y, TYPE_W, TYPE_BIG_H);
	u32 bg = pressed ? C_CARD_HI : C_CARD;
	float mid = TYPE_BIG_Y + TYPE_BIG_H / 2;
	rrect(TYPE_X, TYPE_BIG_Y, TYPE_W, TYPE_BIG_H, 20, bg);
	glyph(GLYPH_KEYS, TYPE_X + 56, mid, 40, C_ORANGE, bg);
	text(TYPE_X + 108, mid - 4, 28, W_BOLD, C_TEXT, ALIGN_LEFT, "Type something\xE2\x80\xA6");
	text(TYPE_X + 108, mid + 26, 18, W_REGULAR, C_TEXT2, ALIGN_LEFT, "It goes where the cursor is on your Mac");
	glyph(GLYPH_CHEVRON_R, TYPE_X + TYPE_W - 30, mid, 20, C_TEXT3, bg);
	if (s_navShown && s_typeSel == 0) focus_ring(TYPE_X, TYPE_BIG_Y, TYPE_W, TYPE_BIG_H, 20);
	if (s_typed[0]) {
		char line[128];
		snprintf(line, sizeof line, "Sent \xE2\x80\x9C%s\xE2\x80\x9D", s_typed);
		text_fit(SCREEN_W / 2, TYPE_SENT_Y, 17, W_REGULAR, C_TEXT3, ALIGN_CENTER, TYPE_W, line);
	}

	for (int m = 0; m < 4; m++) {
		float x = TYPE_X + m * (type_mod_w() + TYPE_GAP);
		rrect(x, TYPE_MOD_Y, type_mod_w(), TYPE_MOD_H, 16, s_mods[m] ? C_ORANGE : C_CARD);
		text(x + type_mod_w() / 2, TYPE_MOD_Y + TYPE_MOD_H / 2 + cap_height(21) / 2, 21, W_BOLD, s_mods[m] ? C_BG : C_TEXT,
		     ALIGN_CENTER, s_modLabels[m]);
		if (s_navShown && s_typeSel == m + 1) focus_ring(x, TYPE_MOD_Y, type_mod_w(), TYPE_MOD_H, 16);
	}

	for (int i = 0; i < TYPE_KEYS; i++) {
		float x, y, w, h;
		type_key_rect(i, &x, &y, &w, &h);
		u32 kbg = s_typeHeld == i ? C_CARD_HI : C_CARD;
		rrect(x, y, w, h, 16, kbg);
		if (s_typeKeys[i].g != GLYPH_NONE) glyph(s_typeKeys[i].g, x + w / 2, y + h / 2, 34, C_TEXT, kbg);
		else text(x + w / 2, y + h / 2 + cap_height(22) / 2, 22, W_REGULAR, C_TEXT, ALIGN_CENTER, s_typeKeys[i].label);
		if (s_navShown && s_typeSel == i + 5) focus_ring(x, y, w, h, 16);
	}

	// how the keys work, or why they don't
	char msg[160], line[200];
	LiveState st = live_state(msg, sizeof msg);
	u32 color = C_TEXT3;
	bool any = s_mods[0] || s_mods[1] || s_mods[2] || s_mods[3];
	if (s_st.off) {
		snprintf(line, sizeof line, "Paused on your Mac: %s", s_st.offMessage);
		color = C_ORANGE;
	} else if (any) {
		copy(line, sizeof line, "The modifiers you picked go with the next key");
	} else if (st == LIVE_WORKING) {
		copy(line, sizeof line, "Keys repeat while you hold them, like on a keyboard");
	} else if (st == LIVE_OUTDATED) {
		copy(line, sizeof line, "Each tap presses a key once. Update Macapad on your Mac for keys that repeat.");
	} else if (st == LIVE_FAILED) {
		copy(line, sizeof line, msg);
		color = C_RED;
	} else {
		copy(line, sizeof line, "Tap a key to press it");
	}
	text_fit(SCREEN_W / 2, 508, 18, W_REGULAR, color, ALIGN_CENTER, TYPE_W, line);
	draw_toast(TYPE_ROW2_Y + TYPE_KEY_H + 10);
}

// ---- the keyboard's answers ------------------------------------------------------------

static void ask_done(Ask purpose, bool ok, char *answer) {
	switch (purpose) {
	case ASK_NONE:
		break;
	case ASK_WEB_URL: {
		char *url = trim(answer);
		if (!ok || !*url || !strcmp(url, "https://")) break;
		char full[200], label[48];
		if (!strstr(url, "://")) snprintf(full, sizeof full, "https://%s", url);
		else copy(full, sizeof full, url);
		url_label(full, label, sizeof label);
		set_key(KEY_WEB, label, full, "", "");
		break;
	}
	case ASK_KEY_NAME:
		if (ok && *trim(answer)) {
			copy(edit_key()->label, sizeof edit_key()->label, trim(answer));
			pads_save();
		}
		open_look();
		break;
	case ASK_PAGE_NAME:
		if (ok && *trim(answer) && pad_index() >= 0) {
			copy(g_pads.pages[pad_index()].name, sizeof g_pads.pages[0].name, trim(answer));
			pads_save();
		}
		if (s_screen == SCR_LIST && s_list.purpose == LIST_MENU) menu_build();
		break;
	case ASK_SEARCH:
		if (ok && s_screen == SCR_LIST) search_answered(answer);
		break;
	case ASK_ADDRESS:
		if (ok && *trim(answer)) address_answered(answer);
		break;
	case ASK_TYPE:
		if (ok && answer[0] && s_screen == SCR_TYPE) {
			type_text(answer);
			copy(s_typed, sizeof s_typed, answer);
		}
		break;
	}
}

// ---- main --------------------------------------------------------------------------

int main(void) {
	sys_init();
	gfx_init();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

	macapad_init();
	config_load();
	pads_load();
	controller_load();
	if (g_config.token[0]) start_main();
	else start_find(NULL);

	while (!s_quit) {
		Input input;
		memset(&input, 0, sizeof input);
		bool tapped = false, released = false;
		int tapX = s_startX, tapY = s_startY;
		if (!ime_active()) {
			input_read(&input);

			// turn raw touch into taps, drags and long presses
			SceTouchData touch;
			memset(&touch, 0, sizeof touch);
			sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
			s_frontTouch = touch;  // every finger, for the trackpad
			bool down = touch.reportNum > 0;
			int x = down ? touch.report[0].x / 2 : 0, y = down ? touch.report[0].y / 2 : 0;
			if (down && !s_touching) {
				s_touching = true;
				s_dragging = s_longFired = s_swiping = s_swipeAxis = false;
				s_startX = s_touchX = s_lastY = x;
				s_startY = s_touchY = s_lastY = y;
				s_touchAt = now_ms();
				s_scrollStart = s_list.scroll;
				s_list.vel = 0;
				s_navShown = false;  // a finger's in charge now: no focus ring
				tapX = x;
				tapY = y;
			} else if (down) {
				s_lastY = s_touchY;
				s_touchX = x;
				s_touchY = y;
				if (abs(x - s_startX) > DRAG_PX || abs(y - s_startY) > DRAG_PX) s_dragging = true;
			} else if (s_touching) {
				s_touching = false;
				released = true;
				tapped = !s_dragging && !s_longFired;
			}
		}

		// back from the PS button or the Vita sleeping: nothing stays held on the Mac
		if (sys_was_away()) {
			if (s_screen == SCR_PAD) controller_release();
			if (s_screen == SCR_TYPE) type_release_now();
		}

		if (s_screen == SCR_MAIN || s_screen == SCR_LIST || s_screen == SCR_KEYS || s_screen == SCR_PAD || s_screen == SCR_TYPE)
			status_get(&s_st);
		if (s_st.online && !s_wasOnline) icons_retry();
		s_wasOnline = s_st.online;
		u8 *px;
		int w, h;
		if (artwork_take(&px, &w, &h)) {
			if (px) picture_load(&s_art, px, w, h);
			else picture_free(&s_art);
			free(px);
		}
		icons_collect();

		switch (s_screen) {
		case SCR_FIND:
			find_update(&input);
			if (tapped && s_screen == SCR_FIND) find_tap(tapX, tapY);
			break;
		case SCR_QR:
			qr_update(&input, tapX, tapY, tapped);
			break;
		case SCR_PAIR:
			pair_update(&input, tapX, tapY, tapped);
			break;
		case SCR_MAIN:
			main_update(&input);
			if (s_screen != SCR_MAIN) break;
			swipe_update(released);
			// hold a key to change it
			if (s_touching && !s_dragging && !s_longFired && now_ms() - s_touchAt > LONG_PRESS_MS) {
				s_longFired = true;
				if (s_view > 0) edit_slot(slot_at(s_startX, s_startY));
			} else if (tapped) {
				if (s_view == 0) dashboard_tap(tapX, tapY);
				else pad_tap(tapX, tapY);
			}
			if (s_screen != SCR_MAIN) {
				s_slide = 0;
				s_swiping = false;
			}
			break;
		case SCR_LIST:
			list_update(&input);
			if (tapped && s_screen == SCR_LIST) list_tap(tapX, tapY);
			break;
		case SCR_KEYS:
			keys_update(&input);
			if (tapped && s_screen == SCR_KEYS) keys_tap(tapX, tapY);
			break;
		case SCR_PAD:
			pad_update(&input, tapX, tapY, tapped);
			break;
		case SCR_TYPE:
			type_update(&input, tapX, tapY, tapped);
			break;
		}

		gfx_begin();
		switch (s_screen) {
		case SCR_FIND: find_draw(); break;
		case SCR_QR: qr_draw(); break;
		case SCR_PAIR: pair_draw(); break;
		case SCR_MAIN: main_draw(); break;
		case SCR_LIST: list_draw(); break;
		case SCR_KEYS: keys_draw(); break;
		case SCR_PAD: pad_draw(); break;
		case SCR_TYPE: type_draw(); break;
		}
		gfx_end();

		if (ime_active()) {
			char answer[256] = "";
			int r = ime_poll(answer, sizeof answer);
			if (r != 0) {
				Ask purpose = s_ask;
				s_ask = ASK_NONE;
				s_prevButtons = ~0u;  // the button that closed the keyboard doesn't count again
				ask_done(purpose, r > 0, answer);
			}
		}
		if (g_config.keepAwake || controller_open()) power_keep_awake();
	}

	scan_finish(&s_scan);
	qrscan_stop();
	pair_finish(&s_pair);
	catalogs_finish();
	live_shutdown();
	macapad_stop();
	picture_free(&s_art);
	for (int i = 0; i < ICON_SLOTS; i++) picture_free(&s_icons[i].pic);
	free(s_list.items);
	gfx_exit();
	sys_exit();
	sceKernelExitProcess(0);
	return 0;
}
