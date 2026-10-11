#include "sys.h"

#include <arpa/inet.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 now_ms(void) { return sceKernelGetProcessTimeWide() / 1000; }

void sleep_ms(int ms) { sceKernelDelayThread(ms * 1000); }

// ---- threads ----------------------------------------------------------------

typedef struct {
	void (*fn)(void *);
	void *arg;
} Start;

static int thread_entry(SceSize args, void *argp) {
	Start start = **(Start **)argp;
	free(*(Start **)argp);
	start.fn(start.arg);
	return sceKernelExitThread(0);
}

Thread thread_start(const char *name, void (*fn)(void *), void *arg) {
	Start *start = malloc(sizeof *start);
	if (!start) return 0;
	start->fn = fn;
	start->arg = arg;
	// Off the main thread's core, so parsing answers and decoding images never cost the screen a frame.
	SceUID t = sceKernelCreateThread(name, thread_entry, 0x10000100, 256 * 1024, 0,
		SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (t < 0) {
		free(start);
		return 0;
	}
	if (sceKernelStartThread(t, sizeof start, &start) < 0) {
		free(start);
		sceKernelDeleteThread(t);
		return 0;
	}
	return t;
}

void thread_join(Thread t) {
	if (t <= 0) return;
	sceKernelWaitThreadEnd(t, NULL, NULL);
	sceKernelDeleteThread(t);
}

void lock_init(Lock *l, const char *name) { l->uid = sceKernelCreateMutex(name, 0, 0, NULL); }
void lock(Lock *l) { sceKernelLockMutex(l->uid, 1, NULL); }
void unlock(Lock *l) { sceKernelUnlockMutex(l->uid, 1); }

void event_init(Event *e, const char *name) { e->uid = sceKernelCreateSema(name, 0, 0, 1, NULL); }

void event_free(Event *e) {
	if (e->uid > 0) sceKernelDeleteSema(e->uid);
	e->uid = 0;
}

// (a full semaphore refuses the signal, which is what "remembered once" means)
void event_signal(Event *e) {
	if (e->uid > 0) sceKernelSignalSema(e->uid, 1);
}

bool event_wait(Event *e, int ms) {
	if (e->uid <= 0) {
		if (ms > 0) sleep_ms(ms);
		return false;
	}
	if (ms < 0) return sceKernelWaitSema(e->uid, 1, NULL) >= 0;
	SceUInt timeout = (SceUInt)ms * 1000;
	return sceKernelWaitSema(e->uid, 1, &timeout) >= 0;
}

// ---- system -----------------------------------------------------------------

static bool s_enterCircle;
static char s_netMem[1024 * 1024];

void sys_init(void) {
	SceAppUtilInitParam init;
	SceAppUtilBootParam boot;
	memset(&init, 0, sizeof init);
	memset(&boot, 0, sizeof boot);
	sceAppUtilInit(&init, &boot);

	// the keyboard follows the system's language and confirm button
	SceCommonDialogConfigParam config;
	sceCommonDialogConfigParamInit(&config);
	int lang = 0, enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
	config.language = lang;
	config.enterButtonAssign = enter;
	sceCommonDialogSetConfigParam(&config);
	s_enterCircle = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;

	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (sceNetShowNetstat() == (int)SCE_NET_ERROR_ENOTINIT) {
		SceNetInitParam net;
		net.memory = s_netMem;
		net.size = sizeof s_netMem;
		net.flags = 0;
		sceNetInit(&net);
	}
	sceNetCtlInit();
}

void sys_exit(void) {
	sceNetCtlTerm();
	sceNetTerm();
	sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
	sceAppUtilShutdown();
}

static u32 netctl_addr(int which) {
	int state = 0;
	if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) return 0;
	SceNetCtlInfo info;
	memset(&info, 0, sizeof info);
	if (sceNetCtlInetGetInfo(which, &info) < 0) return 0;
	const char *s = which == SCE_NETCTL_INFO_GET_NETMASK ? info.netmask : info.ip_address;
	struct in_addr a;
	return inet_aton(s, &a) ? ntohl(a.s_addr) : 0;
}

u32 net_ip(void) { return netctl_addr(SCE_NETCTL_INFO_GET_IP_ADDRESS); }
u32 net_mask(void) { return netctl_addr(SCE_NETCTL_INFO_GET_NETMASK); }

void device_name(char *out, size_t size) {
	char user[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE] = {0};
	if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8 *)user, sizeof user) >= 0 && user[0])
		snprintf(out, size, "%s's PS Vita", user);
	else
		snprintf(out, size, "PS Vita");
}

bool enter_is_circle(void) { return s_enterCircle; }

void power_keep_awake(void) {
	static u64 last;
	u64 now = now_ms();
	if (last && now - last < 1000) return;
	last = now;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

// ---- on-screen keyboard ---------------------------------------------------------

#define IME_MAX 256
static SceWChar16 s_imeTitle[64], s_imeInitial[IME_MAX + 1], s_imeBuf[IME_MAX + 1];
static bool s_imeActive;

static void utf8_to_utf16(SceWChar16 *out, size_t n, const char *in) {
	size_t o = 0;
	const u8 *s = (const u8 *)in;
	while (*s && o + 2 < n) {
		u32 cp;
		if (s[0] < 0x80) { cp = s[0]; s += 1; }
		else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
		else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
		else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
			cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
			s += 4;
		} else { cp = '?'; s += 1; }
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out[o++] = 0xD800 | (cp >> 10);
			out[o++] = 0xDC00 | (cp & 0x3FF);
		} else {
			out[o++] = cp;
		}
	}
	out[o] = 0;
}

static void utf16_to_utf8(char *out, size_t n, const SceWChar16 *in) {
	size_t o = 0;
	for (; *in; in++) {
		u32 cp = *in;
		if (cp >= 0xD800 && cp < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (in[1] - 0xDC00);
			in++;
		}
		char b[4];
		int len;
		if (cp < 0x80) { b[0] = cp; len = 1; }
		else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); len = 2; }
		else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); len = 3; }
		else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); len = 4; }
		if (o + len >= n) break;
		memcpy(out + o, b, len);
		o += len;
	}
	out[o] = 0;
}

bool ime_open(const char *title, const char *initial, ImeType type, int maxLen) {
	if (s_imeActive) return false;
	if (maxLen > IME_MAX) maxLen = IME_MAX;
	utf8_to_utf16(s_imeTitle, sizeof s_imeTitle / sizeof *s_imeTitle, title ? title : "");
	utf8_to_utf16(s_imeInitial, sizeof s_imeInitial / sizeof *s_imeInitial, initial ? initial : "");
	memset(s_imeBuf, 0, sizeof s_imeBuf);

	SceImeDialogParam param;
	sceImeDialogParamInit(&param);
	param.type = type == IME_URL ? SCE_IME_TYPE_URL : SCE_IME_TYPE_DEFAULT;
	param.option = type == IME_URL ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION : 0;
	param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	param.title = s_imeTitle;
	param.maxTextLength = maxLen;
	param.initialText = s_imeInitial;
	param.inputTextBuffer = s_imeBuf;
	s_imeActive = sceImeDialogInit(&param) >= 0;
	return s_imeActive;
}

bool ime_active(void) { return s_imeActive; }

int ime_poll(char *out, size_t size) {
	if (!s_imeActive) return -1;
	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
	SceImeDialogResult result;
	memset(&result, 0, sizeof result);
	sceImeDialogGetResult(&result);
	bool ok = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
	if (ok) utf16_to_utf8(out, size, s_imeBuf);
	sceImeDialogTerm();
	s_imeActive = false;
	return ok ? 1 : -1;
}
