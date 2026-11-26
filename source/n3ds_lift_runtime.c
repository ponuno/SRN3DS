#include "n3ds_lift_runtime.h"
#include "win32_shims.h"
/* Optional measurement hook; absent from ordinary host/ARM test binaries. */
extern void *tesv_sm3_probe_level_ptr(uint32_t address, size_t bytes)
    __attribute__((weak));
extern void *tesv_cube_probe_ptr(uint32_t address, size_t bytes)
    __attribute__((weak));
extern int tesv_cube_probe_dispatch(CPU *c, uint32_t addr)
    __attribute__((weak));
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

static int container_aot_pred(void *user, uint32_t addr) {
    (void)user;
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
            fprintf(stderr,"[guest] unmapped addr=%08x bytes=%lu last_call=%08x\n",
                    addr,(unsigned long)bytes,g_last_call);
            if(g_last_cpu){
                CPU *c=g_last_cpu;
                uint32_t esp=c->r[4];
                fprintf(stderr,"[guest] esp=%08x stack=%08x %08x %08x %08x %08x %08x\n",
                        esp,(unsigned)M32(esp),(unsigned)M32(esp+4),(unsigned)M32(esp+8),
                        (unsigned)M32(esp+12),(unsigned)M32(esp+16),(unsigned)M32(esp+20));
                if(env_flag("TESV_FAULT_REGS",0)){
                    uint32_t ecx=c->r[1];
                    /* CPU has no architectural EIP.  Report the last dispatched
                     * guest target; instrumented slices can refine this with
                     * their PC-aware block trace/backtrace. */
                    fprintf(stderr,"[guest] regs eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x\n",
                            (unsigned)c->r[0],(unsigned)c->r[3],(unsigned)ecx,
                            (unsigned)c->r[2],(unsigned)c->r[6],(unsigned)c->r[7],
                            (unsigned)c->r[5],(unsigned)esp,(unsigned)g_last_call);
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
    (void)c;snprintf(fault,sizeof fault,"Guest fault at %08lx: %s",(unsigned long)addr,what?what:"unknown");
    fprintf(stderr,"%s\n",fault);if(active)longjmp(active->jb,1);abort();
}
void n3ds_lift_exit(uint32_t code){exit_code=code;if(active)longjmp(active->jb,2);abort();}

/* ---- per-thread runtime state ------------------------------------------ */
void n3ds_lift_thread_state_init(N3dsThreadRtState *s) {
    if (!s) return;
    s->active = NULL;
    s->irc_base = 0;
    s->fault[0] = 0;
}
void n3ds_lift_thread_begin(N3dsThreadRtState *s) {
    if (!s) return;
    s->active = NULL;
    s->irc_base = n3ds_irc_depth(ir_container);
    s->fault[0] = 0;
    thread_rt = s;
    thread_irc_base = s->irc_base;
    active = NULL;
    fault[0] = 0;
}
void n3ds_lift_thread_switch(N3dsThreadRtState *from, N3dsThreadRtState *to) {
    if (from) {
        from->active = active;
        memcpy(from->fault, fault, sizeof fault);
        if (thread_rt == from) thread_rt = NULL;
    }
    if (to) {
        active = (InvokeFrame *)to->active;
        memcpy(fault, to->fault, sizeof fault);
        thread_irc_base = to->irc_base;
        thread_rt = to;
    }
}
void rt_int3(CPU *c,uint32_t addr) {rt_unimpl(c,addr,"int3");}
/* Resolve one call target across all tiers. AOT wins when an address is in
 * more than one table; the container is next; the generated slice IR table is
 * the final fallback. */
int n3ds_rt_dispatch(CPU *c,uint32_t addr) {
    if (tesv_cube_probe_dispatch && tesv_cube_probe_dispatch(c, addr)) return 0;
    g_last_call=addr; g_last_cpu=c;
    const FnEntry *e=lookup(addr);
    if(e){e->fn(c);return 0;}
    /* AOT first, then the generated slice IR, then the all-functions container.
     * The slice IR is the verified/hot set; an invalid container entry must not
     * shadow a function the slice already implements. */
    if(n3ds_ir_has(addr)){n3ds_ir_call(c,addr);return 0;}
    if(!container_dispatch(c,addr))return 0;
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
    uint32_t caller=c?M32(c->r[4]):0;
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
    if (tesv_cube_probe_dispatch && tesv_cube_probe_dispatch(c, addr)) return;
    g_last_call=addr; g_last_cpu=c;
    const FnEntry *e=lookup(addr);
    if(e){*cache=e;e->fn(c);return;}
    if(n3ds_ir_has(addr)){
        /* The per-site cache holds FnEntry pointers only. Interpreter targets
         * are not cached; the generated code simply calls rt_call_ic again. */
        *cache=NULL;n3ds_ir_call(c,addr);return;
    }
    if(!container_dispatch(c,addr)){*cache=NULL;return;}
    trace_indirect_fault(c,addr);
    rt_unimpl(c,addr,"unknown indirect function");
}
/* Nonlocal returns depend on the full Wine/Win32 shim semantics. Fail loudly. */
jmp_buf *rt_setjmp_slot(CPU *c,uint32_t guest_jb) {rt_unimpl(c,guest_jb,"setjmp not implemented");}
void rt_setjmp3(CPU *c) {rt_unimpl(c,0,"setjmp3 not implemented");}
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
    if(ir_end)n3ds_ir_run(c,address,ir_end);
    else if(n3ds_rt_dispatch(c,address))rt_unimpl(c,address,"unknown function/import");
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
