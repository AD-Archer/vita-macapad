#include "controller.h"
#include "input.h"

#include <ctype.h>
#include <jansson.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

ControllerConfig g_controller;

#define CONTROLLER_FILE DATA_DIR "/controller.json"

static void copy(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

// ---- presets ------------------------------------------------------------------------

typedef struct {
	const char *value;
	const char *label;  // what it's for, shown before the key ("Jump (Space)"); NULL = just the key
} Map;

typedef struct {
	const char *id, *name, *about;
	Map map[PC_COUNT];
} Preset;

// In PC_ order: up, down, left, right, ✕, ○, □, △, L, R, START, SELECT, left stick, right stick.
static const Preset s_presets[PRESETS] = {
	{"mouse", "Mouse & keys", "Move the pointer, click, scroll and use the arrow keys", {
		{"up"}, {"down"}, {"left"}, {"right"},
		{"mouse:left"}, {"mouse:right"}, {"space"}, {"cmd+tab", "Switch apps"},
		{"cmd+[", "Back"}, {"cmd+]", "Forward"}, {"return"}, {"escape"},
		{"pointer"}, {"scroll"},
	}},
	// RetroArch's own keyboard keys, by where the buttons sit: the right face button is A (X), the
	// bottom one B (Z), the top one X (S) and the left one Y (A)
	{"retro", "Retro games", "RetroArch's keys, by where the buttons sit", {
		{"up"}, {"down"}, {"left"}, {"right"},
		{"z"}, {"x"}, {"a"}, {"s"},
		{"q"}, {"w"}, {"return"}, {"rshift"},
		{"arrows"}, {"nothing"},
	}},
	{"pc", "PC games", "W A S D, mouse look, Space to jump", {
		{"1"}, {"3"}, {"4"}, {"2"},
		{"space", "Jump"}, {"shift", "Sprint"}, {"e", "Use"}, {"r", "Reload"},
		{"mouse:right", "Aim"}, {"mouse:left", "Shoot"}, {"escape", "Menu"}, {"tab", "Map"},
		{"wasd"}, {"look"},
	}},
};

static const char *s_controlIds[PC_COUNT] = {
	"up", "down", "left", "right", "cross", "circle", "square", "triangle", "l", "r", "start", "select",
	"leftStick", "rightStick",
};
static const char *s_controlNames[PC_COUNT] = {
	"Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L", "R", "START", "SELECT",
	"Left stick", "Right stick",
};
static const u32 s_controlBits[PC_COUNT] = {
	SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE,
	SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_START, SCE_CTRL_SELECT, 0, 0,
};

static const char *s_stickIds[STICK_MODES] = {"nothing", "pointer", "scroll", "arrows", "wasd", "look"};
static const char *s_stickNames[STICK_MODES] = {"Nothing", "Moves the pointer", "Scrolls", "Arrow keys", "W A S D", "Mouse look"};

const char *preset_name(int preset) { return s_presets[preset].name; }
const char *preset_about(int preset) { return s_presets[preset].about; }
const char *control_name(int control) { return s_controlNames[control]; }
u32 control_bit(int control) { return s_controlBits[control]; }
const char *stick_mode_id(StickMode mode) { return s_stickIds[mode]; }
const char *stick_mode_name(StickMode mode) { return s_stickNames[mode]; }

static StickMode stick_mode_from(const char *id) {
	for (int m = 0; m < STICK_MODES; m++) if (!strcmp(id, s_stickIds[m])) return m;
	return STICK_NOTHING;
}

// The preset's own mapping. In "Mouse & keys" the confirm button clicks, so on a console set up
// with ○ as confirm, ○ and ✕ swap.
static const Map *preset_map(int control) {
	int c = control;
	if (g_controller.preset == PRESET_MOUSE && enter_is_circle() && (c == PC_CROSS || c == PC_CIRCLE))
		c = c == PC_CROSS ? PC_CIRCLE : PC_CROSS;
	return &s_presets[g_controller.preset].map[c];
}

const char *control_value(int control) {
	const char *changed = g_controller.changed[g_controller.preset][control];
	return changed[0] ? changed : preset_map(control)->value;
}

static const struct { const char *key, *label; } s_keyLabels[] = {
	{"return", "Return"}, {"enter", "Return"}, {"space", "Space"}, {"tab", "Tab"}, {"delete", "Delete"},
	{"escape", "Esc"}, {"esc", "Esc"}, {"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"},
	{"home", "Home"}, {"end", "End"}, {"pageup", "Page Up"}, {"pagedown", "Page Down"},
	{"forwarddelete", "Fwd Delete"},
	{"cmd", "Cmd"}, {"shift", "Shift"}, {"opt", "Opt"}, {"ctrl", "Ctrl"}, {"fn", "Fn"}, {"rshift", "Right Shift"},
};

void value_label(const char *value, char *out, size_t size) {
	out[0] = 0;
	if (!value[0] || !strcmp(value, "none")) { copy(out, size, "Nothing"); return; }
	if (!strcmp(value, "mouse:left")) { copy(out, size, "Click"); return; }
	if (!strcmp(value, "mouse:right")) { copy(out, size, "Right click"); return; }
	if (!strcmp(value, "mouse:middle")) { copy(out, size, "Middle click"); return; }
	char parts[48];
	copy(parts, sizeof parts, value);
	char *save = NULL;
	for (char *p = strtok_r(parts, "+", &save); p; p = strtok_r(NULL, "+", &save)) {
		char one[24];
		const char *label = NULL;
		for (size_t i = 0; i < sizeof s_keyLabels / sizeof *s_keyLabels; i++)
			if (!strcmp(p, s_keyLabels[i].key)) label = s_keyLabels[i].label;
		if (!label) {
			// a letter or an F key in capitals; punctuation as it is
			copy(one, sizeof one, p);
			for (char *c = one; *c; c++) *c = (char)toupper((unsigned char)*c);
			label = one;
		}
		snprintf(out + strlen(out), size - strlen(out), "%s%s", out[0] ? "+" : "", label);
	}
}

void control_label(int control, char *out, size_t size) {
	const char *value = control_value(control);
	if (control >= PC_BUTTONS) {
		copy(out, size, stick_mode_name(stick_mode_from(value)));
		return;
	}
	char key[48];
	value_label(value, key, sizeof key);
	const Map *own = preset_map(control);
	if (!g_controller.changed[g_controller.preset][control][0] && own->label) snprintf(out, size, "%s (%s)", own->label, key);
	else copy(out, size, key);
}

void control_set(int control, const char *value) {
	char *changed = g_controller.changed[g_controller.preset][control];
	if (!strcmp(value, preset_map(control)->value)) changed[0] = 0;
	else copy(changed, sizeof g_controller.changed[0][0], value);
	controller_save();
}

int controls_changed(void) {
	int n = 0;
	for (int c = 0; c < PC_COUNT; c++) n += g_controller.changed[g_controller.preset][c][0] != 0;
	return n;
}

void controls_reset(void) {
	memset(g_controller.changed[g_controller.preset], 0, sizeof g_controller.changed[0]);
	controller_save();
}

// ---- the memory card --------------------------------------------------------------------

void controller_load(void) {
	memset(&g_controller, 0, sizeof g_controller);
	json_t *root = json_load_file(CONTROLLER_FILE, 0, NULL);
	if (!root) return;
	const char *preset = json_string_value(json_object_get(root, "preset"));
	for (int p = 0; p < PRESETS; p++) if (preset && !strcmp(preset, s_presets[p].id)) g_controller.preset = p;
	g_controller.backScroll = json_is_true(json_object_get(root, "backScroll"));
	json_t *changed = json_object_get(root, "changed");
	for (int p = 0; p < PRESETS; p++) {
		json_t *one = json_object_get(changed, s_presets[p].id);
		for (int c = 0; c < PC_COUNT; c++) {
			const char *v = json_string_value(json_object_get(one, s_controlIds[c]));
			if (v) copy(g_controller.changed[p][c], sizeof g_controller.changed[0][0], v);
		}
	}
	json_decref(root);
}

void controller_save(void) {
	mkdir(DATA_DIR, 0777);
	json_t *changed = json_object();
	for (int p = 0; p < PRESETS; p++) {
		json_t *one = json_object();
		for (int c = 0; c < PC_COUNT; c++)
			if (g_controller.changed[p][c][0]) json_object_set_new(one, s_controlIds[c], json_string(g_controller.changed[p][c]));
		json_object_set_new(changed, s_presets[p].id, one);
	}
	json_t *root = json_pack("{s:s,s:b,s:o}", "preset", s_presets[g_controller.preset].id,
		"backScroll", g_controller.backScroll, "changed", changed);
	json_dump_file(root, CONTROLLER_FILE, JSON_INDENT(2));
	json_decref(root);
}

// ---- every frame ------------------------------------------------------------------------

#define HOLD_OFF_MS 120     // START and SELECT wait this long before they count, in case it's both (to leave)
#define LEAVE_MS 600        // hold both this long to leave
#define TAP_MS 60           // a short press is held at least this long, so it's never lost between packets
#define TAP_TIME_MS 300     // a touch shorter than this that hardly moved is a tap
#define TAP_SLOP 10         // px it may move and still be a tap
#define DEAD_ZONE 0.18f     // of a stick's travel
#define SCROLL_FINGER 2.5f  // Mac px scrolled per px a finger moves

static float s_accX, s_accY, s_accSX, s_accSY;  // movement not sent yet (the fractions)
static u64 s_lastFrame;
static u64 s_clickUntil[3];  // taps on the trackpad: held until then
static u32 s_ignore;         // buttons already down when the screen opened, ignored until let go
static bool s_fresh = true;

// START and SELECT
static bool s_was[2], s_went[2], s_combo;
static u64 s_downAt[2], s_tapUntil[2], s_bothAt;

// the trackpad: the fingers on it last frame
static struct {
	bool on;
	int fingers, most, id[2];
	float x[2], y[2], moved;
	u64 at;
} s_pad;
static struct {
	bool on;
	int id;
	float x, y;
} s_rear;

static void held_value(LiveHeld *held, const char *value) {
	if (!value[0] || !strcmp(value, "none")) return;
	if (!strcmp(value, "mouse:left")) { held->mouse |= MOUSE_LEFT; return; }
	if (!strcmp(value, "mouse:right")) { held->mouse |= MOUSE_RIGHT; return; }
	if (!strcmp(value, "mouse:middle")) { held->mouse |= MOUSE_MIDDLE; return; }
	char parts[48];
	copy(parts, sizeof parts, value);
	char *save = NULL;
	for (char *p = strtok_r(parts, "+", &save); p; p = strtok_r(NULL, "+", &save)) live_held_add(held, p);
}

// START (0) or SELECT (1): counts once held a moment without the other, or as a quick tap.
static bool menu_button(int i, bool down, u64 now) {
	if (down && !s_was[i]) {
		s_downAt[i] = now;
		s_went[i] = false;
	}
	if (!down && s_was[i] && !s_went[i] && !s_combo) s_tapUntil[i] = now + TAP_MS;
	s_was[i] = down;
	bool on = down && !s_combo && now - s_downAt[i] >= HOLD_OFF_MS;
	if (on) s_went[i] = true;
	return on || now < s_tapUntil[i];
}

static float axis(u8 v) { return (v - 128) / 127.0f; }

static void stick(int control, u8 sx, u8 sy, float dt, LiveHeld *held) {
	float x = axis(sx), y = axis(sy), mag = sqrtf(x * x + y * y);
	if (mag < DEAD_ZONE) return;
	if (mag > 1) {
		x /= mag;
		y /= mag;
		mag = 1;
	}
	float f = (mag - DEAD_ZONE) / (1 - DEAD_ZONE), ux = x / mag, uy = y / mag;
	switch (stick_mode_from(control_value(control))) {
	case STICK_POINTER:  // slow near the middle for aiming, quick at the edge
		s_accX += ux * f * f * 16 * dt;
		s_accY += uy * f * f * 16 * dt;
		break;
	case STICK_LOOK:  // faster, and more even, for turning a camera
		s_accX += ux * f * 26 * dt;
		s_accY += uy * f * 26 * dt;
		break;
	case STICK_SCROLL:
		s_accSX += ux * f * f * 16 * dt;
		s_accSY += uy * f * f * 16 * dt;
		break;
	case STICK_ARROWS:
	case STICK_WASD: {
		bool wasd = stick_mode_from(control_value(control)) == STICK_WASD;
		if (x < -0.5f) live_held_add(held, wasd ? "a" : "left");
		if (x > 0.5f) live_held_add(held, wasd ? "d" : "right");
		if (y < -0.5f) live_held_add(held, wasd ? "w" : "up");
		if (y > 0.5f) live_held_add(held, wasd ? "s" : "down");
		break;
	}
	default:
		break;
	}
}

// The front touchscreen as a trackpad: one finger moves the pointer, two scroll; a tap clicks,
// a two-finger tap right-clicks.
static void trackpad(const Touches *t, u64 now) {
	if (t->count == 0) {
		if (s_pad.on && s_pad.moved < TAP_SLOP && now - s_pad.at < TAP_TIME_MS)
			s_clickUntil[s_pad.most >= 2 ? 1 : 0] = now + TAP_MS;
		s_pad.on = false;
		return;
	}
	int n = t->count > 2 ? 2 : t->count;
	if (!s_pad.on) {
		memset(&s_pad, 0, sizeof s_pad);
		s_pad.on = true;
		s_pad.at = now;
		s_pad.most = 0;
	} else {
		// how far the fingers that were down last frame too moved
		float dx = 0, dy = 0;
		int same = 0;
		for (int i = 0; i < n; i++)
			for (int j = 0; j < s_pad.fingers; j++)
				if (t->id[i] == s_pad.id[j]) {
					dx += t->x[i] - s_pad.x[j];
					dy += t->y[i] - s_pad.y[j];
					same++;
				}
		if (same && same == n && n == s_pad.fingers) {
			dx /= same;
			dy /= same;
			s_pad.moved += fabsf(dx) + fabsf(dy);
			if (n == 1 && s_pad.most == 1) {
				// a little acceleration: slow moves are precise, quick ones go far
				float speed = sqrtf(dx * dx + dy * dy), gain = 1.0f + speed * 0.18f;
				if (gain > 3.5f) gain = 3.5f;
				s_accX += dx * gain;
				s_accY += dy * gain;
			} else if (n == 2) {
				// content follows the fingers, like a phone
				s_accSX -= dx * SCROLL_FINGER;
				s_accSY -= dy * SCROLL_FINGER;
			}
		}
	}
	if (n > s_pad.most) s_pad.most = n;
	s_pad.fingers = n;
	for (int i = 0; i < n; i++) {
		s_pad.id[i] = t->id[i];
		s_pad.x[i] = t->x[i];
		s_pad.y[i] = t->y[i];
	}
}

// The touchpad on the back, when it's on: one finger scrolls.
static void rear(const Touches *t) {
	if (!g_controller.backScroll || t->count == 0) {
		s_rear.on = false;
		return;
	}
	if (s_rear.on && t->id[0] == s_rear.id) {
		s_accSX -= (t->x[0] - s_rear.x) * SCROLL_FINGER;
		s_accSY -= (t->y[0] - s_rear.y) * SCROLL_FINGER;
	}
	s_rear.on = true;
	s_rear.id = t->id[0];
	s_rear.x = t->x[0];
	s_rear.y = t->y[0];
}

void controller_frame(const ControllerInput *in) {
	u64 now = now_ms();
	float dt = s_lastFrame ? (now - s_lastFrame) / 16.667f : 1;
	if (dt > 3) dt = 3;
	s_lastFrame = now;

	if (s_fresh) {
		s_ignore = in->buttons;
		s_fresh = false;
	}
	s_ignore &= in->buttons;
	u32 buttons = in->buttons & ~s_ignore;

	LiveHeld held;
	memset(&held, 0, sizeof held);
	bool start = buttons & SCE_CTRL_START, select = buttons & SCE_CTRL_SELECT;
	if (start && select) s_combo = true;
	for (int c = 0; c < PC_BUTTONS; c++) {
		bool down = buttons & control_bit(c);
		if (c == PC_START || c == PC_SELECT) down = menu_button(c == PC_SELECT, down, now);
		if (down) held_value(&held, control_value(c));
	}
	if (!start && !select) s_combo = false;

	stick(PC_LSTICK, in->lx, in->ly, dt, &held);
	stick(PC_RSTICK, in->rx, in->ry, dt, &held);
	trackpad(&in->front, now);
	rear(&in->back);
	for (int i = 0; i < 3; i++) if (now < s_clickUntil[i]) held.mouse |= 1u << i;

	int dx = (int)s_accX, dy = (int)s_accY, sx = (int)s_accSX, sy = (int)s_accSY;
	s_accX -= dx;
	s_accY -= dy;
	s_accSX -= sx;
	s_accSY -= sy;
	live_move(dx, dy, sx, sy);
	live_hold(&held);
}

void controller_release(void) {
	LiveHeld none;
	memset(&none, 0, sizeof none);
	live_hold(&none);
	s_accX = s_accY = s_accSX = s_accSY = 0;
	s_lastFrame = 0;
	memset(s_clickUntil, 0, sizeof s_clickUntil);
	memset(s_tapUntil, 0, sizeof s_tapUntil);
	memset(s_was, 0, sizeof s_was);
	s_combo = false;
	s_bothAt = 0;
	s_pad.on = s_rear.on = false;
	s_fresh = true;  // whatever is held when it starts again waits to be let go
}

bool controller_leaving(const ControllerInput *in, float *progress) {
	bool both = (in->buttons & SCE_CTRL_START) && (in->buttons & SCE_CTRL_SELECT);
	*progress = 0;
	if (!both) {
		s_bothAt = 0;
		return false;
	}
	u64 now = now_ms();
	if (!s_bothAt) s_bothAt = now;
	*progress = (now - s_bothAt) / (float)LEAVE_MS;
	return now - s_bothAt >= LEAVE_MS;
}
