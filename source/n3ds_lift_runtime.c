#include "n3ds_lift_runtime.h"
#include "win32_shims.h"
#include "ir_core.h"
/* Optional measurement hook; absent from ordinary host/ARM test binaries. */
extern void *tesv_sm3_probe_level_ptr(uint32_t address, size_t bytes)
    __attribute__((weak));
extern void *tesv_cube_probe_ptr(uint32_t address, size_t bytes)
    __attribute__((weak));
extern int tesv_cube_probe_dispatch(CPU *c, uint32_t addr)
    __attribute__((weak));
extern int tesv_cecdc0_source_active(void) __attribute__((weak));
extern int tesv_cecdc0_source_decide(GuestMemory *m, uint32_t frame,
                                      uint32_t index, uint32_t ebx,
                                      uint32_t *new_base, uint32_t *new_desc)
    __attribute__((weak));
/* Entry hook that some measurement-generated slices call from the BSA reader.
 * A weak no-op keeps such a table linkable without changing guest semantics;
 * a strong definition in the measurement build overrides it. */
__attribute__((weak)) void rt_bsa_trace(CPU *c, uint32_t addr) {
    (void)c; (void)addr;
}
/* Measurement-only memory samples; the slice overrides both.  Recording the
 * guest high-water mark must never alter dispatch or fault behaviour. */
__attribute__((weak)) void n3ds_mem_guest_sample(size_t used, unsigned regions) {
    (void)used; (void)regions;
}
__attribute__((weak)) void n3ds_mem_fault_sample(void) {}
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#ifdef __3DS__
#include <3ds.h>
#endif

static GuestMemory *memory;
static const FnModule *modules;
static unsigned module_count;
static int ever_bound;
#if defined(__ELF__) || defined(__3DS__) || defined(_WIN32)
/* The hybrid build links a generated table of interpreter function ranges. It
 * is optional: a pure-AOT build simply leaves both symbols undefined. */
extern const IrFnEntry g_ir_table[] __attribute__((weak));
extern const unsigned g_ir_count __attribute__((weak));
#endif
/* Invocation frames form a stack: a guest thread body or a guest callback runs
 * inside the caller's frame, and a fault unwinds only to the innermost one. */
typedef struct InvokeFrame { jmp_buf jb; struct InvokeFrame *prev; } InvokeFrame;
static InvokeFrame *active;
static uint32_t exit_code;
static char fault[256];
/* Diagnostic only: last guest function the dispatcher entered. */
static uint32_t g_last_call;
static CPU *g_last_cpu;
/* Which tier serves the hot path: AOT, slice IR, or the indexed container.
 * A dispatch counter per tier makes "the hot loop is native AOT code, so the IR
 * block yield never runs" visible in a hang dump. */
unsigned long long n3ds_lift_tier_aot;
unsigned long long n3ds_lift_tier_slice;
unsigned long long n3ds_lift_tier_container;
void n3ds_lift_dump_tiers(FILE *f) {
    if (!f) return;
    fprintf(f, "[hang] dispatch tiers: aot=%llu slice_ir=%llu container=%llu\n",
            n3ds_lift_tier_aot, n3ds_lift_tier_slice, n3ds_lift_tier_container);
}
/* Diagnostic only (TESV_HANG_WATCHDOG_MS): current IR block address and a
 * short ring of the most recent guest entries. AOT keeps its registers in
 * locals, so the entry ring is the only cheap PC evidence for a lifted spin. */
uint32_t g_ir_ip;
static uint32_t g_stream_obj;
void n3ds_lift_dump_stream(FILE *f) {
    if (!f || !g_stream_obj || !memory) return;
    uint32_t o = g_stream_obj, v[12] = {0};
    for (unsigned i = 0; i < 12; ++i) (void)guest_read32(memory, o + 4u * i, &v[i]);
    fprintf(f, "[stream] dump obj=%08x vt=%08x pos=%08x..%08x base=%08x.%08x buf=%08x size=%08x flags=%08x\n",
            (unsigned)o, (unsigned)v[0], (unsigned)v[2], (unsigned)v[4],
            (unsigned)v[6], (unsigned)v[7], (unsigned)v[8], (unsigned)v[9],
            (unsigned)v[10]);
    if (v[0]) {
        uint32_t m18 = 0, m20 = 0;
        (void)guest_read32(memory, v[0] + 0x18, &m18);
        (void)guest_read32(memory, v[0] + 0x20, &m20);
        fprintf(f, "[stream] dump vt methods +18=%08x +20=%08x\n", (unsigned)m18,
                (unsigned)m20);
    }
}
/* Diagnostic only: TESV_STREAM_TRACE=1 dumps the buffered-reader object at
 * 0x00ac6c10 (the menu reader retry-loop callee) and its vtable. */
static void stream_trace(CPU *c) {
    static unsigned n;
    static unsigned long long total;
    if (!c) return;
    /* Counters advance on every entry; only the detail print is limited. */
    ++total;
    g_stream_obj = c->r[1];
    if (n >= 12) return;
    ++n;
    uint32_t obj = c->r[1], ret = 0;
    if (memory) (void)guest_read32(memory, c->r[4], &ret);
    fprintf(stderr, "[stream] entry#%llu obj=%08x ret=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x esp=%08x\n",
            total, (unsigned)obj, (unsigned)ret, (unsigned)c->r[0], (unsigned)c->r[3],
            (unsigned)c->r[1], (unsigned)c->r[2], (unsigned)c->r[6],
            (unsigned)c->r[7], (unsigned)c->r[4]);
    if (!obj || !memory) return;
    uint32_t f[11] = {0};
    for (unsigned i = 0; i < 11; ++i) (void)guest_read32(memory, obj + 4u * i, &f[i]);
    fprintf(stderr, "[stream] +00=%08x +04=%08x +08=%08x +0c=%08x +10=%08x +14=%08x +18=%08x +1c=%08x +20=%08x +24=%08x +28=%08x\n",
            (unsigned)f[0], (unsigned)f[1], (unsigned)f[2], (unsigned)f[3],
            (unsigned)f[4], (unsigned)f[5], (unsigned)f[6], (unsigned)f[7],
            (unsigned)f[8], (unsigned)f[9], (unsigned)f[10]);
    if (f[0]) {
        uint32_t m18 = 0, m1c = 0, m20 = 0, m24 = 0;
        (void)guest_read32(memory, f[0] + 0x18, &m18);
        (void)guest_read32(memory, f[0] + 0x1c, &m1c);
        (void)guest_read32(memory, f[0] + 0x20, &m20);
        (void)guest_read32(memory, f[0] + 0x24, &m24);
        fprintf(stderr, "[stream] vtable=%08x +18=%08x +1c=%08x +20=%08x +24=%08x\n",
                (unsigned)f[0], (unsigned)m18, (unsigned)m1c, (unsigned)m20,
                (unsigned)m24);
    }
}
/* ---- TESV_MENU_TRACE: retail StartMenu state machine --------------------
 * Diagnostic only. Logs the WinMain->0x876f60->0x876fc0->0x876b90->
 * 0xa60c80->0xa60c10 open chain, the 0x8779d0 message switch (with the
 * message word and first fields), 0x877a50 (GFx FadeInMenu), 0x8765f0
 * (fadeOutStarted) and [0x1b3fc98]/[0x1b3fc94]. Merged with the file-opener
 * URL log so a missing StartMenu.swf and a guest message/fade stall can be
 * told apart. Never changes dispatch. */
static int menu_trace_on(void){
    static int v=-1;
    if(v<0){const char *e=getenv("TESV_MENU_TRACE");v=e&&*e&&strcmp(e,"0");}
    return v;
}
static uint32_t menu_rd32(uint32_t a){uint32_t v=0;if(memory)(void)guest_read32(memory,a,&v);return v;}
static void menu_trace(CPU *c,uint32_t addr){
    if(!menu_trace_on()||!c)return;
    static const uint32_t va[]={
        0x0069de08u,0x006982c0u,0x006f3e20u,0x006f3ea0u,0x006ebc10u,
        0x006f3f00u,0x006f3f80u,0x0069cbd0u,0x004352d0u,0x00696810u,
        0x00699b20u,0x00691410u,0x00898430u,0x00892da0u,0x006f44a0u,0x006f44d0u,
        0x00a5d370u,0x0068c8e0u,0x006e3e30u,0x00649820u,0x0078a0a0u,0x006dc680u,
        0x00690f90u,0x008e1950u,0x00431b00u,0x00a4b470u,0x00a4b500u,0x00a4b5c0u,
        0x00876f60u,0x00876fc0u,0x00876b90u,0x00a60c80u,0x00a60c10u,
        0x00a5eaa0u,0x00a5ed20u,0x008779d0u,0x00877a50u,0x008765f0u,0x00876bf0u,
        0x00876a50u,0x00a64ba0u,0x00a64940u,0x00401710u,0x006f3f9du,0x006f3fd8u,
        0x006f3fe8u,0x0069df7eu,0x0069dfe7u,0x0069e015u};
    unsigned i,n=sizeof va/sizeof va[0];
    for(i=0;i<n&&va[i]!=addr;++i);
    if(i==n)return;
    static unsigned seen[sizeof va/sizeof va[0]];
    unsigned cap=(addr==0x008779d0u)?24u:(addr==0x00401710u?400u:(addr==0x0069df7eu?64u:8u));
    if(seen[i]>=cap)return;
    ++seen[i];
    uint32_t from=menu_rd32(c->r[4]);
    uint32_t sing=menu_rd32(0x1b3fc98u),flag94=menu_rd32(0x1b3fc94u);
    uint32_t a0=menu_rd32(c->r[4]+4),a1=menu_rd32(c->r[4]+8);
    uint32_t mode20=menu_rd32(0x1b39120u),mode24=menu_rd32(0x1b39124u);
    uint32_t prog=menu_rd32(0x1b39128u),inload=menu_rd32(0x12e3561u);
    uint32_t lstate=menu_rd32(0x12e3562u),phase=menu_rd32(0x1ba75f8u);
    fprintf(stderr,"[menu] enter %08x from=%08x mode=%u/%u prog=%u inload=%u lstate=%u phase=%u singleton=%08x flag94=%u a0=%08x a1=%08x ecx=%08x\n",
            (unsigned)addr,(unsigned)from,(unsigned)mode20,(unsigned)mode24,(unsigned)prog,
            (unsigned)(inload&0xffu),(unsigned)(lstate&0xffu),(unsigned)phase,
            (unsigned)sing,(unsigned)(flag94&0xffu),
            (unsigned)a0,(unsigned)a1,(unsigned)c->r[1]);
    if(addr==0x00a5ed20u){
        char name[256];name[0]=0;
        if(a0&&memory){
            for(unsigned k=0;k<sizeof name-1;k++){
                const char *p=guest_ptr(memory,a0+k,1);
                if(!p)break;
                name[k]=*p;
                if(!*p)break;
                name[k+1]=0;
            }
            name[sizeof name-1]=0;
        }
        fprintf(stderr,"[menu] opener url='%s'\n",name);
    }
    if(addr==0x008779d0u){
        fprintf(stderr,"[menu] msg=%08x type=%u d4=%08x d8=%08x dc=%08x\n",
                (unsigned)a0,(unsigned)(a0?menu_rd32(a0)&0xffu:0xffffffffu),
                (unsigned)(a0?menu_rd32(a0+4):0),(unsigned)(a0?menu_rd32(a0+8):0),
                (unsigned)(a0?menu_rd32(a0+0xc):0));
    }
    if(addr==0x00877a50u){
        uint32_t obj=sing?menu_rd32(sing+8):0;
        fprintf(stderr,"[menu] FadeInMenu menu=%08x menu+8=%08x [[menu+8]+10]=%08x\n",
                (unsigned)sing,(unsigned)obj,(unsigned)(obj?menu_rd32(obj+0x10):0));
    }
    if(addr==0x00401710u){
        uint32_t lock=c->r[1]; /* __thiscall: ecx = lock */
        uint32_t owner=menu_rd32(lock),depth=menu_rd32(lock+4);
        fprintf(stderr,"[menu] lock=%08x owner=%08x depth=%08x thread=%u\n",
                (unsigned)lock,(unsigned)owner,(unsigned)depth,
                (unsigned)(n3ds_ir_thread_id_hook?n3ds_ir_thread_id_hook():0));
    }
    if(addr==0x0069df7eu){
        uint32_t app=menu_rd32(0x1b2e860u);
        fprintf(stderr,"[menu] winmain sleep gate a=%08x b=%02x c=%02x "
                "app+4=%02x app+5=%02x app+a=%02x inload=%u lstate=%u phase=%u\n",
                (unsigned)menu_rd32(0x1b2e8ecu),
                (unsigned)(menu_rd32(0x1272eb8u)&0xffu),
                (unsigned)(c?menu_rd32(c->r[4]+0x13u)&0xffu:0),
                (unsigned)(app?menu_rd32(app+4)&0xffu:0xff),
                (unsigned)(app?menu_rd32(app+5)&0xffu:0xff),
                (unsigned)(app?menu_rd32(app+0xau)&0xffu:0xff),
                (unsigned)(menu_rd32(0x12e3561u)&0xffu),
                (unsigned)(menu_rd32(0x12e3562u)&0xffu),
                (unsigned)menu_rd32(0x1ba75f8u));
    }
}
#define RT_CALL_RING 12u
static struct { uint32_t addr, from; } g_call_ring[RT_CALL_RING];
static unsigned g_call_ring_n;

/* ---- TESV_A43F_TRACE: buffered-window translate 0xa43f80 ----------------
 * Leaf 64-bit interval test. With A={+0xc:+8} and B={+0x14:+0x10} it accepts
 * want in [A,B) and writes
 *   *out0 = [+0x24] + (want_lo - A_lo)   (file offset of the window hit)
 *   *out1 = B_lo - want_lo               (bytes left in the window)
 * otherwise it writes *out1=0 and returns false. Blocks: entry 80,
 * mid 93/95/99/fa1/fa7/fab/faf/fb1, success fb5, reject fd3.
 * AOT keeps its registers in c->r[] and runs every block inside one C call, so
 * that tier is counted at dispatch and classified after the call from the
 * caller's out1 slot; IR/IRC are counted per block. Every hook returns after a
 * cached flag test unless TESV_A43F_TRACE is set, so default behaviour and
 * guest memory are untouched. */
#define A43F_ENTRY 0x00a43f80u
#define A43F_OK    0x00a43fb5u
#define A43F_FAIL  0x00a43fd3u
#define A43F_READ  0x00ac6c10u   /* BufferedStream::Read, the retry callee */
#define A43F_RAW   0x00ac6380u   /* its raw read */
#define A43F_NBLK  11

typedef struct {
    int state;                    /* 0 untested, 1 off, 2 on */
    unsigned long long vis[2], ok[2], fail[2];      /* [0]=AOT, [1]=IR/IRC */
    unsigned long long blk[2][A43F_NBLK];
    unsigned long long rep[2], rep_max[2];
    uint32_t last_obj[2], last_lo[2], last_hi[2];
    unsigned long long reads, bytes, raws;
    unsigned tick;                /* dispatches since the last clock() probe */
    double t0, last_t;
    unsigned long long tv[2], tok[2], tfail[2], treads, tbytes;
} A43fTrace;
static A43fTrace g_a43f;
/* Cached after the first a43f_on(): -1 unknown, 1 off, 2 on. The interpreter
 * reads this directly so a disabled trace costs one load per block instead of
 * two cross-TU calls that immediately return. */
int g_a43f_trace_on = -1;

static int a43f_on(void) {
    if (!g_a43f.state) {
        const char *v = getenv("TESV_A43F_TRACE");
        g_a43f.state = (v && v[0] && strcmp(v, "0")) ? 2 : 1;
        g_a43f_trace_on = g_a43f.state;
        if (g_a43f.state == 2) {
            g_a43f.t0 = g_a43f.last_t = (double)clock() / (double)CLOCKS_PER_SEC;
            fprintf(stderr, "[a43f] trace on entry=%08x ok=%08x fail=%08x "
                    "reader=%08x raw=%08x\n", A43F_ENTRY, A43F_OK, A43F_FAIL,
                    A43F_READ, A43F_RAW);
        }
    }
    return g_a43f.state == 2;
}
static const char *a43f_tier(int t) { return t ? "irc" : "aot"; }
static int a43f_blk_index(uint32_t ip) {
    static const uint32_t starts[A43F_NBLK] = {
        0x00a43f80u, 0x00a43f93u, 0x00a43f95u, 0x00a43f99u,
        0x00a43fa1u, 0x00a43fa7u, 0x00a43fabu, 0x00a43fafu,
        0x00a43fb1u, 0x00a43fb5u, 0x00a43fd3u };
    for (int i = 0; i < A43F_NBLK; ++i) if (starts[i] == ip) return i;
    return -1;
}
/* Exact branch decision of the leaf from the fields it already loaded:
 * reject iff A > B, want < A, or want >= B. */
static int a43f_reject(uint32_t lo_a, uint32_t hi_a, uint32_t lo_b,
                       uint32_t hi_b, uint32_t wlo, uint32_t whi) {
    if (hi_a > hi_b || (hi_a == hi_b && lo_a >= lo_b)) return 1; /* ja / jae */
    if (hi_a > whi || (hi_a == whi && lo_a > wlo)) return 2;     /* ja */
    if (whi > hi_b || (whi == hi_b && wlo >= lo_b)) return 3;    /* jae */
    return 0;
}
/* Sampled visit: TEB, guest registers, the five object fields and four stack
 * arguments with their effective addresses, then the Jcc outcome. */
static void a43f_dump(CPU *c, int t, unsigned long long n, const char *tag) {
    uint32_t ret = 0, o0p = 0, o1p = 0, wlo = 0, whi = 0;
    uint32_t f[10] = {0};
    uint32_t obj = c->r[1], esp = c->r[4];
    (void)guest_read32(memory, esp, &ret);
    (void)guest_read32(memory, esp + 4u, &o0p);
    (void)guest_read32(memory, esp + 8u, &o1p);
    (void)guest_read32(memory, esp + 0xcu, &wlo);
    (void)guest_read32(memory, esp + 0x10u, &whi);
    for (unsigned i = 0; i < 10; ++i)
        if (guest_read32(memory, obj + 4u * i, &f[i])) f[i] = 0xdeadbeefu;
    int rej = a43f_reject(f[2], f[3], f[4], f[5], wlo, whi);
    uint32_t hit_off = f[9] + (wlo - f[2]);
    uint32_t hit_len = f[4] - wlo;
    fprintf(stderr,
            "[a43f] #%llu %s tier=%s teb=%08x ret=%08x ecx=%08x esp=%08x "
            "eax=%08x ebx=%08x edx=%08x esi=%08x edi=%08x\n",
            n, tag, a43f_tier(t), (unsigned)c->fs_base, ret, obj, esp,
            (unsigned)c->r[0], (unsigned)c->r[3], (unsigned)c->r[2],
            (unsigned)c->r[6], (unsigned)c->r[7]);
    fprintf(stderr,
            "[a43f]   [ecx+08]=%08x:%08x [ecx+0c]=%08x:%08x [ecx+10]=%08x:%08x "
            "[ecx+14]=%08x:%08x [ecx+24]=%08x:%08x\n",
            obj + 8u, f[2], obj + 0xcu, f[3], obj + 0x10u, f[4], obj + 0x14u,
            f[5], obj + 0x24u, f[9]);
    fprintf(stderr,
            "[a43f]   [esp+0c]=%08x:%08x [esp+10]=%08x:%08x A=%08x.%08x "
            "B=%08x.%08x want=%08x.%08x out0=%08x out1=%08x edge=%s\n",
            esp + 0xcu, wlo, esp + 0x10u, whi, f[3], f[2], f[5], f[4], whi, wlo,
            o0p, o1p, rej ? "REJECT" : "ACCEPT");
    if (!rej)
        fprintf(stderr, "[a43f]   predicted *out0=%08x *out1=%08x "
                "(base %08x + want_lo %08x - A_lo %08x)\n",
                hit_off, hit_len, f[9], wlo, f[2]);
    else
        fprintf(stderr, "[a43f]   predicted *out1=0 (reject check %d)\n", rej);
}
static void a43f_periodic(int force) {
    double now = (double)clock() / (double)CLOCKS_PER_SEC;
    if (!force && now - g_a43f.last_t < 1.0) return;
    g_a43f.last_t = now;
    for (int t = 0; t < 2; ++t) {
        fprintf(stderr,
                "[a43f] t=%.3f tier=%s vis=%llu ok=%llu fail=%llu rep=%llu/%llu "
                "| window dvis=%llu dok=%llu dfail=%llu\n",
                now - g_a43f.t0, a43f_tier(t), g_a43f.vis[t], g_a43f.ok[t],
                g_a43f.fail[t], g_a43f.rep[t], g_a43f.rep_max[t],
                g_a43f.vis[t] - g_a43f.tv[t], g_a43f.ok[t] - g_a43f.tok[t],
                g_a43f.fail[t] - g_a43f.tfail[t]);
        g_a43f.tv[t] = g_a43f.vis[t];
        g_a43f.tok[t] = g_a43f.ok[t];
        g_a43f.tfail[t] = g_a43f.fail[t];
    }
    fprintf(stderr, "[a43f] t=%.3f reader reads=%llu bytes=%llu raw=%llu "
            "| window dreads=%llu dbytes=%llu\n", now - g_a43f.t0, g_a43f.reads,
            g_a43f.bytes, g_a43f.raws, g_a43f.reads - g_a43f.treads,
            g_a43f.bytes - g_a43f.tbytes);
    g_a43f.treads = g_a43f.reads;
    g_a43f.tbytes = g_a43f.bytes;
    if (g_stream_obj && memory) {
        uint32_t o = g_stream_obj, p[11] = {0};
        for (unsigned i = 0; i < 11; ++i) (void)guest_read32(memory, o + 4u * i, &p[i]);
        fprintf(stderr, "[a43f]   stream obj=%08x win=%08x.%08x..%08x.%08x "
                "pos=%08x.%08x base=%08x.%08x buf=%08x flags=%08x\n",
                o, p[3], p[2], p[5], p[4], p[7], p[6], p[9], p[8], p[4], p[10]);
    }
}
void n3ds_a43f_trace_periodic(CPU *c) { (void)c; if (a43f_on()) a43f_periodic(0); }
void n3ds_a43f_trace_prime(void) { (void)a43f_on(); }
/* Reader-side counters, driven from the dispatcher for every tier. */
void n3ds_a43f_trace_dispatch(CPU *c, uint32_t addr) {
    if (!a43f_on() || !c || !memory) return;
    if (addr == A43F_READ) {
        uint32_t len = 0;
        ++g_a43f.reads;
        g_stream_obj = c->r[1];
        if (!guest_read32(memory, c->r[4] + 8u, &len)) g_a43f.bytes += len;
    } else if (addr == A43F_RAW) {
        ++g_a43f.raws;
    }
    /* clock() is not free: probe it once per 4096 dispatches. */
    if ((++g_a43f.tick & 0xfffu) == 0) a43f_periodic(0);
}
/* AOT tier: the body keeps esp on the guest stack, so after its `ret 0x10`
 * the caller's out1 slot is the dword at esp+4. */
void n3ds_a43f_trace_aot_pre(CPU *c) {
    uint32_t wlo = 0, whi = 0;
    ++g_a43f.vis[0];
    (void)guest_read32(memory, c->r[4] + 0xcu, &wlo);
    (void)guest_read32(memory, c->r[4] + 0x10u, &whi);
    if (c->r[1] == g_a43f.last_obj[0] && wlo == g_a43f.last_lo[0] &&
        whi == g_a43f.last_hi[0]) {
        if (++g_a43f.rep[0] > g_a43f.rep_max[0]) g_a43f.rep_max[0] = g_a43f.rep[0];
    } else {
        g_a43f.rep[0] = 0;
        g_a43f.last_obj[0] = c->r[1];
        g_a43f.last_lo[0] = wlo;
        g_a43f.last_hi[0] = whi;
    }
    unsigned long long n = g_a43f.vis[0];
    if (n <= 4 || (n & (n - 1)) == 0) a43f_dump(c, 0, n, "aot");
}
void n3ds_a43f_trace_aot_post(CPU *c) {
    uint32_t out1 = 0;
    (void)guest_read32(memory, c->r[4] + 4u, &out1);
    if (out1) ++g_a43f.ok[0]; else ++g_a43f.fail[0];
}
/* IR/IRC tier, before the block runs: true entry state (registers, TEB, args)
 * for the entry block, plus the identical-(this,want) run detector. */
void n3ds_a43f_trace_pre(CPU *c, uint32_t block_ip, uint32_t func_addr,
                         int is_irc) {
    if (!a43f_on() || !c || block_ip != A43F_ENTRY || func_addr != A43F_ENTRY)
        return;
    int t = is_irc ? 1 : 0;
    uint32_t wlo = 0, whi = 0;
    ++g_a43f.vis[t];
    if (memory) {
        (void)guest_read32(memory, c->r[4] + 0xcu, &wlo);
        (void)guest_read32(memory, c->r[4] + 0x10u, &whi);
    }
    if (c->r[1] == g_a43f.last_obj[t] && wlo == g_a43f.last_lo[t] &&
        whi == g_a43f.last_hi[t]) {
        if (++g_a43f.rep[t] > g_a43f.rep_max[t]) g_a43f.rep_max[t] = g_a43f.rep[t];
    } else {
        g_a43f.rep[t] = 0;
        g_a43f.last_obj[t] = c->r[1];
        g_a43f.last_lo[t] = wlo;
        g_a43f.last_hi[t] = whi;
    }
    unsigned long long n = g_a43f.vis[t];
    if (n <= 4 || (n & (n - 1)) == 0) a43f_dump(c, t, n, "entry");
}
/* IR/IRC tier, after the block runs: block and accept/reject edge counters. */
void n3ds_a43f_trace_block(CPU *c, uint32_t block_ip, uint32_t func_addr,
                           uint32_t next, int is_irc) {
    if (!a43f_on()) return;
    if (func_addr != A43F_ENTRY) return;   /* periodic runs from the dispatcher */
    int t = is_irc ? 1 : 0;
    int bi = a43f_blk_index(block_ip);
    if (bi >= 0) ++g_a43f.blk[t][bi];
    if (block_ip == A43F_OK) ++g_a43f.ok[t];
    else if (block_ip == A43F_FAIL) ++g_a43f.fail[t];
    (void)c; (void)next;
}

void n3ds_lift_set_ir_ip(uint32_t ip){g_ir_ip=ip;}
uint32_t n3ds_lift_current_ip(void){return g_ir_ip;}
uint32_t n3ds_lift_last_call(void){return g_last_call;}void n3ds_lift_dump_recent_calls(FILE *f){
    if(!f)return;
    unsigned n=g_call_ring_n<RT_CALL_RING?g_call_ring_n:RT_CALL_RING;
    fprintf(f,"[hang] recent guest entries (oldest first):");
    for(unsigned k=0;k<n;++k){
        unsigned slot=(g_call_ring_n - n + k)%RT_CALL_RING;
        fprintf(f," %08x<-%08x",(unsigned)g_call_ring[slot].addr,
                (unsigned)g_call_ring[slot].from);
    }
    fprintf(f,"\n");
}
#if defined(_WIN32)
__declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long frames_to_skip, unsigned long frames_to_capture,
    void **frames, unsigned long *hash);
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *name);
#endif
static uint32_t thread_irc_base;   /* container depth at the running thread's root */
static N3dsThreadRtState *thread_rt; /* state block of the running thread */
static const FnEntry *lookup(uint32_t addr);
void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));

/* ---- indexed all-functions IR container (source/ir_container.c) ---- */
/* The container is optional and, unless N3DS_IRC_REQUIRED=1, opened lazily on
 * the first address that is neither AOT nor slice IR. That keeps an AOT-only
 * run from touching SD at all. */
#ifndef N3DS_IRC_INTEGRATION_RECORDS
#define N3DS_IRC_INTEGRATION_RECORDS 256u
#endif
static N3dsIrc *ir_container;
static int ir_container_state;   /* 0 untried, 1 open, -1 unavailable */
static char ir_container_path[256];
static char ir_container_reason[256];
static int ir_container_env_read;
static int ir_container_path_set;
static int ir_container_required;
static int ir_container_enabled = 1;
static size_t ir_container_cache_bytes;
static unsigned ir_container_cache_records;

static void container_read_env(void);
static int container_ensure(CPU *c, int can_fault);
static int container_dispatch(CPU *c, uint32_t addr);
static int container_aot_pred(void *user, uint32_t addr);
static void trace_indirect_fault(CPU *c, uint32_t target);

/* Guest code lives only in the main image (0x00400000..0x02000000) or an
 * imported DLL image (0x08000000..0x0C000000).  A call/return target outside
 * both is a host/app pointer or corrupted guest data; handing it to the IR
 * container would decode the 3dsx's own bytes as x86 and report a bogus
 * opcode instead of naming the bad indirect target. */
static int guest_code_target(uint32_t a) {
    return (a >= 0x00400000u && a < 0x02000000u) ||
           (a >= 0x08000000u && a < 0x0C000000u);
}

static int env_flag(const char *name, int dflt) {
    const char *s = getenv(name);
    if (!s || !*s) return dflt;
    if (s[0] == '0' && !s[1]) return 0;
    if (s[0] == 'n' || s[0] == 'N' || s[0] == 'f' || s[0] == 'F') return 0;
    return 1;
}

static unsigned long env_ulong(const char *name, unsigned long dflt) {
    const char *s = getenv(name);
    if (!s || !*s) return dflt;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || (end && *end)) return dflt;
    return v;
}

static void container_read_env(void) {
    if (ir_container_env_read) return;
    ir_container_env_read = 1;
    ir_container_required = env_flag("N3DS_IRC_REQUIRED", 0);
    ir_container_enabled = env_flag("N3DS_IRC_ENABLE", 1);
    ir_container_cache_bytes = (size_t)env_ulong("N3DS_IRC_CACHE_BYTES",
                                                 N3DS_IRC_DEFAULT_CACHE_BYTES);
    ir_container_cache_records = (unsigned)env_ulong("N3DS_IRC_CACHE_RECORDS",
                                                     N3DS_IRC_INTEGRATION_RECORDS);
    if (!ir_container_path_set) {
        const char *path = getenv("N3DS_IRC_PATH");
        if (!path || !*path) {
#ifdef __3DS__
            path = "sdmc:/Skyrim/ir/tesv.irc";
#else
            path = "tesv.irc";
#endif
        }
        snprintf(ir_container_path, sizeof ir_container_path, "%s", path);
    }
}

int n3ds_lift_set_container_path(const char *path) {
    if (!path || !*path || strlen(path) >= sizeof ir_container_path) return -1;
    if (ir_container_state == 1) return -1;
    snprintf(ir_container_path, sizeof ir_container_path, "%s", path);
    ir_container_path_set = 1;
    ir_container_state = 0;
    ir_container_env_read = 0;
    return 0;
}

int n3ds_lift_container_state(void) { return ir_container_state; }
const char *n3ds_lift_container_error(void) { return ir_container_reason; }

int n3ds_lift_container_stats(N3dsIrcStats *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!ir_container) return -1;
    n3ds_irc_stats(ir_container, out);
    return 0;
}

uint64_t n3ds_lift_container_index_bytes(void) {
    return ir_container ? n3ds_irc_index_size(ir_container) : 0;
}

static int container_aot_pred(void *user, uint32_t addr) {
    (void)user;
    /* TESV_A43F_TRACE: run the traced leaf through the container's exact
     * predecoded blocks instead of the AOT body so every block and edge is
     * countable. Measurement only; the two tiers are semantically identical. */
    if (addr == 0x00a43f80u && a43f_on()) return 0;
    return lookup(addr) != NULL;
}

static int container_ensure(CPU *c, int can_fault) {
    container_read_env();
    if (ir_container_state == 1) return 1;
    if (ir_container_state == -1) {
        if (ir_container_required) {
            if (fault[0] == 0)
                snprintf(fault, sizeof fault, "Guest fault at 00000000: %.200s",
                         ir_container_reason);
            if (can_fault) rt_unimpl(c, 0, ir_container_reason);
        }
        return 0;
    }
    if (!ir_container_enabled) {
        ir_container_state = -1;
        snprintf(ir_container_reason, sizeof ir_container_reason,
                 "IR container disabled by N3DS_IRC_ENABLE");
        return 0;
    }

    int st = N3DS_IRC_ERR_OPEN;
    N3dsIrc *ic = n3ds_irc_open_ex(ir_container_path, &st);
    if (!ic) {
        ir_container_state = -1;
        snprintf(ir_container_reason, sizeof ir_container_reason,
                 "IR container unavailable: %.180s: %s",
                 ir_container_path, n3ds_irc_strstatus(st));
        fprintf(stderr, "%s%s\n", ir_container_reason,
                ir_container_required ? " (required)" : " (using AOT + slice IR)");
        if (ir_container_required) {
            if (fault[0] == 0)
                snprintf(fault, sizeof fault, "Guest fault at 00000000: %.200s",
                         ir_container_reason);
            if (can_fault) rt_unimpl(c, 0, ir_container_reason);
        }
        return 0;
    }
    if (n3ds_irc_cache_init(ic, ir_container_cache_bytes,
                            ir_container_cache_records) != N3DS_IRC_OK) {
        n3ds_irc_close(ic);
        ir_container_state = -1;
        snprintf(ir_container_reason, sizeof ir_container_reason,
                 "IR container cache init failed: %.180s", ir_container_path);
        fprintf(stderr, "%s\n", ir_container_reason);
        if (ir_container_required) {
            if (fault[0] == 0)
                snprintf(fault, sizeof fault, "Guest fault at 00000000: %.200s",
                         ir_container_reason);
            if (can_fault) rt_unimpl(c, 0, ir_container_reason);
        }
        return 0;
    }
    n3ds_irc_set_aot_predicate(ic, container_aot_pred, NULL);
    ir_container = ic;
    ir_container_state = 1;
    fprintf(stderr, "IR container: %u functions (%u decoded) from %s, "
                    "cache %lu B / %u records\n",
            n3ds_irc_function_count(ic), n3ds_irc_decoded_count(ic),
            ir_container_path,
            (unsigned long)ir_container_cache_bytes,
            ir_container_cache_records);
    return 1;
}

/* AOT wins, then a predecoded container record, then the existing slice IR.
 * An indexed-but-invalid or unreadable container entry faults explicitly; it
 * is never handed to the decoder to hide a container problem. */
static int container_dispatch(CPU *c, uint32_t addr) {
    if (!guest_code_target(addr)) {
        trace_indirect_fault(c, addr);
        rt_unimpl(c, addr, "call target outside any guest code module");
        return -1;
    }
    if (!container_ensure(c, 1)) return 1;
    int rc = n3ds_irc_exec_status(ir_container, c, addr, 1);
    if (rc == N3DS_IRC_TIER_CONTAINER) return 0;
    if (rc == N3DS_IRC_TIER_AOT) {
        const FnEntry *e = lookup(addr);
        if (e) { e->fn(c); return 0; }
        return 1;
    }
    if (rc == N3DS_IRC_ERR_NOT_FOUND) return 1;
    if (rc == N3DS_IRC_ERR_UNDECODED) {
        N3dsIrcEntry ent;
        const char *why = "not decoded";
        if (n3ds_irc_lookup(ir_container, addr, &ent) == N3DS_IRC_OK)
            why = n3ds_irc_decode_status_name(ent.decode_status);
        char what[192];
        snprintf(what, sizeof what,
                 "IR container: function %08lx is not decoded (%s)",
                 (unsigned long)addr, why);
        rt_unimpl(c, addr, what);
    }
    {
        char what[192];
        snprintf(what, sizeof what, "IR container: %s (%.120s)",
                 n3ds_irc_strstatus(rc), ir_container_path);
        rt_unimpl(c, addr, what);
    }
}

static int container_has_preentry(uint32_t addr) {
    if (!container_ensure(NULL, 0)) return 0;
    return n3ds_irc_has_preentry(ir_container, addr);
}

int n3ds_lift_bind_modules(GuestMemory *m,const FnModule *list,unsigned n) {
    if(!m || (!list && n) || ever_bound)return -1;
    for(unsigned k=0;k<n;++k){
        const FnEntry *table=list[k].table;unsigned count=list[k].count;
        if(!count || !table)return -1;
        for(unsigned i=0;i<count;++i)if(!table[i].fn || table[i].addr>=WIN32_THUNK_BASE || (i && table[i-1].addr>=table[i].addr))return -1;
        for(unsigned j=0;j<k;++j)if(table[0].addr<=list[j].table[list[j].count-1].addr && list[j].table[0].addr<=table[count-1].addr)return -1;
    }
    memory=m;modules=list;module_count=n;ever_bound=1;
#if defined(__ELF__) || defined(__3DS__) || defined(_WIN32)
    if (g_ir_table && g_ir_count && n3ds_ir_bind(g_ir_table,g_ir_count)) return -1;
#endif
    container_read_env();
    if (ir_container_required && !container_ensure(NULL, 0)) {
        snprintf(fault, sizeof fault, "Guest fault at 00000000: %.200s",
                 ir_container_reason);
        fprintf(stderr, "%s\n", fault);
        return -1;
    }
    return 0;
}
int n3ds_lift_bind(GuestMemory *m,const FnEntry *table,unsigned n) {
    static FnModule single;
    if(ever_bound)return -1;
    single=(FnModule){"single",table,n};return n3ds_lift_bind_modules(m,&single,1);
}
void n3ds_lift_release(void){
    if(!active){
        if(ir_container){n3ds_irc_close(ir_container);ir_container=NULL;}
        ir_container_state=0;
        memory=NULL;modules=NULL;module_count=0;
    }
}
void n3ds_lift_init_cpu(CPU *c,uint32_t stack_top,uint32_t teb) {
    memset(c,0,sizeof *c);c->r[4]=stack_top;c->fs_base=teb;c->fpu_cw=0x37f;c->mxcsr=0x1f80;
}
void *n3ds_guest_ptr(uint32_t addr,size_t bytes) {
    if (tesv_sm3_probe_level_ptr) {
        void *probe = tesv_sm3_probe_level_ptr(addr, bytes);
        if (probe) return probe;
    }
    if (tesv_cube_probe_ptr) {
        void *probe = tesv_cube_probe_ptr(addr, bytes);
        if (probe) return probe;
    }
    void *p=memory?guest_ptr(memory,addr,bytes):NULL;
    if(!p){
        static unsigned logged;
        if(logged<8u){
            ++logged;
            fprintf(stderr,"[guest] unmapped addr=%08x bytes=%lu last_call=%08x "
                    "irc_fn=%08x irc_block=%08x ir_ip=%08x\n",
                    addr,(unsigned long)bytes,g_last_call,
                    (unsigned)n3ds_irc_last_function,(unsigned)n3ds_irc_last_block,
                    (unsigned)g_ir_ip);
            /* The slice executor records its own CPU; g_last_cpu is only the
             * last dispatcher and may belong to another thread that blocked in
             * a shim. Prefer the running interpreter's CPU. */
            CPU *log_cpu = n3ds_irc_current_cpu ? n3ds_irc_current_cpu : g_last_cpu;
            if (n3ds_irc_current_cpu && g_last_cpu && n3ds_irc_current_cpu != g_last_cpu)
                fprintf(stderr,"[guest] cpu mismatch: running=%p last_dispatch=%p\n",
                        (void *)n3ds_irc_current_cpu,(void *)g_last_cpu);
            if(log_cpu){
                CPU *c=log_cpu;
                uint32_t esp=c->r[4], st[6]={0};
                for(unsigned k=0;k<6;++k)(void)guest_read32(memory,esp+4*k,&st[k]);
                fprintf(stderr,"[guest] esp=%08x stack=%08x %08x %08x %08x %08x %08x\n",
                        esp,(unsigned)st[0],(unsigned)st[1],(unsigned)st[2],
                        (unsigned)st[3],(unsigned)st[4],(unsigned)st[5]);
                if(env_flag("TESV_FAULT_REGS",0)){
                    /* The guest stack is below esp here; scan the mapped words
                     * above it for return addresses inside the TESV image. */
                    unsigned shown=0;
                    for(unsigned k=0;k<4096u&&shown<24u;++k){
                        uint32_t a=esp+4u+4u*k,v=0;
                        if(guest_read32(memory,a,&v))break;
                        if(v>=0x00400000u&&v<0x0106a310u){
                            fprintf(stderr,"[guest] ret[%08x]=%08x\n",
                                    (unsigned)a,(unsigned)v);
                            ++shown;
                        }
                    }
                    uint32_t ebp=c->r[5],vt=0,next=0,method=0;
                    if(!guest_read32(memory,ebp,&vt)&&vt){
                        (void)guest_read32(memory,vt+0x64,&method);
                        (void)guest_read32(memory,ebp+0x628,&next);
                        fprintf(stderr,"[guest] recursion ebp=%08x vt=%08x "
                                "method64=%08x next628=%08x\n",
                                (unsigned)ebp,(unsigned)vt,(unsigned)method,
                                (unsigned)next);
                        uint32_t gw=0,gh=0,arr=0,s0=0,s0w=0,s0h=0,s0vt=0;
                        (void)guest_read32(memory,next+8,&gw);
                        (void)guest_read32(memory,next+0xc,&gh);
                        (void)guest_read32(memory,next+0x10,&arr);
                        if(arr){
                            (void)guest_read32(memory,arr,&s0);
                            if(s0){
                                (void)guest_read32(memory,s0+8,&s0w);
                                (void)guest_read32(memory,s0+0xc,&s0h);
                                (void)guest_read32(memory,s0,&s0vt);
                            }
                        }
                        fprintf(stderr,"[guest] group=%08x %ux%u arr=%08x "
                                "s0=%08x %ux%u vt=%08x\n",
                                (unsigned)next,(unsigned)gw,(unsigned)gh,
                                (unsigned)arr,(unsigned)s0,(unsigned)s0w,
                                (unsigned)s0h,(unsigned)s0vt);
                    }
                }
                if(env_flag("TESV_FAULT_REGS",0)){
                    uint32_t ecx=c->r[1];
                    /* CPU has no architectural EIP.  Report the last dispatched
                     * guest target; instrumented slices can refine this with
                     * their PC-aware block trace/backtrace. */
                    fprintf(stderr,"[guest] regs eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x\n",
                            (unsigned)c->r[0],(unsigned)c->r[3],(unsigned)ecx,
                            (unsigned)c->r[2],(unsigned)c->r[6],(unsigned)c->r[7],
                            (unsigned)c->r[5],(unsigned)esp,(unsigned)g_last_call);
                    fprintf(stderr,"[guest] last_block_pc=%08x\n",(unsigned)guest_watch_last_pc());
                    guest_pc_ring_dump(stderr);
                    {
                        unsigned tid = n3ds_ir_thread_id_hook ? n3ds_ir_thread_id_hook() : 0u;
                        fprintf(stderr,"[guest] fault thread=%u\n", tid);
                        guest_pc_ring_dump_thread(stderr, tid);
                    }
                    if (memory) {
                        uint32_t w[8] = {0};
                        fprintf(stderr, "[guest] stack@%08x:", esp);
                        for (unsigned k = 0; k < 8; ++k) {
                            (void)guest_read32(memory, esp + 4u * k, &w[k]);
                            fprintf(stderr, " %08x", w[k]);
                        }
                        fputc('\n', stderr);
                        uint32_t ebp = c->r[5], f[7] = {0};
                        static const uint32_t offs[7] = {0, 4, 0x18, 0x28,
                                                         0x50, 0x54, 0x68};
                        fprintf(stderr, "[guest] ebp=%08x:", ebp);
                        for (unsigned k = 0; k < 7; ++k) {
                            (void)guest_read32(memory, ebp + offs[k], &f[k]);
                            fprintf(stderr, " [+%02x]=%08x", offs[k], f[k]);
                        }
                        fputc('\n', stderr);
                        /* 0x5fc640 saves `this` at its [esp+0x48]; the call to
                         * 0x60aa70 pushed one return address, so the caller's
                         * frame is at esp+4.  Print what the frame points at
                         * rather than trusting a possibly spilled ebp. */
                        uint32_t sv[6] = {0};
                        static const uint32_t soff[6] = {0x4c, 0x50, 0x64,
                                                         0x78, 0x80, 0x84};
                        fprintf(stderr, "[guest] frame@%08x:", esp);
                        for (unsigned k = 0; k < 6; ++k) {
                            (void)guest_read32(memory, esp + soff[k], &sv[k]);
                            fprintf(stderr, " [+%02x]=%08x", soff[k], sv[k]);
                        }
                        fputc('\n', stderr);
                        uint32_t saved_this = sv[0];
                        if (saved_this) {
                            uint32_t g[8] = {0};
                            static const uint32_t goff[8] =
                                {0x00, 0x04, 0x08, 0x54, 0x58, 0x60, 0x68, 0x94};
                            fprintf(stderr, "[guest] saved this=%08x:",
                                    saved_this);
                            for (unsigned k = 0; k < 8; ++k) {
                                (void)guest_read32(memory, saved_this + goff[k],
                                                   &g[k]);
                                fprintf(stderr, " [+%02x]=%08x", goff[k], g[k]);
                            }
                            fputc('\n', stderr);
                            uint32_t data = g[6], e[4] = {0};
                            if (data) {
                                fprintf(stderr, "[guest] array=%08x:", data);
                                for (unsigned k = 0; k < 4; ++k) {
                                    (void)guest_read32(memory, data + 4u * k,
                                                       &e[k]);
                                    fprintf(stderr, " %08x", e[k]);
                                }
                                fputc('\n', stderr);
                            }
                        }
                    }
                    if(memory){
                        const uint8_t *p40=guest_ptr(memory,ecx+0x40,4);
                        const uint8_t *p8=guest_ptr(memory,ecx+8,4);
                        if(p40&&p8){
                            uint32_t field=0,field8=0;
                            memcpy(&field,p40,4);
                            memcpy(&field8,p8,4);
                            const uint8_t *inner=guest_ptr(memory,field,4);
                            if(inner){
                                uint32_t deref=0;memcpy(&deref,inner,4);
                                fprintf(stderr,"[guest] ecx_deref: [ecx+0x40]=%08x [[ecx+0x40]]=%08x [ecx+0x8]=%08x\n",
                                        field,deref,field8);
                            }else{
                                fprintf(stderr,"[guest] ecx_deref: [ecx+0x40]=%08x [[ecx+0x40]]=unreadable [ecx+0x8]=%08x\n",
                                        field,field8);
                            }
                        }else{
                            fprintf(stderr,"[guest] ecx_deref: unreadable\n");
                        }
                        /* Map state of the faulting address: a reserved slab
                         * page (never committed) reads differently from an
                         * address outside every region (freed object/UAF). */
                        {
                            int region=-1;
                            for (size_t ri=0;ri<memory->count;++ri){
                                GuestRegion *r=&memory->regions[ri];
                                if (addr>=r->base && addr-r->base<r->size){region=(int)ri;break;}
                            }
                            GuestSlab *fs=guest_slab_find(memory,addr,1);
                            uint32_t fprot=0;int fstate=guest_slab_state(memory,addr,&fprot);
                            fprintf(stderr,"[guest] fault_map: region=%d slab=%s state=%d protect=%02x",
                                    region, fs?"yes":"no", fstate,(unsigned)fprot);
                            if (fs){
                                uint32_t pg=(addr-fs->base)/GUEST_PAGE_SIZE;
                                fprintf(stderr," slab_base=%08x slab_size=%08x page=%u "
                                        "page_ptr=%p backed=%u used=%lu",
                                        (unsigned)fs->base,(unsigned)fs->size,
                                        (unsigned)pg,(void *)fs->page[pg],
                                        (unsigned)fs->backed_pages,
                                        (unsigned long)memory->used);
                            }
                            fputc('\n',stderr);
                        }
                        /* zlib inflate_table (0x91dd30) / inflate (0x91a7c0):
                         * the state pointer and the frame slots that decide the
                         * table lookup. A value larger than the code count means
                         * the state or the sorted table is corrupt, not merely
                         * an unbacked page. */
                        if (g_ir_ip>=0x91a7c0u && g_ir_ip<=0x91e216u && log_cpu){
                            uint32_t espf=log_cpu->r[4], sl[8]={0};
                            static const uint32_t so[8]={0x14,0x18,0x20,0x24,
                                                         0x2c,0x30,0x34,0xa4};
                            fprintf(stderr,"[guest] zlib esp=%08x:",espf);
                            for(unsigned k=0;k<8;++k){
                                (void)guest_read32(memory,espf+so[k],&sl[k]);
                                fprintf(stderr," [+%02x]=%08x",so[k],sl[k]);
                            }
                            fputc('\n',stderr);
                            uint32_t work=sl[3], state=work-0x2f0u;
                            uint32_t w[16]={0};int okw=1;
                            for(unsigned k=0;k<16;++k)
                                if(guest_read32(memory,work+4u*k,&w[k])){okw=0;break;}
                            fprintf(stderr,"[guest] inflate work=%08x state_guess=%08x words=",work,state);
                            for(unsigned k=0;k<16;++k)
                                fprintf(stderr,"%s%08x",k?" ":"",okw?w[k]:0);
                            fprintf(stderr,"%s\n",okw?"":" unreadable");
                            uint32_t st=log_cpu->r[7], sf[8]={0};
                            static const uint32_t sfo[8]={0x00,0x4c,0x50,0x54,
                                                          0x60,0x64,0x68,0x6c};
                            int oks=1;
                            for(unsigned k=0;k<8;++k)
                                if(guest_read32(memory,st+sfo[k],&sf[k])){oks=0;break;}
                            fprintf(stderr,"[guest] state edi=%08x mode=%08x lencode=%08x "
                                    "distcode=%08x lenbits=%08x nlen=%08x ndist=%08x "
                                    "have=%08x next=%08x%s\n",st,
                                    oks?sf[0]:0,oks?sf[1]:0,oks?sf[2]:0,oks?sf[3]:0,
                                    oks?sf[4]:0,oks?sf[5]:0,oks?sf[6]:0,oks?sf[7]:0,
                                    oks?"":" unreadable");
                        }
                    }else{
                        fprintf(stderr,"[guest] ecx_deref: unreadable\n");
                    }
                }
                if(getenv("TESV_CUBE_PROBE_DEBUG") && g_last_call==0x00fb6090u){
                    uint32_t arg=0, slot=0;
                    (void)guest_read32(memory, esp+4u, &arg);
                    (void)guest_read32(memory, g_last_cpu->r[1]+arg*4u+0x0cu, &slot);
                    fprintf(stderr,"[cube-probe] fault ecx=%08x arg=%08x slot=%08x\n",
                            g_last_cpu->r[1],arg,slot);
                }
            }
#if defined(_WIN32)
            {
                void *frames[20];
                unsigned short n=(unsigned short)RtlCaptureStackBackTrace(0,20,frames,NULL);
                void *modbase=GetModuleHandleA(NULL);
                fprintf(stderr,"[host] bt=%u",(unsigned)n);
                for(unsigned k=0;k<n;++k)
                    fprintf(stderr," %llx",(unsigned long long)((uintptr_t)frames[k]-(uintptr_t)modbase));
                fprintf(stderr,"\n");
            }
#endif
        }
        rt_unimpl(NULL,addr,"unmapped guest memory");
    }
    return p;
}

void n3ds_trace_u32(const char *tag, uint32_t value) {
    fprintf(stderr, "[trace] %s=0x%08x\n", tag ? tag : "value", value);
}
void n3ds_cube_desc_probe(uint32_t arg, uint32_t index, uint32_t base, uint32_t slot) {
    static unsigned emitted;
    if (!env_flag("TESV_CUBE_DESC_PROBE", 0) || emitted >= 32u) return;
    ++emitted;
    uint32_t source = 0, source_index = 0;
    int source_ok = 0;
    if (memory && !guest_read32(memory, arg + 0x604u, &source) && source)
        source_ok = !guest_read32(memory, source + 0x10u, &source_index);
    fprintf(stderr,
            "[cube-desc] arg=%08x index=%u base=%08x slot=%08x slot_null=%s "
            "base_null=%s index_eq_0x16=%s index_ge_0x16=%s "
            "source_obj=%08x source_index=%08x source_ok=%s\n",
            arg, index, base, slot, slot ? "no" : "yes",
            base ? "no" : "yes", index == 0x16u ? "yes" : "no",
            index >= 0x16u ? "yes" : "no", source, source_index,
            source_ok ? "yes" : "no");
    if (memory && base) {
        uint32_t words[8];
        int ok = !guest_read(memory, base, words, sizeof words);
        fprintf(stderr, "[cube-desc] base8=");
        for (unsigned i = 0; i < 8; ++i)
            fprintf(stderr, "%s%08x", i ? " " : "", ok ? words[i] : 0);
        fprintf(stderr, "%s\n", ok ? "" : " unreadable");
    } else {
        fprintf(stderr, "[cube-desc] base8=unreadable\n");
    }
}

/* Measurement-only generated-slice hook: fill f_CECDC0's uninitialised base
 * local from the real source object when the pending descriptor slot is NULL.
 * The original lookup and 68-byte copy still run. */
int n3ds_cecdc0_source_fill(uint32_t frame, uint32_t index, uint32_t ebx) {
    if (!tesv_cecdc0_source_active || !tesv_cecdc0_source_decide) return 0;
    if (!memory || !tesv_cecdc0_source_active()) return 0;
    uint32_t new_base = 0, new_desc = 0;
    int rc = tesv_cecdc0_source_decide(memory, frame, index, ebx, &new_base,
                                        &new_desc);
    if (rc == 1) {
        if (guest_write32(memory, frame + 0x14u, new_desc))
            fprintf(stderr, "[cecdc0-source] REFUSED: descriptor write "
                            "failed; no fill\n");
        return 1; /* skip the base[index] overwrite */
    }
    if (rc == 0 && new_base) {
        if (guest_write32(memory, frame + 0x18u, new_base))
            fprintf(stderr, "[cecdc0-source] REFUSED: base write failed; "
                            "no fill\n");
    }
    return 0;
}

void n3ds_trace_cstr(const char *tag, uint32_t ptr) {
    char text[260];
    size_t n = 0;
    if (memory && guest_ptr(memory, ptr, 1)) {
        while (n + 1 < sizeof text) {
            const uint8_t *p = (const uint8_t *)guest_ptr(memory, ptr + (uint32_t)n, 1);
            if (!p || !*p) break;
            text[n++] = (char)*p;
        }
    }
    text[n] = 0;
    fprintf(stderr, "[trace] %s=0x%08x \"%s\"%s\n", tag ? tag : "text", ptr,
            text, n == sizeof text - 1 ? "...(truncated)" : "");
}

static const FnEntry *lookup(uint32_t addr) {
    if(!memory)return NULL;
    if(addr>=WIN32_THUNK_BASE)return win32_lookup(addr);
    for(unsigned k=0;k<module_count;++k){
        const FnEntry *functions=modules[k].table;unsigned count=modules[k].count;
        if(addr<functions[0].addr || addr>functions[count-1].addr)continue;
        unsigned lo=0,hi=count;
        while(lo<hi) {unsigned mid=lo+(hi-lo)/2;if(functions[mid].addr<addr)lo=mid+1;else hi=mid;}
        return lo<count && functions[lo].addr==addr?functions+lo:NULL;
    }
    return NULL;
}
int n3ds_lift_has_function(uint32_t address){
    if(lookup(address))return 1;
    if(container_ensure(NULL,0)){
        N3dsIrcEntry e;
        if(n3ds_irc_lookup(ir_container,address,&e)==N3DS_IRC_OK)return 1;
    }
    return n3ds_ir_has(address);
}
const char *n3ds_lift_fault(void){return fault;}
void rt_unimpl(CPU *c,uint32_t addr,const char *what) {
    (void)c;
    /* Measurement hook only: the fault text and longjmp below are unchanged. */
    if (memory) n3ds_mem_guest_sample(memory->used, (unsigned)memory->count);
    n3ds_mem_fault_sample();
    snprintf(fault,sizeof fault,"Guest fault at %08lx: %s",(unsigned long)addr,what?what:"unknown");
    fprintf(stderr,"%s\n",fault);if(active)longjmp(active->jb,1);abort();
}
void n3ds_lift_exit(uint32_t code){exit_code=code;if(active)longjmp(active->jb,2);abort();}

/* ---- per-thread runtime state ------------------------------------------ */
void n3ds_lift_thread_state_init(N3dsThreadRtState *s) {
    if (!s) return;
    s->active = NULL;
    s->irc_base = 0;
    s->irc_depth = 0;
    memset(s->irc_frames, 0, sizeof s->irc_frames);
    s->fault[0] = 0;
}
void n3ds_lift_thread_begin(N3dsThreadRtState *s) {
    if (!s) return;
    s->active = NULL;
    /* A guest thread roots its own executor frame stack; the container depth of
     * the thread that created it is irrelevant (that stack is suspended). */
    s->irc_base = 0;
    s->irc_depth = 0;
    memset(s->irc_frames, 0, sizeof s->irc_frames);
    n3ds_irc_frames_load(NULL, 0);
    s->fault[0] = 0;
    thread_rt = s;
    thread_irc_base = s->irc_base;
    active = NULL;
    fault[0] = 0;
}
void n3ds_lift_thread_switch(N3dsThreadRtState *from, N3dsThreadRtState *to) {
    if (from) {
        from->active = active;
        from->irc_depth = n3ds_irc_frames_save(from->irc_frames);
        memcpy(from->fault, fault, sizeof fault);
        if (thread_rt == from) thread_rt = NULL;
    }
    if (to) {
        active = (InvokeFrame *)to->active;
        memcpy(fault, to->fault, sizeof fault);
        n3ds_irc_frames_load(to->irc_frames, to->irc_depth);
        thread_irc_base = to->irc_base;
        thread_rt = to;
    }
}
void rt_int3(CPU *c,uint32_t addr) {rt_unimpl(c,addr,"int3");}
/* Measurement only: a peak can appear between snapshots, so sample the guest
 * high-water mark every 1024 dispatches (one increment + branch otherwise). */
static unsigned mem_sample_tick;
static void mem_sample_dispatch(void) {
    if (memory && ((++mem_sample_tick & 1023u) == 0))
        n3ds_mem_guest_sample(memory->used, (unsigned)memory->count);
}
/* Resolve one call target across all tiers. AOT wins. A container record that
 * explicitly declares a pre-entry cold block must precede slice IR, whose
 * frame starts at the exact entry and cannot execute that block. All other
 * calls keep the verified slice IR before the all-functions container. */
int n3ds_rt_dispatch(CPU *c,uint32_t addr) {
    mem_sample_dispatch();
    if (tesv_cube_probe_dispatch && tesv_cube_probe_dispatch(c, addr)) return 0;
    menu_trace(c, addr);
    if (addr == 0x00ac6c10u && getenv("TESV_STREAM_TRACE")) stream_trace(c);
    if (a43f_on()) {
        n3ds_a43f_trace_dispatch(c, addr);
        /* TESV_A43F_TRACE: measurement-only. Run the traced leaf and the
         * reader through the container/AOT dispatch every time so the AOT
         * inline call cache cannot bypass the counters. The container
         * predicate sends 0xa43f80 to its exact IR record and leaves the two
         * reader addresses on AOT. */
        if (addr == 0x00a43f80u || addr == 0x00ac6c10u || addr == 0x00ac6380u) {
            if (container_dispatch(c, addr) == 0) return 0;
        }
    }
    g_last_call=addr; g_last_cpu=c;
    n3ds_ir_note_pc(addr, c?c->r[4]:0, c?c->r[1]:0, c?c->r[0]:0);
    /* The recent-call ring (including its guest stack read) is consumed only by
     * the hang watchdog dump. Reading the return address on every dispatch costs
     * a checked guest read; skip both when no watchdog/dump env is armed. */
    static int call_ring_on;
    if (!call_ring_on) {
        const char *w = getenv("TESV_HANG_WATCHDOG_MS");
        const char *d = getenv("TESV_DUMP_AFTER_MS");
        call_ring_on = ((w && *w) || (d && *d)) ? 2 : 1;
    }
    if (call_ring_on == 2) {
        uint32_t from=0;
        if(memory&&c) (void)guest_read32(memory,c->r[4],&from);
        g_call_ring[g_call_ring_n%RT_CALL_RING].addr=addr;
        g_call_ring[g_call_ring_n%RT_CALL_RING].from=from;
        ++g_call_ring_n;
    }
    const FnEntry *e=lookup(addr);
    if(e){
        ++n3ds_lift_tier_aot;
        if(a43f_on()&&addr==0x00a43f80u){
            n3ds_a43f_trace_aot_pre(c);
            e->fn(c);
            n3ds_a43f_trace_aot_post(c);
        } else e->fn(c);
        return 0;
    }
    int slice = n3ds_ir_has(addr) != 0;
    if (slice && container_has_preentry(addr)) {
        if (!container_dispatch(c, addr)) { ++n3ds_lift_tier_container; return 0; }
    }
    if (slice) { ++n3ds_lift_tier_slice; n3ds_ir_call(c,addr); return 0; }
    if(!container_dispatch(c,addr)){ ++n3ds_lift_tier_container; return 0; }
    return -1;
}
void rt_call(CPU *c,uint32_t addr) {
    if(n3ds_rt_dispatch(c,addr))rt_unimpl(c,addr,"unknown function/import");
}
/* Diagnostic only: N3DS_INDIRECT_TRACE=1 dumps the failing indirect target,
 * the caller (return address on the guest stack), all registers and, for a
 * `jmp [slot]` thunk, the slot address and its current value.  This never
 * changes dispatch; it only describes the target that dispatch rejected. */
static void trace_indirect_fault(CPU *c,uint32_t target){
    static int trace_state;
    if(!trace_state){
        const char *v=getenv("N3DS_INDIRECT_TRACE");
        trace_state=(v&&v[0]&&strcmp(v,"0"))?2:1;
        if(trace_state==2)
            fprintf(stderr,"[indirect] N3DS_INDIRECT_TRACE=1: reporting rejected indirect targets only\n");
    }
    if(trace_state!=2)return;
    uint32_t caller=0; if(c)(void)guest_read32(memory,c->r[4],&caller);
    fprintf(stderr,"[indirect] target=%08lx caller=%08lx esp=%08lx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx\n",
        (unsigned long)target,(unsigned long)caller,
        c?(unsigned long)c->r[4]:0,(unsigned long)(c?c->r[0]:0),(unsigned long)(c?c->r[3]:0),
        (unsigned long)(c?c->r[1]:0),(unsigned long)(c?c->r[2]:0),
        (unsigned long)(c?c->r[6]:0),(unsigned long)(c?c->r[7]:0),(unsigned long)(c?c->r[5]:0));
    const uint8_t *p=n3ds_guest_ptr(target,6);
    if(!p)return;
    fprintf(stderr,"[indirect] bytes=%02x %02x %02x %02x %02x %02x\n",p[0],p[1],p[2],p[3],p[4],p[5]);
    if(p[0]==0xff&&p[1]==0x25){
        uint32_t slot=(uint32_t)p[2]|((uint32_t)p[3]<<8)|((uint32_t)p[4]<<16)|((uint32_t)p[5]<<24);
        const uint8_t *q=n3ds_guest_ptr(slot,4);uint32_t val=0;
        if(q)val=(uint32_t)q[0]|((uint32_t)q[1]<<8)|((uint32_t)q[2]<<16)|((uint32_t)q[3]<<24);
        fprintf(stderr,"[indirect] thunk-slot=%08lx value=%08lx\n",(unsigned long)slot,(unsigned long)val);
    }
}
void rt_call_ic(CPU *c,uint32_t addr,const FnEntry **cache) {
    mem_sample_dispatch();
    if (tesv_cube_probe_dispatch && tesv_cube_probe_dispatch(c, addr)) return;
    menu_trace(c, addr);
    g_last_call=addr; g_last_cpu=c;
    n3ds_ir_note_pc(addr, c?c->r[4]:0, c?c->r[1]:0, c?c->r[0]:0);
    const FnEntry *e=lookup(addr);
    if(e){*cache=e;e->fn(c);return;}
    int slice = n3ds_ir_has(addr) != 0;
    if (slice && container_has_preentry(addr)) {
        if (!container_dispatch(c, addr)) { *cache=NULL; return; }
    }
    if(slice){
        /* The per-site cache holds FnEntry pointers only. Interpreter targets
         * are not cached; the generated code simply calls rt_call_ic again. */
        *cache=NULL;n3ds_ir_call(c,addr);return;
    }
    if(!container_dispatch(c,addr)){*cache=NULL;return;}
    trace_indirect_fault(c,addr);
    rt_unimpl(c,addr,"unknown indirect function");
}
/* Nonlocal returns for recompiled code.  setjmp.c owns the guest jmp_buf
 * slots; these hooks let it resume the interpreter without jumping into a
 * dead host frame. */
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb);
void rt_setjmp3(CPU *c);
void rt_longjmp(CPU *c, uint32_t jb, uint32_t val);
uint32_t n3ds_lift_current_function_end(void);
void n3ds_lift_guest_resume(CPU *c, uint32_t eip, uint32_t func_end);
void rt_cpuid(CPU *c,uint32_t *a,uint32_t *b,uint32_t *cc,uint32_t *d) {
    (void)c;uint32_t leaf=*a;*a=*b=*cc=*d=0;
    /* Report the same virtual Pentium-4 profile as the upstream lifter. */
    if(leaf==0){*a=1;memcpy(b,"Genu",4);memcpy(d,"ineI",4);memcpy(cc,"ntel",4);}
    else if(leaf==1){*a=0x00000f29;*d=(1u<<0)|(1u<<4)|(1u<<15)|(1u<<23)|(1u<<25)|(1u<<26);}
}
uint64_t rt_rdtsc(void) {
    /* A monotonic host tick source, not a calibrated emulated x86 TSC. */
#ifdef __3DS__
    return svcGetSystemTick();
#else
    return (uint64_t)clock();
#endif
}
uint32_t n3ds_ir_cur_func_end;
uint32_t n3ds_lift_current_function_end(void){return n3ds_ir_cur_func_end;}

static volatile int g_guest_resume_pending;
static uint32_t g_guest_resume_eip, g_guest_resume_end;
void n3ds_lift_guest_resume(CPU *c,uint32_t eip,uint32_t func_end) {
    if(!active)rt_unimpl(c,eip,"longjmp outside an active guest invocation");
    g_guest_resume_eip=eip;
    g_guest_resume_end=func_end;
    g_guest_resume_pending=1;
    longjmp(active->jb,3);
    __builtin_unreachable();
}

static int invoke_common(CPU *c,uint32_t address,uint32_t ir_end,int reentrant,
                         const uint32_t *args,size_t argc,uint32_t *result) {
    int have=ir_end?1:(n3ds_lift_has_function(address)!=0);
    uint32_t sp=c->r[4];
    if(!memory || !have || !result || (!reentrant && active) || (argc&&!args) ||
       argc>=UINT32_MAX/4 || sp<(argc+1)*4)return -1;
    uint32_t frame=(uint32_t)((argc+1)*4), base=sp-frame;
    if(!guest_ptr(memory,base,frame))return -1;
    M32(base)=0xffffffffu;
    for(size_t i=0;i<argc;++i)M32(base+4+(uint32_t)i*4)=args[i];
    InvokeFrame f;f.prev=active;active=&f;fault[0]=0;
    int interrupted=setjmp(f.jb);
    if(interrupted){
        if(ir_container)n3ds_irc_recover_to(ir_container,thread_irc_base);
        active=f.prev;c->r[4]=sp;
        if(interrupted==2){*result=exit_code;return N3DS_INVOKE_EXIT;}
        return N3DS_INVOKE_TRAP;
    }
    c->r[4]=base;
    /* A guest longjmp unwinds to this frame with code 3.  Loop so a resumed
     * function may longjmp again (nested setjmp), and only then take the usual
     * epilogue.  The guest stack/registers were restored by rt_longjmp(). */
    for (;;) {
        if(ir_end)n3ds_ir_run(c,address,ir_end);
        else if(n3ds_rt_dispatch(c,address))rt_unimpl(c,address,"unknown function/import");
        if (!g_guest_resume_pending) break;
        g_guest_resume_pending = 0;
        address = g_guest_resume_eip;
        ir_end = g_guest_resume_end;
        if (!ir_end || !address)
            rt_unimpl(c, address, "longjmp without a resumable IR function range");
    }
    active=f.prev;
    int ok=c->r[4]==base+4 || c->r[4]==sp;
    if(!ok){
        const char *v=getenv("N3DS_INVOKE_TRACE");
        if(v&&*v&&strcmp(v,"0"))fprintf(stderr,"[invoke] stack esp=%08lx base=%08lx sp=%08lx\n",
            (unsigned long)c->r[4],(unsigned long)base,(unsigned long)sp);
    }
    c->r[4]=sp;if(!ok)return -1;
    *result=c->r[0];return 0;
}
int n3ds_lift_invoke(CPU *c,uint32_t address,const uint32_t *args,size_t argc,uint32_t *result) {
    return invoke_common(c,address,0,0,args,argc,result);
}
int n3ds_lift_invoke_reentrant(CPU *c,uint32_t address,const uint32_t *args,size_t argc,uint32_t *result) {
    return invoke_common(c,address,0,1,args,argc,result);
}
int n3ds_ir_invoke(CPU *c,uint32_t address,uint32_t end,const uint32_t *args,size_t argc,uint32_t *result) {
    if(!end)end=n3ds_ir_end(address);
    if(!end)return -1;
    return invoke_common(c,address,end,0,args,argc,result);
}
