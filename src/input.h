#pragma once
// Live input: keys and mouse buttons held down, and the pointer moving, sent to the Mac as it
// happens (Macapad 1.0's input session, over UDP). For the controller and the typing screen.
#include "sys.h"

#include <stdbool.h>
#include <stddef.h>

#define LIVE_KEYS 16

enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 4 };

// Everything held right now: key names from the keystroke vocabulary ("z", "up", "cmd"), and mouse buttons.
typedef struct {
	char keys[LIVE_KEYS][16];
	int count;
	unsigned mouse;
} LiveHeld;

void live_held_add(LiveHeld *held, const char *key);  // once each, up to LIVE_KEYS

// Asks the Mac for a session and starts sending (on a thread of its own).
void live_open(void);
// Lets go of everything on the Mac (three empty packets) and ends the session. Doesn't wait.
void live_close(void);
// At exit: waits for the thread to finish letting go.
void live_shutdown(void);

// What's held right now; sent as soon as it changes, and repeated while anything is held.
void live_hold(const LiveHeld *held);
// Pointer movement (points, dy > 0 = down) and scrolling (px, sy > 0 = further down), added to the next packet.
void live_move(int dx, int dy, int sx, int sy);

typedef enum {
	LIVE_CONNECTING,  // asking for a session, or waiting for the Mac's first answer
	LIVE_WORKING,     // the Mac answered lately: input is getting through
	LIVE_NO_ANSWER,   // a session, but nothing comes back (a firewall, or the Mac's gone)
	LIVE_OFF,         // paused, or outside the Mac's hours: the message says
	LIVE_OUTDATED,    // the Mac's Macapad is older than 1.0
	LIVE_FAILED,      // couldn't get a session: the message says why
} LiveState;
LiveState live_state(char *message, size_t size);
