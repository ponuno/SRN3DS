/* Retail Havok world diagnostic through the predecoded IR container.
 *
 * Usage: havok_world_smoke TESV.IRC [MATCHING_TESV.EXE]
 *
 * The container carries .text only.  The matching PE is therefore required for
 * read-only .text/.rdata/.data (code jump tables, constants, RTTI vtables and
 * jump tables); all writable Havok globals, TLS, the memory router and the
 * allocator below are synthetic.  No game process, renderer or Win32 shim is
 * started.  The current container has no solver/job-queue harness, so the step
 * diagnostic requires the translation to remain unchanged.  The retail MMX
 * AABB helper `0xde8500` runs from the container; no scalar override is used.
 */
#include "n3ds_lift_runtime.h"
#include "guest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HK_WORLD_CINFO_CTOR 0x00d38a70u
#define HK_WORLD_CTOR       0x00d54ca0u
#define HK_SPHERE_CTOR      0x00d5f540u
#define HK_BODY_CTOR        0x00d309d0u
#define HK_WORLD_ADD        0x00d51a60u
#define HK_WORLD_STEP_INTERNAL 0x00d52fe0u
#define HK_BODY_GET_XFORM   0x00d2fdc0u
#define HK_DEFAULT_CINFO    0x132b468u

#define STACK_TOP  0x60018000u
#define TEB_ADDR   0x60020000u
#define STUB_BASE  0x02000000u
#define TLS_GET     (STUB_BASE + 0x00u)
#define TLS_SET     (STUB_BASE + 0x10u)
#define LOCK_ENTER  (STUB_BASE + 0x20u)
#define LOCK_LEAVE  (STUB_BASE + 0x30u)
#define LOCK_DELETE (STUB_BASE + 0x40u)
#define ALLOC       (STUB_BASE + 0x50u)
#define FREE        (STUB_BASE + 0x60u)
#define RET_ZERO    (STUB_BASE + 0x70u)
#define CTX_METHOD  (STUB_BASE + 0x80u)
#define REALLOC     (STUB_BASE + 0x90u)
#define ALLOC_PTR   (STUB_BASE + 0xa0u)

#define SYNTH       0x02001000u
#define ROUTER      (SYNTH + 0x000u)
#define ALLOCATOR   (SYNTH + 0x100u)
#define ALLOC_VT    (SYNTH + 0x200u)
#define CTX_INNER   (SYNTH + 0x300u)
#define CTX_OUTER   (SYNTH + 0x400u)
#define CTX_VT      (SYNTH + 0x500u)
#define LOCK_STATE  (SYNTH + 0x600u)
#define ERROR_OBJ   (SYNTH + 0x700u)
#define ERROR_VT    (SYNTH + 0x800u)

#define WORLD       0x02002000u
#define SPHERE      (WORLD + 0x1100u)
#define BODY_CINFO  (WORLD + 0x1200u)
#define BODY        (WORLD + 0x1400u)
#define BODY_LIST   (WORLD + 0x1700u)
#define STEP_FLAGS  (WORLD + 0x1800u)
#define HEAP_BASE   0x61000000u
#define HEAP_SIZE   0x01000000u

#define G_TLS_MAIN   0x1bb6b08u
#define G_TLS_TRACE  0x1bb8674u
#define G_TLS_OTHER  0x1bbeb54u
#define G_LOCK_PTR   0x1bb0ef0u
#define G_LOCK_TLS   0x1bb0ef4u
#define G_BP_SIMPLE  0x1bba480u
#define G_BP_TREE    0x1bba484u
#define G_BP_HYBRID  0x1bba488u
#define G_BP_QUERY1  0x1bbbc94u
#define G_BP_QUERY2  0x1bbbc98u
#define G_TIME_SCALE 0x1bb5e24u
#define G_TRACE_ON   0x1bbdda0u
#define G_ERROR      0x1bb8360u
#define G_PATCH_ON   0x12d19a0u
#define G_USE_HYBRID  0x1bb835cu

static GuestMemory mem;
static CPU cpu;
static uint32_t heap_next = HEAP_BASE;
static unsigned alloc_calls;
static const char *setup_stage = "PE data";
static uint32_t guest_alloc(CPU *c, uint32_t n);

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint32_t a, uint32_t v) {
    if (guest_write32(&mem, a, v)) {
        fprintf(stderr, "havok_world_smoke: guest_write32(%08x) failed\n", a);
        exit(2);
    }
}
static void wrf(uint32_t a, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    wr32(a, bits);
}
static uint32_t arg32(const CPU *c, unsigned n) {
    uint32_t v = 0;
    if (guest_read32(&mem, c->r[4] + 4u + 4u * n, &v)) return 0;
    return v;
}
static float argf(const CPU *c, unsigned n) {
    uint32_t bits = arg32(c, n);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static void stub_tls_get(CPU *c) {
    uint32_t index = arg32(c, 0);
    c->r[0] = index == 0 ? ROUTER : 0;
    c->r[4] += 8;
}
static void stub_tls_set(CPU *c) { (void)arg32(c, 0); (void)arg32(c, 1); c->r[0] = 1; c->r[4] += 12; }
static void stub_lock_enter(CPU *c) { (void)arg32(c, 0); c->r[0] = 1; c->r[4] += 8; }
static void stub_lock_leave(CPU *c) { (void)arg32(c, 0); c->r[0] = 1; c->r[4] += 8; }
static void stub_lock_delete(CPU *c) { (void)arg32(c, 0); c->r[4] += 8; }
static void stub_ret_zero(CPU *c) { (void)c; c->r[0] = 0; c->r[4] += 4; }
static void stub_ret_zero_3(CPU *c) { (void)c; c->r[0] = 0; c->r[4] += 16; }
static void stub_ctx_method(CPU *c) { (void)c; c->r[0] = 0; c->r[4] += 16; }
static void stub_return_this_1(CPU *c) { c->r[0] = c->r[1]; c->r[4] += 8; }
static void stub_temp_alloc(CPU *c) {
    c->r[0] = guest_alloc(c, arg32(c, 0));
    c->r[4] += 8;
}
static void stub_temp_free(CPU *c) {
    (void)arg32(c, 0); (void)arg32(c, 1);
    c->r[4] += 12;
}
static void stub_compact_array(CPU *c) {
    uint32_t obj = c->r[1], arr = 0, count = 0;
    if (guest_read32(&mem, obj + 0x98, &arr) || guest_read32(&mem, obj + 0x9c, &count))
        rt_unimpl(c, obj, "compact array object");
    for (uint32_t i = count; i-- > 0;) {
        uint32_t v = 0;
        if (guest_read32(&mem, arr + 4u * i, &v)) rt_unimpl(c, arr + 4u * i, "compact array");
        if (v) continue;
        --count;
        for (uint32_t j = i; j < count; ++j) {
            if (guest_read32(&mem, arr + 4u * (j + 1), &v) ||
                guest_write32(&mem, arr + 4u * j, v))
                rt_unimpl(c, arr, "compact array shift");
        }
    }
    if (guest_write32(&mem, obj + 0x9c, count)) rt_unimpl(c, obj, "compact array count");
    c->r[4] += 4;
}
static void stub_null_indirect(CPU *c) {
    uint32_t caller = 0;
    (void)guest_read32(&mem, c->r[4], &caller);
    fprintf(stderr, "havok_world_smoke: null indirect call from %08x ecx=%08x\n",
            caller, c->r[1]);
    rt_unimpl(c, caller, "null indirect call");
}
static void stub_unexpected(CPU *c) {
    uint32_t caller = 0;
    (void)guest_read32(&mem, c->r[4], &caller);
    fprintf(stderr, "havok_world_smoke: data pointer %08x called as code from %08x ecx=%08x\n",
            SYNTH, caller, c->r[1]);
    rt_unimpl(c, caller, "data pointer called as code");
}

static uint32_t guest_alloc(CPU *c, uint32_t n) {
    n = (n + 15u) & ~15u;
    if (!n || n > HEAP_SIZE || heap_next + n > HEAP_BASE + HEAP_SIZE) {
        rt_unimpl(c, HEAP_BASE + HEAP_SIZE, "synthetic Havok allocator exhausted");
    }
    uint32_t p = heap_next;
    heap_next += n;
    memset(guest_ptr(&mem, p, n), 0, n);
    ++alloc_calls;
    return p;
}
static void stub_alloc(CPU *c) {
    c->r[0] = guest_alloc(c, arg32(c, 0));
    c->r[4] += 8; /* __thiscall(size_t): pop return address and one argument. */
}
static void stub_alloc_ptr(CPU *c) {
    uint32_t size = 0;
    uint32_t p = arg32(c, 0);
    (void)guest_read32(&mem, p, &size);
    c->r[0] = guest_alloc(c, size);
    wr32(p, size);
    c->r[4] += 8;
}
static void stub_realloc(CPU *c) {
    uint32_t oldp = arg32(c, 0), oldn = arg32(c, 1), sizep = arg32(c, 2);
    uint32_t newn = 0;
    (void)guest_read32(&mem, sizep, &newn);
    uint32_t p = guest_alloc(c, newn);
    if (oldp && oldn && guest_ptr(&mem, oldp, oldn))
        memcpy(guest_ptr(&mem, p, newn), guest_ptr(&mem, oldp, oldn),
               oldn < newn ? oldn : newn);
    wr32(sizep, newn);
    c->r[0] = p;
    c->r[4] += 4;
}
static void stub_free(CPU *c) { (void)arg32(c, 0); (void)arg32(c, 1); c->r[4] += 12; }

static int map_pe_data(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long length = ftell(f);
    if (length < 0x1000 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)length);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)length, f) != (size_t)length) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    if (buf[0] != 'M' || buf[1] != 'Z') { free(buf); return -1; }
    uint32_t pe = rd32(buf + 0x3c);
    if (pe + 24 + 96 > (uint32_t)length || memcmp(buf + pe, "PE\0\0", 4)) {
        free(buf); return -1;
    }
    uint16_t nsections = rd16(buf + pe + 6);
    uint16_t opt_size = rd16(buf + pe + 20);
    uint32_t image_base = rd32(buf + pe + 24 + 28);
    uint32_t sh = pe + 24 + opt_size;
    if (sh + 40u * nsections > (uint32_t)length) { free(buf); return -1; }
    int mapped = 0;
    for (uint16_t i = 0; i < nsections; ++i) {
        const uint8_t *s = buf + sh + 40u * i;
        if (memcmp(s, ".text", 5) && memcmp(s, ".rdata", 6) && memcmp(s, ".data", 5)) continue;
        uint32_t vs = rd32(s + 8), va = rd32(s + 12);
        uint32_t raw = rd32(s + 16), rp = rd32(s + 20);
        uint32_t span = (vs + 0xfffu) & ~0xfffu;
        if (raw > span) raw = span;
        if (image_base + va + span > HEAP_BASE || guest_map(&mem, image_base + va, span)) {
            free(buf); return -1;
        }
        if (raw && guest_write(&mem, image_base + va, buf + rp, raw)) {
            free(buf); return -1;
        }
        ++mapped;
    }
    free(buf);
    return mapped == 3 ? 0 : -1;
}

static int setup_guest(const char *pe) {
    guest_init(&mem, 64u << 20);
    if (map_pe_data(pe)) {
        setup_stage = "PE data";
        return -1;
    }
    setup_stage = "stack";
    if (guest_map(&mem, STACK_TOP - 0x10000u, 0x11000u)) return -1;
    setup_stage = "TEB";
    if (guest_map(&mem, TEB_ADDR, 0x1000u)) return -1;
    setup_stage = "synthetic objects";
    if (guest_map(&mem, SYNTH, 0x1000u)) return -1;
    setup_stage = "world objects";
    if (guest_map(&mem, WORLD, 0x2000u)) return -1;
    setup_stage = "guest heap";
    if (guest_map(&mem, HEAP_BASE, HEAP_SIZE)) return -1;

    wr32(ROUTER + 0x00, ALLOC_VT);
    wr32(ROUTER + 0x28, ALLOCATOR);
    wr32(ROUTER + 0x2c, ALLOCATOR);
    wr32(ALLOCATOR + 0x00, ALLOC_VT);
    wr32(ALLOC_VT + 0x04, ALLOC);
    wr32(ALLOC_VT + 0x08, FREE);
    wr32(ALLOC_VT + 0x0c, ALLOC_PTR);
    wr32(ALLOC_VT + 0x10, FREE);
    wr32(ALLOC_VT + 0x14, REALLOC);
    wr32(CTX_INNER + 0x00, CTX_VT);
    wr32(CTX_OUTER + 0x00, CTX_INNER);
    wr32(CTX_VT + 0x1c, CTX_METHOD);
    wr32(ERROR_OBJ + 0x00, ERROR_VT);
    wr32(ERROR_VT + 0x0c, RET_ZERO);

    /* The import slots are guest memory, so point them at host AOT stubs. */
    wr32(0x106b130u, TLS_GET);
    wr32(0x106b110u, TLS_SET);
    wr32(0x106b070u, LOCK_ENTER);
    wr32(0x106b074u, LOCK_LEAVE);
    wr32(0x106b06cu, LOCK_DELETE);
    wr32(0x106b1c4u, RET_ZERO); /* GetModuleHandleW: no loaded guest modules */

    wr32(G_TLS_MAIN, 0);
    wr32(G_TLS_TRACE, 0);
    wr32(G_TLS_OTHER, 0);
    wr32(G_LOCK_PTR, LOCK_STATE);
    wr32(G_LOCK_TLS, 0);
    wr32(G_BP_SIMPLE, 0x00dc2c00u);
    wr32(G_BP_TREE, 0x00e91230u);
    wr32(G_BP_HYBRID, 0x00e8ec80u);
    wr32(G_BP_QUERY1, RET_ZERO);
    wr32(G_BP_QUERY2, RET_ZERO);
    wr32(G_TIME_SCALE, 0x3f800000u);
    wr32(G_TRACE_ON, 0);
    wr32(G_ERROR, ERROR_OBJ);
    wr32(G_PATCH_ON, 0);
    wr32(G_USE_HYBRID, 0);
    return 0;
}

static int call_this(uint32_t address, uint32_t self, const uint32_t *args,
                     size_t argc, uint32_t *result, const char *label) {
    n3ds_lift_init_cpu(&cpu, STACK_TOP, TEB_ADDR);
    cpu.r[1] = self;
    int rc = n3ds_lift_invoke(&cpu, address, args, argc, result);
    printf("%-18s rc=%d result=%08x ip=%08x last=%08x allocs=%u\n",
           label, rc, result ? *result : 0, n3ds_lift_current_ip(),
           n3ds_lift_last_call(), alloc_calls);
    if (rc) printf("  fault: %s\n", n3ds_lift_fault());
    return rc;
}

static float read_float(uint32_t a) {
    uint32_t bits = 0;
    float v = 0;
    guest_read32(&mem, a, &bits);
    memcpy(&v, &bits, sizeof v);
    return v;
}

static void put_i16(uint8_t *p, unsigned index, int16_t v) {
    p[2 * index] = (uint8_t)v;
    p[2 * index + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* Execute the retail MMX helper directly before the world objects reuse this
 * scratch page.  PCMPGTW produces -1 masks, so PADDW/PSUBW adds one per true
 * comparison; the second qword's final PAND limits adjustment to lanes 0..1. */
static int test_mmx_aabb(void) {
    enum { ENTRIES = WORLD + 0x1c00u, BOUNDS = WORLD + 0x1e00u };
    static const int16_t input[16] = {
        8, 9, 10, 18, 19, 20, 100, -1,
        8, 9, 10, 18, 19, 20, 100, -1,
    };
    static const int16_t expect[16] = {
        8, 9, 11, 19, 20, 22, 100, -1,
        8, 9, 11, 19, 20, 22, 100, -1,
    };
    uint8_t bytes[32];
    for (unsigned k = 0; k < 16; ++k) put_i16(bytes, k, input[k]);
    if (guest_write(&mem, ENTRIES, bytes, sizeof bytes)) return 1;
    for (unsigned k = 0; k < 6; ++k) put_i16(bytes, k, (int16_t)(k < 2 ? 10 : 20));
    if (guest_write(&mem, BOUNDS, bytes, 12)) return 1;
    uint32_t args[3] = {ENTRIES, 2, BOUNDS}, result = 0;
    if (call_this(0x00de8500u, 0, args, 3, &result, "mmx aabb clip")) return 1;
    if (guest_read(&mem, ENTRIES, bytes, sizeof bytes)) return 1;
    for (unsigned k = 0; k < 16; ++k) {
        int16_t got = (int16_t)(bytes[2 * k] | (bytes[2 * k + 1] << 8));
        if (got != expect[k]) {
            printf("mmx aabb clip: lane %u = %d, expected %d\n", k, got, expect[k]);
            return 1;
        }
    }
    printf("mmx aabb clip: retail 0xde8500 PASS (2 entries / 16 lanes)\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s TESV.IRC [MATCHING_TESV.EXE]\n", argv[0]);
        return 2;
    }
    const char *pe = argc == 3 ? argv[2] : "E:\\skyr\\TESV.exe";
    static const FnEntry table[] = {
        {0, stub_null_indirect},
        {0x00d1fbe0u, stub_temp_alloc},
        {0x00d1fe10u, stub_temp_free},
        {0x00d90510u, stub_return_this_1}, /* Havok version diagnostic only */
        {0x00d90560u, stub_return_this_1}, /* Havok version diagnostic only */
        {0x00dc97a0u, stub_compact_array}, /* unindexed internal vtable entry */
        {0x00de0340u, stub_ret_zero_3}, /* no collision pairs in this one-body smoke */
        {0x00f588afu, stub_ret_zero}, /* avoid the CRT's 60-second module wait */
        {0x00f5d79fu, stub_ret_zero}, /* no CRT thread-locale initialization */
        {TLS_GET, stub_tls_get}, {TLS_SET, stub_tls_set},
        {LOCK_ENTER, stub_lock_enter}, {LOCK_LEAVE, stub_lock_leave},
        {LOCK_DELETE, stub_lock_delete}, {ALLOC, stub_alloc},
        {FREE, stub_free}, {RET_ZERO, stub_ret_zero},
        {CTX_METHOD, stub_ctx_method}, {REALLOC, stub_realloc},
        {ALLOC_PTR, stub_alloc_ptr}, {SYNTH, stub_unexpected},
    };
    if (setup_guest(pe)) {
        fprintf(stderr, "havok_world_smoke: guest mapping failed at %s (used=%zu regions=%zu)\n",
                setup_stage, mem.used, mem.count);
        return 2;
    }
    if (n3ds_lift_set_container_path(argv[1]) || n3ds_lift_bind(&mem, table, 21)) {
        fprintf(stderr, "havok_world_smoke: runtime bind failed: %s\n", n3ds_lift_fault());
        return 2;
    }
    const uint32_t required[] = {
        HK_WORLD_CINFO_CTOR, HK_WORLD_CTOR, HK_SPHERE_CTOR, HK_BODY_CTOR,
        HK_WORLD_ADD, HK_WORLD_STEP_INTERNAL, HK_BODY_GET_XFORM,
        0x00de8500u, /* retail MMX AABB clipping helper */
    };
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; ++i) {
        if (!n3ds_lift_has_function(required[i])) {
            fprintf(stderr, "havok_world_smoke: container lacks %08x\n", required[i]);
            return 2;
        }
    }
    if (test_mmx_aabb()) return 1;

    uint32_t result = 0;
    if (call_this(HK_WORLD_CINFO_CTOR, HK_DEFAULT_CINFO, NULL, 0, &result,
                  "world cinfo ctor")) return 1;
    /* Keep the default solver settings but request a conventional 60 Hz step.
     * The retail static Cinfo has no optional world object/list at +0x54. */
    wr32(HK_DEFAULT_CINFO + 0x54, 0);
    wr32(HK_DEFAULT_CINFO + 0xb0, 0x40000000u); /* angular damping = 2 */
    wr32(HK_DEFAULT_CINFO + 0xb4, 0x3c23d70au); /* 0.0166666665 */
    wr32(HK_DEFAULT_CINFO + 0xbc, 4);
    int world_rc = call_this(HK_WORLD_CTOR, WORLD,
                             (uint32_t[]){HK_DEFAULT_CINFO, HK_DEFAULT_CINFO}, 2, &result,
                             "world ctor");
    if (world_rc) return 1;
    if (result != WORLD) {
        fprintf(stderr, "havok_world_smoke: world ctor returned %08x\n", result);
        return 1;
    }

    if (call_this(HK_SPHERE_CTOR, SPHERE, (uint32_t[]){0x3f800000u}, 1, &result,
                  "sphere ctor")) return 1;
    if (result != SPHERE) return 1;

    memset(guest_ptr(&mem, BODY_CINFO, 0x100), 0, 0x100);
    wr32(BODY_CINFO + 0x00, 0);             /* collision filter info */
    wr32(BODY_CINFO + 0x04, SPHERE);        /* shape */
    wr32(BODY_CINFO + 0x10, 0x00000000u); /* position x */
    wr32(BODY_CINFO + 0x14, 0x41200000u); /* position y = 10 */
    wr32(BODY_CINFO + 0x18, 0x00000000u); /* position z */
    wr32(BODY_CINFO + 0x1c, 0x3f800000u); /* position w */
    wr32(BODY_CINFO + 0x20, 0x00000000u); /* rotation x */
    wr32(BODY_CINFO + 0x24, 0x00000000u); /* rotation y */
    wr32(BODY_CINFO + 0x28, 0x00000000u); /* rotation z */
    wr32(BODY_CINFO + 0x2c, 0x3f800000u); /* rotation w */
    wr32(BODY_CINFO + 0x90, 0x00000000u); /* linear velocity x */
    wr32(BODY_CINFO + 0x94, 0x00000000u); /* linear velocity y */
    wr32(BODY_CINFO + 0x98, 0x00000000u); /* linear velocity z */
    wr32(BODY_CINFO + 0xb0, 0x00000000u); /* angular damping */
    wr32(BODY_CINFO + 0xb4, 0x00000000u); /* linear damping */
    wr32(BODY_CINFO + 0xb8, 0x3f800000u); /* mass */
    wr32(BODY_CINFO + 0xbc, 2);            /* hkpMotionType::DYNAMIC */
    wr32(BODY_CINFO + 0xbf, 0xffu);        /* default collision response */
    wr32(BODY_CINFO + 0xc0, 1);             /* create broadphase handle */
    if (call_this(HK_BODY_CTOR, BODY, (uint32_t[]){BODY_CINFO}, 1, &result,
                  "rigid body ctor")) return 1;
    if (result != BODY) return 1;

    wr32(BODY_LIST, BODY);
    if (call_this(HK_WORLD_ADD, WORLD, (uint32_t[]){BODY_LIST, 1, 0}, 3, &result,
                  "world add body")) return 1;

    if (call_this(HK_BODY_GET_XFORM, BODY, NULL, 0, &result, "get transform")) return 1;
    float before_y = read_float(result + 0x30 + 4);
    printf("before position: (0, %g, 0)\n", before_y);

    wrf(STEP_FLAGS, 1.0f / 60.0f);
    wr32(STEP_FLAGS + 4, 0);
    wr32(STEP_FLAGS + 8, 0);
    wr32(STEP_FLAGS + 12, 0);
    if (call_this(HK_WORLD_STEP_INTERNAL, WORLD,
                  (uint32_t[]){CTX_OUTER, STEP_FLAGS, 0}, 3, &result,
                  "world step internal")) return 1;
    if (call_this(HK_BODY_GET_XFORM, BODY, NULL, 0, &result, "get transform")) return 1;
    float after_y = read_float(result + 0x30 + 4);
    printf("after position:  (0, %g, 0)\n", after_y);

    int fail = before_y != 10.0f || after_y != before_y;
    if (fail) fprintf(stderr, "unexpected translation change in no-solver diagnostic\n");
    n3ds_lift_release();
    guest_destroy(&mem);
    if (!fail) puts("Havok world diagnostic PASS (world/body/add/step; no solver/job queue)");
    return fail ? 1 : 0;
}
