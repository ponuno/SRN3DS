/* Host test for win32_input: DirectInput8 object/vtable plumbing, keyboard and
 * mouse device state, EnumDevices callbacks, and the xinput1_3 ordinals. */
#include "win32_shims.h"
#include "win32_input.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA 0x21000000u
#define STACK 0x22000000u

static int call(CPU *c, const char *dll, const char *name, const uint32_t *args, size_t n, uint32_t *out) {
    PeImport e = {.dll = dll, .name = name};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
#define K(name, args, n) call(cpu, "kernel32.dll", #name, args, n, &r)
#define U(name, args, n) call(cpu, "user32.dll", #name, args, n, &r)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "input test failed at line %d\n", __LINE__); goto done; } } while (0)

static void dummy(CPU *c) { c->r[4] += 4; }
static void enum_callback(CPU *c) {
    uint32_t instance = M32(c->r[4] + 4);
    (void)instance;
    M32(DATA + 0x300) = M32(DATA + 0x300) + 1;
    c->r[0] = 1; /* DIENUM_CONTINUE */
    c->r[4] += 4 + 8;
}
static const FnEntry table[] = {
    {0x00500000u, dummy}, {0x00501000u, enum_callback},
};
static const uint8_t IID_DI8A[16] = {0x30,0x80,0x79,0xBF,0x3A,0x48,0xA2,0x4D,0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00};
static const uint8_t GUID_KEYBOARD[16] = {0x61,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
static const uint8_t GUID_MOUSE[16] = {0x60,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};

/* Call a guest vtable thunk with a real invocation frame. */
static int method(CPU *c, uint32_t vtable, unsigned slot, uint32_t *args, size_t n, uint32_t *out) {
    uint32_t fn = M32(vtable + 4 * slot);
    return n3ds_lift_invoke(c, fn, args, n, out);
}
static int ordinal_call(CPU *c, const char *dll, unsigned ord, const uint32_t *args, size_t n, uint32_t *out) {
    PeImport e = {.dll = dll, .ordinal = (uint16_t)ord};
    uint32_t fn = win32_resolve(&e, NULL);
    return fn ? n3ds_lift_invoke(c, fn, args, n, out) : -1;
}
static int open_device(CPU *c, uint32_t di, const uint8_t *guid, uint32_t *dev, uint32_t *r) {
    uint32_t a[4];
    memcpy(n3ds_guest_ptr(DATA + 0x140, 16), guid, 16);
    a[0] = di; a[1] = DATA + 0x140; a[2] = DATA + 0x160; a[3] = 0;
    if (method(c, M32(di), 3, a, 4, r)) return -1;
    if (*r) return -1;
    *dev = M32(DATA + 0x160);
    return 0;
}

int main(void) {
    int rc = -1, mapped = 0;
    GuestMemory m;
    guest_init(&m, 16u << 20);
    if (guest_map(&m, DATA, 4096) || guest_map(&m, STACK, 4096)) goto done;
    mapped = 1;
    if (n3ds_lift_bind(&m, table, sizeof table / sizeof table[0])) goto done;
    if (win32_init(&m, 0x100000, ".")) goto done;
    CPU *cpu = calloc(1, sizeof *cpu);
    if (!cpu) goto done;
    n3ds_lift_init_cpu(cpu, STACK + 4096 - 64, 0);
    uint32_t r, a[8];

    /* DirectInput8Create with IID_IDirectInput8A. */
    memcpy(n3ds_guest_ptr(DATA + 0x100, 16), IID_DI8A, 16);
    a[0] = 0; a[1] = 0x0800; a[2] = DATA + 0x100; a[3] = DATA + 0x120; a[4] = 0;
    CHECK(!call(cpu, "dinput8.dll", "DirectInput8Create", a, 5, &r) && r == 0);
    uint32_t di = M32(DATA + 0x120);
    CHECK(di != 0 && M32(di) != 0);
    uint32_t divt = M32(di);

    /* Keyboard device, format, cooperative level, acquire, state. */
    uint32_t kbd;
    CHECK(!open_device(cpu, di, GUID_KEYBOARD, &kbd, &r));
    uint32_t kvt = M32(kbd);
    CHECK(kvt != 0 && kvt != divt);
    memset(n3ds_guest_ptr(DATA + 0x200, 24), 0, 24);
    M32(DATA + 0x200) = 24; M32(DATA + 0x20c) = 256; /* DIDATAFORMAT.dwDataSize */
    a[0] = kbd; a[1] = DATA + 0x200; CHECK(!method(cpu, kvt, 11, a, 2, &r) && r == 0);
    a[0] = kbd; a[1] = 0; a[2] = 5; CHECK(!method(cpu, kvt, 13, a, 3, &r) && r == 0);
    a[0] = kbd; CHECK(!method(cpu, kvt, 7, a, 1, &r) && r == 0);
    a[0] = kbd; a[1] = DATA + 0x240; M32(DATA + 0x240) = 44;
    CHECK(!method(cpu, kvt, 3, a, 2, &r) && r == 0);
    CHECK((M32(DATA + 0x240 + 8) & 0xff) == 0x13);
    /* GetDeviceInfo. */
    M32(DATA + 0x280) = 580;
    a[0] = kbd; a[1] = DATA + 0x280; CHECK(!method(cpu, kvt, 15, a, 2, &r) && r == 0);
    CHECK(!strcmp(guest_cstr(&m, DATA + 0x280 + 40, 32), "Keyboard"));
    /* A device method that is deliberately unsupported. */
    a[0] = kbd; a[1] = 0; a[2] = 0; a[3] = 0;
    CHECK(!method(cpu, kvt, 4, a, 4, &r) && r == 0x80004001u);

    /* Inject W and read it back through DirectInput and user32. */
    win32_input_set_key('W', 1);
    a[0] = kbd; a[1] = 256; a[2] = DATA + 0x380;
    CHECK(!method(cpu, kvt, 9, a, 3, &r) && r == 0);
    CHECK(M8(DATA + 0x380 + 0x11) == 0x80);
    a[0] = DATA + 0x480; CHECK(!U(GetKeyboardState, a, 1) && r == 1);
    CHECK(M8(DATA + 0x480 + 0x57) == 0x80);
    a[0] = 'W'; a[1] = 0; CHECK(!U(MapVirtualKeyA, a, 2) && r == 0x11);
    a[0] = 'W'; a[1] = 0x11; a[2] = 0; a[3] = DATA + 0x4a0; a[4] = 4;
    CHECK(!U(ToUnicode, a, 5) && r == 1 && M16(DATA + 0x4a0) == 'w');
    a[0] = 'W'; CHECK(!U(GetAsyncKeyState, a, 1) && (r & 0x8000));
    a[0] = 0; CHECK(!U(GetKeyboardLayout, a, 1) && r == 0x04090409u);

    /* Control profiles: the portable 3DS pad mask must produce the exact guest
     * codes the menu needs, and the legacy mapping must stay unchanged. */
    {
        uint8_t vk[256], mb[8];
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU, WIN32_INPUT_PAD_A, vk, mb);
        CHECK(vk[0x0D] && !vk[0x01] && !mb[0]);          /* Enter, not click */
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU, WIN32_INPUT_PAD_UP, vk, mb);
        CHECK(vk[0x26] && vk['W'] && !vk[0x28]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU,
                            WIN32_INPUT_PAD_B | WIN32_INPUT_PAD_START, vk, mb);
        CHECK(vk[0x1B] && !mb[1]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU, WIN32_INPUT_PAD_SELECT, vk, mb);
        CHECK(vk[0x09]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU,
                            WIN32_INPUT_PAD_X | WIN32_INPUT_PAD_Y, vk, mb);
        CHECK(vk[0x20] && vk['E'] && !vk[0x01]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_MENU, 0, vk, mb);
        CHECK(!vk[0x26] && !vk[0x0D] && !mb[0]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_LEGACY, WIN32_INPUT_PAD_A, vk, mb);
        CHECK(vk[0x01] && mb[0] && !vk[0x0D]);
        win32_input_map_pad(WIN32_INPUT_PROFILE_LEGACY, WIN32_INPUT_PAD_UP, vk, mb);
        CHECK(vk['W'] && !vk[0x26]);
        /* The gameplay default used to be legacy; the menu profile is the port
         * default and is switchable by name. */
        CHECK(win32_input_profile() == WIN32_INPUT_PROFILE_MENU);
        CHECK(!win32_input_set_profile("legacy") &&
              win32_input_profile() == WIN32_INPUT_PROFILE_LEGACY);
        CHECK(win32_input_set_profile("bogus") == -1 &&
              win32_input_profile() == WIN32_INPUT_PROFILE_LEGACY);
        CHECK(!win32_input_set_profile("menu"));
    }
    /* End-to-end: pad A in the menu profile is Enter through every input API. */
    win32_input_apply_pad(WIN32_INPUT_PAD_A);
    a[0] = kbd; a[1] = 256; a[2] = DATA + 0x380;
    CHECK(!method(cpu, kvt, 9, a, 3, &r) && r == 0);
    CHECK(M8(DATA + 0x380 + 0x1C) == 0x80);              /* DIK_RETURN */
    a[0] = 0x0D; CHECK(!U(GetAsyncKeyState, a, 1) && (r & 0x8000));
    a[0] = 0x0D; CHECK(!U(GetKeyState, a, 1) && (r & 0x8000));
    win32_input_apply_pad(0);
    a[0] = 0x0D; CHECK(!U(GetAsyncKeyState, a, 1) && !(r & 0x8000));
    a[0] = kbd; a[1] = 256; a[2] = DATA + 0x380;
    CHECK(!method(cpu, kvt, 9, a, 3, &r) && r == 0);
    CHECK(M8(DATA + 0x380 + 0x1C) == 0);

    /* Mouse device with a 16-byte DIMOUSESTATE. */
    uint32_t mouse;
    CHECK(!open_device(cpu, di, GUID_MOUSE, &mouse, &r));
    uint32_t mvt = M32(mouse);
    M32(DATA + 0x200) = 24; M32(DATA + 0x20c) = 16;
    a[0] = mouse; a[1] = DATA + 0x200; CHECK(!method(cpu, mvt, 11, a, 2, &r) && r == 0);
    a[0] = mouse; CHECK(!method(cpu, mvt, 7, a, 1, &r) && r == 0);
    win32_input_set_mouse(10, 20, 5, -3, 1);
    a[0] = mouse; a[1] = 16; a[2] = DATA + 0x500;
    CHECK(!method(cpu, mvt, 9, a, 3, &r) && r == 0);
    CHECK(M32(DATA + 0x500) == 5 && M32(DATA + 0x504) == (uint32_t)-3);
    CHECK(M8(DATA + 0x500 + 12) == 0x80);
    a[0] = DATA + 0x520; CHECK(!U(GetCursorPos, a, 1) && r == 1);
    CHECK(M32(DATA + 0x520) == 10 && M32(DATA + 0x524) == 20);

    /* EnumDevices invokes the guest callback once per attached device. */
    M32(DATA + 0x300) = 0;
    a[0] = di; a[1] = 0; a[2] = 0x00501000u; a[3] = 0; a[4] = 0;
    CHECK(!method(cpu, divt, 4, a, 5, &r) && r == 0 && M32(DATA + 0x300) == 2);

    /* Release everything: the vtable cells are cleared. */
    a[0] = kbd; CHECK(!method(cpu, kvt, 2, a, 1, &r) && r == 0 && M32(kbd) == 0);
    a[0] = mouse; CHECK(!method(cpu, mvt, 2, a, 1, &r) && r == 0);
    a[0] = di; CHECK(!method(cpu, divt, 2, a, 1, &r) && r == 0);

    /* XInput ordinals #2/#3/#4. */
    win32_input_set_controller(1, 0x1000, 255, 0, -1234, 2345, 0, 0);
    a[0] = 0; a[1] = DATA + 0x600;
    CHECK(!ordinal_call(cpu, "xinput1_3.dll", 2, a, 2, &r) && r == 0);
    CHECK(M16(DATA + 0x600 + 4) == 0x1000 && M8(DATA + 0x600 + 6) == 255);
    CHECK((int16_t)M16(DATA + 0x600 + 8) == -1234 && (int16_t)M16(DATA + 0x600 + 10) == 2345);
    a[0] = 0; a[1] = 0; a[2] = DATA + 0x640;
    CHECK(!ordinal_call(cpu, "xinput1_3.dll", 4, a, 3, &r) && r == 0);
    CHECK(M8(DATA + 0x640) == 1 && M8(DATA + 0x641) == 1);
    a[0] = 0; a[1] = DATA + 0x660;
    CHECK(!ordinal_call(cpu, "xinput1_3.dll", 3, a, 2, &r) && r == 0);
    win32_input_set_controller(0, 0, 0, 0, 0, 0, 0, 0);
    a[0] = 0; a[1] = DATA + 0x600;
    CHECK(!ordinal_call(cpu, "xinput1_3.dll", 2, a, 2, &r) && r == 1167);

    /* No duplicate window: input thunks stay inside their own range. */
    CHECK(win32_input_resolve("dinput8.dll", "DirectInput8Create") >= WIN32_INPUT_BASE);
    CHECK(win32_input_resolve_ordinal("xinput1_3.dll", 2) != 0);
    CHECK(win32_input_resolve_ordinal("xinput1_3.dll", 1) == 0);

    rc = 0;
done:
    if (cpu) free(cpu);
    win32_shutdown();
    if (mapped) guest_destroy(&m);
    return rc;
}
