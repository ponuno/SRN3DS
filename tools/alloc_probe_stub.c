/* Allocator/startup probe stub: deliberately leaves D3D9 unavailable so the
 * AOT path can be measured while the graphics/IR owners edit d3d9_*. The guest
 * has not reached the renderer at this stage; d9_com_init() returning nonzero
 * makes tesv_slice_main.c skip the COM module exactly as the real code does. */
#include "d3d9_com.h"
#include "d3d9_null.h"

int d9_com_init(GuestMemory *memory, D9Device *device) {
    (void)memory; (void)device;
    return -1;
}
int d9_com_init_backend(GuestMemory *memory, const D9Backend *ops, void *ctx,
                        size_t limit) {
    (void)memory; (void)ops; (void)ctx; (void)limit;
    return -1;
}
int d9_com_set_backend(const D9Backend *ops, void *ctx, size_t limit) {
    (void)ops; (void)ctx; (void)limit;
    return 0;
}
void d9_com_shutdown(void) {}
int d9_com_pixel_shader_tev(uint32_t address, D9TevProgram *out) {
    (void)address; (void)out;
    return 0;
}
const char *d9_com_last_shader_rejection(void) {
    return "D3D9 disabled for allocator/startup probe";
}
uint32_t d9_com_resolve(const PeImport *entry, void *ctx) {
    (void)entry; (void)ctx;
    return 0;
}
FnModule d9_com_module(void) {
    FnModule m = {"alloc_probe_none", NULL, 0};
    return m;
}
int d9_null_register(void) { return -1; }
