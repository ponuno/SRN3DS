/* Win32 input module: keyboard/mouse state, DirectInput8 objects and the three
 * xinput1_3 ordinals, fed from libctru HID on the 3DS.
 *
 * Contract (also in win32_input.h): every method has real bookkeeping. Things
 * that cannot be backed are refused explicitly:
 *  - DirectInput effects, force feedback, EnumObjects/GetObjectInfo (per-object
 *    descriptors) return DIERR_UNSUPPORTED. Device enumeration (EnumDevices) is
 *    implemented and calls the guest callback.
 *  - XInput reports no vibration motors (XInputGetCapabilities) and
 *    XInputSetState logs that the 3DS has no rumble motor but returns success,
 *    because XInput has no "unsupported" result for an advisory output.
 *  - Buffered DirectInput data is an empty queue (GetDeviceData returns 0
 *    elements); GetDeviceState is the state path.
 *  - On the host there is no HID source: the controller reports "not
 *    connected" and the keyboard/mouse are driven through the injection API
 *    used by the tests. On the 3DS libctru HID is sampled at 60 Hz and mapped
 *    through a selectable control profile (default: menu pad -> arrows/Enter/
 *    Escape/Tab, touch -> mouse; legacy keeps the old A/B/X mouse mapping).
 */
#include "win32_input.h"
#include "win32_sync.h"
#include "guest_process.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#endif

#define IN_MEM_BASE 0x1e100000u
#define IN_MEM_BYTES 0x3000u
#define IN_CB_STACK 0x1e110000u
#define IN_CB_STACK_BYTES 0x10000u
#define MAX_OBJECTS 16u
#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOINTERFACE 0x80004002u
#define DIERR_GENERIC 0x80004005u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_NOTINITIALIZED 0x80070015u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_ACQUIRED 0x800700AAu
#define DIERR_NOAGGREGATION 0x80040110u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_OLDDIRECTINPUTVERSION 0x8007047Eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u
#define DIERR_OBJECTNOTFOUND 0x80070002u
#define DIERR_OUTOFMEMORY 0x8007000Eu
#define DIRECTINPUT_VERSION 0x0800u
#define DIENUM_STOP 0u
#define DIENUM_CONTINUE 1u
#define DI8DEVTYPE_DEVICE 0x11u
#define DI8DEVTYPE_MOUSE 0x12u
#define DI8DEVTYPE_KEYBOARD 0x13u
#define DI8DEVTYPE_MOUSE_TRADITIONAL 0x0200u
#define DI8DEVTYPE_KEYBOARD_PCENH 0x0400u
#define DIDC_ATTACHED 0x00000001u
#define DIDC_EMULATED 0x00000004u
#define DIPROP_BUFFERSIZE 1u
#define DIPROP_RANGE 4u
#define DIPROP_DEADZONE 5u
#define DIPROP_SATURATION 6u
#define DISCL_ALL 0x0000000Fu
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_BAD_ARGUMENTS 160u
#define VK_LBUTTON 0x01u
#define VK_RBUTTON 0x02u
#define VK_MBUTTON 0x04u

/* ---- virtual key <-> DirectInput scan code ------------------------------------ */
static uint8_t vk2dik[256], dik2vk[256], dik2vk_ex[256];
static const uint8_t dik_letters[26] = {
    0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
    0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C
};
static void build_key_tables(void) {
    memset(vk2dik, 0, sizeof vk2dik);
    memset(dik2vk, 0, sizeof dik2vk);
    memset(dik2vk_ex, 0, sizeof dik2vk_ex);
    for (unsigned i = 0; i < 26; ++i) {
        uint8_t vk = (uint8_t)('A' + i);
        vk2dik[vk] = dik_letters[i];
        dik2vk[dik_letters[i]] = vk;
        dik2vk_ex[dik_letters[i]] = vk;
    }
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t vk = (uint8_t)('0' + i);
        uint8_t dik = (uint8_t)(i ? 0x01 + i : 0x0B);
        vk2dik[vk] = dik; dik2vk[dik] = vk; dik2vk_ex[dik] = vk;
    }
    static const struct { uint8_t vk, dik, generic; } t[] = {
        {0x08,0x0E,0x08}, {0x09,0x0F,0x09}, {0x0D,0x1C,0x0D}, {0x13,0x45,0x13},
        {0x14,0x3A,0x14}, {0x1B,0x01,0x1B}, {0x20,0x39,0x20},
        {0x21,0x49,0x21}, {0x22,0x51,0x22}, {0x23,0x4F,0x23}, {0x24,0x47,0x24},
        {0x25,0x4B,0x25}, {0x26,0x48,0x26}, {0x27,0x4D,0x27}, {0x28,0x50,0x28},
        {0x2D,0x52,0x2D}, {0x2E,0x53,0x2E},
        {0x5B,0xDB,0x5B}, {0x5C,0xDC,0x5C}, {0x5D,0xDD,0x5D},
        {0xBA,0x27,0xBA}, {0xBB,0x0D,0xBB}, {0xBC,0x33,0xBC}, {0xBD,0x0C,0xBD},
        {0xBE,0x34,0xBE}, {0xBF,0x35,0xBF}, {0xC0,0x29,0xC0}, {0xDB,0x1A,0xDB},
        {0xDC,0x2B,0xDC}, {0xDD,0x1B,0xDD}, {0xDE,0x28,0xDE},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; ++i) {
        vk2dik[t[i].vk] = t[i].dik;
        dik2vk[t[i].dik] = t[i].generic;
        dik2vk_ex[t[i].dik] = t[i].vk;
    }
    for (unsigned i = 0; i < 12; ++i) {
        uint8_t vk = (uint8_t)(0x70 + i), dik = (uint8_t)(i < 10 ? 0x3B + i : 0x57 + i - 10);
        vk2dik[vk] = dik; dik2vk[dik] = vk; dik2vk_ex[dik] = vk;
    }
    vk2dik[0x10] = 0x2A; vk2dik[0xA0] = 0x2A; vk2dik[0xA1] = 0x36;
    dik2vk[0x2A] = 0x10; dik2vk[0x36] = 0x10;
    dik2vk_ex[0x2A] = 0xA0; dik2vk_ex[0x36] = 0xA1;
    vk2dik[0x11] = 0x1D; vk2dik[0xA2] = 0x1D; vk2dik[0xA3] = 0x1D;
    dik2vk[0x1D] = 0x11; dik2vk_ex[0x1D] = 0xA2;
    vk2dik[0x12] = 0x38; vk2dik[0xA4] = 0x38; vk2dik[0xA5] = 0x38;
    dik2vk[0x38] = 0x12; dik2vk_ex[0x38] = 0xA4;
}
static int vk_char(uint32_t vk, int shift, int caps) {
    if (vk >= 'A' && vk <= 'Z') return (shift ^ caps) ? (int)vk : (int)vk + 32;
    if (vk >= '0' && vk <= '9') {
        static const char sh[] = ")!@#$%^&*(";
        return shift ? sh[vk - '0'] : (int)vk;
    }
    switch (vk) {
    case 0x08: return 8; case 0x09: return '\t'; case 0x0D: return '\r';
    case 0x1B: return 27; case 0x20: return ' ';
    case 0xBA: return shift ? ':' : ';'; case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ','; case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`'; case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\'; case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\''; default: return 0;
    }
}

/* ---- control profiles ---------------------------------------------------------- */
/* Portable pad mask -> guest virtual keys.  This is the single mapping table used
 * by the 3DS sampler, so a host test can prove exactly what the guest will see. */
void win32_input_map_pad(Win32InputProfile profile, uint32_t pad,
                         uint8_t vk_down[256], uint8_t mouse_buttons[8]) {
    uint8_t *vk = vk_down;
    memset(vk, 0, 256);
    memset(mouse_buttons, 0, 8);
#define SET(v) (vk[(unsigned)(v)] = 1)
    if (profile == WIN32_INPUT_PROFILE_LEGACY) {
        /* Pre-profile behaviour: A/B/X are mouse buttons, D-pad is WASD. */
        if (pad & WIN32_INPUT_PAD_A) { SET(VK_LBUTTON); mouse_buttons[0] = 1; }
        if (pad & WIN32_INPUT_PAD_B) { SET(VK_RBUTTON); mouse_buttons[1] = 1; }
        if (pad & WIN32_INPUT_PAD_X) { SET(VK_MBUTTON); mouse_buttons[2] = 1; SET(0x20); }
        if (pad & WIN32_INPUT_PAD_Y) SET('E');
        if (pad & WIN32_INPUT_PAD_UP) SET('W');
        if (pad & WIN32_INPUT_PAD_DOWN) SET('S');
        if (pad & WIN32_INPUT_PAD_LEFT) SET('A');
        if (pad & WIN32_INPUT_PAD_RIGHT) SET('D');
    } else {
        /* Menu profile: arrows for range selection, Enter to accept, Escape for
         * back, Tab for the tween/back menu.  WASD stays held as well because the
         * retail ControlMap may bind menu navigation to either set; touch is the
         * only left-click source, so A is never a click here. */
        if (pad & WIN32_INPUT_PAD_UP) { SET(0x26); SET('W'); }
        if (pad & WIN32_INPUT_PAD_DOWN) { SET(0x28); SET('S'); }
        if (pad & WIN32_INPUT_PAD_LEFT) { SET(0x25); SET('A'); }
        if (pad & WIN32_INPUT_PAD_RIGHT) { SET(0x27); SET('D'); }
        if (pad & WIN32_INPUT_PAD_A) SET(0x0D);      /* VK_RETURN */
        if (pad & WIN32_INPUT_PAD_B) SET(0x1B);      /* VK_ESCAPE */
        if (pad & WIN32_INPUT_PAD_START) SET(0x1B);  /* VK_ESCAPE */
        if (pad & WIN32_INPUT_PAD_SELECT) SET(0x09); /* VK_TAB */
        if (pad & WIN32_INPUT_PAD_X) SET(0x20);      /* VK_SPACE */
        if (pad & WIN32_INPUT_PAD_Y) SET('E');
    }
    if (pad & WIN32_INPUT_PAD_L) SET(0xA0);          /* VK_LSHIFT */
    if (pad & WIN32_INPUT_PAD_R) SET('R');
    if (pad & WIN32_INPUT_PAD_ZL) SET(0xA2);         /* VK_RCONTROL */
    if (pad & WIN32_INPUT_PAD_ZR) SET(0xA4);         /* VK_RMENU */
#undef SET
}

/* ---- state --------------------------------------------------------------------- */
enum { OBJ_FREE = 0, OBJ_DI_A, OBJ_DI_W, OBJ_KB_A, OBJ_KB_W, OBJ_MOUSE_A, OBJ_MOUSE_W };
typedef struct {
    uint8_t kind, refs, acquired, wide, mouse;
    uint32_t guest, data_size, notify, coop, buffer_size, deadzone, saturation;
} DIObj;
static struct {
    GuestMemory *mem;
    Win32InputProfile profile;
    uint8_t down[256], prev[256], pressed[256], toggled[256];
    int32_t mx, my, dx, dy, dz;
    uint8_t mb[8], mb_prev[8];
    uint64_t last_sample;
    int pad_connected;
    uint16_t pad_buttons;
    uint8_t pad_lt, pad_rt;
    int16_t pad_lx, pad_ly, pad_rx, pad_ry;
    uint32_t packet;
    DIObj obj[MAX_OBJECTS];
    unsigned next;
    CPU cb_cpu;
    int cb_ready;
    int rumble_logged;
} g;

static void set_error(CPU *c, uint32_t v) {
    if (c && c->fs_base) M32(c->fs_base + 0x34) = v;
    win32_set_last_error(v);
}
#define A(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)

static void apply_pad_state(uint32_t pad) {
    uint8_t nd[256], nb[8];
    win32_input_map_pad(g.profile, pad, nd, nb);
    memcpy(g.down, nd, sizeof nd);
    memcpy(g.mb, nb, sizeof nb);
}
static void sample_3ds(void) {
#ifdef __3DS__
    uint64_t now = svcGetSystemTick();
    if (g.last_sample && (now - g.last_sample) * 1000 / SYSCLOCK_ARM11 < 16) return;
    g.last_sample = now;
    hidScanInput();
    uint32_t held = hidKeysHeld();
    uint32_t pad = 0;
#define PAD(bit, m) do { if (held & (bit)) pad |= (m); } while (0)
    PAD(KEY_DUP, WIN32_INPUT_PAD_UP); PAD(KEY_DDOWN, WIN32_INPUT_PAD_DOWN);
    PAD(KEY_DLEFT, WIN32_INPUT_PAD_LEFT); PAD(KEY_DRIGHT, WIN32_INPUT_PAD_RIGHT);
    PAD(KEY_A, WIN32_INPUT_PAD_A); PAD(KEY_B, WIN32_INPUT_PAD_B);
    PAD(KEY_X, WIN32_INPUT_PAD_X); PAD(KEY_Y, WIN32_INPUT_PAD_Y);
    PAD(KEY_L, WIN32_INPUT_PAD_L); PAD(KEY_R, WIN32_INPUT_PAD_R);
    PAD(KEY_ZL, WIN32_INPUT_PAD_ZL); PAD(KEY_ZR, WIN32_INPUT_PAD_ZR);
    PAD(KEY_START, WIN32_INPUT_PAD_START); PAD(KEY_SELECT, WIN32_INPUT_PAD_SELECT);
#undef PAD
    apply_pad_state(pad);
    circlePosition cp; hidCircleRead(&cp);
    touchPosition tp; hidTouchRead(&tp);
    int touch = (held & KEY_TOUCH) != 0;
    if (cp.dx > 20 || cp.dx < -20) { g.dx += cp.dx / 24; g.mx += cp.dx / 24; }
    if (cp.dy > 20 || cp.dy < -20) { g.dy -= cp.dy / 24; g.my -= cp.dy / 24; }
    if (touch) {
        g.mx = tp.px * 400 / 320; g.my = tp.py;
        g.down[VK_LBUTTON] = 1;
        g.mb[0] = 1;
    }
    if (g.mx < 0) g.mx = 0; if (g.mx > 399) g.mx = 399;
    if (g.my < 0) g.my = 0; if (g.my > 239) g.my = 239;
    g.pad_connected = 1;
    g.pad_buttons = 0;
    if (held & KEY_A) g.pad_buttons |= 0x1000;
    if (held & KEY_B) g.pad_buttons |= 0x2000;
    if (held & KEY_X) g.pad_buttons |= 0x4000;
    if (held & KEY_Y) g.pad_buttons |= 0x8000;
    if (held & KEY_L) g.pad_buttons |= 0x0100;
    if (held & KEY_R) g.pad_buttons |= 0x0200;
    if (held & KEY_START) g.pad_buttons |= 0x0010;
    if (held & KEY_SELECT) g.pad_buttons |= 0x0020;
    if (held & KEY_DUP) g.pad_buttons |= 0x0001;
    if (held & KEY_DDOWN) g.pad_buttons |= 0x0002;
    if (held & KEY_DLEFT) g.pad_buttons |= 0x0004;
    if (held & KEY_DRIGHT) g.pad_buttons |= 0x0008;
    g.pad_lt = (held & KEY_ZL) ? 255 : 0;
    g.pad_rt = (held & KEY_ZR) ? 255 : 0;
    g.pad_lx = (int16_t)(cp.dx * 32767 / 156);
    g.pad_ly = (int16_t)(-cp.dy * 32767 / 156);
    if (held & KEY_CSTICK_RIGHT) g.pad_rx = 32767;
    else if (held & KEY_CSTICK_LEFT) g.pad_rx = -32768;
    else g.pad_rx = 0;
    if (held & KEY_CSTICK_UP) g.pad_ry = 32767;
    else if (held & KEY_CSTICK_DOWN) g.pad_ry = -32768;
    else g.pad_ry = 0;
#endif
}
/* Recompute per-key edges and signal DirectInput events on any change. */
static int refresh(void) {
    sample_3ds();
    g.down[VK_LBUTTON] = g.mb[0] ? 1 : 0;
    g.down[VK_RBUTTON] = g.mb[1] ? 1 : 0;
    g.down[VK_MBUTTON] = g.mb[2] ? 1 : 0;
    int changed = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (g.down[i] && !g.prev[i]) {
            g.pressed[i] = 1; changed = 1;
            if (i == 0x14 || i == 0x90 || i == 0x91) g.toggled[i] ^= 1u;
        }
        if (g.down[i] != g.prev[i]) changed = 1;
        g.prev[i] = g.down[i];
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (g.mb[i] != g.mb_prev[i]) changed = 1;
        g.mb_prev[i] = g.mb[i];
    }
    if (changed)
        for (unsigned i = 0; i < g.next; ++i)
            if (g.obj[i].kind != OBJ_FREE && g.obj[i].notify)
                win32_sync_signal(g.obj[i].notify);
    return changed;
}
void win32_input_poll(void) { refresh(); }
void win32_input_apply_pad(uint32_t pad) { apply_pad_state(pad); refresh(); }
const char *win32_input_profile_name(void) {
    return g.profile == WIN32_INPUT_PROFILE_LEGACY ? "legacy" : "menu";
}
Win32InputProfile win32_input_profile(void) { return g.profile; }
int win32_input_set_profile(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "menu")) { g.profile = WIN32_INPUT_PROFILE_MENU; return 0; }
    if (!strcmp(name, "legacy")) { g.profile = WIN32_INPUT_PROFILE_LEGACY; return 0; }
    fprintf(stderr, "[input] unknown control profile \"%s\" (expected menu|legacy); "
                    "keeping \"%s\"\n", name, win32_input_profile_name());
    return -1;
}
void win32_input_set_key(unsigned vk, int down) {
    if (vk > 255) return;
    g.down[vk] = down ? 1 : 0;
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) g.mb[vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : 2] = (uint8_t)(down ? 1 : 0);
    refresh();
}
void win32_input_set_mouse(int32_t x, int32_t y, int32_t dx, int32_t dy, unsigned buttons) {
    g.mx = x; g.my = y; g.dx += dx; g.dy += dy;
    for (unsigned i = 0; i < 8; ++i) g.mb[i] = (uint8_t)((buttons >> i) & 1);
    refresh();
}
void win32_input_set_controller(int connected, unsigned buttons, unsigned char lt, unsigned char rt,
                                int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    g.pad_connected = connected != 0;
    g.pad_buttons = (uint16_t)buttons;
    g.pad_lt = lt; g.pad_rt = rt;
    g.pad_lx = lx; g.pad_ly = ly; g.pad_rx = rx; g.pad_ry = ry;
}

/* ---- GUIDs -------------------------------------------------------------------- */
static const uint8_t IID_UNKNOWN[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
static const uint8_t IID_DI8A[16] = {0x30,0x80,0x79,0xBF,0x3A,0x48,0xA2,0x4D,0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00};
static const uint8_t IID_DI8W[16] = {0x31,0x80,0x79,0xBF,0x3A,0x48,0xA2,0x4D,0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00};
static const uint8_t IID_DEV8A[16] = {0x80,0x10,0xD4,0x54,0x15,0xDC,0x33,0x48,0xA4,0x1B,0x74,0x8F,0x73,0xA3,0x81,0x79};
static const uint8_t IID_DEV8W[16] = {0x81,0x10,0xD4,0x54,0x15,0xDC,0x33,0x48,0xA4,0x1B,0x74,0x8F,0x73,0xA3,0x81,0x79};
static const uint8_t GUID_KEYBOARD[16] = {0x61,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
static const uint8_t GUID_MOUSE[16] = {0x60,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
static int guid_is(uint32_t addr, const uint8_t *want) {
    if (!addr) return 0;
    const uint8_t *p = guest_ptr(g.mem, addr, 16);
    return p && !memcmp(p, want, 16);
}

/* ---- DirectInput COM objects ---------------------------------------------------- */
#define VT_DI_A (IN_MEM_BASE + 0x000u)
#define VT_DI_W (IN_MEM_BASE + 0x200u)
#define VT_DEV_A (IN_MEM_BASE + 0x400u)
#define VT_DEV_W (IN_MEM_BASE + 0x600u)
#define OBJ_CELL_BASE (IN_MEM_BASE + 0x1000u)

enum {
    I_DirectInput8Create,
    I_XInputGetState, I_XInputSetState, I_XInputGetCapabilities,
    I_GetKeyboardState, I_GetAsyncKeyState, I_GetKeyState, I_MapVirtualKeyA, I_ToUnicode,
    I_GetKeyboardLayout, I_GetCursorPos, I_SetCursorPos,
    I_unsupported, I_query, I_addref, I_release,
    I_di_create_device, I_di_enum_devices, I_di_get_device_status,
    I_di_run_control_panel, I_di_initialize,
    I_dev_caps, I_dev_enum_objects, I_dev_get_property, I_dev_set_property,
    I_dev_acquire, I_dev_unacquire, I_dev_get_state, I_dev_get_data,
    I_dev_set_format, I_dev_set_notification, I_dev_set_coop, I_dev_get_object_info,
    I_dev_get_info, I_dev_poll, I_dev_escape,
    FN_COUNT
};
#define TH(i) (WIN32_INPUT_BASE + 4u * (i))

static DIObj *dobj(uint32_t addr, int any) {
    if (addr < OBJ_CELL_BASE || (addr - OBJ_CELL_BASE) % 16) return NULL;
    unsigned i = (addr - OBJ_CELL_BASE) / 16;
    if (i >= g.next || g.obj[i].kind == OBJ_FREE) return NULL;
    if (!any && g.obj[i].guest != addr) return NULL;
    return &g.obj[i];
}
static DIObj *new_obj(unsigned kind, uint32_t vtable) {
    unsigned i;
    for (i = 0; i < g.next; ++i) if (g.obj[i].kind == OBJ_FREE) break;
    if (i == g.next) {
        if (g.next >= MAX_OBJECTS) return NULL;
        ++g.next;
    }
    DIObj *o = &g.obj[i];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->guest = OBJ_CELL_BASE + i * 16;
    o->wide = (uint8_t)(kind == OBJ_DI_W || kind == OBJ_KB_W || kind == OBJ_MOUSE_W);
    o->mouse = (uint8_t)(kind == OBJ_MOUSE_A || kind == OBJ_MOUSE_W);
    o->saturation = 10000;
    M32(o->guest) = vtable;
    return o;
}
static void wstr(uint32_t addr, const char *s) {
    for (size_t i = 0; i <= strlen(s); ++i) M16(addr + (uint32_t)i * 2) = (uint8_t)s[i];
}
static void fill_device_instance(uint32_t p, int wide, int mouse) {
    uint32_t size = wide ? 1100u : 580u;
    uint8_t *b = guest_ptr(g.mem, p, size);
    if (!b) return;
    memset(b, 0, size);
    M32(p) = size;
    const uint8_t *guid = mouse ? GUID_MOUSE : GUID_KEYBOARD;
    memcpy(b + 4, guid, 16);
    memcpy(b + 20, guid, 16);
    M32(p + 36) = mouse ? (DI8DEVTYPE_MOUSE | DI8DEVTYPE_MOUSE_TRADITIONAL)
                        : (DI8DEVTYPE_KEYBOARD | DI8DEVTYPE_KEYBOARD_PCENH);
    const char *iname = mouse ? "Mouse" : "Keyboard";
    const char *pname = mouse ? "3DS virtual mouse" : "3DS virtual keyboard";
    if (wide) { wstr(p + 40, iname); wstr(p + 40 + 520, pname); }
    else { memcpy(b + 40, iname, strlen(iname)); memcpy(b + 300, pname, strlen(pname)); }
}

static void s_unsupported(CPU *c) { fprintf(stderr, "[input] unsupported DirectInput8 method\n"); rt_unimpl(c, 0, "unsupported DirectInput8 method"); }
static void s_query(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t out = A(2);
    if (!o) RET(DIERR_INVALIDPARAM, 3);
    if (!out || !guest_ptr(g.mem, out, 4)) RET(DIERR_INVALIDPARAM, 3);
    uint8_t kind = o->kind;
    int ok = guid_is(A(1), IID_UNKNOWN) ||
             (kind == OBJ_DI_A && guid_is(A(1), IID_DI8A)) ||
             (kind == OBJ_DI_W && guid_is(A(1), IID_DI8W)) ||
             ((kind == OBJ_KB_A || kind == OBJ_MOUSE_A) && guid_is(A(1), IID_DEV8A)) ||
             ((kind == OBJ_KB_W || kind == OBJ_MOUSE_W) && guid_is(A(1), IID_DEV8W));
    if (!ok) { M32(out) = 0; RET(DIERR_NOINTERFACE, 3); }
    ++o->refs;
    M32(out) = o->guest;
    RET(S_OK, 3);
}
static void s_addref(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(0, 1);
    if (o->refs < 255) ++o->refs;
    RET(o->refs, 1);
}
static void s_release(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(0, 1);
    uint32_t refs = o->refs ? --o->refs : 0;
    if (!refs) { M32(o->guest) = 0; memset(o, 0, sizeof *o); }
    RET(refs, 1);
}

static void s_di_create_device(CPU *c) {
    DIObj *parent = dobj(A(0), 0);
    uint32_t out = A(2);
    if (!parent || !(parent->kind == OBJ_DI_A || parent->kind == OBJ_DI_W)) RET(DIERR_INVALIDPARAM, 4);
    if (!out || !guest_ptr(g.mem, out, 4)) RET(DIERR_INVALIDPARAM, 4);
    if (A(3)) { M32(out) = 0; RET(DIERR_NOAGGREGATION, 4); }
    int mouse;
    if (guid_is(A(1), GUID_KEYBOARD)) mouse = 0;
    else if (guid_is(A(1), GUID_MOUSE)) mouse = 1;
    else { M32(out) = 0; RET(DIERR_DEVICENOTREG, 4); }
    unsigned kind = parent->wide ? (mouse ? OBJ_MOUSE_W : OBJ_KB_W) : (mouse ? OBJ_MOUSE_A : OBJ_KB_A);
    DIObj *o = new_obj(kind, parent->wide ? VT_DEV_W : VT_DEV_A);
    if (!o) { M32(out) = 0; RET(DIERR_OUTOFMEMORY, 4); }
    M32(out) = o->guest;
    RET(S_OK, 4);
}
/* Call a guest DirectInput callback (cdecl, two arguments) on a private CPU. */
static int call_guest(CPU *caller, uint32_t fn, const uint32_t *args, size_t argc, uint32_t *result) {
    if (!g.cb_ready) { *result = 0; return -1; }
    int rc = n3ds_lift_invoke_reentrant(&g.cb_cpu, fn, args, argc, result);
    if (rc) { rt_unimpl(caller, fn, n3ds_lift_fault()); }
    return rc;
}
static void s_di_enum_devices(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t filter = A(1), cb = A(2), ref = A(3);
    if (!o || !(o->kind == OBJ_DI_A || o->kind == OBJ_DI_W)) RET(DIERR_INVALIDPARAM, 5);
    if (!guest_ptr(g.mem, IN_MEM_BASE + 0x2000, 1100)) RET(DIERR_GENERIC, 5);
    static const unsigned types[2] = {DI8DEVTYPE_KEYBOARD, DI8DEVTYPE_MOUSE};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned type = types[i];
        int want = !filter || filter == DI8DEVTYPE_DEVICE || (filter & 0xff) == type;
        if (!want) continue;
        uint32_t inst = IN_MEM_BASE + 0x2000;
        fill_device_instance(inst, o->wide, type == DI8DEVTYPE_MOUSE);
        if (cb) {
            uint32_t args[2] = {inst, ref}, res = DIENUM_CONTINUE;
            call_guest(c, cb, args, 2, &res);
            if (res == DIENUM_STOP) break;
        }
    }
    RET(S_OK, 5);
}
static void s_di_get_status(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(DIERR_INVALIDPARAM, 2);
    RET(guid_is(A(1), GUID_KEYBOARD) || guid_is(A(1), GUID_MOUSE) ? S_OK : S_FALSE, 2);
}
static void s_di_run_control_panel(CPU *c) { RET(S_OK, 3); }
static void s_di_initialize(CPU *c) { (void)A(1); (void)A(2); (void)A(3); RET(S_OK, 4); }

static void s_dev_caps(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t p = A(1);
    if (!o || !p || !guest_ptr(g.mem, p, 4)) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = M32(p);
    if (size < 44) RET(DIERR_INVALIDPARAM, 2);
    uint8_t *b = guest_ptr(g.mem, p, 44);
    if (!b) RET(DIERR_INVALIDPARAM, 2);
    memset(b + 4, 0, 40);
    M32(p + 4) = DIDC_ATTACHED | DIDC_EMULATED;
    M32(p + 8) = o->mouse ? (DI8DEVTYPE_MOUSE | DI8DEVTYPE_MOUSE_TRADITIONAL)
                          : (DI8DEVTYPE_KEYBOARD | DI8DEVTYPE_KEYBOARD_PCENH);
    M32(p + 12) = o->mouse ? 3u : 0u;
    M32(p + 16) = o->mouse ? (o->data_size == 20 ? 8u : 3u) : 0u;
    M32(p + 20) = 0;
    RET(S_OK, 2);
}
static void s_dev_enum_objects(CPU *c) {
    (void)c;
    fprintf(stderr, "[input] IDirectInputDevice8::EnumObjects is not implemented "
                    "(per-object descriptors); returning DIERR_UNSUPPORTED\n");
    RET(DIERR_UNSUPPORTED, 4);
}
static void s_dev_get_property(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t prop = A(1), ph = A(2);
    if (!o || !ph || !guest_ptr(g.mem, ph, 16)) RET(DIERR_INVALIDPARAM, 3);
    uint32_t size = M32(ph);
    if (size < 16) RET(DIERR_INVALIDPARAM, 3);
    if (!guest_ptr(g.mem, ph, size)) RET(DIERR_INVALIDPARAM, 3);
    if (prop == DIPROP_BUFFERSIZE && size >= 20) { M32(ph + 16) = o->buffer_size; RET(S_OK, 3); }
    if (prop == DIPROP_RANGE && size >= 24 && o->mouse) {
        M32(ph + 16) = (uint32_t)-1000; M32(ph + 20) = 1000; RET(S_OK, 3);
    }
    if (prop == DIPROP_DEADZONE && size >= 20) { M32(ph + 16) = o->deadzone; RET(S_OK, 3); }
    if (prop == DIPROP_SATURATION && size >= 20) { M32(ph + 16) = o->saturation; RET(S_OK, 3); }
    RET(DIERR_UNSUPPORTED, 3);
}
static void s_dev_set_property(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t prop = A(1), ph = A(2);
    if (!o || !ph || !guest_ptr(g.mem, ph, 16)) RET(DIERR_INVALIDPARAM, 3);
    uint32_t size = M32(ph);
    if (size < 16 || !guest_ptr(g.mem, ph, size)) RET(DIERR_INVALIDPARAM, 3);
    if (prop == DIPROP_BUFFERSIZE && size >= 20) { o->buffer_size = M32(ph + 16); RET(S_OK, 3); }
    if (prop == DIPROP_DEADZONE && size >= 20) { o->deadzone = M32(ph + 16); RET(S_OK, 3); }
    if (prop == DIPROP_SATURATION && size >= 20) { o->saturation = M32(ph + 16); RET(S_OK, 3); }
    if (prop == DIPROP_RANGE && size >= 24) RET(S_FALSE, 3); /* DI_PROPNOEFFECT */
    RET(DIERR_UNSUPPORTED, 3);
}
static void s_dev_acquire(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(DIERR_INVALIDPARAM, 1);
    if (!o->data_size) RET(DIERR_NOTINITIALIZED, 1);
    if (o->acquired) RET(DIERR_ACQUIRED, 1);
    o->acquired = 1;
    RET(S_OK, 1);
}
static void s_dev_unacquire(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(DIERR_INVALIDPARAM, 1);
    o->acquired = 0;
    RET(S_OK, 1);
}
static void s_dev_get_state(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t size = A(1), p = A(2);
    if (!o) RET(DIERR_INVALIDPARAM, 3);
    if (!o->acquired) RET(DIERR_NOTACQUIRED, 3);
    if (!o->data_size) RET(DIERR_NOTINITIALIZED, 3);
    if (!p || size < o->data_size || !guest_ptr(g.mem, p, o->data_size)) RET(DIERR_INVALIDPARAM, 3);
    refresh();
    uint8_t *b = guest_ptr(g.mem, p, o->data_size);
    memset(b, 0, o->data_size);
    if (!o->mouse) {
        for (unsigned vk = 0; vk < 256; ++vk)
            if (g.down[vk] && vk2dik[vk]) b[vk2dik[vk]] = 0x80;
    } else {
        int32_t *axes = (int32_t *)b;
        axes[0] = g.dx; axes[1] = g.dy; axes[2] = g.dz;
        g.dx = g.dy = g.dz = 0;
        unsigned buttons = o->data_size >= 20 ? 8 : 4;
        for (unsigned i = 0; i < buttons; ++i) b[12 + i] = g.mb[i] ? 0x80 : 0;
    }
    RET(S_OK, 3);
}
static void s_dev_get_data(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t out = A(3);
    if (!o) RET(DIERR_INVALIDPARAM, 5);
    if (!o->acquired) RET(DIERR_NOTACQUIRED, 5);
    if (!out || !guest_ptr(g.mem, out, 4)) RET(DIERR_INVALIDPARAM, 5);
    M32(out) = 0; /* buffered queue is empty; GetDeviceState is the state path */
    RET(S_OK, 5);
}
static void s_dev_set_format(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t fmt = A(1);
    if (!o) RET(DIERR_INVALIDPARAM, 2);
    if (!fmt || !guest_ptr(g.mem, fmt, 24)) RET(DIERR_INVALIDPARAM, 2);
    uint32_t dw_size = M32(fmt), data_size = M32(fmt + 12);
    if (dw_size < 24) RET(DIERR_INVALIDPARAM, 2);
    if (o->mouse) {
        if (data_size != 16 && data_size != 20) RET(DIERR_INVALIDPARAM, 2);
    } else if (data_size != 256) RET(DIERR_INVALIDPARAM, 2);
    o->data_size = data_size;
    RET(S_OK, 2);
}
static void s_dev_set_notification(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(DIERR_INVALIDPARAM, 2);
    o->notify = A(1);
    RET(S_OK, 2);
}
static void s_dev_set_coop(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    if (!o) RET(DIERR_INVALIDPARAM, 3);
    if (A(2) & ~DISCL_ALL) RET(DIERR_INVALIDPARAM, 3);
    o->coop = A(2);
    RET(S_OK, 3);
}
static void s_dev_get_object_info(CPU *c) {
    (void)c;
    fprintf(stderr, "[input] IDirectInputDevice8::GetObjectInfo is not implemented; "
                    "returning DIERR_OBJECTNOTFOUND\n");
    RET(DIERR_OBJECTNOTFOUND, 4);
}
static void s_dev_get_info(CPU *c) {
    DIObj *o = dobj(A(0), 0);
    uint32_t p = A(1);
    if (!o || !p) RET(DIERR_INVALIDPARAM, 2);
    uint32_t need = o->wide ? 1100u : 580u;
    if (!guest_ptr(g.mem, p, 4) || M32(p) < need) RET(DIERR_INVALIDPARAM, 2);
    fill_device_instance(p, o->wide, o->mouse);
    RET(S_OK, 2);
}
static void s_dev_poll(CPU *c) { refresh(); RET(S_OK, 1); }
static void s_dev_escape(CPU *c) {
    (void)A(1);
    RET(DIERR_UNSUPPORTED, 2);
}

static void s_DirectInput8Create(CPU *c) {
    uint32_t version = A(1), riid = A(2), out = A(3), outer = A(4);
    if (!out || !guest_ptr(g.mem, out, 4)) RET(DIERR_INVALIDPARAM, 5);
    if (version != DIRECTINPUT_VERSION) {
        M32(out) = 0;
        RET(version < DIRECTINPUT_VERSION ? DIERR_OLDDIRECTINPUTVERSION : DIERR_BETADIRECTINPUTVERSION, 5);
    }
    if (outer) { M32(out) = 0; RET(DIERR_NOAGGREGATION, 5); }
    int wide;
    if (guid_is(riid, IID_DI8A)) wide = 0;
    else if (guid_is(riid, IID_DI8W)) wide = 1;
    else { M32(out) = 0; RET(DIERR_NOINTERFACE, 5); }
    DIObj *o = new_obj(wide ? OBJ_DI_W : OBJ_DI_A, wide ? VT_DI_W : VT_DI_A);
    if (!o) { M32(out) = 0; RET(DIERR_OUTOFMEMORY, 5); }
    M32(out) = o->guest;
    RET(S_OK, 5);
}

/* ---- keyboard / mouse ---------------------------------------------------------- */
static void s_GetKeyboardState(CPU *c) {
    uint32_t p = A(0);
    if (!p || !guest_ptr(g.mem, p, 256)) { set_error(c, 87); RET(0, 1); }
    refresh();
    for (unsigned i = 0; i < 256; ++i)
        M8(p + i) = (uint8_t)((g.down[i] ? 0x80 : 0) | (g.toggled[i] ? 1 : 0));
    set_error(c, 0);
    RET(1, 1);
}
static void s_GetAsyncKeyState(CPU *c) {
    uint32_t vk = A(0);
    if (vk > 255) RET(0, 1);
    refresh();
    uint32_t v = (g.down[vk] ? 0x8000u : 0) | (g.pressed[vk] ? 1u : 0);
    g.pressed[vk] = 0;
    RET((uint32_t)(int32_t)(int16_t)v, 1);
}
/* GetKeyState is a latched/keyboard-state query, unlike GetAsyncKeyState: no
 * "pressed" bit, but the low toggle bit for Caps/Num/Scroll Lock.  The 3DS has
 * no such keys, so toggles only change through injection/GetDeviceState. */
static void s_GetKeyState(CPU *c) {
    uint32_t vk = A(0) & 0xffu;
    refresh();
    uint32_t v = (g.down[vk] ? 0x8000u : 0) | (g.toggled[vk] ? 1u : 0);
    RET((uint32_t)(int32_t)(int16_t)v, 1);
}
static void s_MapVirtualKeyA(CPU *c) {
    uint32_t code = A(0), type = A(1);
    uint32_t r = 0;
    switch (type) {
    case 0: case 4: r = vk2dik[code & 0xff]; break;
    case 1: r = dik2vk[code & 0xff]; break;
    case 2: r = (uint32_t)vk_char(code & 0xff, 0, 0); break;
    case 3: r = dik2vk_ex[code & 0xff]; break;
    default: r = 0; break;
    }
    RET(r, 2);
}
static void s_ToUnicode(CPU *c) {
    uint32_t vk = A(0), scan = A(1), state = A(2), out = A(3);
    int32_t cch = (int32_t)A(4);
    if (!vk && scan) vk = dik2vk[scan & 0xff];
    if (!out || cch <= 0) RET(0, 5);
    refresh();
    int shift = 0, caps = 0;
    if (state && guest_ptr(g.mem, state, 256)) {
        shift = (M8(state + 0x10) & 0x80) || (M8(state + 0xA0) & 0x80);
        caps = (M8(state + 0x14) & 1) != 0;
    }
    int ch = vk_char(vk, shift, caps);
    if (!ch) RET(0, 5);
    M16(out) = (uint16_t)ch;
    RET(1, 5);
}
static void s_GetKeyboardLayout(CPU *c) { (void)A(0); RET(0x04090409u, 1); }
static void s_GetCursorPos(CPU *c) {
    uint32_t p = A(0);
    if (!p || !guest_ptr(g.mem, p, 8)) { set_error(c, 87); RET(0, 1); }
    refresh();
    M32(p) = (uint32_t)g.mx; M32(p + 4) = (uint32_t)g.my;
    set_error(c, 0);
    RET(1, 1);
}
static void s_SetCursorPos(CPU *c) {
    g.mx = (int32_t)A(0); g.my = (int32_t)A(1);
    set_error(c, 0);
    RET(1, 2);
}

/* ---- XInput ordinals #2/#3/#4 -------------------------------------------------- */
static void s_XInputGetState(CPU *c) {
    uint32_t user = A(0), p = A(1);
    if (user > 3) RET(ERROR_BAD_ARGUMENTS, 2);
    if (user != 0 || !g.pad_connected) RET(ERROR_DEVICE_NOT_CONNECTED, 2);
    if (!p || !guest_ptr(g.mem, p, 16)) RET(ERROR_BAD_ARGUMENTS, 2);
    M32(p) = g.packet++;
    M16(p + 4) = g.pad_buttons;
    M8(p + 6) = g.pad_lt; M8(p + 7) = g.pad_rt;
    M16(p + 8) = (uint16_t)g.pad_lx; M16(p + 10) = (uint16_t)g.pad_ly;
    M16(p + 12) = (uint16_t)g.pad_rx; M16(p + 14) = (uint16_t)g.pad_ry;
    RET(0, 2);
}
static void s_XInputSetState(CPU *c) {
    uint32_t user = A(0), p = A(1);
    if (user != 0) RET(ERROR_BAD_ARGUMENTS, 2);
    if (!g.pad_connected) RET(ERROR_DEVICE_NOT_CONNECTED, 2);
    if (!p || !guest_ptr(g.mem, p, 4)) RET(ERROR_BAD_ARGUMENTS, 2);
    if (!g.rumble_logged) {
        g.rumble_logged = 1;
        fprintf(stderr, "[input] XInputSetState: the 3DS has no rumble motor; "
                        "vibration request accepted but not actuated\n");
    }
    RET(0, 2);
}
static void s_XInputGetCapabilities(CPU *c) {
    uint32_t user = A(0), flags = A(1), p = A(2);
    if (user != 0 || !g.pad_connected) RET(ERROR_DEVICE_NOT_CONNECTED, 3);
    if (flags != 0 || !p || !guest_ptr(g.mem, p, 20)) RET(ERROR_BAD_ARGUMENTS, 3);
    memset(guest_ptr(g.mem, p, 20), 0, 20);
    M8(p) = 1;      /* XINPUT_DEVTYPE_GAMEPAD */
    M8(p + 1) = 1;  /* XINPUT_DEVSUBTYPE_GAMEPAD */
    M16(p + 2) = 0; /* no force feedback */
    M16(p + 4) = 0xffff; /* all standard buttons */
    RET(0, 3);
}

/* ---- module plumbing ----------------------------------------------------------- */
static GuestFn fn_table[FN_COUNT];
static void init_functions(void) {
    fn_table[I_DirectInput8Create] = s_DirectInput8Create;
    fn_table[I_XInputGetState] = s_XInputGetState;
    fn_table[I_XInputSetState] = s_XInputSetState;
    fn_table[I_XInputGetCapabilities] = s_XInputGetCapabilities;
    fn_table[I_GetKeyboardState] = s_GetKeyboardState;
    fn_table[I_GetAsyncKeyState] = s_GetAsyncKeyState;
    fn_table[I_GetKeyState] = s_GetKeyState;
    fn_table[I_MapVirtualKeyA] = s_MapVirtualKeyA;
    fn_table[I_ToUnicode] = s_ToUnicode;
    fn_table[I_GetKeyboardLayout] = s_GetKeyboardLayout;
    fn_table[I_GetCursorPos] = s_GetCursorPos;
    fn_table[I_SetCursorPos] = s_SetCursorPos;
    fn_table[I_unsupported] = s_unsupported;
    fn_table[I_query] = s_query;
    fn_table[I_addref] = s_addref;
    fn_table[I_release] = s_release;
    fn_table[I_di_create_device] = s_di_create_device;
    fn_table[I_di_enum_devices] = s_di_enum_devices;
    fn_table[I_di_get_device_status] = s_di_get_status;
    fn_table[I_di_run_control_panel] = s_di_run_control_panel;
    fn_table[I_di_initialize] = s_di_initialize;
    fn_table[I_dev_caps] = s_dev_caps;
    fn_table[I_dev_enum_objects] = s_dev_enum_objects;
    fn_table[I_dev_get_property] = s_dev_get_property;
    fn_table[I_dev_set_property] = s_dev_set_property;
    fn_table[I_dev_acquire] = s_dev_acquire;
    fn_table[I_dev_unacquire] = s_dev_unacquire;
    fn_table[I_dev_get_state] = s_dev_get_state;
    fn_table[I_dev_get_data] = s_dev_get_data;
    fn_table[I_dev_set_format] = s_dev_set_format;
    fn_table[I_dev_set_notification] = s_dev_set_notification;
    fn_table[I_dev_set_coop] = s_dev_set_coop;
    fn_table[I_dev_get_object_info] = s_dev_get_object_info;
    fn_table[I_dev_get_info] = s_dev_get_info;
    fn_table[I_dev_poll] = s_dev_poll;
    fn_table[I_dev_escape] = s_dev_escape;
}
static int build_vtables(void) {
    uint32_t di_a[8] = {TH(I_query),TH(I_addref),TH(I_release),TH(I_di_create_device),
        TH(I_di_enum_devices),TH(I_di_get_device_status),TH(I_di_run_control_panel),TH(I_di_initialize)};
    uint32_t di_w[8]; memcpy(di_w, di_a, sizeof di_w);
    uint32_t dev_a[32], dev_w[32];
    for (unsigned i = 0; i < 32; ++i) { dev_a[i] = TH(I_unsupported); dev_w[i] = TH(I_unsupported); }
    for (unsigned i = 0; i < 3; ++i) { dev_a[i] = di_a[i]; dev_w[i] = di_a[i]; }
    dev_a[3] = TH(I_dev_caps); dev_a[4] = TH(I_dev_enum_objects);
    dev_a[5] = TH(I_dev_get_property); dev_a[6] = TH(I_dev_set_property);
    dev_a[7] = TH(I_dev_acquire); dev_a[8] = TH(I_dev_unacquire);
    dev_a[9] = TH(I_dev_get_state); dev_a[10] = TH(I_dev_get_data);
    dev_a[11] = TH(I_dev_set_format); dev_a[12] = TH(I_dev_set_notification);
    dev_a[13] = TH(I_dev_set_coop); dev_a[14] = TH(I_dev_get_object_info);
    dev_a[15] = TH(I_dev_get_info); dev_a[16] = TH(I_di_run_control_panel);
    dev_a[17] = TH(I_di_initialize); dev_a[24] = TH(I_dev_escape); dev_a[25] = TH(I_dev_poll);
    memcpy(dev_w, dev_a, sizeof dev_w);
    for (unsigned i = 0; i < 8; ++i) {
        if (guest_write32(g.mem, VT_DI_A + 4 * i, di_a[i]) ||
            guest_write32(g.mem, VT_DI_W + 4 * i, di_w[i])) return -1;
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (guest_write32(g.mem, VT_DEV_A + 4 * i, dev_a[i]) ||
            guest_write32(g.mem, VT_DEV_W + 4 * i, dev_w[i])) return -1;
    }
    return 0;
}

int win32_input_init(GuestMemory *m) {
    if (!m || g.mem) return -1;
    memset(&g, 0, sizeof g);
    g.mem = m;
    g.profile = WIN32_INPUT_PROFILE_MENU;
    {
        const char *want = getenv("TESV_INPUT_PROFILE");
        if (want && *want) (void)win32_input_set_profile(want); /* logs named refusal */
    }
    if (guest_map(m, IN_MEM_BASE, IN_MEM_BYTES)) { g.mem = NULL; return -1; }
    if (guest_map(m, IN_CB_STACK, IN_CB_STACK_BYTES)) {
        guest_unmap(m, IN_MEM_BASE); g.mem = NULL; return -1;
    }
    build_key_tables();
    init_functions();
    if (build_vtables()) {
        guest_unmap(m, IN_CB_STACK);
        guest_unmap(m, IN_MEM_BASE);
        g.mem = NULL;
        return -1;
    }
    uint32_t teb = guest_ptr(m, GP_TEB, 0x100) ? GP_TEB : 0;
    n3ds_lift_init_cpu(&g.cb_cpu, IN_CB_STACK + IN_CB_STACK_BYTES - 64, teb);
    g.cb_ready = 1;
    g.next = 0;
    g.pad_connected = 0;
    return 0;
}
void win32_input_shutdown(void) {
    if (!g.mem) return;
    guest_unmap(g.mem, IN_CB_STACK);
    guest_unmap(g.mem, IN_MEM_BASE);
    memset(&g, 0, sizeof g);
}

static int dll_is(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
typedef struct { const char *dll, *name; unsigned fn; } NamedDef;
static const NamedDef named[] = {
    {"dinput8.dll", "DirectInput8Create", I_DirectInput8Create},
    {"xinput1_3.dll", "XInputGetState", I_XInputGetState},
    {"xinput1_3.dll", "XInputSetState", I_XInputSetState},
    {"xinput1_3.dll", "XInputGetCapabilities", I_XInputGetCapabilities},
    {"user32.dll", "GetKeyboardState", I_GetKeyboardState},
    {"user32.dll", "GetAsyncKeyState", I_GetAsyncKeyState},
    {"user32.dll", "GetKeyState", I_GetKeyState},
    {"user32.dll", "MapVirtualKeyA", I_MapVirtualKeyA},
    {"user32.dll", "ToUnicode", I_ToUnicode},
    {"user32.dll", "GetKeyboardLayout", I_GetKeyboardLayout},
    {"user32.dll", "GetCursorPos", I_GetCursorPos},
    {"user32.dll", "SetCursorPos", I_SetCursorPos},
};
const Win32InputDef *win32_input_defs(unsigned *count) {
    static Win32InputDef defs[sizeof named / sizeof named[0]];
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; ++i) {
        defs[i].dll = named[i].dll; defs[i].name = named[i].name; defs[i].fn = fn_table[named[i].fn];
    }
    if (count) *count = (unsigned)(sizeof named / sizeof named[0]);
    return defs;
}
uint32_t win32_input_resolve(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; ++i)
        if (dll_is(dll, named[i].dll) && !strcmp(name, named[i].name))
            return TH(named[i].fn);
    return 0;
}
uint32_t win32_input_resolve_ordinal(const char *dll, unsigned ordinal) {
    if (!dll || !dll_is(dll, "xinput1_3.dll")) return 0;
    switch (ordinal) {
    case 2: return TH(I_XInputGetState);
    case 3: return TH(I_XInputSetState);
    case 4: return TH(I_XInputGetCapabilities);
    default: return 0;
    }
}
static FnEntry entries[FN_COUNT];
const FnEntry *win32_input_lookup(uint32_t addr) {
    if (addr < WIN32_INPUT_BASE || (addr & 3)) return NULL;
    unsigned i = (addr - WIN32_INPUT_BASE) / 4;
    if (i >= FN_COUNT || !fn_table[i]) return NULL;
    entries[i] = (FnEntry){addr, fn_table[i]};
    return &entries[i];
}
