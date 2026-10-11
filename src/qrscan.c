#include "qrscan.h"
#include "gfx.h"
#include "sys.h"

#include <psp2/camera.h>
#include <quirc.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#define CAMERA SCE_CAMERA_DEVICE_BACK

// The camera writes each frame into the preview texture. A capture thread asks it for frames and,
// whenever the decoder is free, hands it a grayscale copy; the decoder runs quirc on that. So the
// preview keeps moving while a frame is being read, which takes longer than a frame.
static vita2d_texture *s_tex;
static struct quirc *s_quirc;
static Thread s_captureThread, s_decodeThread;
static Event s_frameReady;
static Lock s_lock;
static bool s_lockMade;
static volatile bool s_running;
static volatile bool s_wantFrame;  // the decoder is waiting for one
static u8 *s_gray;                 // quirc's buffer, filled by the capture thread when s_wantFrame
static char s_found[1024];

static void capture_thread(void *arg) {
	while (s_running) {
		SceCameraRead read;
		memset(&read, 0, sizeof read);
		read.size = sizeof read;
		sceCameraRead(CAMERA, &read);
		if (s_wantFrame && s_gray) {
			// luma from the RGBA texture: about (77 R + 150 G + 29 B) / 256
			const u8 *px = vita2d_texture_get_datap(s_tex);
			unsigned stride = vita2d_texture_get_stride(s_tex);
			for (int y = 0; y < QRSCAN_H; y++) {
				const u8 *row = px + y * stride;
				u8 *out = s_gray + y * QRSCAN_W;
				for (int x = 0; x < QRSCAN_W; x++, row += 4) out[x] = (row[0] * 77 + row[1] * 150 + row[2] * 29) >> 8;
			}
			s_wantFrame = false;
			event_signal(&s_frameReady);
		}
		sleep_ms(30);
	}
}

static void decode_thread(void *arg) {
	while (s_running) {
		int w, h;
		s_gray = quirc_begin(s_quirc, &w, &h);
		__sync_synchronize();  // the buffer before the flag that says it's there
		s_wantFrame = true;
		while (s_running && !event_wait(&s_frameReady, 200)) {}
		if (!s_running) break;
		quirc_end(s_quirc);
		int n = quirc_count(s_quirc);
		for (int i = 0; i < n; i++) {
			struct quirc_code code;
			struct quirc_data data;
			quirc_extract(s_quirc, i, &code);
			if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
			lock(&s_lock);
			int len = data.payload_len < (int)sizeof s_found - 1 ? data.payload_len : (int)sizeof s_found - 1;
			memcpy(s_found, data.payload, len);
			s_found[len] = 0;
			unlock(&s_lock);
			break;
		}
	}
	s_wantFrame = false;
}

bool qrscan_start(void) {
	qrscan_stop();
	if (!s_lockMade) {
		lock_init(&s_lock, "qrscan");
		s_lockMade = true;
	}
	s_found[0] = 0;
	s_tex = vita2d_create_empty_texture(QRSCAN_W, QRSCAN_H);
	s_quirc = quirc_new();
	if (!s_tex || !s_quirc || quirc_resize(s_quirc, QRSCAN_W, QRSCAN_H) < 0) {
		qrscan_stop();
		return false;
	}
	vita2d_texture_set_filters(s_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);

	SceCameraInfo info;
	memset(&info, 0, sizeof info);
	info.size = sizeof info;
	info.priority = SCE_CAMERA_PRIORITY_SHARE;
	info.format = SCE_CAMERA_FORMAT_ABGR;  // bytes R, G, B, A: the texture's own layout
	info.resolution = SCE_CAMERA_RESOLUTION_640_480;
	info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
	info.sizeIBase = vita2d_texture_get_stride(s_tex) * QRSCAN_H;
	info.pitch = vita2d_texture_get_stride(s_tex) / 4 - QRSCAN_W;
	info.pIBase = vita2d_texture_get_datap(s_tex);
	if (sceCameraOpen(CAMERA, &info) < 0) {
		qrscan_stop();
		return false;
	}
	if (sceCameraStart(CAMERA) < 0) {
		sceCameraClose(CAMERA);
		qrscan_stop();
		return false;
	}
	event_init(&s_frameReady, "qrframe");
	s_running = true;
	s_captureThread = thread_start("camera", capture_thread, NULL);
	s_decodeThread = thread_start("qrdecode", decode_thread, NULL);
	return true;
}

void qrscan_stop(void) {
	if (s_running) {
		s_running = false;
		event_signal(&s_frameReady);
		thread_join(s_captureThread);
		thread_join(s_decodeThread);
		s_captureThread = s_decodeThread = 0;
		sceCameraStop(CAMERA);
		sceCameraClose(CAMERA);
		event_free(&s_frameReady);
	}
	s_gray = NULL;
	if (s_quirc) quirc_destroy(s_quirc);
	s_quirc = NULL;
	if (s_tex) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(s_tex);
	}
	s_tex = NULL;
}

void qrscan_draw(float x, float y, float w, float h) {
	if (!s_tex) return;
	gfx_flush();
	vita2d_draw_texture_scale(s_tex, x, y, w / QRSCAN_W, h / QRSCAN_H);
}

bool qrscan_take(char *out, size_t size) {
	if (!s_lockMade) return false;
	lock(&s_lock);
	bool got = s_found[0] != 0;
	if (got) {
		snprintf(out, size, "%s", s_found);
		s_found[0] = 0;
	}
	unlock(&s_lock);
	return got;
}
