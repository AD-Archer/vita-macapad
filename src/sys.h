#pragma once
// The few things the rest of the app needs from the Vita, behind plain names: a clock, threads,
// locks, the network, the on-screen keyboard and the screen's power.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#define DATA_DIR "ux0:data/macapad"

// Milliseconds since the app started (never goes backwards).
u64 now_ms(void);
void sleep_ms(int ms);

// ---- threads ----------------------------------------------------------------

typedef int Thread;  // 0 = none
// Runs fn(arg) on a worker thread, on the cores the screen doesn't use.
Thread thread_start(const char *name, void (*fn)(void *), void *arg);
void thread_join(Thread t);

typedef struct { int uid; } Lock;
void lock_init(Lock *l, const char *name);
void lock(Lock *l);
void unlock(Lock *l);

// An event a thread waits on; signals while nobody waits are remembered (once).
typedef struct { int uid; } Event;
void event_init(Event *e, const char *name);
void event_free(Event *e);
void event_signal(Event *e);
// Waits up to ms (-1 = forever). True when signalled.
bool event_wait(Event *e, int ms);

// ---- system -----------------------------------------------------------------

// Once, first: the app utility, the network, the common dialogs.
void sys_init(void);
void sys_exit(void);

// The Vita's address on Wi-Fi as a number in host order, and its netmask; 0 when it isn't connected.
u32 net_ip(void);
u32 net_mask(void);

// "<user name>'s PS Vita", or just "PS Vita".
void device_name(char *out, size_t size);

// True when the confirm button is ○ (Japanese consoles), so ✕ goes back.
bool enter_is_circle(void);

// Keeps the screen on while the app runs (call every frame; cheap).
void power_keep_awake(void);

// ---- on-screen keyboard ---------------------------------------------------------

// IME_URL: no capitals at the start, and ":" "/" "." close at hand (also for IP addresses).
typedef enum { IME_TEXT, IME_URL } ImeType;
// Opens the system keyboard over the app. False if it couldn't.
bool ime_open(const char *title, const char *initial, ImeType type, int maxLen);
bool ime_active(void);
// After each frame is drawn while it's open: 0 still open, 1 confirmed (text in out), -1 cancelled.
int ime_poll(char *out, size_t size);
