#pragma once
#include <stdbool.h>
#include <stddef.h>

// Reading a QR code with the back camera, for pairing with the code Macapad shows on the Mac.
// Frames come in at 640x480, straight into a texture the screen draws as the preview.
#define QRSCAN_W 640
#define QRSCAN_H 480

// Turns the camera on and starts reading on its own threads. False if the camera won't start
// (a PS TV has none).
bool qrscan_start(void);
void qrscan_stop(void);

// The live preview, scaled into (x, y, w, h).
void qrscan_draw(float x, float y, float w, float h);

// The text of a QR code it read, once; then it goes back to looking.
bool qrscan_take(char *out, size_t size);
