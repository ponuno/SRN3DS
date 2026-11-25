#pragma once
#include <stdint.h>
#include "rt_table.h"
#include "x86.h"
#include "guest.h"
#include "win32_shims.h" /* WIN32_THUNK_BASE only; no shim internals */

/* Win32 input for the 3DS port: keyboard/mouse state, DirectInput8 objects and
 * the three xinput1_3 ordinals, fed from libctru HID.
 *
 * This header is the public contract; the implementation module owns the thunk
 * window, all guest-side objects and the virtual input devices.
 *
 * Thunk window: WIN32_INPUT_BASE + 4*i, disjoint from the other shim modules.
 * A dispatcher chains win32_input_lookup() after the main win32_lookup(). */
#define WIN32_INPUT_BASE (WIN32_THUNK_BASE + 0x00060000u)

typedef struct { const char *dll, *name; GuestFn fn; } Win32InputDef;

int win32_input_init(GuestMemory *m);
void win32_input_shutdown(void);
const Win32InputDef *win32_input_defs(unsigned *count);
/* Name-based resolution for the static IAT. */
uint32_t win32_input_resolve(const char *dll, const char *name);
/* Ordinal resolution (xinput1_3 imports #2/#3/#4 by ordinal). 0 if unknown. */
uint32_t win32_input_resolve_ordinal(const char *dll, unsigned ordinal);
const FnEntry *win32_input_lookup(uint32_t addr);

/* Sample libctru HID once (no-op on the host). The API queries also sample,
 * throttled to the 60 Hz input rate, so a front-end poll is recommended but
 * not required. */
void win32_input_poll(void);
/* Test / front-end injection. State changes raise GetAsyncKeyState's "pressed
 * since last call" bit and signal a DirectInput event set with
 * SetEventNotification. `buttons` bit 0 = left, 1 = right, 2 = middle. */
void win32_input_set_key(unsigned vk, int down);
void win32_input_set_mouse(int32_t x, int32_t y, int32_t dx, int32_t dy, unsigned buttons);
void win32_input_set_controller(int connected, unsigned buttons,
                                unsigned char left_trigger, unsigned char right_trigger,
                                int16_t lx, int16_t ly, int16_t rx, int16_t ry);
