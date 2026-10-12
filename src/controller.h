#pragma once
// Using the Vita as a controller for the Mac: what each button and stick does (a preset, with any
// buttons changed on top, kept on the memory card), and turning the Vita's buttons, sticks and
// touch into live input.
#include "sys.h"

#include <stdbool.h>
#include <stddef.h>

// The controls that can be changed: the buttons, then the two sticks.
enum {
	PC_UP, PC_DOWN, PC_LEFT, PC_RIGHT, PC_CROSS, PC_CIRCLE, PC_SQUARE, PC_TRIANGLE,
	PC_L, PC_R, PC_START, PC_SELECT, PC_LSTICK, PC_RSTICK,
	PC_COUNT,
};
#define PC_BUTTONS PC_LSTICK  // the first ones are buttons

typedef enum { STICK_NOTHING, STICK_POINTER, STICK_SCROLL, STICK_ARROWS, STICK_WASD, STICK_LOOK, STICK_MODES } StickMode;

enum { PRESET_MOUSE, PRESET_RETRO, PRESET_PC, PRESETS };

typedef struct {
	int preset;
	bool backScroll;  // the touchpad on the back scrolls
	// Changes to each preset: "" = the preset's own. A button's is a key combo ("z", "cmd+tab"),
	// "mouse:left" / "mouse:right" / "mouse:middle", or "none"; a stick's is a StickMode's id.
	char changed[PRESETS][PC_COUNT][24];
} ControllerConfig;

extern ControllerConfig g_controller;

void controller_load(void);
void controller_save(void);

const char *preset_name(int preset);
const char *preset_about(int preset);
const char *control_name(int control);  // "Cross", "Left stick"
u32 control_bit(int control);           // its SCE_CTRL_ bit; 0 for the sticks

// What a control does now, as saved ("z", "mouse:left", "none", "pointer"), and in words.
const char *control_value(int control);
void control_label(int control, char *out, size_t size);
// Changes it in the current preset; its preset's own value undoes the change. Saved.
void control_set(int control, const char *value);
int controls_changed(void);  // in the current preset
void controls_reset(void);   // back to the preset; saved

// "cmd+tab" -> "Cmd+Tab", "mouse:left" -> "Click", "none" -> "Nothing"
void value_label(const char *value, char *out, size_t size);
const char *stick_mode_id(StickMode mode);
const char *stick_mode_name(StickMode mode);

// ---- the controller screen, every frame ----------------------------------------------

typedef struct {
	int count;
	int id[4];
	float x[4], y[4];  // screen px
} Touches;

typedef struct {
	u32 buttons;            // SCE_CTRL_ bits
	u8 lx, ly, rx, ry;      // the sticks, 0..255
	Touches front, back;    // front: only touches that began on the trackpad
} ControllerInput;

// Turns this frame's buttons, sticks and touches into what's held and moved on the Mac.
void controller_frame(const ControllerInput *in);
// Lets go of everything (leaving, a pause, the Vita waking up).
void controller_release(void);
// START and SELECT held together: true once held long enough to leave; *progress 0..1 meanwhile.
bool controller_leaving(const ControllerInput *in, float *progress);
