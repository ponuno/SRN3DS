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

/* ---- 3DS pad -> guest control profile ----------------------------------------
 * The sampler translates a raw libctru button word into these portable bits, so
 * the mapping itself is host-testable without libctru.  Profiles:
 *   menu   (default) D-pad -> arrow keys (plus WASD), A -> Enter, B/Start ->
 *          Escape, Select -> Tab, X -> Space, Y -> E, touch -> left mouse.
 *   legacy        the pre-profile mapping: A/B/X -> mouse buttons and
 *          D-pad -> WASD (kept selectable for the old gameplay probes).
 * `TESV_INPUT_PROFILE=menu|legacy` selects at init; an unknown value is a named
 * stderr refusal that keeps the default. */
#define WIN32_INPUT_PAD_UP     0x00001u
#define WIN32_INPUT_PAD_DOWN   0x00002u
#define WIN32_INPUT_PAD_LEFT   0x00004u
#define WIN32_INPUT_PAD_RIGHT  0x00008u
#define WIN32_INPUT_PAD_A      0x00010u
#define WIN32_INPUT_PAD_B      0x00020u
#define WIN32_INPUT_PAD_X      0x00040u
#define WIN32_INPUT_PAD_Y      0x00080u
#define WIN32_INPUT_PAD_L      0x00100u
#define WIN32_INPUT_PAD_R      0x00200u
#define WIN32_INPUT_PAD_ZL     0x00400u
#define WIN32_INPUT_PAD_ZR     0x00800u
#define WIN32_INPUT_PAD_START  0x01000u
#define WIN32_INPUT_PAD_SELECT 0x02000u

typedef enum {
    WIN32_INPUT_PROFILE_MENU = 0,
    WIN32_INPUT_PROFILE_LEGACY = 1
} Win32InputProfile;

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

/* Control profile: select by name ("menu"/"legacy"), read it back, or apply a
 * portable pad mask through the active profile.  `win32_input_set_profile`
 * returns 0 on success and -1 for an unknown name (current profile unchanged).
 * The *apply* call replaces the pad-derived key/mouse state and raises the same
 * edges as a real sample; touch and analog stick are applied by the sampler. */
int win32_input_set_profile(const char *name);
const char *win32_input_profile_name(void);
Win32InputProfile win32_input_profile(void);
void win32_input_apply_pad(uint32_t pad_mask);
/* Pure mapping used by the sampler and the host test.  Clears and fills the
 * 256-entry virtual-key table plus the 8 mouse-button bits. */
void win32_input_map_pad(Win32InputProfile profile, uint32_t pad_mask,
                         uint8_t vk_down[256], uint8_t mouse_buttons[8]);
/* Test / front-end injection. State changes raise GetAsyncKeyState's "pressed
 * since last call" bit and signal a DirectInput event set with
 * SetEventNotification. `buttons` bit 0 = left, 1 = right, 2 = middle. */
void win32_input_set_key(unsigned vk, int down);
void win32_input_set_mouse(int32_t x, int32_t y, int32_t dx, int32_t dy, unsigned buttons);
void win32_input_set_controller(int connected, unsigned buttons,
                                unsigned char left_trigger, unsigned char right_trigger,
                                int16_t lx, int16_t ly, int16_t rx, int16_t ry);
