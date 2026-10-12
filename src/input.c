// Live input over Macapad 1.0's input session: a session from POST /api/input/session, then the whole
// input state (every key and mouse button held, and movement since the last one) in a UDP packet
// whenever it changes, again every 90 ms while anything is held, and a heartbeat every second. The
// Mac lets go of everything it hasn't heard about for half a second, so nothing can stay stuck.
#include "input.h"
#include "http.h"
#include "macapad.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REPEAT_MS 90       // while anything is held or moving
#define HEARTBEAT_MS 900   // while nothing is
#define ACK_EVERY_MS 1000  // "ack": true at most this often
#define ANSWER_MS 3000     // no "ok" for this long: the Mac isn't answering
#define MOVE_MAX 500       // per packet; the rest goes in the next one

static Lock s_lock;
static bool s_lockMade;
static Thread s_thread;
static Event s_wake;
static volatile bool s_quit;

// set by the main thread, sent by the live thread
static LiveHeld s_held;
static u32 s_heldVersion;  // bumps whenever what's held changes
static int s_dx, s_dy, s_sx, s_sy;

static LiveState s_state;
static char s_message[160];

// Packet numbers keep counting up across sessions: the Mac only takes one numbered higher than the last.
static long long s_packetNo;

static void copy(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

static const char *jstr(json_t *obj, const char *key) {
	const char *s = json_string_value(json_object_get(obj, key));
	return s ? s : "";
}

void live_held_add(LiveHeld *held, const char *key) {
	if (!key[0] || held->count >= LIVE_KEYS) return;
	for (int i = 0; i < held->count; i++) if (!strcmp(held->keys[i], key)) return;
	copy(held->keys[held->count++], sizeof held->keys[0], key);
}

static void set_state(LiveState state, const char *message) {
	lock(&s_lock);
	s_state = state;
	copy(s_message, sizeof s_message, message);
	unlock(&s_lock);
}

LiveState live_state(char *message, size_t size) {
	if (!s_lockMade) {
		copy(message, size, "");
		return LIVE_CONNECTING;
	}
	lock(&s_lock);
	LiveState state = s_state;
	copy(message, size, s_message);
	unlock(&s_lock);
	return state;
}

void live_hold(const LiveHeld *held) {
	if (!s_lockMade) return;
	lock(&s_lock);
	bool changed = held->count != s_held.count || held->mouse != s_held.mouse;
	for (int i = 0; !changed && i < held->count; i++) changed = strcmp(held->keys[i], s_held.keys[i]) != 0;
	if (changed) {
		s_held = *held;
		s_heldVersion++;
	}
	unlock(&s_lock);
	if (changed) event_signal(&s_wake);
}

void live_move(int dx, int dy, int sx, int sy) {
	if (!s_lockMade || !(dx || dy || sx || sy)) return;
	lock(&s_lock);
	s_dx += dx;
	s_dy += dy;
	s_sx += sx;
	s_sy += sy;
	unlock(&s_lock);
	event_signal(&s_wake);
}

// ---- the session ------------------------------------------------------------------

// Asks the Mac for a session. False with the state set to why not, and *retryMs to when to ask again.
static bool get_session(char *session, size_t size, struct sockaddr_in *to, int *retryMs) {
	char host[64];
	int port;
	struct in_addr addr;
	if (!url_split(g_config.url, host, sizeof host, &port) || !inet_aton(host, &addr)) {
		set_state(LIVE_FAILED, "Pair with your Mac first.");
		*retryMs = 5000;
		return false;
	}
	HttpResponse res;
	bool sent = http_request("POST", host, port, "/api/input/session", g_config.token, "{}", &res, 4000);
	json_t *root = sent ? json_loadb(res.body, res.len, 0, NULL) : NULL;
	const char *error = jstr(json_object_get(root, "error"), "message");
	bool ok = false;
	if (!sent) {
		set_state(LIVE_FAILED, "Can't reach your Mac");
		*retryMs = 2000;
	} else if (res.status == 200 && *jstr(root, "session")) {
		copy(session, size, jstr(root, "session"));
		json_t *udp = json_object_get(root, "udpPort");
		memset(to, 0, sizeof *to);
		to->sin_family = AF_INET;
		to->sin_port = htons(json_is_integer(udp) ? (int)json_integer_value(udp) : 7317);
		to->sin_addr = addr;
		ok = true;
	} else if (res.status == 404) {
		set_state(LIVE_OUTDATED, "Update Macapad on your Mac");
		*retryMs = 15000;  // (in case it's updated while this is open)
	} else if (res.status == 423) {
		const char *off = jstr(json_object_get(root, "off"), "message");
		set_state(LIVE_OFF, *off ? off : *error ? error : "Macapad is paused on your Mac.");
		*retryMs = 3000;
	} else if (res.status == 401) {
		set_state(LIVE_FAILED, "Your Mac forgot this PS Vita. Pair again from the menu.");
		*retryMs = 5000;
	} else {
		set_state(LIVE_FAILED, *error ? error : "Your Mac said no.");
		*retryMs = 3000;
	}
	json_decref(root);
	http_free(&res);
	return ok;
}

static int udp_open(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	return fd;
}

static const char *s_mouseNames[] = {"left", "right", "middle"};

// One packet: the whole state, plus the session.
static bool send_state(int fd, const struct sockaddr_in *to, long long n, const char *session, const LiveHeld *held,
                       int dx, int dy, int sx, int sy, bool ack) {
	json_t *o = json_object();
	json_object_set_new(o, "n", json_integer(n));
	json_object_set_new(o, "s", json_string(session));
	if (held && held->count) {
		json_t *keys = json_array();
		for (int i = 0; i < held->count; i++) json_array_append_new(keys, json_string(held->keys[i]));
		json_object_set_new(o, "keys", keys);
	}
	if (held && held->mouse) {
		json_t *buttons = json_array();
		for (int i = 0; i < 3; i++) if (held->mouse & (1u << i)) json_array_append_new(buttons, json_string(s_mouseNames[i]));
		json_object_set_new(o, "buttons", buttons);
	}
	if (dx) json_object_set_new(o, "dx", json_integer(dx));
	if (dy) json_object_set_new(o, "dy", json_integer(dy));
	if (sx) json_object_set_new(o, "sx", json_integer(sx));
	if (sy) json_object_set_new(o, "sy", json_integer(sy));
	if (ack) json_object_set_new(o, "ack", json_true());
	char *body = json_dumps(o, JSON_COMPACT);
	json_decref(o);
	if (!body) return false;
	int len = strlen(body);
	bool sent = len <= 1024 && sendto(fd, body, len, 0, (const struct sockaddr *)to, sizeof *to) == len;
	free(body);
	return sent;
}

static int clamp_move(int v) { return v > MOVE_MAX ? MOVE_MAX : v < -MOVE_MAX ? -MOVE_MAX : v; }

static void live_thread(void *arg) {
	int fd = -1;
	char session[48] = "";
	struct sockaddr_in to;
	u64 nextTry = 0, sessionAt = 0, lastSent = 0, lastAskedAck = 0, firstAskedAck = 0, lastAck = 0, reopenAt = 0;
	u32 sentVersion = 0;
	bool sentAny = false;
	char buf[1100];

	while (!s_quit) {
		event_wait(&s_wake, 30);
		if (s_quit) break;
		u64 now = now_ms();

		if (!session[0]) {
			if (now < nextTry) continue;
			int retryMs = 3000;
			if (!get_session(session, sizeof session, &to, &retryMs)) {
				nextTry = now_ms() + retryMs;
				continue;
			}
			now = now_ms();
			sessionAt = now;
			firstAskedAck = lastAskedAck = 0;
			sentAny = false;
			if (lastAck && now - lastAck < ANSWER_MS) set_state(LIVE_WORKING, "");
			else set_state(LIVE_CONNECTING, "");
		}
		if (fd < 0) {
			if (now < reopenAt) continue;
			fd = udp_open();
			if (fd < 0) {
				reopenAt = now + 500;
				continue;
			}
		}

		// the Mac's answers
		for (;;) {
			struct sockaddr_in from;
			socklen_t fromLen = sizeof from;
			int got = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fromLen);
			if (got <= 0) break;
			buf[got] = 0;
			json_t *root = json_loads(buf, 0, NULL);
			const char *type = jstr(root, "type");
			if (!strcmp(type, "ok")) {
				lastAck = now;
			} else if (!strcmp(type, "error")) {
				const char *reason = jstr(root, "reason");
				if (!strcmp(reason, "off")) {
					// paused or outside its hours: ask for a session again every few seconds
					set_state(LIVE_OFF, *jstr(root, "message") ? jstr(root, "message") : "Macapad is paused on your Mac.");
					session[0] = 0;
					nextTry = now + 3000;
				} else if (!strcmp(reason, "session")) {
					// expired, the Mac restarted, or this Vita was unpaired: a new one, and carry on
					session[0] = 0;
					nextTry = now - sessionAt < 2000 ? now + 1000 : now;
				}
			}
			json_decref(root);
			if (!session[0]) break;
		}
		if (!session[0]) continue;

		if (lastAck && now - lastAck < ANSWER_MS) set_state(LIVE_WORKING, "");
		else if (firstAskedAck && now - firstAskedAck > ANSWER_MS)
			set_state(LIVE_NO_ANSWER, "Your Mac isn't answering. Is its firewall blocking Macapad?");

		lock(&s_lock);
		LiveHeld held = s_held;
		u32 version = s_heldVersion;
		int dx = clamp_move(s_dx), dy = clamp_move(s_dy), sx = clamp_move(s_sx), sy = clamp_move(s_sy);
		unlock(&s_lock);
		bool moving = dx || dy || sx || sy, holding = held.count || held.mouse;
		bool due = !sentAny || version != sentVersion || moving || now - lastSent >= (holding ? REPEAT_MS : HEARTBEAT_MS);
		if (!due) continue;

		bool ack = !lastAskedAck || now - lastAskedAck >= ACK_EVERY_MS;
		if (send_state(fd, &to, ++s_packetNo, session, &held, dx, dy, sx, sy, ack)) {
			lock(&s_lock);
			s_dx -= dx;
			s_dy -= dy;
			s_sx -= sx;
			s_sy -= sy;
			unlock(&s_lock);
			lastSent = now;
			sentVersion = version;
			sentAny = true;
			if (ack) {
				lastAskedAck = now;
				if (!firstAskedAck) firstAskedAck = now;
			}
		} else {
			// the network went away (the Vita slept?): a new socket in a moment
			close(fd);
			fd = -1;
			reopenAt = now + 500;
		}
	}

	// leaving: let go of everything, three times in case one is lost
	if (session[0]) {
		if (fd < 0) fd = udp_open();
		for (int i = 0; i < 3 && fd >= 0; i++) {
			send_state(fd, &to, ++s_packetNo, session, NULL, 0, 0, 0, 0, false);
			sleep_ms(15);
		}
	}
	if (fd >= 0) close(fd);
}

// ---- lifetime -----------------------------------------------------------------------

static void wait_finished(void) {
	if (!s_thread) return;
	thread_join(s_thread);
	s_thread = 0;
	event_free(&s_wake);
}

void live_open(void) {
	if (!s_lockMade) {
		lock_init(&s_lock, "live");
		s_lockMade = true;
	}
	wait_finished();  // the last one, letting go (long done, usually)
	lock(&s_lock);
	memset(&s_held, 0, sizeof s_held);
	s_heldVersion++;
	s_dx = s_dy = s_sx = s_sy = 0;
	s_state = LIVE_CONNECTING;
	s_message[0] = 0;
	unlock(&s_lock);
	s_quit = false;
	event_init(&s_wake, "live");
	s_thread = thread_start("live", live_thread, NULL);
}

void live_close(void) {
	if (!s_thread) return;
	if (s_lockMade) {
		lock(&s_lock);
		memset(&s_held, 0, sizeof s_held);
		s_dx = s_dy = s_sx = s_sy = 0;
		unlock(&s_lock);
	}
	s_quit = true;
	event_signal(&s_wake);
}

void live_shutdown(void) {
	live_close();
	wait_finished();
}
