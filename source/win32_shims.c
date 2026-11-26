#define _POSIX_C_SOURCE 200809L
#include "win32_shims.h"
#include "tesv_tls_policy.h"
#include "win32_sync.h"
#include "win32_user.h"
#include "win32_locale.h"
#include "win32_shell.h"
#include "win32_audio.h"
#include "win32_thread.h"
#include "win32_input.h"
#include "win32_steam.h"
#include "win32_registry.h"
#include "guest_heap.h"
#include "guest_process.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef __3DS__
#include <3ds.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m)&_S_IFMT)==_S_IFDIR)
#endif
#if defined(_WIN32)
__declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long frames_to_skip, unsigned long frames_to_capture,
    void **frames, unsigned long *hash);
#endif

#define INVALID_HANDLE UINT32_MAX
#define HEAP_HANDLE 1u
#define ERR_INVALID_HANDLE 6u
#define ERR_INVALID_PARAMETER 87u
#define ERR_NO_MEMORY 8u
#define ERR_FILE_NOT_FOUND 2u
#define ERR_ACCESS_DENIED 5u
#define ERR_READ_FAULT 30u
#define ERR_WRITE_FAULT 29u
#define ERR_NOT_SUPPORTED 50u
#define ERR_FILE_EXISTS 80u
#define ERR_ALREADY_EXISTS 183u
#define ERR_NO_MORE_FILES 18u
#define ERR_INSUFFICIENT_BUFFER 122u
#define MAX_FILES 16
#define MAX_FINDS 8
#define FIND_HANDLE_BASE 0x300u
#define ATTR_DIRECTORY 0x10u
#define ATTR_NORMAL 0x80u
static GuestMemory *mem;
static GuestHeap heap;
static char root[512];
static char cwd[512];
static uint32_t last_error;
static uint64_t start_tick;

/* ---- allocation accounting (diagnostic) ----------------------------------------
 * Answers the memory question with a measurement: how much does the guest
 * actually ask for, and where does the port stop being able to serve it.
 *
 * `probe` additionally reports a 512 MiB commit limit in GlobalMemoryStatus(Ex).
 * That value is a deliberate measurement-only experiment: it lets the engine's
 * minimum-configuration check pass so the real allocation pattern can be
 * observed. It is logged as a probe and must never be enabled in a build that
 * claims to be honest about its memory. */
enum { MEM_VA = 0, MEM_HEAP = 1, MEM_CRT = 2 };
typedef struct { uint8_t kind, failed; uint32_t size; } MemEvent;
static struct {
    int probe, reported;
    uint64_t va_total, heap_total, crt_total;
    uint64_t va_live, heap_live, crt_live, peak_live;
    uint32_t va_calls, heap_calls, crt_calls, failures;
    unsigned events;
    MemEvent ring[256];
} memt;

void win32_set_memory_probe(int on) { memt.probe = on ? 1 : 0; }
int win32_memory_probe(void) { return memt.probe; }
static uint64_t *mem_live(int kind) {
    return kind == MEM_VA ? &memt.va_live : kind == MEM_HEAP ? &memt.heap_live : &memt.crt_live;
}
static uint64_t *mem_total(int kind) {
    return kind == MEM_VA ? &memt.va_total : kind == MEM_HEAP ? &memt.heap_total : &memt.crt_total;
}
static void mem_track(int kind, uint32_t size, int ok) {
    if (kind == MEM_VA) ++memt.va_calls;
    else if (kind == MEM_HEAP) ++memt.heap_calls;
    else ++memt.crt_calls;
    if (ok) {
        *mem_total(kind) += size;
        *mem_live(kind) += size;
        uint64_t all = memt.va_live + memt.heap_live + memt.crt_live;
        if (all > memt.peak_live) memt.peak_live = all;
    } else {
        ++memt.failures;
        if (memt.failures == 1)
            fprintf(stderr, "[mem] first allocation failure: kind=%d size=%lu\n",
                    kind, (unsigned long)size);
    }
    memt.ring[memt.events++ & 255] = (MemEvent){(uint8_t)kind, (uint8_t)(ok ? 0 : 1), size};
}
static void mem_untrack(int kind, uint32_t size) {
    uint64_t *live = mem_live(kind);
    *live = size <= *live ? *live - size : 0;
}
static void va_report(FILE *f);
static uint64_t va_promise_live_bytes(void);
static uint64_t va_backed_live_bytes(void);
static uint64_t va_backed_peak_bytes(void);
void win32_memory_report(FILE *f) {
    if (!f) return;
    fprintf(f, "[mem] va: %lu calls, %lu KiB requested, %lu KiB promise live, %lu KiB backed\n",
            (unsigned long)memt.va_calls, (unsigned long)(memt.va_total >> 10),
            (unsigned long)(va_promise_live_bytes() >> 10),
            (unsigned long)(va_backed_live_bytes() >> 10));
    va_report(f);
    if (mem) {
        GuestPageStats ps;
        guest_page_stats(mem, &ps);
        fprintf(f, "[mem] pages: first-touch %lu (anon %lu, file %lu), dirty-written %lu, "
                   "decommitted %lu, evicted %lu (clean %lu), reloads %lu, failures %lu\n",
                (unsigned long)ps.first_touched, (unsigned long)ps.anon,
                (unsigned long)ps.file, (unsigned long)ps.dirty_written,
                (unsigned long)ps.decommitted, (unsigned long)ps.evicted,
                (unsigned long)ps.evicted_clean, (unsigned long)ps.reloaded,
                (unsigned long)ps.failures);
    }
    fprintf(f, "[mem] heap: %lu calls, %lu KiB requested, %lu KiB live\n",
            (unsigned long)memt.heap_calls, (unsigned long)(memt.heap_total >> 10),
            (unsigned long)(memt.heap_live >> 10));
    fprintf(f, "[mem] crt: %lu calls, %lu KiB requested, %lu KiB live\n",
            (unsigned long)memt.crt_calls, (unsigned long)(memt.crt_total >> 10),
            (unsigned long)(memt.crt_live >> 10));
    fprintf(f, "[mem] peak live %lu KiB (heap/crt) + %lu KiB VA backed, failures %lu, probe %s\n",
            (unsigned long)(memt.peak_live >> 10),
            (unsigned long)(va_backed_peak_bytes() >> 10),
            (unsigned long)memt.failures,
            memt.probe ? "ON (measurement only)" : "off");
    unsigned n = memt.events < 256 ? memt.events : 256;
    if (n) {
        fprintf(f, "[mem] last %u events (kind size failed):\n", n);
        for (unsigned i = 0; i < n; ++i) {
            const MemEvent *e = &memt.ring[(memt.events - n + i) & 255];
            fprintf(f, "  %u %lu %u\n", e->kind, (unsigned long)e->size, e->failed);
        }
    }
}
static FILE *files[MAX_FILES];
static char file_path[MAX_FILES][1024];
static uint8_t file_can_read[MAX_FILES];
static uint8_t file_can_write[MAX_FILES];
static FILE *file_for(uint32_t h);
static int path_for(const char *name,char *out,size_t cap);
/* Enumeration state. One small fixed table (MAX_FINDS concurrent searches);
 * find handles are FIND_HANDLE_BASE+i, disjoint from file (0x100) and heap
 * (0x200) handles. The pattern is kept even on Windows for diagnostics. */
typedef struct {
    int used;
    char pattern[260];
    char dirpath[1024];
#ifdef _WIN32
    intptr_t cookie;
#else
    DIR *dir;
#endif
} FindState;
static FindState finds[MAX_FINDS];
/* Scratch stack for the one-time executable TLS callback invocation (see
 * guest_tls_bootstrap below). Mapped lazily only for the TESV image. */
#define GUEST_BOOT_STACK 0x1e200000u
#define GUEST_BOOT_STACK_BYTES 0x10000u
static uint32_t guest_boot_stack;
static int guest_boot_done;
#define TLS_SLOTS 576
static uint32_t tls_values[TLS_SLOTS];
static uint8_t tls_used[TLS_SLOTS];
static const PeLinker *process_linker;
static PeImportResolve external_resolver;
static void *external_context;
#define MAX_EXTRA_RESOLVERS 4
static PeImportResolve extra_resolvers[MAX_EXTRA_RESOLVERS];
static void *extra_context[MAX_EXTRA_RESOLVERS];
#define MAX_TRAPS 1024
static struct { char text[192]; } traps[MAX_TRAPS];
static unsigned trap_count;
static int trap_enabled;
static int trap_index;
static FnEntry trap_entries[MAX_TRAPS];
static void trap_fn(CPU *c){
    if(trap_index<0 || (unsigned)trap_index>=trap_count)rt_unimpl(c,0,"unresolved import");
    rt_unimpl(c,0,traps[trap_index].text);
}
void win32_set_unresolved_trap(int enabled){trap_enabled=enabled;}
unsigned win32_unresolved_trap_count(void){return trap_count;}
const char *win32_unresolved_trap_text(unsigned i){return i<trap_count?traps[i].text:NULL;}

static uint64_t tick(void) {
#ifdef __3DS__
    return svcGetSystemTick();
#elif defined(_WIN32)
    LARGE_INTEGER now;
    if(!QueryPerformanceCounter(&now))return 0;
    return (uint64_t)now.QuadPart;
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
#endif
}
static uint64_t freq(void) {
#ifdef __3DS__
    return SYSCLOCK_ARM11;
#elif defined(_WIN32)
    LARGE_INTEGER f;
    if(!QueryPerformanceFrequency(&f)||!f.QuadPart)return 1000000000u;
    return (uint64_t)f.QuadPart;
#else
    return 1000000000u;
#endif
}
static void va_shutdown(void);
static void mappings_shutdown(void);
static int mapping_close(uint32_t h);
int win32_init(GuestMemory *m,uint32_t bytes,const char *game_root) {
    if(mem || !m || !game_root || !*game_root || strlen(game_root)>=sizeof root || !bytes)return -1;
    if(heap_create(&heap,m,0x20000000u,bytes))return -1;
    mem=m;strcpy(root,game_root);
    /* Normalise trailing separators (C:\Games\Skyrim\ -> C:\Games\Skyrim).
     * Keep a drive root (C:\) and '/' intact; path_for_impl also accepts a
     * separator-terminated root defensively. */
    size_t root_len=strlen(root);
    while(root_len>1 && (root[root_len-1]=='/'||root[root_len-1]=='\\')){
        if(root_len==3 && root[1]==':')break;
        root[--root_len]=0;
    }
    strcpy(cwd,root);last_error=0;start_tick=tick();
    memset(tls_used,0,sizeof tls_used);memset(tls_values,0,sizeof tls_values);
    if(win32_user_init(m)){mem=NULL;return -1;}
    if(win32_thread_init(m)){win32_user_shutdown();mem=NULL;return -1;}
    win32_sync_set_thread_query(win32_thread_query);
    win32_sync_set_blocker(win32_thread_block, win32_thread_deadlock_text);
    if(win32_input_init(m)){win32_thread_shutdown();win32_user_shutdown();mem=NULL;return -1;}
    if(win32_steam_init(m)){
        win32_input_shutdown();
        win32_thread_shutdown();
        win32_user_shutdown();
        mem=NULL;
        return -1;
    }
    if(win32_registry_init(m)){
        win32_steam_shutdown();
        win32_input_shutdown();
        win32_thread_shutdown();
        win32_user_shutdown();
        mem=NULL;
        return -1;
    }
    win32_shell_init(m);
    win32_shell_set_root(game_root);
    return 0;
}
void win32_shutdown(void) {
    if(!mem)return;
    win32_registry_shutdown();
    win32_steam_shutdown();
    win32_input_shutdown();
    win32_thread_shutdown();
    win32_user_shutdown();
    win32_shell_shutdown();
    for(unsigned i=0;i<MAX_FILES;++i)if(files[i]){fclose(files[i]);files[i]=NULL;file_can_read[i]=0;file_can_write[i]=0;file_path[i][0]=0;}
    for(unsigned i=0;i<MAX_FINDS;++i)if(finds[i].used){
#ifdef _WIN32
        _findclose(finds[i].cookie);
#else
        closedir(finds[i].dir);
#endif
        finds[i].used=0;
    }
    if(guest_boot_stack){guest_unmap(mem,guest_boot_stack);guest_boot_stack=0;}
    mappings_shutdown();
    va_shutdown();
    heap_destroy(&heap);mem=NULL;process_linker=NULL;external_resolver=NULL;external_context=NULL;
    memset(extra_resolvers,0,sizeof extra_resolvers);memset(extra_context,0,sizeof extra_context);
}
void win32_set_external_resolver(PeImportResolve r,void *ctx){external_resolver=r;external_context=ctx;}
int win32_add_external_resolver(PeImportResolve r,void *ctx){
    if(!r)return -1;
    for(unsigned i=0;i<MAX_EXTRA_RESOLVERS;++i)if(extra_resolvers[i]==r&&extra_context[i]==ctx)return 0;
    for(unsigned i=0;i<MAX_EXTRA_RESOLVERS;++i)if(!extra_resolvers[i]){extra_resolvers[i]=r;extra_context[i]=ctx;return 0;}
    return -1;
}
void win32_set_linker(const PeLinker *linker){process_linker=linker;}
static uint32_t arg(CPU *c,unsigned n) {return M32(c->r[4]+4+n*4);}
static void result(CPU *c,uint32_t v,unsigned slots) {c->r[0]=v;c->r[4]+=4+slots*4;}
#define A(n) arg(c,n)
#define RET(v,n) do{result(c,(uint32_t)(v),n);return;}while(0)
#define CRT(v) RET(v,0)
static uint32_t teb_value(CPU *c,uint32_t off,uint32_t fallback){uint32_t v;return c->fs_base && !guest_read32(mem,c->fs_base+off,&v)?v:fallback;}
static void set_error(CPU *c,uint32_t v){last_error=v;if(c->fs_base)guest_write32(mem,c->fs_base+0x34,v);}
void win32_set_last_error(uint32_t v){last_error=v;}
#define FAIL(err,v,n) do{set_error(c,err);RET(v,n);}while(0)
static void *checked(CPU *c,uint32_t addr,uint32_t n) {
    void *p=guest_ptr(mem,addr,n);if(!p)rt_unimpl(c,addr,"invalid shim memory range");return p;
}
static const char *str(CPU *c,uint32_t a) {
    const char *p=guest_cstr(mem,a,1024*1024);if(!p)rt_unimpl(c,a,"invalid shim string");return p;
}
static unsigned lower_ascii(unsigned c) {return c>='A'&&c<='Z'?c+32:c;}
static int icmp(const char *a,const char *b) {
    while(*a && lower_ascii((unsigned char)*a)==lower_ascii((unsigned char)*b)){++a;++b;}
    return (int)lower_ascii((unsigned char)*a)-(int)lower_ascii((unsigned char)*b);
}
static uint32_t hsize(GuestHeap *h,uint32_t p) {
    if(!h || p<h->base)return 0;
    for(size_t i=0;i<h->count;++i)if(!h->blocks[i].free && h->blocks[i].offset==p-h->base)return h->blocks[i].size;
    return 0;
}

static void s_GetLastError(CPU *c){RET(teb_value(c,0x34,last_error),0);}
static void s_SetLastError(CPU *c){set_error(c,A(0));RET(0,1);}
static void s_GetProcessHeap(CPU *c){RET(HEAP_HANDLE,0);}
#define MAX_HEAPS 4
static GuestHeap heaps[MAX_HEAPS];
static uint32_t heap_bases[MAX_HEAPS];
static GuestHeap *heap_for(uint32_t h){
    if(h==HEAP_HANDLE)return &heap;
    if(h>=0x200&&h<0x200+MAX_HEAPS&&heap_bases[h-0x200])return &heaps[h-0x200];
    return NULL;
}
static void s_HeapCreate(CPU *c){
    uint32_t flags=A(0),initial=A(1);
    if(flags&~0x00040001u){mem_track(MEM_HEAP,initial,0);FAIL(ERR_INVALID_PARAMETER,0,3);}
    uint32_t size=initial?(initial+0xffffu)&~0xffffu:0x10000u;
    if(size<0x10000u)size=0x10000u;if(size>0x800000u)size=0x800000u;
    mem_track(MEM_HEAP,size,1);
    unsigned i;for(i=0;i<MAX_HEAPS&&heap_bases[i];++i){}if(i==MAX_HEAPS)FAIL(ERR_NO_MEMORY,0,3);
    uint32_t base=0x30000000u+i*0x01000000u;
    if(heap_create(&heaps[i],mem,base,size))FAIL(ERR_NO_MEMORY,0,3);
    heap_bases[i]=base;RET(0x200+i,3);}
static void s_HeapDestroy(CPU *c){
    uint32_t h=A(0),i=h-0x200;
    if(h<0x200||h>=0x200+MAX_HEAPS||!heap_bases[i])FAIL(ERR_INVALID_HANDLE,0,1);
    heap_destroy(&heaps[i]);heap_bases[i]=0;RET(1,1);
}
static void s_HeapAlloc(CPU *c){
    GuestHeap *h=heap_for(A(0));if(!h)FAIL(ERR_INVALID_HANDLE,0,3);
    uint32_t want=A(2)?A(2):1;
    uint32_t p=heap_alloc(h,want);if(p && (A(1)&8)){uint32_t z=hsize(h,p);memset(checked(c,p,z),0,z);}
    mem_track(MEM_HEAP,want,p!=0);
    RET(p,3);
}
static void s_HeapFree(CPU *c){
    GuestHeap *h=heap_for(A(0));if(!h || !A(2))FAIL(ERR_INVALID_PARAMETER,0,3);
    uint32_t n=hsize(h,A(2));
    if(heap_free(h,A(2)))FAIL(ERR_INVALID_PARAMETER,0,3);
    mem_untrack(MEM_HEAP,n);RET(1,3);
}
static void s_HeapSize(CPU *c){GuestHeap *h=heap_for(A(0));uint32_t n=h?hsize(h,A(2)):0;if(!h || !n)FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,3);RET(n,3);}
static void s_HeapReAlloc(CPU *c){
    GuestHeap *h=heap_for(A(0));if(!h)FAIL(ERR_INVALID_HANDLE,0,4);
    uint32_t old=hsize(h,A(2)),n=A(3)?A(3):1;
    if(!old)FAIL(ERR_INVALID_PARAMETER,0,4);
    if((A(1)&0x10) && n>old)RET(0,4); /* HEAP_REALLOC_IN_PLACE_ONLY */
    uint32_t p=heap_realloc(h,A(2),n);if(p && n>old && (A(1)&8))memset(checked(c,p+old,n-old),0,n-old);
    RET(p,4);
}
/* ---- virtual address space ------------------------------------------------------
 * Win32 reserve/commit split backed by one lazy slab per reservation.
 *
 * MEM_RESERVE records address space only. MEM_COMMIT records the *promise* for
 * every 4 KiB page in the (address-rounded-down, size-rounded-up) range:
 *   - unaligned commit addresses are legal and rounded down, like Windows;
 *   - re-committing an already committed page is an idempotent success and
 *     preserves its contents;
 *   - the host backing for a committed page is allocated on the guest's first
 *     read or write (source/guest.c), not at commit time.
 * An access to a reserved (or decommitted) page still faults through
 * n3ds_guest_ptr() with exactly "unmapped guest memory".
 *
 * The VA window is 0x40000000..0x70000000 (768 MiB). It is disjoint from the
 * guest images, the process stack/PEB/TEB/TLS (0x10000000..0x18300000), the
 * thread stacks (0x1c000000+), the guest heaps (0x20000000/0x30000000+) and the
 * D3D9 COM arena (0x30000000). A single request is capped at 1 GiB. The guest
 * pool caps physical backing, so a large commit promise costs page tables only.
 */
#define VA_WINDOW_BASE 0x40000000u
#define VA_WINDOW_END 0x70000000u
#define VA_GRANULARITY 0x10000u
#define VA_PAGE_SIZE 0x1000u
#define VA_MAX_REQUEST 0x40000000u
#define VA_MAX_RES 64
#define VA_MEM_COMMIT 0x1000u
#define VA_MEM_RESERVE 0x2000u
#define VA_MEM_DECOMMIT 0x4000u
#define VA_MEM_RELEASE 0x8000u
#define VA_STATE_COMMIT 0x1000u
#define VA_STATE_RESERVE 0x2000u
#define VA_TYPE_PRIVATE 0x20000u
#define VA_PAGE_NOACCESS 0x01u
#define ERR_INVALID_ADDRESS 487u

typedef struct { uint32_t base, size; } VaRes;

static VaRes va_res[VA_MAX_RES];
static unsigned va_res_count;
static uint64_t va_res_live, va_res_total;
static uint64_t va_promise_live, va_promise_peak;
static uint32_t va_commit_failures, va_last_fail_size;

void win32_va_stats(Win32VaStats *out) {
    if (!out) return;
    out->reserved_live = va_res_live;
    out->reserved_total = va_res_total;
    out->reservations = va_res_count;
    out->committed_live = va_promise_live;
    out->committed_peak = va_promise_peak;
    out->commit_failures = va_commit_failures;
    out->last_fail_size = va_last_fail_size;
    out->backed_live = mem ? guest_slab_backed(mem) : 0;
    out->backed_peak = mem ? guest_slab_backed_peak(mem) : 0;
}
static uint64_t va_promise_live_bytes(void) { return va_promise_live; }
static uint64_t va_backed_live_bytes(void) { return mem ? guest_slab_backed(mem) : 0; }
static uint64_t va_backed_peak_bytes(void) { return mem ? guest_slab_backed_peak(mem) : 0; }

static VaRes *va_res_find(uint32_t a) {
    for (unsigned i = 0; i < va_res_count; ++i)
        if (a >= va_res[i].base && a - va_res[i].base < va_res[i].size) return &va_res[i];
    return NULL;
}
static int va_res_overlaps(uint32_t a, uint32_t n) {
    uint64_t end = (uint64_t)a + n;
    for (unsigned i = 0; i < va_res_count; ++i) {
        uint64_t rb = va_res[i].base, re = rb + va_res[i].size;
        if (rb < end && (uint64_t)a < re) return 1;
    }
    return 0;
}
static int va_host_overlaps(uint32_t a, uint32_t n) {
    uint64_t end = (uint64_t)a + n;
    for (size_t i = 0; i < mem->count; ++i) {
        uint64_t rb = mem->regions[i].base, re = rb + mem->regions[i].size;
        if (rb < end && (uint64_t)a < re) return 1;
    }
    return 0;
}
static int va_res_add(uint32_t base, uint32_t size) {
    if (va_res_count >= VA_MAX_RES) return -1;
    unsigned i = 0;
    while (i < va_res_count && va_res[i].base < base) ++i;
    memmove(&va_res[i + 1], &va_res[i], (va_res_count - i) * sizeof(VaRes));
    va_res[i] = (VaRes){base, size};
    ++va_res_count;
    return 0;
}
static void va_res_remove(unsigned i) {
    memmove(&va_res[i], &va_res[i + 1], (va_res_count - i - 1) * sizeof(VaRes));
    --va_res_count;
}
/* First-fit gap at Windows' 64 KiB allocation granularity. Reservations are
 * kept sorted; host-mapped regions inside the window are also avoided. */
static int va_gap(uint32_t size, uint32_t *out) {
    uint64_t candidate = VA_WINDOW_BASE;
    for (unsigned i = 0; i < va_res_count; ++i) {
        uint64_t rb = va_res[i].base, re = rb + va_res[i].size;
        if (candidate + size <= rb) break;
        if (re > candidate) candidate = (re + VA_GRANULARITY - 1) & ~(uint64_t)(VA_GRANULARITY - 1);
    }
    for (size_t i = 0; i < mem->count; ++i) {
        uint64_t rb = mem->regions[i].base, re = rb + mem->regions[i].size;
        if (re <= VA_WINDOW_BASE || rb >= VA_WINDOW_END) continue;
        if (candidate + size <= rb) continue;
        if (re > candidate) candidate = (re + VA_GRANULARITY - 1) & ~(uint64_t)(VA_GRANULARITY - 1);
    }
    if (candidate + size > VA_WINDOW_END) return -1;
    *out = (uint32_t)candidate;
    return 0;
}
/* Mark [a,a+size) committed. Returns 0 or a Win32 error. */
static uint32_t va_commit_range(uint32_t a, uint32_t size, uint32_t protect, uint64_t *newly) {
    VaRes *res = va_res_find(a);
    if (!res || (uint64_t)a + size > (uint64_t)res->base + res->size)
        return ERR_INVALID_ADDRESS;
    uint64_t add = 0;
    for (uint32_t off = 0; off < size; off += VA_PAGE_SIZE)
        if (guest_slab_state(mem, a + off, NULL) == 0) add += VA_PAGE_SIZE;
    if (guest_slab_commit(mem, a, size, protect)) return ERR_NO_MEMORY;
    if (newly) *newly = add;
    return 0;
}
static uint32_t va_release(uint32_t a) {
    VaRes *res = va_res_find(a);
    if (!res || res->base != a) return ERR_INVALID_ADDRESS;
    uint64_t promise = 0;
    for (uint32_t off = 0; off < res->size; off += VA_PAGE_SIZE)
        if (guest_slab_state(mem, a + off, NULL) == 1) promise += VA_PAGE_SIZE;
    uint32_t size = res->size;
    unsigned i = (unsigned)(res - va_res);
    if (guest_slab_release(mem, a)) return ERR_INVALID_ADDRESS;
    va_promise_live = promise <= va_promise_live ? va_promise_live - promise : 0;
    va_res_live = size <= va_res_live ? va_res_live - size : 0;
    va_res_remove(i);
    if (size >= (1u << 20))
        fprintf(stderr, "[mem] VirtualAlloc release: %lu KiB at %08lx\n",
                (unsigned long)(size >> 10), (unsigned long)a);
    return 0;
}

static void va_shutdown(void) {
    while (va_res_count) {
        --va_res_count;
        guest_slab_release(mem, va_res[va_res_count].base);
    }
    va_res_live = va_res_total = 0;
    va_promise_live = va_promise_peak = 0;
    va_commit_failures = va_last_fail_size = 0;
}
static void va_report(FILE *f) {
    fprintf(f, "[mem] va reserved: %lu KiB live (%u reservations, %lu KiB total requested)\n",
            (unsigned long)(va_res_live >> 10), va_res_count,
            (unsigned long)(va_res_total >> 10));
    fprintf(f, "[mem] va committed: %lu KiB promise live, peak %lu KiB\n",
            (unsigned long)(va_promise_live >> 10), (unsigned long)(va_promise_peak >> 10));
    fprintf(f, "[mem] va backed: %lu KiB live, peak %lu KiB\n",
            (unsigned long)(va_backed_live_bytes() >> 10),
            (unsigned long)(va_backed_peak_bytes() >> 10));
    fprintf(f, "[mem] va commit failures: %lu (last request %lu KiB)\n",
            (unsigned long)va_commit_failures, (unsigned long)(va_last_fail_size >> 10));
}

static void s_VirtualAlloc(CPU *c){
    uint32_t requested=A(0),n=A(1),flags=A(2),prot=A(3);
    if(!n || n>VA_MAX_REQUEST || (flags!=VA_MEM_COMMIT && flags!=VA_MEM_RESERVE && flags!=0x3000) ||
       (prot!=4 && prot!=0x40) || n>UINT32_MAX-(VA_PAGE_SIZE-1)){
        fprintf(stderr,"[mem] VirtualAlloc rejected: size=%lu flags=%08lx prot=%08lx\n",
                (unsigned long)n,(unsigned long)flags,(unsigned long)prot);
        FAIL(ERR_INVALID_PARAMETER,0,4);
    }
    n=(n+VA_PAGE_SIZE-1)&~(VA_PAGE_SIZE-1);
    if(flags==VA_MEM_COMMIT && !requested)flags=0x3000; /* commit without an address: reserve+commit */
    if(flags&VA_MEM_RESERVE){
        /* Allocation granularity applies to reservations only. */
        if(requested){
            if((requested&(VA_GRANULARITY-1)) || requested<VA_WINDOW_BASE ||
               (uint64_t)requested+n>VA_WINDOW_END){FAIL(ERR_INVALID_PARAMETER,0,4);}
            if(va_res_overlaps(requested,n)||va_host_overlaps(requested,n))FAIL(ERR_INVALID_ADDRESS,0,4);
        }else if(va_gap(n,&requested)){
            fprintf(stderr,"[mem] VirtualAlloc: no %lu KiB gap in the VA window\n",(unsigned long)(n>>10));
            FAIL(ERR_NO_MEMORY,0,4);
        }
        if(!guest_slab_reserve(mem,requested,n)){
            fprintf(stderr,"[mem] VirtualAlloc reserve failed: %lu KiB at %08lx (page tables or VA\n"
                           "      window unavailable; guest pool has %lu KiB free)\n",
                    (unsigned long)(n>>10),(unsigned long)requested,
                    (unsigned long)((mem->limit>mem->used?mem->limit-mem->used:0)>>10));
            ++va_commit_failures;va_last_fail_size=n;
            FAIL(ERR_NO_MEMORY,0,4);
        }
        va_res_add(requested,n);
        va_res_live+=n;va_res_total+=n;
        ++memt.va_calls;memt.va_total+=n;
        if(n>=(1u<<20))fprintf(stderr,"[mem] VirtualAlloc reserve: %lu KiB at %08lx\n",
                               (unsigned long)(n>>10),(unsigned long)requested);
        if(flags==VA_MEM_RESERVE){set_error(c,0);RET(requested,4);}
    }else{
        /* MEM_COMMIT inside an existing reservation. Windows rounds the address
         * down and the end up to 4 KiB; the allocation granularity rule does
         * not apply. */
        uint32_t a=requested&~(VA_PAGE_SIZE-1);
        uint64_t end=((uint64_t)requested+n+VA_PAGE_SIZE-1)&~(uint64_t)(VA_PAGE_SIZE-1);
        VaRes *res=va_res_find(a);
        if(!res || end>(uint64_t)res->base+res->size){
            fprintf(stderr,"[mem] commit outside reservation: %lu KiB at %08lx "
                           "res=%s base=%08lx size=%lu KiB\n",
                    (unsigned long)(n>>10),(unsigned long)a,res?"yes":"no",
                    (unsigned long)(res?res->base:0),
                    (unsigned long)(res?(res->size>>10):0));
            FAIL(ERR_INVALID_ADDRESS,0,4);
        }
        uint32_t size=(uint32_t)(end-a);
        uint64_t add=0;
        uint32_t err=va_commit_range(a,size,prot,&add);
        if(err){
            GuestSlab *slab=guest_slab_find(mem,a,size);
            ++va_commit_failures;va_last_fail_size=size;
            fprintf(stderr,"[mem] VirtualAlloc commit failed: %lu KiB at %08lx "
                           "err=%lu slab=%s\n",
                    (unsigned long)(size>>10),(unsigned long)a,
                    (unsigned long)err,slab?"yes":"no");
            FAIL(err,0,4);
        }
        va_promise_live+=add;
        if(va_promise_live>va_promise_peak)va_promise_peak=va_promise_live;
        ++memt.va_calls;memt.va_total+=size;
        if(size>=(1u<<20))fprintf(stderr,"[mem] VirtualAlloc commit: %lu KiB promise at %08lx (backing lazy)\n",
                                   (unsigned long)(size>>10),(unsigned long)a);
        set_error(c,0);RET(a,4);
    }
    if(flags==0x3000){
        uint64_t add=0;
        uint32_t err=va_commit_range(requested,n,prot,&add);
        if(err){
            GuestSlab *slab=guest_slab_find(mem,requested,n);
            ++va_commit_failures;va_last_fail_size=n;
            fprintf(stderr,"[mem] VirtualAlloc reserve+commit failed: %lu KiB at %08lx "
                           "err=%lu slab=%s\n",
                    (unsigned long)(n>>10),(unsigned long)requested,
                    (unsigned long)err,slab?"yes":"no");
            va_release(requested);
            FAIL(err,0,4);
        }
        va_promise_live+=add;
        if(va_promise_live>va_promise_peak)va_promise_peak=va_promise_live;
        ++memt.va_calls;memt.va_total+=n;
        set_error(c,0);RET(requested,4);
    }
}
static void s_VirtualFree(CPU *c){
    uint32_t a=A(0),n=A(1),flags=A(2);
    if(flags==VA_MEM_RELEASE){
        if(n)FAIL(ERR_INVALID_PARAMETER,0,3);
        uint32_t err=va_release(a);
        if(err)FAIL(err,0,3);
        set_error(c,0);RET(1,3);
    }
    if(flags==VA_MEM_DECOMMIT){
        if(!n || (a&(VA_PAGE_SIZE-1)) || n>UINT32_MAX-(VA_PAGE_SIZE-1))
            FAIL(ERR_INVALID_PARAMETER,0,3);
        n=(n+VA_PAGE_SIZE-1)&~(VA_PAGE_SIZE-1);
        VaRes *res=va_res_find(a);
        if(!res || (uint64_t)a+n>(uint64_t)res->base+res->size)FAIL(ERR_INVALID_ADDRESS,0,3);
        uint64_t dropped=0;
        for(uint32_t off=0;off<n;off+=VA_PAGE_SIZE)
            if(guest_slab_state(mem,a+off,NULL)==1)dropped+=VA_PAGE_SIZE;
        if(guest_slab_decommit(mem,a,n))FAIL(ERR_INVALID_ADDRESS,0,3);
        va_promise_live=dropped<=va_promise_live?va_promise_live-dropped:0;
        set_error(c,0);RET(1,3);
    }
    FAIL(ERR_INVALID_PARAMETER,0,3);
}
static void s_GetTickCount(CPU *c){uint64_t t=tick()-start_tick;RET((t/freq())*1000+(t%freq())*1000/freq(),0);}
static void s_QueryPerformanceCounter(CPU *c){uint64_t v=tick();if(!guest_ptr(mem,A(0),8))FAIL(ERR_INVALID_PARAMETER,0,1);M64(A(0))=v;RET(1,1);}
static void s_QueryPerformanceFrequency(CPU *c){if(!guest_ptr(mem,A(0),8))FAIL(ERR_INVALID_PARAMETER,0,1);M64(A(0))=freq();RET(1,1);}
static void s_GetCurrentProcessId(CPU *c){RET(1,0);}
static void s_GetCurrentThreadId(CPU *c){RET(win32_thread_current_id(),0);}
static void s_GetCurrentProcess(CPU *c){RET(UINT32_MAX,0);}
static void s_GetCurrentThread(CPU *c){RET(UINT32_MAX-1,0);}
static uint32_t tls_off(uint32_t i){return i<64?0xe10+4*i:0x1800+4*(i-64);}
static void tls_set(CPU *c,uint32_t i,uint32_t v){tls_values[i]=v;if(c->fs_base)guest_write32(mem,c->fs_base+tls_off(i),v);}
static void s_TlsAlloc(CPU *c){for(unsigned i=0;i<TLS_SLOTS;++i)if(!tls_used[i]){tls_used[i]=1;tls_set(c,i,0);RET(i,0);}FAIL(ERR_NO_MEMORY,UINT32_MAX,0);}
static void s_TlsFree(CPU *c){uint32_t i=A(0);if(i>=TLS_SLOTS||!tls_used[i])FAIL(ERR_INVALID_PARAMETER,0,1);tls_used[i]=0;tls_set(c,i,0);RET(1,1);}
static void s_TlsSetValue(CPU *c){uint32_t i=A(0);if(i>=TLS_SLOTS||!tls_used[i])FAIL(ERR_INVALID_PARAMETER,0,2);tls_set(c,i,A(1));RET(1,2);}
static void s_TlsGetValue(CPU *c){uint32_t i=A(0);if(i>=TLS_SLOTS||!tls_used[i])FAIL(ERR_INVALID_PARAMETER,0,1);set_error(c,0);RET(teb_value(c,tls_off(i),tls_values[i]),1);}
/* Fiber-local storage: single expected guest thread, independent slot space. */
#define FLS_SLOTS 128
static uint32_t fls_values[FLS_SLOTS];
static uint8_t fls_used[FLS_SLOTS];
static void s_FlsAlloc(CPU *c){
    for(unsigned i=0;i<FLS_SLOTS;++i)if(!fls_used[i]){fls_used[i]=1;fls_values[i]=0;RET(i,1);}
    FAIL(ERR_NO_MEMORY,UINT32_MAX,1);
}
static void s_FlsFree(CPU *c){uint32_t i=A(0);if(i>=FLS_SLOTS||!fls_used[i])FAIL(ERR_INVALID_PARAMETER,0,1);fls_used[i]=0;fls_values[i]=0;RET(1,1);}
static void s_FlsSetValue(CPU *c){uint32_t i=A(0);if(i>=FLS_SLOTS||!fls_used[i])FAIL(ERR_INVALID_PARAMETER,0,2);fls_values[i]=A(1);RET(1,2);}
static void s_FlsGetValue(CPU *c){uint32_t i=A(0);if(i>=FLS_SLOTS||!fls_used[i])FAIL(ERR_INVALID_PARAMETER,0,1);set_error(c,0);RET(fls_values[i],1);}
/* The emulated process has one logical CPU (PEB NumberOfProcessors == 1).
 * Report one RelationProcessorCore record, matching GetSystemInfo rather than
 * inventing host topology. */
static void s_GetLogicalProcessorInformation(CPU *c){
    uint32_t out=A(0),lenp=A(1);
    if(!lenp || !guest_ptr(mem,lenp,4))FAIL(ERR_INVALID_PARAMETER,0,2);
    uint32_t cap=M32(lenp);
    if(!out || cap<24){M32(lenp)=24;FAIL(ERR_INSUFFICIENT_BUFFER,0,2);}
    uint8_t *b=checked(c,out,24);memset(b,0,24);
    M32(out)=1;                 /* ProcessorMask */
    M32(out+4)=0;               /* Relationship = RelationProcessorCore */
    M32(out+8)=1;               /* ProcessorCore.Flags: one SMT thread */
    M32(lenp)=24;set_error(c,0);RET(1,2);
}
static void s_GetSystemInfo(CPU *c){
    uint32_t p=A(0);memset(checked(c,p,36),0,36);
    /* Guest-facing 32-bit x86 SYSTEM_INFO, not ARM hardware details. */
    M16(p)=0;M32(p+4)=4096;M32(p+8)=0x10000;M32(p+12)=0x7ffeffff;
    M32(p+16)=1;M32(p+20)=1;M32(p+24)=586;M32(p+28)=65536;M16(p+32)=5;
    RET(0,1);
}
static void s_GetSystemTimeAsFileTime(CPU *c){
    uint32_t p=A(0);checked(c,p,8);time_t t=time(NULL);
    uint64_t v=t==(time_t)-1?0:((uint64_t)t+UINT64_C(11644473600))*UINT64_C(10000000);
    M64(p)=v;RET(0,1);
}
/* Thread lifecycle lives in win32_thread.c (cooperative one-shot model). */
/* Sleep is a scheduling point: other guest threads run while this one waits.
 * An infinite Sleep can only be ended by an APC, which this port does not
 * implement, so it stays an explicit fault. */
static void s_Sleep(CPU *c){
    uint32_t ms=A(0);
    if(ms==UINT32_MAX)rt_unimpl(c,0,"infinite Sleep cannot be ended (no APCs)");
    if(ms>=1u){static unsigned logged;if(logged<20u){++logged;uint32_t ret=M32(c->r[4]);if(ret==0x00401755u){static unsigned spin_logged;uint32_t lock=c->r[6];fprintf(stderr,"[lock] spin sleep lock=%08x owner=%08x rec=%08x thread=%u\n",lock,lock?M32(lock):0u,lock?M32(lock+4):0u,win32_thread_current_id());if(!spin_logged&&lock){spin_logged=1;
#ifdef _WIN32
{void *frames[20];unsigned short n=(unsigned short)RtlCaptureStackBackTrace(0,20,frames,NULL);void *mod=GetModuleHandleA(NULL);fprintf(stderr,"[lock] bt=%u",(unsigned)n);for(unsigned k=0;k<n;++k)fprintf(stderr," %llx",(unsigned long long)((uintptr_t)frames[k]-(uintptr_t)mod));fprintf(stderr,"\n");}
#else
fprintf(stderr,"[lock] bt=unavailable on this host\n");
#endif
fprintf(stderr,"[lock] lock[-8..+24]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",M32(lock-8),M32(lock-4),M32(lock),M32(lock+4),M32(lock+8),M32(lock+12),M32(lock+16),M32(lock+20),M32(lock+24),M32(lock+28));uint32_t o=M32(lock);if(o){fprintf(stderr,"[lock] owner[-8..+24]=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",M32(o-8),M32(o-4),M32(o),M32(o+4),M32(o+8),M32(o+12),M32(o+16),M32(o+20),M32(o+24),M32(o+28));}}}else fprintf(stderr,"[sync] Sleep %lu ms from %08x thread=%u\n",(unsigned long)ms,ret,win32_thread_current_id());}}
    win32_thread_sleep(ms);
    RET(0,1);
}
static void s_InterlockedIncrement(CPU *c){uint32_t p=A(0);M32(p)+=1;guest_watch_check(mem,"InterlockedIncrement");RET(M32(p),1);}
static void s_InterlockedDecrement(CPU *c){uint32_t p=A(0);M32(p)-=1;guest_watch_check(mem,"InterlockedDecrement");RET(M32(p),1);}
#define ICX_RING 8
static struct {uint32_t p,expect,repl,thread;} icx_ring[ICX_RING];
static unsigned icx_n;
void win32_icx_dump(FILE *f){
    if(!f)return;
    unsigned n=icx_n<ICX_RING?icx_n:ICX_RING;
    fprintf(f,"[hang] interlocked ring (oldest first):");
    for(unsigned k=0;k<n;++k){
        unsigned slot=(icx_n-n+k)%ICX_RING;
        fprintf(f," t%lu@%08x exp=%08x set=%08x now=%08x",
                (unsigned long)icx_ring[slot].thread,(unsigned)icx_ring[slot].p,
                (unsigned)icx_ring[slot].expect,(unsigned)icx_ring[slot].repl,
                (unsigned)M32(icx_ring[slot].p));
    }
    fputc('\n',f);
}
static void s_InterlockedExchange(CPU *c){uint32_t p=A(0),v=M32(p);unsigned s=icx_n%ICX_RING;icx_ring[s].p=p;icx_ring[s].expect=v;icx_ring[s].repl=A(1);icx_ring[s].thread=win32_thread_current_id();++icx_n;M32(p)=A(1);guest_watch_check(mem,"InterlockedExchange");RET(v,2);}
static void s_InterlockedCompareExchange(CPU *c){uint32_t p=A(0),v=M32(p);icx_ring[icx_n%ICX_RING].p=p;icx_ring[icx_n%ICX_RING].expect=v;icx_ring[icx_n%ICX_RING].repl=A(1);icx_ring[icx_n%ICX_RING].thread=win32_thread_current_id();++icx_n;if(v==A(2))M32(p)=A(1);guest_watch_check(mem,"InterlockedCompareExchange");RET(v,3);}
static void s_lstrlenA(CPU *c){RET(A(0)?strlen(str(c,A(0))):0,1);}
static void s_lstrcmpiA(CPU *c){RET(icmp(str(c,A(0)),str(c,A(1))),2);}
static void s_OutputDebugStringA(CPU *c){fprintf(stderr,"[guest] %s\n",str(c,A(0)));RET(0,1);}
static uint32_t last_exit_code, last_exit_from;
uint32_t win32_last_exit_code(void){return last_exit_code;}
uint32_t win32_last_exit_from(void){return last_exit_from;}
static void s_ExitProcess(CPU *c){
    uint32_t code=A(0),ret=M32(c->r[4]);
    last_exit_code=code;last_exit_from=ret;
    fprintf(stderr,"[guest] ExitProcess(%lu) from %08lx stack:",(unsigned long)code,(unsigned long)ret);
    for(unsigned i=0;i<8;++i){uint32_t v=0;if(!guest_read32(mem,c->r[4]+4*i,&v))fprintf(stderr," %08lx",(unsigned long)v);}
    fputc('\n',stderr);
    n3ds_lift_exit(code);
}
static void s_GetCommandLineA(CPU *c){RET(guest_ptr(mem,GP_COMMAND_A,1)?GP_COMMAND_A:0,0);}
static void s_GetCommandLineW(CPU *c){RET(guest_ptr(mem,GP_COMMAND_W,2)?GP_COMMAND_W:0,0);}
static void s_GetEnvironmentStringsA(CPU *c){RET(guest_ptr(mem,GP_ENV_A,1)?GP_ENV_A:0,0);}
static void s_GetEnvironmentStringsW(CPU *c){RET(guest_ptr(mem,GP_ENV_W,2)?GP_ENV_W:0,0);}
static void s_FreeEnvironmentStringsA(CPU *c){if(A(0)!=GP_ENV_A)FAIL(ERR_INVALID_PARAMETER,0,1);RET(1,1);}
static void s_FreeEnvironmentStringsW(CPU *c){if(A(0)!=GP_ENV_W)FAIL(ERR_INVALID_PARAMETER,0,1);RET(1,1);}
static void s_GetStartupInfoA(CPU *c){uint32_t p=A(0);memset(checked(c,p,68),0,68);M32(p)=68;RET(0,1);}
/* Critical sections are real per-thread ownership records with the standard
 * 24-byte guest layout: +4 owner thread id (0xffffffff = free), +8 recursion,
 * +12 "held" flag, +20 spin count. A section held by another guest thread
 * cannot be waited for in the one-shot thread model, so the attempt faults
 * instead of silently entering. */
#define CS_BYTES 24
#define CS_MAX_TRACKED 64
static uint32_t cs_tracked[CS_MAX_TRACKED];
static unsigned cs_tracked_n;
static void cs_track_add(uint32_t p){
    for(unsigned i=0;i<cs_tracked_n;++i)if(cs_tracked[i]==p)return;
    if(cs_tracked_n<CS_MAX_TRACKED)cs_tracked[cs_tracked_n++]=p;
}
static void cs_track_del(uint32_t p){
    for(unsigned i=0;i<cs_tracked_n;++i)if(cs_tracked[i]==p){cs_tracked[i]=cs_tracked[--cs_tracked_n];return;}
}
/* Hang-watchdog diagnostic: tracked critical sections and their owners. */
void win32_cs_dump(FILE *f){
    if(!f)return;
    fprintf(f,"[hang] critical sections tracked=%u\n",cs_tracked_n);
    for(unsigned i=0;i<cs_tracked_n;++i){
        uint32_t p=cs_tracked[i],owner=0,rec=0,held=0;
        if(!guest_read32(mem,p+4,&owner)){}
        if(!guest_read32(mem,p+8,&rec)){}
        if(!guest_read32(mem,p+12,&held)){}
        fprintf(f,"[hang] cs %08x owner=%08x rec=%lu held=%lu\n",p,owner,
                (unsigned long)rec,(unsigned long)held);
    }
}
static void cs_init(CPU *c,uint32_t p,uint32_t spin){
    if(spin>0x00ffffffu)spin=0x00ffffffu;
    memset(checked(c,p,CS_BYTES),0,CS_BYTES);M32(p+4)=0xffffffffu;M32(p+20)=spin;
    cs_track_add(p);
}
static void s_InitializeCriticalSection(CPU *c){cs_init(c,A(0),0);RET(0,1);}
static void s_InitializeCriticalSectionAndSpinCount(CPU *c){cs_init(c,A(0),A(1));RET(1,2);}
static void s_DeleteCriticalSection(CPU *c){uint32_t p=A(0);memset(checked(c,p,CS_BYTES),0,CS_BYTES);M32(p+4)=0xffffffffu;cs_track_del(p);RET(0,1);}
static void s_EnterCriticalSection(CPU *c){
    uint32_t p=A(0),me=win32_sync_current_thread();checked(c,p,CS_BYTES);
    uint32_t owner=M32(p+4);
    if(owner!=0xffffffffu && owner!=me)
        rt_unimpl(c,p,"EnterCriticalSection held by another guest thread (no cooperative wait)");
    if(owner==0xffffffffu){M32(p+4)=me;M32(p+8)=1;}
    else M32(p+8)+=1;
    M32(p+12)=1;RET(0,1);
}
static void s_LeaveCriticalSection(CPU *c){
    uint32_t p=A(0),me=win32_sync_current_thread();checked(c,p,CS_BYTES);
    uint32_t rec=M32(p+8);
    if(!rec){
        /* Windows does not validate ownership; TESV double-releases a stream
         * lock while reading Plugins.txt. Emulate the release as a logged
         * no-op, never as fabricated ownership. */
        static uint32_t unheld[8];
        static unsigned unheld_n;
        unsigned i;
        for(i=0;i<unheld_n;++i)if(unheld[i]==p)break;
        if(i==unheld_n){
            if(unheld_n<sizeof unheld/sizeof unheld[0])unheld[unheld_n++]=p;
            fprintf(stderr,"[cs] leave on unheld section %08x (Windows-compatible no-op)\n",p);
        }
        RET(0,1);
    }
    if(M32(p+4)!=me)rt_unimpl(c,p,"LeaveCriticalSection from a non-owning guest thread");
    if(rec==1){M32(p+4)=0xffffffffu;M32(p+8)=0;M32(p+12)=0;}
    else M32(p+8)=rec-1;
    RET(0,1);
}
static void s_GetCurrentDirectoryA(CPU *c){
    uint32_t n=A(0),buf=A(1);size_t len=strlen(cwd)+1;
    if(!n){RET(len,2);}
    if(!buf)FAIL(ERR_INVALID_PARAMETER,0,2);
    if(n<len){RET(len,2);}
    memcpy(checked(c,buf,(uint32_t)len),cwd,len);RET(len-1,2);
}
static void s_SetCurrentDirectoryA(CPU *c){
    const char *path=str(c,A(0));
    if(!*path)FAIL(ERR_INVALID_PARAMETER,0,1);
    if(path[0]=='/'||path[0]=='\\'||(isalpha((unsigned char)path[0])&&path[1]==':'))FAIL(ERR_INVALID_PARAMETER,0,1);
    if(!strcmp(path,"."))RET(1,1);
    if(strlen(path)>=sizeof cwd)FAIL(ERR_INVALID_PARAMETER,0,1);
    const char *p=path;
    while(*p){
        const char *s=p;while(*p&&*p!='/'&&*p!='\\')++p;
        size_t n=(size_t)(p-s);
        if(!n||(n==1&&*s=='.')||(n==2&&s[0]=='.'&&s[1]=='.'))FAIL(ERR_INVALID_PARAMETER,0,1);
        if(*p)++p;
    }
    size_t rl=strlen(cwd);if(rl+1+strlen(path)>=sizeof cwd)FAIL(ERR_INVALID_PARAMETER,0,1);
    if(cwd[rl-1]!='/'&&cwd[rl-1]!='\\')cwd[rl++]='/';
    memcpy(cwd+rl,path,strlen(path)+1);
    RET(1,1);
}
/* Code pages: CP_ACP (1252) is fully mapped, UTF-8 is supported. CP_OEMCP is
 * accepted for queries but its non-ASCII bytes are not converted (returns 0
 * with ERROR_INVALID_PARAMETER) instead of silently guessing. */
#define ACP 1252u
#define OEMCP 437u
#define UTF8CP 65001u
#define ERR_INSUFFICIENT_BUFFER 122u
#define ERR_NO_UNICODE_TRANSLATION 1113u
static uint32_t acp_to_uni(uint8_t b){
    static const uint16_t high[32]={0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,0x0090,0x2018,0x2019,0x201c,
        0x201d,0x2022,0x2013,0x2014,0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178};
    return b>=0x80&&b<=0x9f?high[b-0x80]:b;
}
static uint8_t uni_to_acp(uint32_t u){
    static const uint16_t high[32]={0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,0x0090,0x2018,0x2019,0x201c,
        0x201d,0x2022,0x2013,0x2014,0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178};
    if(u<0x80||(u>=0xa0&&u<=0xff))return (uint8_t)u;
    for(unsigned i=0;i<32;++i)if(high[i]==u)return (uint8_t)(0x80+i);
    return '?';
}
static int utf8_dec(const uint8_t *s,size_t n,size_t *used,uint32_t *cp){
    if(!n)return 0;
    uint8_t b=s[0];int extra;uint32_t v,min;
    if(b<0x80){*cp=b;*used=1;return 1;}
    if((b&0xe0)==0xc0){extra=1;v=b&0x1f;min=0x80;}
    else if((b&0xf0)==0xe0){extra=2;v=b&0x0f;min=0x800;}
    else if((b&0xf8)==0xf0){extra=3;v=b&0x07;min=0x10000;}
    else return 0;
    if(n<(size_t)extra+1)return 0;
    for(int i=1;i<=extra;++i){if((s[i]&0xc0)!=0x80)return 0;v=(v<<6)|(s[i]&0x3f);}
    if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
    *cp=v;*used=(size_t)extra+1;return 1;
}
static int mb_len(uint32_t cp,int utf8){
    if(!utf8)return 1;
    return cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
}
static void s_GetACP(CPU *c){RET(ACP,0);}
static void s_GetOEMCP(CPU *c){RET(OEMCP,0);}
static void s_IsValidCodePage(CPU *c){uint32_t cp=A(0);RET(cp==ACP||cp==OEMCP||cp==UTF8CP||cp==20127u,1);}
static void s_GetCPInfo(CPU *c){
    uint32_t cp=A(0),p=A(1);uint8_t *info=checked(c,p,18);
    if(cp!=ACP&&cp!=OEMCP&&cp!=UTF8CP&&cp!=20127u)FAIL(ERR_INVALID_PARAMETER,0,2);
    memset(info,0,18);M32(p)=cp==UTF8CP?4:1;info[4]='?';RET(1,2);
}
static uint16_t ctype1_w(uint32_t u){
    uint16_t t=0;
    if(u<0x80){
        if(u>='A'&&u<='Z')t|=0x0001;
        else if(u>='a'&&u<='z')t|=0x0002;
        else if(u>='0'&&u<='9')t|=0x0004;
        if(u==' '||u=='\t')t|=0x0040;
        if(u==' '||u=='\t'||u=='\n'||u=='\r'||u=='\f'||u=='\v')t|=0x0008;
        if(u<0x20||u==0x7f)t|=0x0020;
        if(u>0x20&&u<0x7f&&strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",(int)u))t|=0x0010;
        if((t&0x0001)||(t&0x0002))t|=0x0100;
        if(u>='0'&&u<='9')t|=0x0080;
        else if(u>='A'&&u<='F')t|=0x0080;
        else if(u>='a'&&u<='f')t|=0x0080;
    }else if(u>=0xc0&&u<=0xff&&u!=0xd7&&u!=0xf7){
        t|=0x0100|(u<=0xde?0x0001:0x0002);
    }
    return t;
}
static int lcid_ok(uint32_t l){return l==0x0409u||l==0x0400u||l==0x0800u||l==0;}
static uint16_t lc_map_w(uint16_t u,int upper){
    if(upper){
        if(u>='a'&&u<='z')return (uint16_t)(u-32);
        if(u>=0xe0&&u<=0xfe&&u!=0xf7)return (uint16_t)(u-32);
    }else{
        if(u>='A'&&u<='Z')return (uint16_t)(u+32);
        if(u>=0xc0&&u<=0xde&&u!=0xd7)return (uint16_t)(u+32);
    }
    return u;
}
static void s_LCMapStringW(CPU *c){
    uint32_t l=A(0),fl=A(1),src=A(2);int32_t cs=(int32_t)A(3),dst=A(4),cd=(int32_t)A(5);
    if(!lcid_ok(l)||(fl!=0x0100u&&fl!=0x0200u))FAIL(ERR_INVALID_PARAMETER,0,6);
    size_t n;
    if(cs<0){n=0;while(n<0x100000&&M16(src+(uint32_t)(n*2)))++n;++n;}
    else{if(cs==0)FAIL(ERR_INVALID_PARAMETER,0,6);checked(c,src,(uint32_t)cs*2);n=(size_t)cs;}
    if(!cd)RET(n,6);
    if(cd<0||(size_t)cd<n)FAIL(ERR_INSUFFICIENT_BUFFER,0,6);
    checked(c,dst,(uint32_t)(n*2));
    for(size_t i=0;i<n;++i)M16((uint32_t)(dst+i*2))=lc_map_w(M16(src+(uint32_t)(i*2)),fl==0x0200u);
    RET(n,6);
}
static void s_LCMapStringA(CPU *c){
    uint32_t l=A(0),fl=A(1),src=A(2);int32_t cs=(int32_t)A(3),dst=A(4),cd=(int32_t)A(5);
    if(!lcid_ok(l)||(fl!=0x0100u&&fl!=0x0200u))FAIL(ERR_INVALID_PARAMETER,0,6);
    size_t n;
    if(cs<0){const char *s=guest_cstr(mem,src,0x100000);if(!s)rt_unimpl(c,src,"invalid string");n=strlen(s)+1;}
    else{if(cs==0)FAIL(ERR_INVALID_PARAMETER,0,6);checked(c,src,(uint32_t)cs);n=(size_t)cs;}
    if(!cd)RET(n,6);
    if(cd<0||(size_t)cd<n)FAIL(ERR_INSUFFICIENT_BUFFER,0,6);
    uint8_t *d=checked(c,dst,(uint32_t)n);
    for(size_t i=0;i<n;++i)d[i]=uni_to_acp(lc_map_w(acp_to_uni(M8(src+(uint32_t)i)),fl==0x0200u));
    RET(n,6);
}
static void s_GetStringTypeW(CPU *c){
    uint32_t type=A(0),src=A(1);int32_t cch=(int32_t)A(2),out=A(3);
    if(type!=1)FAIL(ERR_INVALID_PARAMETER,0,4);
    size_t n;
    if(cch<0){n=0;while(n<0x100000&&M16(src+(uint32_t)(n*2)))++n;++n;}
    else{if(cch==0)RET(1,4);checked(c,src,(uint32_t)cch*2);n=(size_t)cch;}
    uint8_t *d=checked(c,out,(uint32_t)(n*2));
    for(size_t i=0;i<n;++i)M16((uint32_t)(out+i*2))=ctype1_w(M16(src+(uint32_t)(i*2)));
    (void)d;RET(1,4);
}
static void s_GetStringTypeA(CPU *c){
    uint32_t l=A(0),type=A(1),src=A(2);int32_t cch=(int32_t)A(3),out=A(4);
    if(!lcid_ok(l)||type!=1)FAIL(ERR_INVALID_PARAMETER,0,5);
    size_t n;
    if(cch<0){const char *s=guest_cstr(mem,src,0x100000);if(!s)rt_unimpl(c,src,"invalid string");n=strlen(s)+1;}
    else{if(cch==0)RET(1,5);checked(c,src,(uint32_t)cch);n=(size_t)cch;}
    checked(c,out,(uint32_t)(n*2));
    for(size_t i=0;i<n;++i)M16((uint32_t)(out+i*2))=ctype1_w(acp_to_uni(M8(src+(uint32_t)i)));
    RET(1,5);
}
static void s_MultiByteToWideChar(CPU *c){
    uint32_t cp=A(0),flags=A(1),src=A(2);int32_t cb=(int32_t)A(3);
    if(cp==0)cp=ACP; /* CP_ACP */
    uint32_t dst=A(4);int32_t cch=(int32_t)A(5);
    if(cp!=ACP&&cp!=OEMCP&&cp!=UTF8CP&&cp!=20127u)FAIL(ERR_INVALID_PARAMETER,0,6);
    if(cp==OEMCP)FAIL(ERR_INVALID_PARAMETER,0,6);
    size_t n;const uint8_t *s;
    if(cb<0){const char *z=guest_cstr(mem,src,0x100000);if(!z)rt_unimpl(c,src,"invalid string");s=(const uint8_t*)z;n=strlen(z)+1;}
    else{s=checked(c,src,(uint32_t)cb);n=(size_t)cb;}
    int utf8=(cp==UTF8CP||cp==20127u);
    size_t i=0,need=0;
    while(i<n){
        if(!utf8){++need;++i;continue;}
        size_t used;uint32_t v;
        if(!utf8_dec(s+i,n-i,&used,&v)){if(flags&0x8)FAIL(ERR_NO_UNICODE_TRANSLATION,0,6);v=0xfffd;used=1;}
        (void)v;++need;i+=used;
    }
    if(!cch){RET(need,6);}
    if((uint32_t)cch<need)FAIL(ERR_INSUFFICIENT_BUFFER,0,6);
    (void)checked(c,dst,(uint32_t)(need*2));i=0;size_t k=0;
    while(i<n&&k<need){
        uint32_t v;size_t used;
        if(!utf8){v=acp_to_uni(s[i]);used=1;}
        else if(!utf8_dec(s+i,n-i,&used,&v)){v=0xfffd;used=1;}
        M16(dst+(uint32_t)(k*2))=(uint16_t)v;
        i+=used;++k;
    }
    RET(need,6);
}
static void s_WideCharToMultiByte(CPU *c){
    uint32_t cp=A(0),flags=A(1),src=A(2);int32_t cch=(int32_t)A(3);
    if(cp==0)cp=ACP; /* CP_ACP */
    uint32_t dst=A(4);int32_t cb=(int32_t)A(5),defc=A(6),useddef=A(7);
    (void)flags;
    if(cp!=ACP&&cp!=OEMCP&&cp!=UTF8CP&&cp!=20127u)FAIL(ERR_INVALID_PARAMETER,0,8);
    if(cp==OEMCP)FAIL(ERR_INVALID_PARAMETER,0,8);
    size_t n;
    if(cch<0){n=0;while(n<0x100000){uint32_t ch=M16(src+(uint32_t)(n*2));++n;if(!ch)break;}}
    else{checked(c,src,(uint32_t)cch*2);n=(size_t)cch;}
    int utf8=(cp==UTF8CP||cp==20127u);int defused=0;
    size_t need=0;
    for(size_t i=0;i<n;++i){
        uint32_t u=M16(src+(uint32_t)(i*2));size_t step;
        if(!utf8){uint8_t b=uni_to_acp(u);if(b=='?'&&u!='?')defused=1;step=1;}
        else{if(u>=0xd800&&u<=0xdfff){u=0xfffd;defused=1;}step=(size_t)mb_len(u,1);}
        (void)defc;need+=step;
    }
    if(!cb){if(useddef)M32(useddef)=defused;RET(need,8);}
    if((uint32_t)cb<need)FAIL(ERR_INSUFFICIENT_BUFFER,0,8);
    uint8_t *d=(uint8_t*)checked(c,dst,(uint32_t)need);
    size_t k=0;
    for(size_t i=0;i<n;++i){
        uint32_t u=M16(src+(uint32_t)(i*2));
        if(!utf8){uint8_t b=uni_to_acp(u);if(b=='?'&&u!='?'){uint8_t alt=defc?M8(defc):'?';b=alt;defused=1;}d[k++]=b;}
        else{
            if(u>=0xd800&&u<=0xdfff){u=0xfffd;defused=1;}
            if(u<0x80)d[k++]=(uint8_t)u;
            else if(u<0x800){d[k++]=(uint8_t)(0xc0|(u>>6));d[k++]=(uint8_t)(0x80|(u&0x3f));}
            else if(u<0x10000){d[k++]=(uint8_t)(0xe0|(u>>12));d[k++]=(uint8_t)(0x80|((u>>6)&0x3f));d[k++]=(uint8_t)(0x80|(u&0x3f));}
            else{d[k++]=(uint8_t)(0xf0|(u>>18));d[k++]=(uint8_t)(0x80|((u>>12)&0x3f));d[k++]=(uint8_t)(0x80|((u>>6)&0x3f));d[k++]=(uint8_t)(0x80|(u&0x3f));}
        }
    }
    if(useddef)M32(useddef)=defused;
    RET(need,8);
}
#define STD_IN 0xfffffff6u
#define STD_OUT 0xfffffff5u
#define STD_ERR 0xfffffff4u
static uint32_t std_handles[3]={STD_IN,STD_OUT,STD_ERR};
static void s_GetStdHandle(CPU *c){
    uint32_t n=A(0);
    if(n==(uint32_t)-10)RET(std_handles[0],1);
    if(n==(uint32_t)-11)RET(std_handles[1],1);
    if(n==(uint32_t)-12)RET(std_handles[2],1);
    FAIL(ERR_INVALID_HANDLE,UINT32_MAX,1);
}
static void s_SetStdHandle(CPU *c){
    uint32_t n=A(0),h=A(1);
    if(n==(uint32_t)-10)std_handles[0]=h;else if(n==(uint32_t)-11)std_handles[1]=h;
    else if(n==(uint32_t)-12)std_handles[2]=h;else FAIL(ERR_INVALID_HANDLE,0,2);
    RET(1,2);
}
static void s_GetFileType(CPU *c){
    uint32_t h=A(0);
    if(h==STD_IN||h==STD_OUT||h==STD_ERR)RET(2,1); /* FILE_TYPE_CHAR */
    if(file_for(h))RET(1,1);                     /* FILE_TYPE_DISK */
    RET(0,1);                                     /* FILE_TYPE_UNKNOWN */
}
static void s_IsDebuggerPresent(CPU *c){RET(0,0);}
static void s_IsProcessorFeaturePresent(CPU *c){
    /* Conservative: claim no optional CPU features so the static CRT selects
     * scalar paths covered by the lifter. Reporting an unsupported feature
     * would execute instructions the runtime does not implement. */
    (void)A(0);RET(0,1);
}
/* ---- executable TLS callback bootstrap -------------------------------------------
 * The PE TLS callback array starts with NULL, so Windows runs no callbacks.
 * The CRT still needs its dynamic-TLS/thread-data initializer before C++
 * constructors; invoke only 0xf794ab at the last C initializer. The next
 * pointer, 0xf5b303, is a CRT terminator, not a TLS callback: calling it frees
 * the live CRT lock table and causes a use-after-free when that block is
 * reused by the engine heap. */
static void guest_tls_bootstrap(CPU *caller){
    if(guest_boot_done)return;
    guest_boot_done=1;
    if(!guest_ptr(mem,0x1b4a998u,4))return; /* only the TESV image */
    unsigned available=0;
    for(unsigned i=0;i<TESV_TLS_BOOTSTRAP_COUNT;++i)
        if(n3ds_lift_has_function(tesv_tls_bootstrap_targets[i]))++available;
    if(!available)return;
    if(!guest_boot_stack){
        if(guest_map(mem,GUEST_BOOT_STACK,GUEST_BOOT_STACK_BYTES))return;
        guest_boot_stack=GUEST_BOOT_STACK;
    }
    CPU cpu;
    n3ds_lift_init_cpu(&cpu,guest_boot_stack+GUEST_BOOT_STACK_BYTES-64,
                       caller&&caller->fs_base?caller->fs_base:GP_TEB);
    static const uint32_t tls_args[3]={0x00400000u,1u,0u};
    uint32_t res=0;
    int rc=0;
    for(unsigned i=0;i<TESV_TLS_BOOTSTRAP_COUNT && !rc;++i)
        if(n3ds_lift_has_function(tesv_tls_bootstrap_targets[i]))
            rc=n3ds_lift_invoke_reentrant(&cpu,tesv_tls_bootstrap_targets[i],tls_args,3,&res);
    if(rc)rt_unimpl(caller,0,rc==N3DS_INVOKE_EXIT?"executable TLS initializer called ExitProcess"
                                                     :"executable TLS initializer faulted");
    fprintf(stderr,"[win32] ran executable TLS initializer (CRT thread data)\n");
}

static uint32_t exception_filter;
static void s_SetUnhandledExceptionFilter(CPU *c){
    guest_tls_bootstrap(c);
    uint32_t old=exception_filter;exception_filter=A(0);RET(old,1);
}
static void s_UnhandledExceptionFilter(CPU *c){
    if(exception_filter)
        fprintf(stderr,"[guest] UnhandledExceptionFilter: a guest filter is installed but SEH "
                        "dispatch is not implemented; returning EXCEPTION_CONTINUE_SEARCH\n");
    set_error(c,0);RET(0,1); /* EXCEPTION_CONTINUE_SEARCH */
}
static void s_TerminateProcess(CPU *c){
    uint32_t h=A(0),code=A(1);
    if(h!=UINT32_MAX)FAIL(ERR_INVALID_HANDLE,0,2);
    fprintf(stderr,"[guest] TerminateProcess(%lu) from %08lx\n",(unsigned long)code,(unsigned long)M32(c->r[4]));
    n3ds_lift_exit(code);
}
static void s_DebugBreak(CPU *c){rt_unimpl(c,0,"DebugBreak");}
static void s_SetHandleCount(CPU *c){uint32_t n=A(0);RET(n>255u?255u:n,1);}
static unsigned com_refs;
static void s_CoInitializeEx(CPU *c){
    uint32_t reserved=A(0),flags=A(1);
    if(reserved)FAIL(ERR_INVALID_PARAMETER,0x80070057u,2);
    if(flags&~0x1Eu)FAIL(ERR_INVALID_PARAMETER,0x80070057u,2);
    unsigned old=com_refs;if(com_refs<0xffffffffu)++com_refs;
    RET(old?1u:0u,2); /* S_FALSE on repeat, S_OK on first init */
}
static void s_CoInitialize(CPU *c){uint32_t reserved=A(0);if(reserved)FAIL(ERR_INVALID_PARAMETER,0x80070057u,1);unsigned old=com_refs;if(com_refs<0xffffffffu)++com_refs;RET(old?1u:0u,1);}
static void s_CoUninitialize(CPU *c){if(com_refs)--com_refs;RET(0,0);}
static void s_CoCreateInstance(CPU *c){set_error(c,0);RET(0x80040154u,5); /* REGDB_E_CLASSNOTREG */ }
static void s_GetActiveWindow(CPU *c){RET(0,0);}
static void s_DispatchMessageA(CPU *c){RET(0,1); /* minimal message loop: nothing queued */ }
static void s_GetUserNameA(CPU *c){
    uint32_t buf=A(0),pcb=A(1);if(!pcb)FAIL(ERR_INVALID_PARAMETER,0,2);
    uint32_t cap=M32(pcb);static const char name[]="Player";
    if(!buf||cap<sizeof name){M32(pcb)=(uint32_t)sizeof name;FAIL(122,0,2);}
    memcpy(checked(c,buf,(uint32_t)sizeof name),name,sizeof name);M32(pcb)=(uint32_t)sizeof name;RET(1,2);
}
static void s_CopyFileA(CPU *c){
    const char *src=guest_cstr(mem,A(0),512),*dst=guest_cstr(mem,A(1),512);char sp[1024],dp[1024];
    if(!src||!dst||path_for(src,sp,sizeof sp)||path_for(dst,dp,sizeof dp))FAIL(ERR_INVALID_PARAMETER,0,3);
    if(A(2)){FILE *ex=fopen(dp,"rb");if(ex){fclose(ex);FAIL(80,0,3);}} /* ERROR_FILE_EXISTS */
    FILE *in=fopen(sp,"rb");if(!in)FAIL(ERR_FILE_NOT_FOUND,0,3);
    FILE *out=fopen(dp,"wb");if(!out){fclose(in);FAIL(ERR_ACCESS_DENIED,0,3);}
    char block[4096];size_t n;int ok=1;
    while((n=fread(block,1,sizeof block,in))>0)if(fwrite(block,1,n,out)!=n){ok=0;break;}
    if(ferror(in))ok=0;if(fclose(in))ok=0;if(fclose(out))ok=0;
    if(!ok){remove(dp);FAIL(ERR_READ_FAULT,0,3);}
    RET(1,3);
}
static uint32_t ptr_key;
static uint32_t encode_key(void){
    if(!ptr_key)ptr_key=(uint32_t)((uint64_t)time(NULL)*UINT64_C(2654435761)+(uint64_t)tick())|1u;
    return ptr_key;
}
static void s_EncodePointer(CPU *c){RET(A(0)^encode_key(),1);}
static void s_DecodePointer(CPU *c){RET(A(0)^encode_key(),1);}
static void s_CompareFileTime(CPU *c){
    uint64_t a=M64(A(0)),b=M64(A(1));
    RET(a<b?(uint32_t)-1:a>b?1u:0u,2);
}
static void lstr_copy(CPU *c,uint32_t dst,uint32_t src,int append){
    const char *s=guest_cstr(mem,src,0x100000);if(!s)rt_unimpl(c,src,"invalid string");
    size_t base=0;
    if(append){const char *d=guest_cstr(mem,dst,0x100000);if(!d)rt_unimpl(c,dst,"invalid string");base=strlen(d);}
    size_t n=strlen(s);
    for(size_t i=0;i<=n;++i){char *p=guest_ptr(mem,dst+(uint32_t)(base+i),1);if(!p)rt_unimpl(c,dst,"lstrcpy destination range");p[0]=s[i];}
    RET(dst,2);
}
static void s_lstrcpyA(CPU *c){lstr_copy(c,A(0),A(1),0);}
static void s_lstrcatA(CPU *c){lstr_copy(c,A(0),A(1),1);}
static void s_InterlockedExchangeAdd(CPU *c){uint32_t p=A(0),old=M32(p);M32(p)=old+A(1);guest_watch_check(mem,"InterlockedExchangeAdd");RET(old,2);}
static void s_GetDriveTypeA(CPU *c){
    const char *path=str(c,A(0));
    if(isalpha((unsigned char)path[0])&&path[1]==':')RET(3,1); /* DRIVE_FIXED */
    RET(1,1);                                                  /* DRIVE_NO_ROOT_DIR */
}
/* FILETIME is 100 ns ticks since 1601-01-01 UTC. SYSTEMTIME is eight WORDs. */
#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)
static void ft_split(uint64_t ft,int64_t *sec,int *ms){
    int64_t ticks=(int64_t)ft-(int64_t)FILETIME_UNIX_EPOCH;
    int64_t s=ticks/10000000, rem=ticks%10000000;
    if(rem<0){rem+=10000000;--s;}
    *sec=s;*ms=(int)(rem/10000);
}
static void civil_from_days(int64_t z,int *y,int *m,int *d){
    z+=719468;int64_t era=(z>=0?z:z-146096)/146097;uint32_t doe=(uint32_t)(z-era*146097);
    uint32_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t yy=(int64_t)yoe+era*400;
    uint32_t doy=doe-(365*yoe+yoe/4-yoe/100);
    uint32_t mp=(5*doy+2)/153,dd=doy-(153*mp+2)/5+1,mm=mp+(mp<10?3:-9);
    if(mm<=2)++yy;*y=(int)yy;*m=(int)mm;*d=(int)dd;
}
static int64_t days_from_civil(int y,int m,int d){
    y-=m<=2;int64_t era=(y>=0?y:y-399)/400;uint32_t yoe=(uint32_t)(y-era*400);
    uint32_t doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    return era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
}
static int ft_to_system(uint64_t ft,uint32_t p){
    int64_t sec;int ms;ft_split(ft,&sec,&ms);
    int64_t days=sec/86400,rem=sec%86400;if(rem<0){rem+=86400;--days;}
    int y,m,d;civil_from_days(days,&y,&m,&d);
    int wd=(int)(((days%7)+7+4)%7);
    M16(p)=(uint16_t)y;M16(p+2)=(uint16_t)m;M16(p+4)=(uint16_t)wd;M16(p+6)=(uint16_t)d;
    M16(p+8)=(uint16_t)(rem/3600);M16(p+10)=(uint16_t)((rem%3600)/60);M16(p+12)=(uint16_t)(rem%60);M16(p+14)=(uint16_t)ms;
    return 1;
}
static int system_to_ft(uint32_t p,uint64_t *ft){
    int y=M16(p),m=M16(p+2),d=M16(p+6),hh=M16(p+8),mm=M16(p+10),ss=M16(p+12),ms=M16(p+14);
    if(y<1601||m<1||m>12||d<1||d>31||hh>23||mm>59||ss>59||ms>999)return 0;
    int64_t days=days_from_civil(y,m,d);
    *ft=(uint64_t)((days*86400+hh*3600+mm*60+ss)*INT64_C(10000000)+ms*10000)+FILETIME_UNIX_EPOCH;
    return 1;
}
static int64_t local_offset_seconds(int64_t unix_sec){
    time_t t=(time_t)unix_sec;struct tm lt,gt;
#ifdef _WIN32
    localtime_s(&lt,&t);gmtime_s(&gt,&t);
#else
    localtime_r(&t,&lt);gmtime_r(&t,&gt);
#endif
    time_t l=mktime(&lt),g=mktime(&gt);
    return (int64_t)difftime(l,g);
}
static void s_FileTimeToSystemTime(CPU *c){
    uint64_t ft=M64(A(0));uint32_t p=A(1);checked(c,p,16);
    if(!ft_to_system(ft,p))FAIL(ERR_INVALID_PARAMETER,0,2);
    RET(1,2);
}
static void s_SystemTimeToFileTime(CPU *c){
    uint64_t ft;uint32_t p=A(0),out=A(1);checked(c,p,16);checked(c,out,8);
    if(!system_to_ft(p,&ft))FAIL(ERR_INVALID_PARAMETER,0,2);
    M64(out)=ft;RET(1,2);
}
static void s_FileTimeToLocalFileTime(CPU *c){
    uint64_t ft=M64(A(0));uint32_t out=A(1);checked(c,out,8);int64_t sec;int ms;ft_split(ft,&sec,&ms);
    int64_t local=(int64_t)ft+local_offset_seconds(sec)*INT64_C(10000000);
    M64(out)=(uint64_t)local;RET(1,2);
}
static void s_LocalFileTimeToFileTime(CPU *c){
    uint64_t ft=M64(A(0));uint32_t out=A(1);checked(c,out,8);int64_t sec;int ms;ft_split(ft,&sec,&ms);
    int64_t utc=(int64_t)ft-local_offset_seconds(sec)*INT64_C(10000000);
    M64(out)=(uint64_t)utc;RET(1,2);
}
static void s_GetLocalTime(CPU *c){
    uint32_t p=A(0);checked(c,p,16);
    uint64_t ft=(uint64_t)(((int64_t)time(NULL)+INT64_C(11644473600))*INT64_C(10000000));
    int64_t sec;int ms;ft_split(ft,&sec,&ms);
    ft=(uint64_t)((int64_t)ft+local_offset_seconds(sec)*INT64_C(10000000));
    ft_to_system(ft,p);RET(0,1);
}
static void s_GetTimeZoneInformation(CPU *c){
    uint32_t p=A(0);uint8_t *tz=checked(c,p,172);
    memset(tz,0,172);
    int64_t off=local_offset_seconds((int64_t)time(NULL));
    /* TIME_ZONE_INFORMATION.Bias is minutes to add to local time to get UTC. */
    M32(p)=(uint32_t)(-(int32_t)(off/60));
    static const uint16_t sn[9]={ 'S','t','a','n','d','a','r','d',0 };
    static const uint16_t dn[9]={ 'D','a','y','l','i','g','h','t',0 };
    for(unsigned i=0;i<9;++i){M16(p+4+i*2)=sn[i];M16(p+88+i*2)=dn[i];}
    RET(0,1); /* TIME_ZONE_ID_UNKNOWN: no transition dates provided */
}
static void s_LocalAlloc(CPU *c){
    uint32_t flags=A(0),n=A(1);uint32_t p=heap_alloc(&heap,n?n:1);
    if(!p)FAIL(ERR_NO_MEMORY,0,2);
    if(flags&0x40)memset(checked(c,p,n?n:1),0,n?n:1);
    RET(p,2);
}
static void s_LocalFree(CPU *c){uint32_t p=A(0);if(p&&heap_free(&heap,p))FAIL(ERR_INVALID_HANDLE,0,1);RET(0,1);}
static void s_FormatMessageA(CPU *c){
    uint32_t buf=A(4),n=A(5);
    static const char msg[]="Unknown system error.";
    if(!buf||n<2)RET(0,7);
    size_t len=sizeof msg-1;if((size_t)n-1<len)len=(size_t)n-1;
    memcpy(checked(c,buf,(uint32_t)(len+1)),msg,len);M8(buf+(uint32_t)len)=0;
    RET(len,7); /* characters copied, excluding NUL */
}
static const char *basename_dll(const char *s){const char *p=s;while(*s){if(*s=='/'||*s=='\\')p=s+1;++s;}return p;}
#define KERNEL_HANDLE 0xefff0000u
#define CRT_HANDLE 0xefff0004u
#define D3D_HANDLE 0xefff0008u
static uint32_t module_handle(const char *name){
    if(!name)return process_linker&&process_linker->count?process_linker->modules[0].image->base:0;
    name=basename_dll(name);
    if(!icmp(name,"kernel32.dll")||!icmp(name,"kernel32"))return KERNEL_HANDLE;
    if(!icmp(name,"msvcrt.dll")||!icmp(name,"msvcrt"))return CRT_HANDLE;
    if(!icmp(name,"d3d9.dll")||!icmp(name,"d3d9")){PeImport e={.dll="d3d9.dll",.name="Direct3DCreate9"};if(win32_resolve(&e,NULL))return D3D_HANDLE;}
    if(process_linker)for(size_t i=0;i<process_linker->count;++i)if(!icmp(name,process_linker->modules[i].dll))return process_linker->modules[i].image->base;
    return 0;
}
static const char *module_name(uint32_t handle){
    if(handle==KERNEL_HANDLE)return "kernel32.dll";
    if(handle==CRT_HANDLE)return "msvcrt.dll";
    if(handle==D3D_HANDLE)return "d3d9.dll";
    if(process_linker)for(size_t i=0;i<process_linker->count;++i)if(handle==process_linker->modules[i].image->base)return process_linker->modules[i].dll;
    return NULL;
}
static void s_GetModuleHandleA(CPU *c){uint32_t h=module_handle(A(0)?str(c,A(0)):NULL);if(!h)FAIL(126,0,1);RET(h,1);}
static void s_GetModuleHandleW(CPU *c){
    uint32_t p=A(0),h;char name[260];
    if(!p){h=module_handle(NULL);if(!h)FAIL(126,0,1);RET(h,1);}
    size_t i=0;
    for(;i<sizeof name-1;++i){uint32_t ch=M16(p+(uint32_t)(i*2));if(!ch)break;if(ch>0x7f)FAIL(ERR_INVALID_PARAMETER,0,1);name[i]=(char)ch;}
    if(i==sizeof name-1&&M16(p+(uint32_t)(i*2)))FAIL(ERR_INVALID_PARAMETER,0,1);
    name[i]=0;h=module_handle(name);if(!h)FAIL(126,0,1);RET(h,1);
}
static void s_LoadLibraryA(CPU *c){uint32_t h=module_handle(str(c,A(0)));if(!h)FAIL(126,0,1);RET(h,1);}
static void s_FreeLibrary(CPU *c){if(!module_name(A(0)))FAIL(ERR_INVALID_HANDLE,0,1);RET(1,1); /* statically loaded modules stay resident */}
static void s_GetProcAddress(CPU *c){
    const char *dll=module_name(A(0));if(!dll)FAIL(ERR_INVALID_HANDLE,0,2);
    uint32_t a=A(1);PeImport e={.dll=dll};if(a<=0xffff)e.ordinal=(uint16_t)a;else e.name=str(c,a);
    uint32_t p=win32_resolve(&e,NULL);if(!p && process_linker)p=pe_link_resolve(&e,(void *)process_linker);
    if(!p)FAIL(127,0,2);RET(p,2);
}
static void s_GetModuleFileNameA(CPU *c){
    uint32_t h=A(0)?A(0):module_handle(NULL);const char *name=module_name(h);uint32_t n=A(2);
    if(!name || !n || !guest_ptr(mem,A(1),n))FAIL(ERR_INVALID_PARAMETER,0,3);
    char path[128];int len=snprintf(path,sizeof path,"C:\\Skyrim\\%s",name);if(len<0 || (size_t)len>=sizeof path)FAIL(ERR_INVALID_PARAMETER,0,3);
    size_t copy=(size_t)len<n-1?(size_t)len:n-1;memcpy(guest_ptr(mem,A(1),n),path,copy);M8(A(1)+(uint32_t)copy)=0;
    if((uint32_t)len>=n)FAIL(122,n,3);RET((uint32_t)len,3);
}

/* Sandboxed relative paths below the configured Skyrim data root. */
static int path_for_impl(const char *name,char *out,size_t cap) {
    /* TESV's resource sources register absolute virtual roots such as
     * C:\Skyrim\ and F:\Skyrim\. They are aliases of the one configured sandbox
     * root, not permission to access arbitrary host paths. Strip only that
     * recognised prefix; every other drive/absolute path stays rejected. */
    const char *relative=name;
    const char *base=cwd;
    int virtual_root=0;
    /* Guest code may already hand back an SD path (and mixes separators when
     * it joins the configured root): sdmc:/Skyrim\DATA\... .  Treat that as
     * the sandbox root, not as an arbitrary component with a colon. */
    if(name && (name[0]=='s'||name[0]=='S') && (name[1]=='d'||name[1]=='D') &&
       (name[2]=='m'||name[2]=='M') && (name[3]=='c'||name[3]=='C') &&
       name[4]==':' && (name[5]=='/'||name[5]=='\\')) {
        const char *p=name+6;
        /* Drop an optional leading "Skyrim\" component so sdmc:/Skyrim\DATA
         * does not become sdmc:/Skyrim/Skyrim/DATA. */
        {
            const char *word="skyrim";
            unsigned k;
            for(k=0;k<6;++k){
                char ch=p[k];
                if(!ch)break;
                if(ch>='A'&&ch<='Z')ch=(char)(ch+('a'-'A'));
                if(ch!=word[k])break;
            }
            if(k==6&&(p[6]==0||p[6]=='/'||p[6]=='\\')){
                p+=6;
                if(*p)++p;
            }
        }
        relative=*p?p:".";
        base=root;
        virtual_root=1;
    }
    if(name && ((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')) &&
       name[1]==':' && (name[2]=='\\'||name[2]=='/')) {
        const char *p=name+3;
        const char *word="skyrim";
        unsigned i;
        for(i=0;i<6;++i){
            char c=p[i],w=word[i];
            if(!c)break;
            if(c>='A'&&c<='Z')c=(char)(c+('a'-'A'));
            if(c!=w)break;
        }
        if(i==6 && (p[6]==0||p[6]=='\\'||p[6]=='/')) {
            p+=6;
            if(*p)++p;
            relative=*p?p:".";
            base=root;
            virtual_root=1;
        }
    }
    if(!relative || !*relative || *relative=='/' || *relative=='\\')return -1;
    if(name && name[1]==':') {
        static unsigned logged;
        if(logged<20u){++logged;fprintf(stderr,"[path] relative='%s' base='%s' delta=%ld bytes=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",relative,base,(long)(relative-name),(unsigned char)name[0],(unsigned char)name[1],(unsigned char)name[2],(unsigned char)name[3],(unsigned char)name[4],(unsigned char)name[5],(unsigned char)name[6],(unsigned char)name[7],(unsigned char)name[8],(unsigned char)name[9],(unsigned char)name[10],(unsigned char)name[11]);}
    }
    if(virtual_root && !strcmp(relative,".")) {
        if(strlen(root)>=cap)return -1;
        strcpy(out,root);
        return 0;
    }
    size_t len=strlen(base);if(len+2>=cap)return -1;
    memcpy(out,base,len);
    if(len&&out[len-1]!='/'&&out[len-1]!='\\')out[len++]='/';
    (void)virtual_root;
    const char *p=relative;
    while(*p) {
        const char *start=p;while(*p && *p!='/' && *p!='\\'){if(*p==':')return -1;++p;}
        size_t n=(size_t)(p-start);
        if(!n || (n==1 && *start=='.') || (n==2 && !memcmp(start,"..",2)) || len+n+1>=cap)return -1;
        memcpy(out+len,start,n);len+=n;
        if(*p){
            ++p;
            /* A trailing separator names a directory; accept it without
             * emitting another slash. */
            if(!*p)break;
            out[len++]='/';
        }
    }
    out[len]=0;
    size_t rl=strlen(root);
    int root_sep=rl&&(root[rl-1]=='/'||root[rl-1]=='\\');
    if(strncmp(out,root,rl)||(!root_sep&&out[rl]&&out[rl]!='/'))return -1;
    return 0;
}
static int path_for(const char *name,char *out,size_t cap) {
    int rc=path_for_impl(name,out,cap);
    if(rc && name && strstr(name,"Skyrim")) {
        static unsigned logged;
        if(logged<20u) {
            ++logged;
            fprintf(stderr,"[path] reject name='%s' cwd='%s' root='%s' out='%s'\n",
                    name,cwd,root,out);
        }
    }
    return rc;
}
static FILE *file_for(uint32_t h){return h>=0x100 && h<0x100+MAX_FILES?files[h-0x100]:NULL;}
int win32_guest_path(const char *guest_name,char *out,size_t capacity){
    if(!mem||!root[0]||!cwd[0])return WIN32_GUEST_PATH_NO_ROOT;
    if(!guest_name||!*guest_name)return WIN32_GUEST_PATH_EMPTY;
    if(!out||capacity<2u)return WIN32_GUEST_PATH_INVALID;
    size_t guest_len=strlen(guest_name);
    if(guest_len>=260u||strlen(root)+guest_len+2u>capacity)return WIN32_GUEST_PATH_TOO_LONG;
    return path_for(guest_name,out,capacity)?WIN32_GUEST_PATH_OUTSIDE:WIN32_GUEST_PATH_OK;
}
static FindState *find_for(uint32_t h){
    uint32_t i=h-FIND_HANDLE_BASE;
    return h>=FIND_HANDLE_BASE && h<FIND_HANDLE_BASE+MAX_FINDS && finds[i].used?&finds[i]:NULL;
}
static uint64_t unix_to_ft(int64_t sec){
    if(sec<-(int64_t)INT64_C(11644473600))return 0;
    return (uint64_t)sec*UINT64_C(10000000)+FILETIME_UNIX_EPOCH;
}
/* errno -> Win32 error. EEXIST is used by both CreateDirectoryA (183) and
 * CreateFileA(CREATE_NEW) (80); callers that need 183 override it. */
static uint32_t errno_error(void){
    switch(errno){
    case ENOENT:return ERR_FILE_NOT_FOUND;
    case EACCES:case EPERM:return ERR_ACCESS_DENIED;
    case EEXIST:return ERR_ALREADY_EXISTS;
    case ENOSPC:return 112u;
    case EINVAL:return ERR_INVALID_PARAMETER;
    default:return 31u; /* ERROR_GEN_FAILURE */
    }
}
static uint32_t attr_of(const struct stat *st){return S_ISDIR(st->st_mode)?ATTR_DIRECTORY:ATTR_NORMAL;}
static uint64_t size_of(const struct stat *st){return S_ISDIR(st->st_mode)?0:(uint64_t)st->st_size;}
static int64_t file_tell(FILE *f){
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
static int file_seek(FILE *f,int64_t off,int origin){
#ifndef _WIN32
    if((int64_t)(off_t)off!=off)return -1;
#endif
#ifdef _WIN32
    return _fseeki64(f,off,origin);
#else
    return fseeko(f,(off_t)off,origin);
#endif
}
static int set_end_of_file(FILE *f,int64_t pos){
    if(pos<0 || fflush(f))return -1;
#ifdef _WIN32
    return _chsize_s(_fileno(f),pos)?-1:0;
#else
    if((int64_t)(off_t)pos!=pos)return -1;
    return ftruncate(fileno(f),(off_t)pos)?-1:0;
#endif
}
/* '*' and '?' wildcard match, case-insensitive like the Windows filesystem. */
static int wild_match(const char *pat,const char *name){
    const char *star=NULL,*mark=name;
    while(*name){
        if(*pat=='?' || (*pat && lower_ascii((unsigned char)*pat)==lower_ascii((unsigned char)*name))){++pat;++name;}
        else if(*pat=='*'){star=pat++;mark=name;}
        else if(star){pat=star+1;name=++mark;}
        else return 0;
    }
    while(*pat=='*')++pat;
    return !*pat;
}
/* Split "dir/pattern" into a sandboxed directory and a leaf pattern. Both the
 * directory and the leaf are validated: no drive letters, no "."/"..", and the
 * directory must pass path_for(). The leaf may contain '*' and '?'. */
static int find_split(const char *name,char *base,size_t cap,char *pat,size_t pcap){
    size_t len=strlen(name);if(!len)return -1;
    const char *sep=NULL;
    for(const char *p=name;*p;++p)if(*p=='/'||*p=='\\')sep=p;
    const char *leaf=sep?sep+1:name;
    size_t plen=strlen(leaf);
    if(!plen || plen>=pcap || plen>=260)return -1;
    if(!strcmp(leaf,".")||!strcmp(leaf,".."))return -1;
    for(size_t i=0;i<plen;++i)if(leaf[i]==':')return -1;
    memcpy(pat,leaf,plen+1);
    if(!sep){if(strlen(cwd)>=cap)return -1;strcpy(base,cwd);return 0;}
    size_t dlen=(size_t)(sep-name);
    if(!dlen)return -1; /* absolute or root-relative */
    char dir[1024];if(dlen>=sizeof dir)return -1;
    memcpy(dir,name,dlen);dir[dlen]=0;
    return path_for(dir,base,cap);
}
static void fill_find_data(CPU *c,uint32_t p,const char *name,uint32_t attrib,uint64_t create,uint64_t access,uint64_t write,uint64_t size){
    uint8_t *d=(uint8_t*)checked(c,p,320);
    size_t n=strlen(name);if(n>259)n=259;
    memset(d,0,320);
    M32(p)=attrib;M64(p+4)=create;M64(p+12)=access;M64(p+20)=write;
    M32(p+28)=(uint32_t)(size>>32);M32(p+32)=(uint32_t)size;
    memcpy(d+44,name,n);
    /* cAlternateFileName stays empty: no 8.3 alias is synthesized. */
}
/* The UCRT _findfirst does not implement '?' matching faithfully, so both
 * platforms enumerate all entries and filter with wild_match(). */
#ifdef _WIN32
static int find_entry_win(FindState *s,int first,char *name,size_t cap,uint32_t *attrib,uint64_t *size,uint64_t *create,uint64_t *access,uint64_t *write){
    struct _finddata_t fd;
    while(1){
        if(first){
            first=0;
            char full[1280];int n=snprintf(full,sizeof full,"%s/*",s->dirpath);
            if(n<0||(size_t)n>=sizeof full)return -1;
            intptr_t c=_findfirst(full,&fd);
            if(c==-1)return 0;
            s->cookie=c;
        }else if(_findnext(s->cookie,&fd))return 0;
        if(!strcmp(fd.name,".")||!strcmp(fd.name,".."))continue;
        if(!wild_match(s->pattern,fd.name))continue;
        size_t ln=strlen(fd.name);if(ln>=cap)return -1;
        memcpy(name,fd.name,ln+1);
        *attrib=fd.attrib&ATTR_DIRECTORY?ATTR_DIRECTORY:ATTR_NORMAL;
        *size=(uint64_t)fd.size;
        *create=unix_to_ft((int64_t)fd.time_create);
        *access=unix_to_ft((int64_t)fd.time_access);
        *write=unix_to_ft((int64_t)fd.time_write);
        return 1;
    }
}
#endif
#ifndef _WIN32
static int find_entry_unix(FindState *s,char *name,size_t cap,uint32_t *attrib,uint64_t *size,uint64_t *create,uint64_t *access,uint64_t *write){
    while(1){
        struct dirent *e=readdir(s->dir);
        if(!e)return 0;
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(!wild_match(s->pattern,e->d_name))continue;
        char full[1280];int n=snprintf(full,sizeof full,"%s/%s",s->dirpath,e->d_name);
        if(n<0||(size_t)n>=sizeof full)return -1;
        struct stat st;
        if(stat(full,&st)){
            *attrib=0;*size=0;*create=*access=*write=0;
        }else{
            *attrib=attr_of(&st);*size=size_of(&st);
            *create=unix_to_ft((int64_t)st.st_ctime);*access=unix_to_ft((int64_t)st.st_atime);*write=unix_to_ft((int64_t)st.st_mtime);
        }
        size_t ln=strlen(e->d_name);if(ln>=cap)return -1;
        memcpy(name,e->d_name,ln+1);
        return 1;
    }
}
#endif
static void s_GetFileAttributesA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);char path[1024];struct stat st;
    if(!name || path_for(name,path,sizeof path))FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,1);
    if(stat(path,&st))FAIL(errno_error(),UINT32_MAX,1);
    set_error(c,0);RET(attr_of(&st),1);
}
static void s_GetFileAttributesExA(CPU *c){
    static unsigned entry_logged;
    if(entry_logged<20u){++entry_logged;fprintf(stderr,"[attr] enter ptr=%08x level=%u out=%08x\n",A(0),A(1),A(2));}
    const char *name=guest_cstr(mem,A(0),1024);uint32_t level=A(1),out=A(2);char path[1024];struct stat st;
    if(level)FAIL(ERR_INVALID_PARAMETER,0,3);
    if(!name || path_for(name,path,sizeof path))FAIL(ERR_INVALID_PARAMETER,0,3);
    int trace_control = name && (strstr(name,"ControlMap") != NULL || strstr(name,"Update.bsa") != NULL);
    static unsigned attr_logged;
    if(!trace_control && attr_logged < 8u)trace_control=1;
    if(trace_control && attr_logged < 32u){++attr_logged;fprintf(stderr,"[attr] guest='%s' host='%s'\n",name,path);}
    errno=0;int rc=stat(path,&st);
    if(trace_control)fprintf(stderr,"[attr] stat rc=%d errno=%d\n",rc,errno);
    if(rc)FAIL(errno_error(),0,3);
    checked(c,out,36);
    uint64_t size=size_of(&st);
    M32(out)=attr_of(&st);M64(out+4)=unix_to_ft((int64_t)st.st_ctime);
    M64(out+12)=unix_to_ft((int64_t)st.st_atime);M64(out+20)=unix_to_ft((int64_t)st.st_mtime);
    M32(out+28)=(uint32_t)(size>>32);M32(out+32)=(uint32_t)size;
    set_error(c,0);RET(1,3);
}
static void s_FindFirstFileA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);char base[1024],pat[260];uint32_t out=A(1);
    if(!name || find_split(name,base,sizeof base,pat,sizeof pat))FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,2);
    checked(c,out,320);
    unsigned i;for(i=0;i<MAX_FINDS && finds[i].used;++i){}if(i==MAX_FINDS)FAIL(ERR_NO_MEMORY,INVALID_HANDLE,2);
#ifdef _WIN32
    finds[i].used=1;finds[i].cookie=-1;memcpy(finds[i].pattern,pat,strlen(pat)+1);
    memcpy(finds[i].dirpath,base,strlen(base)+1);
    char ename[260];uint32_t attrib;uint64_t size,ct,at,wt;
    int got=find_entry_win(&finds[i],1,ename,sizeof ename,&attrib,&size,&ct,&at,&wt);
    if(got<=0){
        if(finds[i].cookie!=-1)_findclose(finds[i].cookie);
        finds[i].used=0;
        if(got<0)FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,2);
        FAIL(errno_error(),INVALID_HANDLE,2);
    }
    fill_find_data(c,out,ename,attrib,ct,at,wt,size);
#else
    DIR *d=opendir(base);
    if(!d)FAIL(errno_error(),INVALID_HANDLE,2);
    finds[i].used=1;finds[i].dir=d;memcpy(finds[i].pattern,pat,strlen(pat)+1);
    memcpy(finds[i].dirpath,base,strlen(base)+1);
    char ename[260];uint32_t attrib;uint64_t size,ct,at,wt;
    int got=find_entry_unix(&finds[i],ename,sizeof ename,&attrib,&size,&ct,&at,&wt);
    if(got<=0){
        closedir(d);finds[i].used=0;
        if(got<0)FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,2);
        FAIL(ERR_FILE_NOT_FOUND,INVALID_HANDLE,2);
    }
    fill_find_data(c,out,ename,attrib,ct,at,wt,size);
#endif
    set_error(c,0);RET(FIND_HANDLE_BASE+i,2);
}
static void s_FindNextFileA(CPU *c){
    FindState *s=find_for(A(0));uint32_t out=A(1);
    if(!s)FAIL(ERR_INVALID_HANDLE,0,2);
    checked(c,out,320);
#ifdef _WIN32
    char ename[260];uint32_t attrib;uint64_t size,ct,at,wt;
    int got=find_entry_win(s,0,ename,sizeof ename,&attrib,&size,&ct,&at,&wt);
    if(got<=0){set_error(c,got<0?ERR_INVALID_PARAMETER:ERR_NO_MORE_FILES);RET(0,2);}
    fill_find_data(c,out,ename,attrib,ct,at,wt,size);
#else
    char ename[260];uint32_t attrib;uint64_t size,ct,at,wt;
    int got=find_entry_unix(s,ename,sizeof ename,&attrib,&size,&ct,&at,&wt);
    if(got<=0){set_error(c,got<0?ERR_INVALID_PARAMETER:ERR_NO_MORE_FILES);RET(0,2);}
    fill_find_data(c,out,ename,attrib,ct,at,wt,size);
#endif
    set_error(c,0);RET(1,2);
}
static void s_FindClose(CPU *c){
    FindState *s=find_for(A(0));if(!s)FAIL(ERR_INVALID_HANDLE,0,1);
#ifdef _WIN32
    _findclose(s->cookie);
#else
    closedir(s->dir);
#endif
    s->used=0;set_error(c,0);RET(1,1);
}
static void s_CreateDirectoryA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);char path[1024];char clean[1024];
    if(!name)FAIL(ERR_INVALID_PARAMETER,0,2);
    /* Win32 accepts a trailing separator on a directory name. */
    size_t n=strlen(name);
    while(n>1 && (name[n-1]=='/'||name[n-1]=='\\'))--n;
    if(n>=sizeof clean)FAIL(ERR_INVALID_PARAMETER,0,2);
    memcpy(clean,name,n);clean[n]=0;
    if(path_for(clean,path,sizeof path))FAIL(ERR_INVALID_PARAMETER,0,2);
#ifdef _WIN32
    if(_mkdir(path))FAIL(errno_error(),0,2);
#else
    if(mkdir(path,0777))FAIL(errno_error(),0,2);
#endif
    set_error(c,0);RET(1,2);
}
static void delete_file(CPU *c,const char *name){
    char path[1024];
    if(!name || path_for(name,path,sizeof path))FAIL(ERR_INVALID_PARAMETER,0,1);
    if(remove(path))FAIL(errno_error(),0,1);
    set_error(c,0);RET(1,1);
}
static void s_DeleteFileA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);
    if(!name)FAIL(ERR_INVALID_PARAMETER,0,1);
    delete_file(c,name);
}
/* Access mask bits we understand: GENERIC_READ/WRITE plus SYNCHRONIZE (all of
 * our handles are synchronous). Anything else is rejected, not ignored. */
#define ACCESS_ALLOWED 0xC0100000u
static void create_file(CPU *c,const char *name){
    uint32_t access=A(1),share=A(2),disp=A(4),flags=A(5);
    if((access&~ACCESS_ALLOWED) || share>3 || disp<1 || disp>5 || (flags&0x40000000u) || A(6))
        FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,7);
    int rd=access==0 || (access&0x80000000u)!=0;
    int wr=(access&0x40000000u)!=0;
    if(disp==5 && !wr)FAIL(ERR_ACCESS_DENIED,INVALID_HANDLE,7);
    char path[1024];
    if(!name || path_for(name,path,sizeof path))FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,7);
    unsigned i;for(i=0;i<MAX_FILES && files[i];++i){}if(i==MAX_FILES)FAIL(ERR_NO_MEMORY,INVALID_HANDLE,7);
    const char *mode;
    switch(disp){
    case 1:mode="w+bx";break;             /* CREATE_NEW */
    case 2:mode="w+b";break;              /* CREATE_ALWAYS */
    case 3:mode=wr?"r+b":"rb";break;      /* OPEN_EXISTING */
    case 4:mode=wr?"r+b":"rb";break;      /* OPEN_ALWAYS (create below) */
    default:mode="r+b";break;             /* TRUNCATE_EXISTING */
    }
    errno=0;FILE *f=fopen(path,mode);
    if(!f && disp==4 && wr && errno==ENOENT){errno=0;f=fopen(path,"w+b");}
    if(!f){
        uint32_t e=errno_error();
        if(disp==1 && errno==EEXIST)e=ERR_FILE_EXISTS;
        fprintf(stderr,"[file] FAIL %s errno=%d\n",path,errno);
        FAIL(e,INVALID_HANDLE,7);
    }
    if(disp==5 && set_end_of_file(f,0)){fclose(f);FAIL(ERR_ACCESS_DENIED,INVALID_HANDLE,7);}
    files[i]=f;file_can_read[i]=(uint8_t)rd;file_can_write[i]=(uint8_t)wr;
    snprintf(file_path[i],sizeof file_path[i],"%s",path);
    set_error(c,0);RET(0x100+i,7);
}
static void s_CreateFileA(CPU *c){
    const char *name=guest_cstr(mem,A(0),512);
    if(!name)FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,7);
    { static unsigned n; if(n<60u){++n;fprintf(stderr,"[file] A %s\n",name);} }
    create_file(c,name);
}
static void s_ReadFile(CPU *c){
    FILE *f=file_for(A(0));uint32_t n=A(2),bytes=A(3);
    if(!f)FAIL(ERR_INVALID_HANDLE,0,5);
    if(!file_can_read[A(0)-0x100])FAIL(ERR_ACCESS_DENIED,0,5);
    if(A(4) || !bytes || !guest_ptr(mem,bytes,4) || (n&&!guest_ptr(mem,A(1),n)))FAIL(ERR_INVALID_PARAMETER,0,5);
    M32(bytes)=0;size_t read=n?fread(guest_ptr(mem,A(1),n),1,n,f):0;M32(bytes)=(uint32_t)read;
    if(n>=65536u){static unsigned io;if(io<80u){++io;fprintf(stderr,"[io] read %lu bytes -> %lu\n",(unsigned long)n,(unsigned long)read);}}
    if(ferror(f))FAIL(ERR_READ_FAULT,0,5);
    set_error(c,0);RET(1,5);
}
static void s_WriteFile(CPU *c){
    FILE *f=file_for(A(0));uint32_t n=A(2),bytes=A(3);
    if(A(4))FAIL(ERR_NOT_SUPPORTED,0,5); /* overlapped I/O is rejected at open */
    if(!bytes || !guest_ptr(mem,bytes,4) || (n&&!guest_ptr(mem,A(1),n)))FAIL(ERR_INVALID_PARAMETER,0,5);
    M32(bytes)=0;
    if(A(0)==STD_OUT || A(0)==STD_ERR){
        FILE *out=A(0)==STD_OUT?stdout:stderr;
        size_t w=n?fwrite(guest_ptr(mem,A(1),n),1,n,out):0;M32(bytes)=(uint32_t)w;
        if(w!=n)FAIL(ERR_WRITE_FAULT,0,5);
        set_error(c,0);RET(1,5);
    }
    if(!f)FAIL(ERR_INVALID_HANDLE,0,5);
    if(!file_can_write[A(0)-0x100])FAIL(ERR_ACCESS_DENIED,0,5);
    size_t w=n?fwrite(guest_ptr(mem,A(1),n),1,n,f):0;M32(bytes)=(uint32_t)w;
    if(w!=n)FAIL(ERR_WRITE_FAULT,0,5);
    set_error(c,0);RET(1,5);
}
static void s_CloseHandle(CPU *c){
    uint32_t h=A(0);
    fprintf(stderr,"[close] h=%03x thread=%u\n",(unsigned)h,win32_thread_current_id());
    if(win32_sync_close(h)){set_error(c,0);RET(1,1);}
    if(win32_thread_close(h)){set_error(c,0);RET(1,1);}
    if(mapping_close(h)){set_error(c,0);RET(1,1);}
    FILE *f=file_for(h);if(!f)FAIL(ERR_INVALID_HANDLE,0,1);
    int rc=fclose(f);uint32_t i=h-0x100;files[i]=NULL;file_can_read[i]=0;file_can_write[i]=0;file_path[i][0]=0;
    set_error(c,0);RET(rc==0,1);
}
static void s_GetFileSize(CPU *c){
    FILE *f=file_for(A(0));if(!f)FAIL(ERR_INVALID_HANDLE,UINT32_MAX,2);
    if(A(1)&&!guest_ptr(mem,A(1),4))FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,2);
    int64_t old=file_tell(f);if(old<0 || file_seek(f,0,SEEK_END))FAIL(ERR_READ_FAULT,UINT32_MAX,2);
    int64_t n=file_tell(f);if(file_seek(f,old,SEEK_SET) || n<0)FAIL(ERR_READ_FAULT,UINT32_MAX,2);
    if(A(1))M32(A(1))=(uint32_t)((uint64_t)n>>32);
    set_error(c,0);RET((uint32_t)n,2);
}
static void s_GetFileSizeEx(CPU *c){
    FILE *f=file_for(A(0));if(!f)FAIL(ERR_INVALID_HANDLE,0,2);
    if(!guest_ptr(mem,A(1),8))FAIL(ERR_INVALID_PARAMETER,0,2);
    int64_t old=file_tell(f);if(old<0 || file_seek(f,0,SEEK_END))FAIL(ERR_READ_FAULT,0,2);
    int64_t n=file_tell(f);if(file_seek(f,old,SEEK_SET) || n<0)FAIL(ERR_READ_FAULT,0,2);
    M64(A(1))=(uint64_t)n;set_error(c,0);RET(1,2);
}
static void s_SetFilePointer(CPU *c){
    FILE *f=file_for(A(0));if(!f)FAIL(ERR_INVALID_HANDLE,UINT32_MAX,4);
    int32_t off=(int32_t)A(1);uint32_t hi=A(2),whence=A(3);
    if(whence>2 || (hi&&(!guest_ptr(mem,hi,4)||M32(hi)!=(off<0?UINT32_MAX:0))))FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,4);
    const int origins[]={SEEK_SET,SEEK_CUR,SEEK_END};if(file_seek(f,off,origins[whence]))FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,4);
    int64_t p=file_tell(f);if(p<0)FAIL(ERR_INVALID_PARAMETER,UINT32_MAX,4);
    {static unsigned sw;if(sw<80u){++sw;fprintf(stderr,"[io] seek %ld whence %lu -> %lld\n",(long)off,(unsigned long)whence,(long long)p);}}
    if(hi)M32(hi)=(uint32_t)((uint64_t)p>>32);
    set_error(c,0);RET((uint32_t)p,4);
}
static void s_SetFilePointerEx(CPU *c){
    FILE *f=file_for(A(0));if(!f)FAIL(ERR_INVALID_HANDLE,0,5);
    if(A(3)&&!guest_ptr(mem,A(3),8))FAIL(ERR_INVALID_PARAMETER,0,5);
    int64_t dist=(int64_t)(((uint64_t)A(2)<<32)|A(1));
    uint32_t method=A(4);if(method>2)FAIL(ERR_INVALID_PARAMETER,0,5);
    const int origins[]={SEEK_SET,SEEK_CUR,SEEK_END};
    if(file_seek(f,dist,origins[method]))FAIL(ERR_INVALID_PARAMETER,0,5);
    int64_t p=file_tell(f);if(p<0)FAIL(ERR_INVALID_PARAMETER,0,5);
    if(A(3))M64(A(3))=(uint64_t)p;
    set_error(c,0);RET(1,5);
}
static void s_SetEndOfFile(CPU *c){
    FILE *f=file_for(A(0));if(!f)FAIL(ERR_INVALID_HANDLE,0,1);
    if(!file_can_write[A(0)-0x100])FAIL(ERR_ACCESS_DENIED,0,1);
    int64_t pos=file_tell(f);if(pos<0 || set_end_of_file(f,pos))FAIL(ERR_WRITE_FAULT,0,1);
    set_error(c,0);RET(1,1);
}
static void s_FlushFileBuffers(CPU *c){
    uint32_t h=A(0);
    if(h==STD_OUT || h==STD_ERR){
        if(fflush(h==STD_OUT?stdout:stderr))FAIL(ERR_WRITE_FAULT,0,1);
        set_error(c,0);RET(1,1);
    }
    FILE *f=file_for(h);if(!f)FAIL(ERR_INVALID_HANDLE,0,1);
    if(fflush(f))FAIL(ERR_WRITE_FAULT,0,1);
#ifdef _WIN32
    if(_commit(_fileno(f)))FAIL(ERR_WRITE_FAULT,0,1);
#else
    if(fsync(fileno(f)))FAIL(ERR_WRITE_FAULT,0,1);
#endif
    set_error(c,0);RET(1,1);
}
/* Pseudo-Windows path from the sandbox cwd; separators normalized to '\'.
 * Relative-only by design: absolute/drive/".." component paths are rejected
 * by path_for() rather than resolved. */
static void s_GetFullPathNameA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024*1024);char probe[1024],out[1024];
    if(!name || path_for(name,probe,sizeof probe))FAIL(ERR_INVALID_PARAMETER,0,4);
    if(A(3)&&!guest_ptr(mem,A(3),4))FAIL(ERR_INVALID_PARAMETER,0,4);
    const char *src=!strcmp(cwd,".")?name:probe;
    size_t len=strlen(src);if(len>=sizeof out)FAIL(ERR_INVALID_PARAMETER,0,4);
    for(size_t i=0;i<=len;++i)out[i]=src[i]=='/'?'\\':src[i];
    uint32_t n=A(1),buf=A(2);
    if(!buf||!n||n<len+1)RET((uint32_t)len+1,4);
    memcpy(checked(c,buf,(uint32_t)len+1),out,len+1);
    if(A(3)){const char *last=strrchr(out,'\\');M32(A(3))=buf+(uint32_t)(last?last-out+1:0);}
    set_error(c,0);RET((uint32_t)len,4);
}

/* ---- file mappings / lazy file-backed views ------------------------------------
 * CreateFileMappingA validates the guest handle and records the file path and
 * size. MapViewOfFile reserves guest VA (no host copy) and installs a page
 * provider: the first access to each 4 KiB page reads it from the file; a
 * writable page is written back on eviction/unmap. This is the mechanism the
 * engine needs for Skyrim.esm (249 MiB) and the BSAs (up to 1.5 GiB), which
 * cannot be copied into the guest pool. */
#define MAPPING_HANDLE_BASE 0x400u
#define MAX_MAPPINGS 16
#define MAX_VIEWS 32
#define FILE_VIEW_BASE 0x70000000u
#define FILE_VIEW_END  0xa0000000u
#define VA_FILE_MAP_WRITE 0x0002u
#define VA_FILE_MAP_READ  0x0004u
#define VA_FILE_MAP_COPY  0x0001u
#define VA_FILE_MAP_EXECUTE 0x0020u

typedef struct {
    int used, anon, writable;
    uint64_t size;
    uint32_t protect;
    char path[1024];
    char name[64];
} Mapping;
static Mapping mappings[MAX_MAPPINGS];
static struct { uint32_t base; FILE *f; } views[MAX_VIEWS];
static unsigned view_count;

static Mapping *mapping_for(uint32_t h) {
    uint32_t i = h - MAPPING_HANDLE_BASE;
    return h >= MAPPING_HANDLE_BASE && h < MAPPING_HANDLE_BASE + MAX_MAPPINGS &&
           mappings[i].used ? &mappings[i] : NULL;
}
static int mapping_close(uint32_t h) {
    Mapping *mp = mapping_for(h);
    if (!mp) return 0;
    mp->used = 0;
    mp->name[0] = 0;
    return 1;
}
static int file_page_fill(void *ctx, uint64_t off, uint8_t *dst, size_t n,
                          size_t *got) {
    FILE *f = (FILE *)ctx;
    if (file_seek(f, (int64_t)off, SEEK_SET)) return -1;
    size_t r = fread(dst, 1, n, f);
    if (ferror(f)) return -1;
    *got = r;
    return 0;
}
static int file_page_flush(void *ctx, uint64_t off, const uint8_t *src, size_t n) {
    FILE *f = (FILE *)ctx;
    if (file_seek(f, (int64_t)off, SEEK_SET)) return -1;
    if (fwrite(src, 1, n, f) != n) return -1;
    return fflush(f);
}
static int view_overlaps(uint32_t a, uint32_t n) {
    uint64_t end = (uint64_t)a + n;
    for (size_t i=0; i<mem->count; ++i) {
        uint64_t rb = mem->regions[i].base, re = rb + mem->regions[i].size;
        if (rb < end && (uint64_t)a < re) return 1;
    }
    return 0;
}
static int view_gap(uint32_t size, uint32_t *out) {
    for (uint64_t a=FILE_VIEW_BASE; a + size <= FILE_VIEW_END; a += 0x10000u) {
        if (guest_slab_find(mem, (uint32_t)a, 1)) continue;
        if (view_overlaps((uint32_t)a, size)) continue;
        *out = (uint32_t)a;
        return 0;
    }
    return -1;
}
static void s_CreateFileMappingA(CPU *c) {
    uint32_t h=A(0), prot=A(2), high=A(3), low=A(4), name=A(5);
    uint32_t baseprot=prot&0xffu, flags=prot&~0xffu;
    if ((baseprot!=2 && baseprot!=4 && baseprot!=8) ||
        (flags & ~0x08000000u)) { /* SEC_IMAGE/SEC_RESERVE are not implemented */
        fprintf(stderr,"[mapping] unsupported CreateFileMappingA protect=%08lx\n",
                (unsigned long)prot);
        FAIL(ERR_INVALID_PARAMETER,0,6);
    }
    int anon = h==INVALID_HANDLE;
    FILE *src = anon?NULL:file_for(h);
    if (!anon && !src) FAIL(ERR_INVALID_HANDLE,0,6);
    if (!anon && baseprot==4 && !file_can_write[h-0x100])
        FAIL(ERR_ACCESS_DENIED,0,6);
    uint64_t size=((uint64_t)high<<32)|low;
    if (!size) {
        if (anon) FAIL(ERR_INVALID_PARAMETER,0,6);
        int64_t old=file_tell(src);
        if (old<0 || file_seek(src,0,SEEK_END)) FAIL(ERR_READ_FAULT,0,6);
        int64_t end=file_tell(src);
        if (file_seek(src,old,SEEK_SET) || end<=0) FAIL(ERR_READ_FAULT,0,6);
        size=(uint64_t)end;
    }
    if (!size || size > UINT32_MAX) FAIL(ERR_INVALID_PARAMETER,0,6);
    const char *nm = name?guest_cstr(mem,name,64):NULL;
    if (nm && *nm) {
        for (unsigned i=0;i<MAX_MAPPINGS;++i)
            if (mappings[i].used && !strcmp(mappings[i].name,nm)) {
                set_error(c,ERR_ALREADY_EXISTS);
                RET(MAPPING_HANDLE_BASE+i,6);
            }
    }
    unsigned i=0; while (i<MAX_MAPPINGS && mappings[i].used) ++i;
    if (i==MAX_MAPPINGS) FAIL(ERR_NO_MEMORY,0,6);
    Mapping *mp=&mappings[i];
    memset(mp,0,sizeof *mp);
    mp->used=1; mp->anon=anon; mp->size=size; mp->protect=baseprot;
    mp->writable=(baseprot!=2);
    if (!anon) snprintf(mp->path,sizeof mp->path,"%s",file_path[h-0x100]);
    if (nm && *nm) snprintf(mp->name,sizeof mp->name,"%s",nm);
    set_error(c,0);RET(MAPPING_HANDLE_BASE+i,6);
}
static void s_MapViewOfFile(CPU *c) {
    uint32_t h=A(0), access=A(1), high=A(2), low=A(3), bytes=A(4);
    Mapping *mp=mapping_for(h);
    if (!mp) FAIL(ERR_INVALID_HANDLE,0,5);
    uint64_t off=((uint64_t)high<<32)|low;
    if (off & 0xffffu) FAIL(ERR_INVALID_PARAMETER,0,5); /* allocation granularity */
    uint64_t size=bytes?bytes:mp->size-off;
    if (off>mp->size || size>mp->size-off || !size || size>UINT32_MAX-0xfffu)
        FAIL(ERR_INVALID_PARAMETER,0,5);
    if (access & ~(VA_FILE_MAP_WRITE|VA_FILE_MAP_READ|VA_FILE_MAP_COPY))
        FAIL(ERR_INVALID_PARAMETER,0,5); /* VA_FILE_MAP_EXECUTE unsupported */
    int want_write=(access&(VA_FILE_MAP_WRITE|VA_FILE_MAP_COPY))!=0;
    if (want_write && mp->protect==2) FAIL(ERR_ACCESS_DENIED,0,5);
    uint32_t aligned=(uint32_t)((size+0xfffu)&~0xfffu);
    uint32_t base=0;
    if (view_gap(aligned,&base)) FAIL(ERR_NO_MEMORY,0,5);
    if (!guest_slab_reserve(mem,base,aligned)) FAIL(ERR_NO_MEMORY,0,5);
    FILE *f=NULL;
    int writable=want_write && !(access&VA_FILE_MAP_COPY);
    if (!mp->anon) {
        f=fopen(mp->path,writable?"r+b":"rb");
        if (!f) { guest_slab_release(mem,base); FAIL(ERR_FILE_NOT_FOUND,0,5); }
    }
    if (guest_slab_set_provider(mem,base,writable?4:2,writable,
                                f?file_page_fill:NULL,
                                (f&&writable)?file_page_flush:NULL,f,off)) {
        if (f) fclose(f);
        guest_slab_release(mem,base);
        FAIL(ERR_NO_MEMORY,0,5);
    }
    if (f) {
        if (view_count>=MAX_VIEWS) {
            fclose(f); guest_slab_release(mem,base); FAIL(ERR_NO_MEMORY,0,5);
        }
        views[view_count].base=base; views[view_count].f=f; ++view_count;
    }
    if (size>=(1u<<20))
        fprintf(stderr,"[mapping] view %lu KiB at %08lx file='%s' offset=%lu KiB (%s)\n",
                (unsigned long)(size>>10),(unsigned long)base,
                mp->anon?"<pagefile>":mp->path,
                (unsigned long)(off>>10),writable?"writable":"read-only");
    set_error(c,0);RET(base,5);
}
static void s_UnmapViewOfFile(CPU *c) {
    uint32_t base=A(0);
    GuestSlab *s=guest_slab_find(mem,base,1);
    if (!s || !s->mapped || s->base!=base) FAIL(ERR_INVALID_ADDRESS,0,1);
    if (guest_slab_release(mem,base)) FAIL(ERR_INVALID_ADDRESS,0,1);
    for (unsigned i=0;i<view_count;++i)
        if (views[i].base==base) {
            if (views[i].f) fclose(views[i].f);
            views[i]=views[view_count-1]; --view_count;
            break;
        }
    set_error(c,0);RET(1,1);
}
static void mappings_shutdown(void) {
    for (unsigned i=0;i<view_count;++i) {
        if (views[i].base) guest_slab_release(mem,views[i].base);
        if (views[i].f) fclose(views[i].f);
        views[i].base=0; views[i].f=NULL;
    }
    view_count=0;
    memset(mappings,0,sizeof mappings);
}

static void s_malloc(CPU *c){
    uint32_t n=A(0)?A(0):1;uint32_t p=heap_alloc(&heap,n);
    mem_track(MEM_CRT,n,p!=0);CRT(p);
}
static void s_calloc(CPU *c){
    uint64_t n=(uint64_t)A(0)*A(1);if(n>UINT32_MAX)CRT(0);
    uint32_t p=heap_alloc(&heap,n?(uint32_t)n:1);
    if(p){uint32_t z=hsize(&heap,p);memset(checked(c,p,z),0,z);}
    mem_track(MEM_CRT,(uint32_t)n,p!=0);CRT(p);
}
static void s_realloc(CPU *c){
    uint32_t old=A(0)?hsize(&heap,A(0)):0,n=A(1)?A(1):1;
    uint32_t p=heap_realloc(&heap,A(0),n);
    if(p){if(old)mem_untrack(MEM_CRT,old);mem_track(MEM_CRT,n,1);}
    else mem_track(MEM_CRT,n,0);
    CRT(p);
}
static void s_free(CPU *c){
    uint32_t n=A(0)?hsize(&heap,A(0)):0;
    if(heap_free(&heap,A(0)))rt_unimpl(c,A(0),"invalid free");
    if(n)mem_untrack(MEM_CRT,n);
    CRT(0);
}
static void s_memcpy(CPU *c){uint32_t n=A(2),d=A(0);if(n)memmove(checked(c,d,n),checked(c,A(1),n),n);if(n)guest_watch_check(mem,"memcpy");CRT(d);}
static void s_memset(CPU *c){uint32_t n=A(2),d=A(0);if(n)memset(checked(c,d,n),(int)A(1),n);if(n)guest_watch_check(mem,"memset");CRT(d);}
static void s_memcmp(CPU *c){uint32_t n=A(2);CRT(n?memcmp(checked(c,A(0),n),checked(c,A(1),n),n):0);}
static void s_strlen(CPU *c){CRT(strlen(str(c,A(0))));}
static void s_strcmp(CPU *c){CRT(strcmp(str(c,A(0)),str(c,A(1))));}
static void s_stricmp(CPU *c){CRT(icmp(str(c,A(0)),str(c,A(1))));}
static void s_strcpy(CPU *c){const char *s=str(c,A(1));size_t n=strlen(s)+1;memmove(checked(c,A(0),(uint32_t)n),s,n);CRT(A(0));}
static void s_strdup(CPU *c){const char *s=str(c,A(0));uint32_t n=(uint32_t)strlen(s)+1,p=heap_alloc(&heap,n);if(p)memcpy(checked(c,p,n),s,n);CRT(p);}

/* Case-insensitive equality for ASCII identifiers (INI names, env names). */
static int ci_eq(const char *a,const char *b){
    while(*a&&*b){if(lower_ascii((unsigned char)*a)!=lower_ascii((unsigned char)*b))return 0;++a;++b;}
    return *a==*b;
}
static int ci_eq_n(const char *a,const char *b,size_t n){
    for(size_t i=0;i<n;++i)if(lower_ascii((unsigned char)a[i])!=lower_ascii((unsigned char)b[i]))return 0;
    return 1;
}
static int ci_prefix(const char *s,const char *pfx){
    while(*pfx){if(lower_ascii((unsigned char)*s)!=lower_ascii((unsigned char)*pfx))return 0;++s;++pfx;}
    return 1;
}
/* Create the parent directories of a host path; existing ones are ignored. */
static int mkpath_parent(const char *path){
    char tmp[1024];size_t n=strlen(path);if(n>=sizeof tmp)return -1;
    memcpy(tmp,path,n+1);
    char *slash=strrchr(tmp,'/');if(!slash)slash=strrchr(tmp,'\\');
    if(!slash)return 0;*slash=0;
    for(char *p=tmp+1;*p;++p){
        if(*p=='/'||*p=='\\'){
            char save=*p;*p=0;
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp,0777);
#endif
            *p=save;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp,0777);
#endif
    return 0;
}

/* ---- version / machine identity -------------------------------------------------
 * These describe the emulated Win32 environment the guest is actually running on:
 * a Windows XP (5.1/2600) API subset, one CPU, 1252 ANSI code page. They are not
 * claims about the 3DS hardware. */
static void s_GetVersionExA(CPU *c){
    uint32_t p=A(0);
    if(!p)FAIL(ERR_INVALID_PARAMETER,0,1);
    uint32_t size;if(guest_read32(mem,p,&size)){rt_unimpl(c,p,"invalid OSVERSIONINFOA");return;}
    if(size<148)FAIL(ERR_INSUFFICIENT_BUFFER,0,1);
    uint8_t *b=checked(c,p,size<284?148:284);
    M32(p+4)=5;M32(p+8)=1;M32(p+12)=2600;M32(p+16)=2;
    memset(b+20,0,strlen("")+1);
    if(size>=284){M16(p+276)=0;M16(p+278)=0;M16(p+280)=0;b[282]=1;b[283]=0;}
    set_error(c,0);RET(1,1);
}
static void s_GetComputerNameA(CPU *c){
    uint32_t buf=A(0),np=A(1);static const char name[]="N3DS";
    if(!np||!guest_ptr(mem,np,4))FAIL(ERR_INVALID_PARAMETER,0,2);
    uint32_t cap=M32(np),need=(uint32_t)sizeof name-1;
    if(buf && cap>need){char *o=checked(c,buf,need+1);memcpy(o,name,need+1);}
    M32(np)=need;
    if(cap<=need)FAIL(111u,0,2); /* ERROR_BUFFER_OVERFLOW */
    set_error(c,0);RET(1,2);
}
static void s_GetConsoleCP(CPU *c){set_error(c,0);RET(1252,0);}
static void s_GetConsoleOutputCP(CPU *c){set_error(c,0);RET(1252,0);}
static void s_GetConsoleMode(CPU *c){
    /* The lifted console is stdout/stderr, not a Win32 console object. */
    if(!guest_ptr(mem,A(1),4))FAIL(ERR_INVALID_PARAMETER,0,2);
    FAIL(ERR_INVALID_HANDLE,0,2);
}
static void s_WriteConsoleA(CPU *c){
    uint32_t h=A(0),text=A(1),n=A(2),written=A(3);
    if(h!=STD_OUT && h!=STD_ERR && h!=STD_IN)FAIL(ERR_INVALID_HANDLE,0,5);
    if(!guest_ptr(mem,text,n) || !guest_ptr(mem,written,4))FAIL(ERR_INVALID_PARAMETER,0,5);
    FILE *f=h==STD_ERR?stderr:stdout;
    size_t w=n?fwrite(guest_ptr(mem,text,n),1,n,f):0;
    M32(written)=(uint32_t)w;
    if(w!=n)FAIL(ERR_WRITE_FAULT,0,5);
    set_error(c,0);RET(1,5);
}
static void s_WriteConsoleW(CPU *c){
    uint32_t h=A(0),text=A(1),n=A(2),written=A(3);
    if(h!=STD_OUT && h!=STD_ERR && h!=STD_IN)FAIL(ERR_INVALID_HANDLE,0,5);
    if(!guest_ptr(mem,text,(uint64_t)n*2) || !guest_ptr(mem,written,4))FAIL(ERR_INVALID_PARAMETER,0,5);
    FILE *f=h==STD_ERR?stderr:stdout;size_t w=0;
    for(uint32_t i=0;i<n;++i){uint8_t raw[2];uint16_t u;if(guest_read(mem,text+i*2,raw,2))break;
        u=(uint16_t)(raw[0]|((uint16_t)raw[1]<<8));if(u<0x80)fputc((int)u,f);else fputc('?',f);++w;}
    M32(written)=(uint32_t)w;
    if(w!=n)FAIL(ERR_WRITE_FAULT,0,5);
    set_error(c,0);RET(1,5);
}
static void s_EnumSystemLocalesA(CPU *c){
    /* Win32 requires a synchronous callback into guest code for every locale.
     * The shim layer has no re-entrant callback contract, so this call fails
     * explicitly instead of pretending the callback ran. */
    (void)A(0);(void)A(1);
    fprintf(stderr,"[guest] EnumSystemLocalesA: callback enumeration is not implemented\n");
    FAIL(ERR_NOT_SUPPORTED,0,2);
}
static void s_SystemTimeToTzSpecificLocalTime(CPU *c){
    /* The emulated time zone is UTC (GetTimeZoneInformation reports bias 0). */
    uint32_t tz=A(0),utc=A(1),loc=A(2);
    if(tz && !guest_ptr(mem,tz,172))FAIL(ERR_INVALID_PARAMETER,0,3);
    uint8_t *u=checked(c,utc,16),*l=checked(c,loc,16);
    memmove(l,u,16);
    if(tz){int32_t bias;memcpy(&bias,checked(c,tz,4),4);if(bias)FAIL(ERR_INVALID_PARAMETER,0,3);}
    set_error(c,0);RET(1,3);
}

/* ---- pointer validation / virtual memory --------------------------------------- */
static int range_readable(uint32_t a,uint32_t n){
    if(!n)return 1;
    return guest_ptr(mem,a,n)!=NULL;
}
static int range_writable(uint32_t a,uint32_t n){
    if(!n)return 1;
    return guest_ptr(mem,a,n)!=NULL;
}
static void s_IsBadWritePtr(CPU *c){RET(range_writable(A(0),A(1))?0:1,2);}
static void s_IsBadReadPtr(CPU *c){RET(range_readable(A(0),A(1))?0:1,2);}
static void s_IsBadCodePtr(CPU *c){RET(A(0)&&n3ds_lift_has_function(A(0))?0:1,1);}
static void s_VirtualQuery(CPU *c){
    uint32_t addr=A(0),out=A(1),n=A(2);
    if(n<28||!guest_ptr(mem,out,n))FAIL(ERR_INVALID_PARAMETER,0,3);
    uint8_t *b=guest_ptr(mem,out,n);
    VaRes *res=va_res_find(addr);
    if(res){
        uint32_t protect=0;
        int st=guest_slab_state(mem,addr,&protect);
        if(st<0)FAIL(ERR_INVALID_PARAMETER,0,3);
        /* Extend over contiguous pages with the same state and protection.
         * Backing is an implementation detail: a committed page reports
         * MEM_COMMIT whether or not it has been touched yet. */
        uint64_t end=(uint64_t)addr+VA_PAGE_SIZE;
        while(end<(uint64_t)res->base+res->size){
            uint32_t next_protect=0;
            int next=guest_slab_state(mem,(uint32_t)end,&next_protect);
            if(next!=st || (st==1 && next_protect!=protect))break;
            end+=VA_PAGE_SIZE;
        }
        memset(b,0,28);
        uint32_t shown=st?protect:VA_PAGE_NOACCESS;
        M32(out)=addr;M32(out+4)=res->base;M32(out+8)=shown;
        M32(out+12)=(uint32_t)(end-addr);
        M32(out+16)=st?VA_STATE_COMMIT:VA_STATE_RESERVE;
        M32(out+20)=shown;M32(out+24)=st?VA_TYPE_PRIVATE:0;
        set_error(c,0);RET(28,3);
    }
    for(size_t i=0;i<mem->count;++i){
        GuestRegion *r=&mem->regions[i];
        if(addr>=r->base&&addr-r->base<r->size){
            memset(b,0,28);
            M32(out)=addr;M32(out+4)=r->base;M32(out+8)=0x04;
            M32(out+12)=r->size-(addr-r->base);
            M32(out+16)=0x1000;M32(out+20)=0x04;M32(out+24)=0x20000;
            set_error(c,0);RET(28,3);
        }
    }
    GuestSlab *fs=guest_slab_find(mem,addr,1);
    if(fs&&fs->mapped){
        uint32_t protect=0;
        int st=guest_slab_state(mem,addr,&protect);
        if(st<0)FAIL(ERR_INVALID_PARAMETER,0,3);
        uint64_t end=(uint64_t)addr+VA_PAGE_SIZE;
        while(end<(uint64_t)fs->base+fs->size){
            uint32_t next=0;
            int s2=guest_slab_state(mem,(uint32_t)end,&next);
            if(s2!=st||(st==1&&next!=protect))break;
            end+=VA_PAGE_SIZE;
        }
        memset(b,0,28);
        uint32_t shown=st?protect:VA_PAGE_NOACCESS;
        M32(out)=addr;M32(out+4)=fs->base;M32(out+8)=shown;
        M32(out+12)=(uint32_t)(end-addr);
        M32(out+16)=st?VA_STATE_COMMIT:VA_STATE_RESERVE;
        M32(out+20)=shown;M32(out+24)=st?fs->type:0;
        set_error(c,0);RET(28,3);
    }
    FAIL(ERR_INVALID_PARAMETER,0,3);
}
static void s_GlobalMemoryStatus(CPU *c){
    uint32_t p=A(0);(void)checked(c,p,32);
    uint64_t total=(uint64_t)mem->limit;
    uint64_t avail=total>(uint64_t)mem->used?total-(uint64_t)mem->used:0;
    uint64_t commit=memt.probe?(UINT64_C(512)<<20):total;
    M32(p)=32;M32(p+4)=total?(uint32_t)(100-avail*100/total):0;
    M32(p+8)=(uint32_t)total;M32(p+12)=(uint32_t)avail;
    M32(p+16)=(uint32_t)commit;M32(p+20)=(uint32_t)(memt.probe?(UINT64_C(512)<<20):avail);
    M32(p+24)=0x7ffeffffu;M32(p+28)=(uint32_t)avail;
    if(memt.probe && !memt.reported){
        memt.reported=1;
        fprintf(stderr,"[mem] PROBE: reporting a 512 MiB commit limit this port cannot back "
                       "(measurement only, not a shippable value)\n");
    }
    set_error(c,0);RET(0,1);
}

/* ---- version strings and process policy ---------------------------------------- */
static void s_GetVersion(CPU *c){
    /* 5.1 build 2600, platform 2: the emulated Win32 API level. */
    set_error(c,0);RET((2u<<30)|(2600u<<16)|(1u<<8)|5u,0);
}
static void s_AreFileApisANSI(CPU *c){set_error(c,0);RET(1,0);}
static void s_SetErrorMode(CPU *c){set_error(c,0);RET(0,1); /* previous mode was 0 */}

/* ---- environment lookup -------------------------------------------------------- */
static int env_find(const char *name,const char **value){
    const char *p=(const char*)guest_ptr(mem,GP_ENV_A,1);
    if(!p)return 0;
    size_t len=strlen(name);
    while(*p){
        if(ci_prefix(p,name)&&p[len]=='='){*value=p+len+1;return 1;}
        p+=strlen(p)+1;
    }
    return 0;
}
static void s_GetEnvironmentVariableA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);
    if(!name||!*name||strchr(name,'='))FAIL(ERR_INVALID_PARAMETER,0,3);
    const char *v=NULL;
    if(!env_find(name,&v))FAIL(203u,0,3); /* ERROR_ENVVAR_NOT_FOUND */
    size_t need=strlen(v)+1;uint32_t buf=A(1),cap=A(2);
    if(!buf||cap<need){if(cap)RET((uint32_t)need,3);RET((uint32_t)need,3);}
    memcpy(checked(c,buf,(uint32_t)need),v,need);
    set_error(c,0);RET((uint32_t)need-1,3);
}
static void s_ExpandEnvironmentStringsA(CPU *c){
    const char *src=guest_cstr(mem,A(0),4096);uint32_t dst=A(1),cap=A(2);
    if(!src)FAIL(ERR_INVALID_PARAMETER,0,3);
    char out[4096];size_t used=0;
    for(const char *p=src;*p;){
        if(*p=='%'){
            const char *end=strchr(p+1,'%');
            if(end&&end>p+1){
                char name[256];size_t n=(size_t)(end-p-1);
                if(n<sizeof name){
                    memcpy(name,p+1,n);name[n]=0;
                    const char *v=NULL;
                    if(env_find(name,&v)){size_t vn=strlen(v);
                        if(used+vn>=sizeof out)FAIL(ERR_INSUFFICIENT_BUFFER,0,3);
                        memcpy(out+used,v,vn);used+=vn;p=end+1;continue;}
                }
            }
        }
        if(used+1>=sizeof out)FAIL(ERR_INSUFFICIENT_BUFFER,0,3);
        out[used++]=*p++;
    }
    out[used]=0;
    if(!dst||cap<=used)RET((uint32_t)used+1,3);
    memcpy(checked(c,dst,(uint32_t)used+1),out,used+1);
    set_error(c,0);RET((uint32_t)used+1,3);
}
static void s_GetTempPathA(CPU *c){
    static const char dir[]="Temp\\";
    uint32_t n=A(0),buf=A(1);
    size_t need=sizeof dir; /* includes the trailing NUL */
    if(!buf||n<need)RET((uint32_t)need,2);
    memcpy(checked(c,buf,(uint32_t)need),dir,need);
    set_error(c,0);RET((uint32_t)(need-1),2);
}

/* ---- emulated media/version helpers used by legacy installers ------------------- */
static void s_timeGetTime(CPU *c){
    uint64_t t=tick()-start_tick;set_error(c,0);
    RET((uint32_t)((t/freq())*1000+(t%freq())*1000/freq()),0);
}
static void s_timeBeginPeriod(CPU *c){(void)A(0);set_error(c,0);RET(0,1);}
static void s_timeEndPeriod(CPU *c){(void)A(0);set_error(c,0);RET(0,1);}

/* ---- memory / pointer validation ------------------------------------------------ */
static void s_GlobalMemoryStatusEx(CPU *c){
    uint32_t p=A(0);uint8_t *b=checked(c,p,64);
    uint32_t len;memcpy(&len,b,4);
    if(len<64)FAIL(ERR_INVALID_PARAMETER,0,1);
    /* Honest model of the emulated machine: the guest can only use the memory
     * this port actually maps for it, and there is no page file/swap. A larger
     * page-file value would let Skyrim's startup check pass on a promise the
     * port cannot keep. */
    uint64_t total=(uint64_t)mem->limit;
    uint64_t avail=total>(uint64_t)mem->used?total-(uint64_t)mem->used:0;
    memset(b,0,64);M32(p)=64;M64(p+8)=total;M64(p+16)=avail;
    /* MEMORYSTATUSEX: TotalPageFile is the commit limit of the process and
     * AvailPageFile is how much of it the process can still commit. With no
     * page file both equal the physical pool, which is exactly what this port
     * can back. In probe mode a 512 MiB commit limit is reported so the engine's
     * minimum-configuration check can be passed for one measurement run; the
     * value is fictional and is logged as such. */
    uint64_t commit_total=total, commit_avail=avail;
    if(memt.probe){
        commit_total=commit_avail=UINT64_C(512)<<20;
        if(!memt.reported){
            memt.reported=1;
            fprintf(stderr,"[mem] PROBE: reporting a 512 MiB commit limit this port cannot back "
                           "(measurement only, not a shippable value)\n");
        }
    }
    M64(p+24)=commit_total;M64(p+32)=commit_avail;
    M64(p+40)=UINT64_C(0x7ffeffff);M64(p+48)=avail;
    set_error(c,0);RET(1,1);
}

/* ---- private profile (INI) ------------------------------------------------------
 * Real line-oriented reader/writer over the guest-visible file system. Only the
 * documented Win32 shape is implemented: [section], key=value, ';'/'#' comments,
 * case-insensitive names, double quotes stripped, CRLF or LF accepted. */
#define INI_MAX_BYTES (1024u*1024u)
static char *ini_read(const char *name,size_t *len){
    char path[1024];if(path_for(name,path,sizeof path)){errno=ENOENT;return NULL;}
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    char *buf=malloc(INI_MAX_BYTES);if(!buf){fclose(f);errno=ENOMEM;return NULL;}
    size_t n=fread(buf,1,INI_MAX_BYTES-1,f);buf[n]=0;fclose(f);*len=n;return buf;
}
static char *trim(char *s){while(*s==' '||*s=='\t')++s;char *e=s+strlen(s);while(e>s&&(e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n'))*--e=0;return s;}
static int is_section(const char *line,const char *want){
    if(*line!='[')return 0;const char *e=strchr(line+1,']');if(!e)return 0;
    size_t n=(size_t)(e-line-1);return strlen(want)==n && ci_eq_n(line+1,want,n);
}
static int ini_each(const char *name,const char *section,const char *key,
                    int (*cb)(const char*,const char*,void*),void *ctx){
    size_t len;char *buf=ini_read(name,&len);if(!buf)return -1;
    int in=0,found=0;char *p=buf;
    while(*p){
        char *e=strchr(p,'\n');if(e)*e=0;
        char *line=trim(p);
        if(*line==';'||*line=='#'){}
        else if(*line=='['){in=is_section(line,section);if(in)++found;}
        else if(in && *line && strchr(line,'=')){
            char *eq=strchr(line,'=');*eq=0;char *k=trim(line),*v=trim(eq+1);
            size_t vn=strlen(v);
            if(vn>=2 && v[0]=='"' && v[vn-1]=='"'){v[vn-1]=0;++v;}
            if(!key)cb(k,v,ctx);
            else if(ci_eq(k,key))cb(k,v,ctx);
        }
        if(!e)break;p=e+1;
    }
    free(buf);return found;
}
typedef struct {char *out;size_t cap,used;unsigned count;} IniEnum;
static int ini_enum_cb(const char *k,const char *v,void *ctx){
    (void)v;IniEnum *e=ctx;size_t n=strlen(k)+1;
    if(e->used+n+1>e->cap)return 1;
    memcpy(e->out+e->used,k,n);e->used+=n;e->out[e->used]=0;++e->count;return 0;
}
typedef struct {char *out;size_t cap;size_t used;unsigned hit;} IniValue;
static int ini_value_cb(const char *k,const char *v,void *ctx){
    (void)k;IniValue *s=ctx;size_t n=strlen(v);
    if(n>=s->cap)n=s->cap? s->cap-1:0;
    memcpy(s->out,v,n);s->out[n]=0;s->used=n;s->hit=1;return 0;
}
static void s_GetPrivateProfileStringA(CPU *c){
    uint32_t secp=A(0),keyp=A(1),defp=A(2),outp=A(3),cap=A(4);
    const char *name=guest_cstr(mem,A(5),1024);
    if(!name||!outp||!cap||!guest_ptr(mem,outp,cap))FAIL(ERR_INVALID_PARAMETER,0,6);
    const char *section=secp?str(c,secp):NULL,*key=keyp?str(c,keyp):NULL;
    char *out=guest_ptr(mem,outp,cap);out[0]=0;
    if(!section){ /* enumerate section names */
        IniEnum e={out,cap,0,0};
        size_t len;char *buf=ini_read(name,&len);
        if(buf){char *p=buf;while(*p){char *nl=strchr(p,'\n');if(nl)*nl=0;char *l=trim(p);
            if(*l=='['){char *q=strchr(l+1,']');if(q){*q=0;size_t n=strlen(l+1)+1;
                if(e.used+n+1<=e.cap){memcpy(e.out+e.used,l+1,n);e.used+=n;e.out[e.used]=0;++e.count;}}}
            if(!nl)break;p=nl+1;}free(buf);}
        set_error(c,0);RET((uint32_t)e.count,6);
    }
    if(!key){ /* enumerate keys in one section */
        IniEnum e={out,cap,0,0};ini_each(name,section,NULL,ini_enum_cb,&e);
        set_error(c,0);RET((uint32_t)e.count,6);
    }
    IniValue v={out,cap,0,0};
    ini_each(name,section,key,ini_value_cb,&v);
    if(!v.hit && defp){const char *d=str(c,defp);size_t n=strlen(d);if(n>=cap)n=cap-1;memcpy(out,d,n);out[n]=0;v.used=n;}
    set_error(c,0);RET((uint32_t)v.used,6);
}
static void s_GetPrivateProfileIntA(CPU *c){
    uint32_t secp=A(0),keyp=A(1),def=A(2);
    const char *name=guest_cstr(mem,A(3),1024);
    if(!name)FAIL(ERR_INVALID_PARAMETER,0,4);
    char out[64];
    const char *section=secp?str(c,secp):NULL,*key=keyp?str(c,keyp):NULL;
    if(!section||!key)RET(def,4);
    IniValue v={out,sizeof out,0,0};
    ini_each(name,section,key,ini_value_cb,&v);
    if(!v.hit)RET(def,4);
    char *end;long n=strtol(out,&end,10);
    if(end==out)RET(def,4);
    set_error(c,0);RET((uint32_t)n,4);
}
static void s_WritePrivateProfileStringA(CPU *c){
    const char *section=guest_cstr(mem,A(0),1024);
    const char *key=A(1)?guest_cstr(mem,A(1),1024):NULL;
    const char *val=A(2)?guest_cstr(mem,A(2),1024):NULL;
    const char *name=guest_cstr(mem,A(3),1024);
    if(!section||!*section||!name)FAIL(ERR_INVALID_PARAMETER,0,4);
    if(key&&!*key)FAIL(ERR_INVALID_PARAMETER,0,4);
    size_t len=0;char *buf=ini_read(name,&len);
    size_t cap=len+4096+(key?strlen(key):0)+(val?strlen(val):0);
    char *out=malloc(cap?cap:1);
    if(!out){free(buf);FAIL(ERR_NO_MEMORY,0,4);}
    size_t used=0;int cur=0,seen=0,done=0;char *p=buf?buf:"";
    while(*p){
        char *nl=strchr(p,'\n');size_t n=nl?(size_t)(nl-p):strlen(p);
        char line[4096];if(n>=sizeof line)n=sizeof line-1;
        memcpy(line,p,n);line[n]=0;
        char *l=trim(line);
        int hdr=(*l=='[');
        int match=hdr&&is_section(l,section);
        if(hdr){
            if(cur&&!match&&!done&&key){int m=snprintf(out+used,cap-used,"%s=%s\n",key,val?val:"");if(m>0)used+=(size_t)m;done=1;}
            cur=match;if(match)seen=1;
            if(cur&&!key)goto next;          /* delete: drop header and body */
        }else if(cur){
            if(!key)goto next;               /* delete: drop body */
            if(*l&&strchr(l,'=')){
                char *eq=strchr(l,'=');*eq=0;
                if(ci_eq(trim(l),key)){
                    if(val){int m=snprintf(out+used,cap-used,"%s=%s\n",key,val);if(m>0)used+=(size_t)m;}
                    done=1;goto next;
                }
            }
        }
        memcpy(out+used,p,n);used+=n;out[used++]='\n';
next:   if(!nl)break;p=nl+1;
    }
    if(key&&!done){
        if(!seen){cur=1;int m=snprintf(out+used,cap-used,"[%s]\n",section);if(m>0)used+=(size_t)m;}
        int m=snprintf(out+used,cap-used,"%s=%s\n",key,val?val:"");if(m>0)used+=(size_t)m;
    }
    char path[1024];int ok=0;
    if(!path_for(name,path,sizeof path) && !mkpath_parent(path)){
        FILE *f=fopen(path,"wb");
        if(f){ok=fwrite(out,1,used,f)==used;fclose(f);}
    }
    free(out);free(buf);
    if(!ok)FAIL(ERR_ACCESS_DENIED,0,4);
    set_error(c,0);RET(1,4);
}

/* ---- file metadata ------------------------------------------------------------- */
static int path_stat(const char *path,struct stat *st){
#ifdef _WIN32
    return _stat64(path,(struct _stat64*)st);
#else
    return stat(path,st);
#endif
}
static uint64_t ft_from_stat_time(time_t t){return unix_to_ft((int64_t)t);}
static int set_file_times(const char *path,time_t at,time_t wt){
#ifdef _WIN32
    struct _utimbuf ut;ut.actime=at;ut.modtime=wt;return _utime(path,&ut);
#else
    struct utimbuf ut;ut.actime=at;ut.modtime=wt;return utime(path,&ut);
#endif
}
static void s_GetFileInformationByHandle(CPU *c){
    uint32_t h=A(0),p=A(1);FILE *f=file_for(h);
    if(!f)FAIL(ERR_INVALID_HANDLE,0,2);
    uint8_t *b=checked(c,p,52);struct stat st;
    if(path_stat(file_path[h-0x100],&st))FAIL(ERR_READ_FAULT,0,2);
    memset(b,0,52);
    M32(p)=S_ISDIR(st.st_mode)?0x00000010u:0x00000080u;
    uint64_t ct=ft_from_stat_time(st.st_ctime),at=ft_from_stat_time(st.st_atime),wt=ft_from_stat_time(st.st_mtime);
    memcpy(b+4,&ct,8);memcpy(b+12,&at,8);memcpy(b+20,&wt,8);
    M32(p+28)=0;                       /* no volume serial on this file system */
    uint64_t size=(uint64_t)st.st_size;
    M32(p+32)=(uint32_t)(size>>32);M32(p+36)=(uint32_t)size;
    M32(p+40)=1;M32(p+44)=0;M32(p+48)=0; /* one link; no stable file index */
    set_error(c,0);RET(1,2);
}
static void s_GetFileTime(CPU *c){
    uint32_t h=A(0),ct=A(1),at=A(2),wt=A(3);FILE *f=file_for(h);
    if(!f)FAIL(ERR_INVALID_HANDLE,0,4);
    struct stat st;if(path_stat(file_path[h-0x100],&st))FAIL(ERR_READ_FAULT,0,4);
    uint64_t c0=ft_from_stat_time(st.st_ctime),a0=ft_from_stat_time(st.st_atime),w0=ft_from_stat_time(st.st_mtime);
    if(ct){uint8_t *b=checked(c,ct,8);memcpy(b,&c0,8);} if(at){uint8_t *b=checked(c,at,8);memcpy(b,&a0,8);} if(wt){uint8_t *b=checked(c,wt,8);memcpy(b,&w0,8);}
    set_error(c,0);RET(1,4);
}
static void s_SetFileTime(CPU *c){
    uint32_t h=A(0),ct=A(1),at=A(2),wt=A(3);FILE *f=file_for(h);
    if(!f)FAIL(ERR_INVALID_HANDLE,0,4);
    if(!file_can_write[h-0x100])FAIL(ERR_ACCESS_DENIED,0,4);
    if(ct)FAIL(ERR_NOT_SUPPORTED,0,4); /* creation time does not exist on this host FS */
    struct stat st;if(path_stat(file_path[h-0x100],&st))FAIL(ERR_READ_FAULT,0,4);
    time_t a0=st.st_atime,w0=st.st_mtime;
    if(at){uint64_t v;memcpy(&v,checked(c,at,8),8);
        if(v&&v<FILETIME_UNIX_EPOCH)FAIL(ERR_INVALID_PARAMETER,0,4);
        a0=v?(time_t)((v-FILETIME_UNIX_EPOCH)/UINT64_C(10000000)):(time_t)0;}
    if(wt){uint64_t v;memcpy(&v,checked(c,wt,8),8);
        if(v&&v<FILETIME_UNIX_EPOCH)FAIL(ERR_INVALID_PARAMETER,0,4);
        w0=v?(time_t)((v-FILETIME_UNIX_EPOCH)/UINT64_C(10000000)):(time_t)0;}
    if((at||wt) && set_file_times(file_path[h-0x100],a0,w0))FAIL(ERR_WRITE_FAULT,0,4);
    set_error(c,0);RET(1,4);
}
static void move_file(CPU *c,const char *from,const char *to){
    char a[1024],b[1024];struct stat st;
    if(!from||!to||path_for(from,a,sizeof a)||path_for(to,b,sizeof b))FAIL(ERR_INVALID_PARAMETER,0,2);
    if(stat(b,&st)==0)FAIL(ERR_ALREADY_EXISTS,0,2);
    errno=0;
    if(rename(a,b))FAIL(errno_error(),0,2);
    set_error(c,0);RET(1,2);
}
static void s_MoveFileA(CPU *c){
    const char *from=guest_cstr(mem,A(0),1024),*to=guest_cstr(mem,A(1),1024);
    if(!from||!to)FAIL(ERR_INVALID_PARAMETER,0,2);
    move_file(c,from,to);
}

/* ---- environment ---------------------------------------------------------------- */
/* Entries are rebuilt in the guest PEB block (GP_ENV_A/GP_ENV_W, 256 bytes each).
 * A change that does not fit fails with ERROR_INSUFFICIENT_BUFFER instead of
 * silently truncating the environment. */
static void s_SetEnvironmentVariableA(CPU *c){
    const char *name=guest_cstr(mem,A(0),1024);
    const char *val=A(1)?guest_cstr(mem,A(1),1024):NULL;
    if(!name||!*name||strchr(name,'='))FAIL(ERR_INVALID_PARAMETER,0,2);
    char *block=guest_ptr(mem,GP_ENV_A,1);
    if(!block)FAIL(ERR_NO_MEMORY,0,2);
    char build[256];size_t used=0;int replaced=0;
    const char *p=block;
    while(p && *p){
        size_t n=strlen(p);
        if(ci_prefix(p,name) && p[strlen(name)]=='='){
            if(val){int m=snprintf(build+used,sizeof build-used,"%s=%s",name,val);
                if(m<0||(size_t)m>=sizeof build-used)FAIL(ERR_INSUFFICIENT_BUFFER,0,2);used+=(size_t)m;build[used++]=0;}
            replaced=1;
        } else {
            if(used+n+2>sizeof build)FAIL(ERR_INSUFFICIENT_BUFFER,0,2);
            memcpy(build+used,p,n+1);used+=n+1;
        }
        p+=n+1;
    }
    if(!replaced && val){
        size_t n=strlen(name)+strlen(val)+2;
        if(used+n+2>sizeof build)FAIL(ERR_INSUFFICIENT_BUFFER,0,2);
        memcpy(build+used,name,strlen(name));used+=strlen(name);
        build[used++]='=';memcpy(build+used,val,strlen(val)+1);used+=strlen(val)+1;
    }
    build[used++]=0;
    memcpy(block,build,used);
    /* wide copy */
    uint8_t *w=guest_ptr(mem,GP_ENV_W,(uint64_t)used*2);
    if(w){size_t i=0;for(;i<used;++i){uint16_t u=(uint8_t)build[i];w[i*2]=(uint8_t)u;w[i*2+1]=(uint8_t)(u>>8);}}
    set_error(c,0);RET(1,2);
}

/* ---- volume / timing ------------------------------------------------------------ */
#ifdef __3DS__
#include <sys/statvfs.h>
#endif
static void s_GetDiskFreeSpaceExA(CPU *c){
    uint32_t dirp=A(0),freep=A(1),totalp=A(2),availp=A(3);
    uint64_t freeb=0,totalb=0;int rc=-1;
    char host[1024];const char *dir=guest_cstr(mem,dirp,1024);
    if(!dir)FAIL(ERR_INVALID_PARAMETER,0,4);
    if(!path_for(dir,host,sizeof host)){
#ifdef _WIN32
        ULARGE_INTEGER f,t,a;
        if(GetDiskFreeSpaceExA(host,&f,&t,&a)){freeb=f.QuadPart;totalb=t.QuadPart;rc=0;}
#elif defined(__3DS__)
        struct statvfs v;if(statvfs(host,&v)==0){freeb=(uint64_t)v.f_bavail*v.f_frsize;totalb=(uint64_t)v.f_blocks*v.f_frsize;rc=0;}
#endif
    }
    if(rc){FAIL(ERR_FILE_NOT_FOUND,0,4);}
    if(freep){uint8_t *b=checked(c,freep,8);memcpy(b,&freeb,8);}
    if(totalp){uint8_t *b=checked(c,totalp,8);memcpy(b,&totalb,8);}
    if(availp){uint8_t *b=checked(c,availp,8);memcpy(b,&freeb,8);}
    set_error(c,0);RET(1,4);
}
static void s_SleepEx(CPU *c){
    uint32_t ms=A(0),alert=A(1);
    if(ms==UINT32_MAX)rt_unimpl(c,0,"SleepEx(INFINITE) cannot be ended (no APCs)");
    if(ms>=1u){static unsigned logged;if(logged<20u){++logged;fprintf(stderr,"[sync] SleepEx %lu ms from %08x thread=%u\n",(unsigned long)ms,M32(c->r[4]),win32_thread_current_id());}}
    win32_thread_sleep(ms);
    (void)alert;set_error(c,0);RET(0,2); /* no APCs are ever queued */
}

/* ---- threads ---------------------------------------------------------------
 * Lifecycle thunks live in win32_thread.c; only the ideal-processor hint and
 * the current-thread id remain here. */
static void s_SetThreadIdealProcessor(CPU *c){
    if(!guest_ptr(mem,A(0),1)){}
    set_error(c,0);RET(0,2); /* previous ideal processor 0 */
}

/* ---- wide-character file/directory helpers ------------------------------------- */
/* Convert a guest UTF-16 string to ASCII. Non-ASCII code units and over-long
 * strings fail instead of being silently truncated or transliterated. */
static int wide_arg(uint32_t addr,char *out,size_t cap){
    if(!addr||!cap)return -1;
    for(size_t i=0;i<cap;++i){
        uint16_t ch=M16(addr+(uint32_t)(i*2));
        if(!ch){out[i]=0;return 0;}
        if(ch>0x7f)return -1;
        out[i]=(char)ch;
    }
    return -1;
}
static void wide_out(uint32_t addr,const char *s){
    for(size_t i=0;i<=strlen(s);++i)M16(addr+(uint32_t)(i*2))=(uint8_t)s[i];
}
static void s_CreateFileW(CPU *c){
    char name[1024];
    if(wide_arg(A(0),name,sizeof name))FAIL(ERR_INVALID_PARAMETER,INVALID_HANDLE,7);
    { static unsigned n; if(n<60u){++n;fprintf(stderr,"[file] W %s\n",name);} }
    create_file(c,name);
}
static void s_DeleteFileW(CPU *c){
    char name[1024];
    if(wide_arg(A(0),name,sizeof name))FAIL(ERR_INVALID_PARAMETER,0,1);
    delete_file(c,name);
}
static void s_MoveFileW(CPU *c){
    char from[1024],to[1024];
    if(wide_arg(A(0),from,sizeof from)||wide_arg(A(1),to,sizeof to))
        FAIL(ERR_INVALID_PARAMETER,0,2);
    move_file(c,from,to);
}
static void s_GetFullPathNameW(CPU *c){
    char name[1024],probe[1024],out[1024];
    if(wide_arg(A(0),name,sizeof name)||path_for(name,probe,sizeof probe))
        FAIL(ERR_INVALID_PARAMETER,0,4);
    if(A(3)&&!guest_ptr(mem,A(3),4))FAIL(ERR_INVALID_PARAMETER,0,4);
    const char *src=!strcmp(cwd,".")?name:probe;
    size_t len=strlen(src);if(len>=sizeof out)FAIL(ERR_INVALID_PARAMETER,0,4);
    for(size_t i=0;i<=len;++i)out[i]=src[i]=='/'?'\\':src[i];
    uint32_t n=A(1),buf=A(2);
    if(!buf||!n||n<len+1)RET((uint32_t)len+1,4);
    if(!guest_ptr(mem,buf,(uint32_t)((len+1)*2)))FAIL(ERR_INVALID_PARAMETER,0,4);
    wide_out(buf,out);
    if(A(3)){const char *last=strrchr(out,'\\');M32(A(3))=buf+(uint32_t)(last?last-out+1:0)*2;}
    set_error(c,0);RET((uint32_t)len,4);
}
static void s_GetModuleFileNameW(CPU *c){
    uint32_t h=A(0)?A(0):module_handle(NULL);const char *name=module_name(h);uint32_t n=A(2);
    if(!name||!n||!guest_ptr(mem,A(1),(uint64_t)n*2))FAIL(ERR_INVALID_PARAMETER,0,3);
    char path[128];int len=snprintf(path,sizeof path,"C:\\Skyrim\\%s",name);
    if(len<0||(size_t)len>=sizeof path)FAIL(ERR_INVALID_PARAMETER,0,3);
    size_t copy=(size_t)len<n-1?(size_t)len:n-1;
    for(size_t i=0;i<copy;++i)M16(A(1)+(uint32_t)(i*2))=(uint8_t)path[i];
    M16(A(1)+(uint32_t)(copy*2))=0;
    if((uint32_t)len>=n)FAIL(122,(uint32_t)len,3);
    set_error(c,0);RET((uint32_t)len,3);
}
static void s_GetTempPathW(CPU *c){
    static const char dir[]="Temp\\";
    uint32_t n=A(0),buf=A(1);size_t need=sizeof dir;
    if(!buf||n<need)RET((uint32_t)need,2);
    if(!guest_ptr(mem,buf,(uint32_t)(need*2)))FAIL(ERR_INVALID_PARAMETER,0,2);
    for(size_t i=0;i<need;++i)M16(buf+(uint32_t)(i*2))=(uint8_t)dir[i];
    set_error(c,0);RET((uint32_t)(need-1),2);
}
/* Create one empty file named <dir>/<prefix><hex>.TMP; returns the number used
 * (0 on failure) and the guest-style relative name. */
static uint32_t create_temp_file(CPU *c,const char *dir,const char *prefix,char *relout,size_t cap){
    char rel[1024],host[1024];
    if(!dir||!prefix)return 0;
    if(!*dir){memcpy(rel,"Temp",5);}
    else{
        size_t n=strlen(dir);while(n>0&&(dir[n-1]=='/'||dir[n-1]=='\\'))--n;
        if(!n||n>=sizeof rel)return 0;
        memcpy(rel,dir,n);rel[n]=0;
    }
    if(path_for(rel,host,sizeof host))return 0;
    char pfx[4]={'T','M','P',0};
    for(unsigned i=0;i<3&&prefix[i];++i)pfx[i]=(char)toupper((unsigned char)prefix[i]);
    unsigned start=A(2)&0xffff;if(!start)start=1;
    for(unsigned tries=0;tries<0x10000;++tries){
        unsigned u=(start+tries)&0xffff;if(!u)continue;
        char leaf[16];snprintf(leaf,sizeof leaf,"%s%04X.TMP",pfx,u);
        char full[1280];int m=snprintf(full,sizeof full,"%s/%s",host,leaf);
        if(m<0||(size_t)m>=sizeof full)return 0;
        FILE *f=fopen(full,"wbx");
        if(f){
            fclose(f);
            char out[1024];int k=snprintf(out,sizeof out,"%s\\%s",rel,leaf);
            if(k<0||(size_t)k>=cap||(size_t)k>=sizeof out)return 0;
            memcpy(relout,out,(size_t)k+1);
            return u;
        }
        if(errno!=EEXIST){set_error(c,errno_error());return 0;}
    }
    set_error(c,ERR_FILE_EXISTS);
    return 0;
}
static void s_GetTempFileNameA(CPU *c){
    const char *dir=guest_cstr(mem,A(0),1024);const char *prefix=guest_cstr(mem,A(1),16);
    uint32_t out=A(3);
    if(!dir||!prefix||!out||!guest_ptr(mem,out,260))FAIL(ERR_INVALID_PARAMETER,0,4);
    char rel[1024];uint32_t u=create_temp_file(c,dir,prefix,rel,sizeof rel);
    if(!u)RET(0,4);
    memcpy(guest_ptr(mem,out,260),rel,strlen(rel)+1);
    set_error(c,0);RET(u,4);
}
static void s_GetTempFileNameW(CPU *c){
    char dir[1024],prefix[16];
    if(wide_arg(A(0),dir,sizeof dir)||wide_arg(A(1),prefix,sizeof prefix))FAIL(ERR_INVALID_PARAMETER,0,4);
    uint32_t out=A(3);
    if(!out||!guest_ptr(mem,out,520))FAIL(ERR_INVALID_PARAMETER,0,4);
    char rel[1024];uint32_t u=create_temp_file(c,dir,prefix,rel,sizeof rel);
    if(!u)RET(0,4);
    wide_out(out,rel);
    set_error(c,0);RET(u,4);
}

/* ---- fixed system directories --------------------------------------------------- */
static void dir_answer(CPU *c,const char *dir){
    uint32_t n=A(0),buf=A(1);size_t len=strlen(dir);
    if(!buf||n<=len)RET((uint32_t)len+1,2);
    memcpy(checked(c,buf,(uint32_t)len+1),dir,len+1);
    set_error(c,0);RET((uint32_t)len,2);
}
static void s_GetWindowsDirectoryA(CPU *c){dir_answer(c,"C:\\Windows");}
static void s_GetSystemDirectoryA(CPU *c){dir_answer(c,"C:\\Windows\\System32");}

/* ---- module loading variants ---------------------------------------------------- */
static void s_LoadLibraryExA(CPU *c){
    uint32_t flags=A(2);
    if(flags&~0x0000000Au)FAIL(ERR_INVALID_PARAMETER,0,3);
    uint32_t h=module_handle(guest_cstr(mem,A(0),1024));
    if(!h)FAIL(126,0,3);
    set_error(c,0);RET(h,3);
}
static void s_LoadLibraryExW(CPU *c){
    char name[1024];
    if(wide_arg(A(0),name,sizeof name)||(A(2)&~0x0000000Au))FAIL(ERR_INVALID_PARAMETER,0,3);
    uint32_t h=module_handle(name);
    if(!h)FAIL(126,0,3);
    set_error(c,0);RET(h,3);
}
static void s_GetModuleHandleExW(CPU *c){
    uint32_t flags=A(0),value=A(1),outp=A(2);
    if(!outp||!guest_ptr(mem,outp,4)||(flags&~7u))FAIL(ERR_INVALID_PARAMETER,0,3);
    uint32_t h=0;
    if(flags&4u){
        if(process_linker)for(size_t i=0;i<process_linker->count;++i){
            const GuestImage *im=process_linker->modules[i].image;
            if(im&&value>=im->base&&value-im->base<im->size)h=im->base;
        }
    }else if(value){
        char name[1024];if(wide_arg(value,name,sizeof name))FAIL(ERR_INVALID_PARAMETER,0,3);
        h=module_handle(name);
    }else h=module_handle(NULL);
    if(!h)FAIL(126,0,3);
    M32(outp)=h;set_error(c,0);RET(1,3);
}
static void s_GetStartupInfoW(CPU *c){uint32_t p=A(0);memset(checked(c,p,68),0,68);M32(p)=68;RET(0,1);}
static void s_OutputDebugStringW(CPU *c){
    char text[1024];size_t i=0;
    if(!A(0))RET(0,1);
    for(;i<sizeof text-1;++i){uint16_t ch=M16(A(0)+(uint32_t)(i*2));if(!ch)break;text[i]=ch<0x80?(char)ch:'?';}
    text[i]=0;fprintf(stderr,"[guest] %s\n",text);RET(0,1);
}
static void s_ReadConsoleW(CPU *c){
    uint32_t out=A(3);
    if(out&&guest_ptr(mem,out,4))M32(out)=0;
    fprintf(stderr,"[guest] ReadConsoleW: this process has no Win32 console input\n");
    FAIL(ERR_INVALID_HANDLE,0,5);
}
static void s_IsDBCSLeadByte(CPU *c){(void)A(0);RET(0,1); /* code page 1252 has no lead bytes */}
static void s_DisableThreadLibraryCalls(CPU *c){(void)A(0);RET(1,1);}

/* D3DPERF performance markers: no profiler is attached, so the option is
 * accepted as a documented no-op and logged once. */
static int d3dperf_logged;
static void s_D3DPERF_SetOptions(CPU *c){
    if(!d3dperf_logged){
        d3dperf_logged=1;
        fprintf(stderr,"[d3d9] D3DPERF_SetOptions(%lu): performance markers are not "
                       "implemented; accepted as a no-op\n",(unsigned long)A(0));
    }
    RET(0,1);
}

/* ---- explicitly unsupported Win32 surfaces -------------------------------------- */
static void s_PeekNamedPipe(CPU *c){(void)c;FAIL(ERR_INVALID_HANDLE,0,6);}
static void s_ReadFileEx(CPU *c){FAIL(ERR_NOT_SUPPORTED,0,5);}
static void s_WriteFileEx(CPU *c){FAIL(ERR_NOT_SUPPORTED,0,5);}
/* The documented SetThreadName helper raises 0x406d1388 with
 * {0x1000, name, thread_id, 0} inside __except(CONTINUE_EXECUTION).
 * Support that exact benign pattern; every other raise remains an explicit
 * refusal until a broader proven SEH dispatcher exists. */
static void s_RaiseException(CPU *c){
    if(A(0)==0x406d1388u && A(1)==0 && A(2)==4){
        uint32_t info[4]={0};
        int ok=!guest_read(mem,A(3),info,sizeof info);
        if(ok && info[0]==0x1000u && info[3]==0u){
            fprintf(stderr,"[guest] SetThreadName exception name=%08lx tid=%08lx: continue execution\n",
                    (unsigned long)info[1],(unsigned long)info[2]);
            RET(0,4);
        }
    }
    uint32_t ret=M32(c->r[4]),args=A(3);
    fprintf(stderr,"[guest] RaiseException code=%08lx flags=%08lx args=%lu ret=%08lx esp=%08lx fs=%08lx: "
                    "structured exception dispatch is not implemented\n",
            (unsigned long)A(0),(unsigned long)A(1),(unsigned long)A(2),
            (unsigned long)ret,(unsigned long)c->r[4],(unsigned long)c->fs_base);
    unsigned n=A(2)<16u?A(2):16u;
    for(unsigned i=0;i<n;++i){uint32_t v=0;if(!guest_read32(mem,args+4*i,&v))fprintf(stderr,"[seh] arg[%u]=%08lx\n",i,(unsigned long)v);}
    uint32_t cur=0;
    if(c->fs_base && !guest_read32(mem,c->fs_base,&cur)){
        for(unsigned i=0;i<8 && cur;++i){
            uint32_t next=0,handler=0,scope=0,filter=0,safe=0;
            if(guest_read32(mem,cur,&next)||guest_read32(mem,cur+4,&handler))break;
            (void)guest_read32(mem,cur+8,&scope);(void)guest_read32(mem,cur+12,&filter);(void)guest_read32(mem,cur+16,&safe);
            fprintf(stderr,"[seh] frame[%u]=%08lx next=%08lx handler=%08lx scope=%08lx filter=%08lx safe_ebp=%08lx\n",
                    i,(unsigned long)cur,(unsigned long)next,(unsigned long)handler,
                    (unsigned long)scope,(unsigned long)filter,(unsigned long)safe);
            cur=next;
        }
    }
    rt_unimpl(c,A(0),"RaiseException: SEH dispatch is not implemented");
}
static void s_RtlUnwind(CPU *c){
    fprintf(stderr,"[guest] RtlUnwind target=%08lx: structured unwinding is not implemented\n",
            (unsigned long)A(1));
    rt_unimpl(c,A(0),"RtlUnwind: SEH unwinding is not implemented");
}

/* ---- Bink video / sound middleware -----------------------------------------
 * binkw32.dll is mapped at 0x09000000 and its exports are real x86 bodies, but
 * it is not lifted or predecoded: a call into 0x0900d630 (_BinkSetSoundSystem)
 * was the first post-thread blocker.  Bink video has no 3DS implementation, so
 * the open/sound entry points return a documented NULL instead of executing
 * unlifted code; frame/close operations on a handle that BinkOpen never
 * produced are explicit named refusals.  This is middleware skipping, not a
 * faked render: nothing is decoded, copied or presented. */
static int bink_logged;
static void bink_skip(const char *what){
    if(bink_logged)return;
    bink_logged=1;
    fprintf(stderr,"[bink] %s: Bink video/sound is not implemented; returning NULL "
                   "(middleware skipped, no frame decoded)\n",what);
}
static void s_BinkOpen(CPU *c){
    bink_skip("_BinkOpen@8");
    RET(0,2);
}
static void s_BinkOpenDirectSound(CPU *c){
    bink_skip("_BinkOpenDirectSound@4");
    RET(0,1);
}
static void s_BinkSetSoundSystem(CPU *c){
    bink_skip("_BinkSetSoundSystem@8");
    RET(0,2); /* no previous sound system */
}
static void s_BinkClose(CPU *c){
    if(A(0))rt_unimpl(c,A(0),"_BinkClose@4: Bink video is not implemented (no Bink handle exists)");
    RET(0,1);
}
static void s_BinkDoFrame(CPU *c){
    rt_unimpl(c,A(0),"_BinkDoFrame@4: Bink video is not implemented (no Bink handle exists)");
}
static void s_BinkNextFrame(CPU *c){
    rt_unimpl(c,A(0),"_BinkNextFrame@4: Bink video is not implemented (no Bink handle exists)");
}
static void s_BinkWait(CPU *c){
    rt_unimpl(c,A(0),"_BinkWait@4: Bink video is not implemented (no Bink handle exists)");
}
static void s_BinkPause(CPU *c){
    rt_unimpl(c,A(0),"_BinkPause@8: Bink video is not implemented (no Bink handle exists)");
}
static void s_BinkCopyToBufferRect(CPU *c){
    rt_unimpl(c,A(0),"_BinkCopyToBufferRect@44: Bink video is not implemented (no Bink handle exists)");
}

#define BINK(X) \
 X("binkw32.dll","_BinkOpen@8",BinkOpen) X("binkw32.dll","_BinkOpenDirectSound@4",BinkOpenDirectSound) \
 X("binkw32.dll","_BinkSetSoundSystem@8",BinkSetSoundSystem) X("binkw32.dll","_BinkClose@4",BinkClose) \
 X("binkw32.dll","_BinkDoFrame@4",BinkDoFrame) X("binkw32.dll","_BinkNextFrame@4",BinkNextFrame) \
 X("binkw32.dll","_BinkWait@4",BinkWait) X("binkw32.dll","_BinkPause@8",BinkPause) \
 X("binkw32.dll","_BinkCopyToBufferRect@44",BinkCopyToBufferRect)

#define K(X) \
 X("kernel32.dll","GetLastError",GetLastError) X("kernel32.dll","SetLastError",SetLastError) \
 X("kernel32.dll","GetProcessHeap",GetProcessHeap) X("kernel32.dll","HeapAlloc",HeapAlloc) \
 X("kernel32.dll","HeapFree",HeapFree) X("kernel32.dll","HeapSize",HeapSize) X("kernel32.dll","HeapReAlloc",HeapReAlloc) \
 X("kernel32.dll","HeapCreate",HeapCreate) X("kernel32.dll","HeapDestroy",HeapDestroy) \
 X("kernel32.dll","VirtualAlloc",VirtualAlloc) X("kernel32.dll","VirtualFree",VirtualFree) \
 X("kernel32.dll","GetTickCount",GetTickCount) X("kernel32.dll","QueryPerformanceCounter",QueryPerformanceCounter) \
 X("kernel32.dll","QueryPerformanceFrequency",QueryPerformanceFrequency) X("kernel32.dll","GetCurrentProcessId",GetCurrentProcessId) \
 X("kernel32.dll","GetCurrentThreadId",GetCurrentThreadId) X("kernel32.dll","Sleep",Sleep) \
 X("kernel32.dll","GetCurrentProcess",GetCurrentProcess) X("kernel32.dll","GetCurrentThread",GetCurrentThread) \
 X("kernel32.dll","TlsAlloc",TlsAlloc) X("kernel32.dll","TlsFree",TlsFree) X("kernel32.dll","TlsSetValue",TlsSetValue) X("kernel32.dll","TlsGetValue",TlsGetValue) \
 X("kernel32.dll","FlsAlloc",FlsAlloc) X("kernel32.dll","FlsFree",FlsFree) X("kernel32.dll","FlsSetValue",FlsSetValue) X("kernel32.dll","FlsGetValue",FlsGetValue) \
 X("kernel32.dll","SetHandleCount",SetHandleCount) X("kernel32.dll","CopyFileA",CopyFileA) \
 X("advapi32.dll","GetUserNameA",GetUserNameA) \
 X("user32.dll","GetActiveWindow",GetActiveWindow) X("user32.dll","DispatchMessageA",DispatchMessageA) \
 X("ole32.dll","CoInitialize",CoInitialize) X("ole32.dll","CoInitializeEx",CoInitializeEx) X("ole32.dll","CoUninitialize",CoUninitialize) X("ole32.dll","CoCreateInstance",CoCreateInstance) \
 X("kernel32.dll","GetSystemInfo",GetSystemInfo) X("kernel32.dll","GetLogicalProcessorInformation",GetLogicalProcessorInformation) X("kernel32.dll","GetSystemTimeAsFileTime",GetSystemTimeAsFileTime) \
 X("kernel32.dll","ExitProcess",ExitProcess) X("kernel32.dll","GetCommandLineA",GetCommandLineA) X("kernel32.dll","GetCommandLineW",GetCommandLineW) \
 X("kernel32.dll","GetEnvironmentStrings",GetEnvironmentStringsA) X("kernel32.dll","GetEnvironmentStringsA",GetEnvironmentStringsA) X("kernel32.dll","GetEnvironmentStringsW",GetEnvironmentStringsW) \
 X("kernel32.dll","FreeEnvironmentStringsA",FreeEnvironmentStringsA) X("kernel32.dll","FreeEnvironmentStringsW",FreeEnvironmentStringsW) \
 X("kernel32.dll","GetStartupInfoA",GetStartupInfoA) X("kernel32.dll","GetModuleHandleA",GetModuleHandleA) X("kernel32.dll","GetProcAddress",GetProcAddress) \
 X("kernel32.dll","LoadLibraryA",LoadLibraryA) X("kernel32.dll","FreeLibrary",FreeLibrary) X("kernel32.dll","GetModuleFileNameA",GetModuleFileNameA) \
 X("kernel32.dll","InterlockedIncrement",InterlockedIncrement) X("kernel32.dll","InterlockedDecrement",InterlockedDecrement) \
 X("kernel32.dll","InterlockedExchange",InterlockedExchange) X("kernel32.dll","InterlockedCompareExchange",InterlockedCompareExchange) \
 X("kernel32.dll","lstrlenA",lstrlenA) X("kernel32.dll","lstrcmpiA",lstrcmpiA) X("kernel32.dll","OutputDebugStringA",OutputDebugStringA) \
 X("kernel32.dll","CreateFileA",CreateFileA) X("kernel32.dll","ReadFile",ReadFile) X("kernel32.dll","CloseHandle",CloseHandle) \
 X("kernel32.dll","CreateFileMappingA",CreateFileMappingA) X("kernel32.dll","MapViewOfFile",MapViewOfFile) X("kernel32.dll","UnmapViewOfFile",UnmapViewOfFile) \
 X("kernel32.dll","WriteFile",WriteFile) X("kernel32.dll","SetFilePointerEx",SetFilePointerEx) \
 X("kernel32.dll","SetEndOfFile",SetEndOfFile) X("kernel32.dll","FlushFileBuffers",FlushFileBuffers) \
 X("kernel32.dll","GetFileSizeEx",GetFileSizeEx) X("kernel32.dll","GetFileSize",GetFileSize) X("kernel32.dll","SetFilePointer",SetFilePointer) \
 X("kernel32.dll","GetFileAttributesA",GetFileAttributesA) X("kernel32.dll","GetFileAttributesExA",GetFileAttributesExA) \
 X("kernel32.dll","FindFirstFileA",FindFirstFileA) X("kernel32.dll","FindNextFileA",FindNextFileA) X("kernel32.dll","FindClose",FindClose) \
 X("kernel32.dll","CreateDirectoryA",CreateDirectoryA) X("kernel32.dll","DeleteFileA",DeleteFileA) \
 X("kernel32.dll","GetFullPathNameA",GetFullPathNameA) \
 X("kernel32.dll","InitializeCriticalSection",InitializeCriticalSection) \
 X("kernel32.dll","InitializeCriticalSectionAndSpinCount",InitializeCriticalSectionAndSpinCount) \
 X("kernel32.dll","DeleteCriticalSection",DeleteCriticalSection) X("kernel32.dll","EnterCriticalSection",EnterCriticalSection) \
 X("kernel32.dll","LeaveCriticalSection",LeaveCriticalSection) \
 X("kernel32.dll","GetCurrentDirectoryA",GetCurrentDirectoryA) X("kernel32.dll","SetCurrentDirectoryA",SetCurrentDirectoryA) \
 X("kernel32.dll","GetACP",GetACP) X("kernel32.dll","GetOEMCP",GetOEMCP) X("kernel32.dll","IsValidCodePage",IsValidCodePage) X("kernel32.dll","GetCPInfo",GetCPInfo) \
 X("kernel32.dll","MultiByteToWideChar",MultiByteToWideChar) X("kernel32.dll","WideCharToMultiByte",WideCharToMultiByte) \
 X("kernel32.dll","GetStringTypeA",GetStringTypeA) X("kernel32.dll","GetStringTypeW",GetStringTypeW) \
 X("kernel32.dll","LCMapStringW",LCMapStringW) X("kernel32.dll","LCMapStringA",LCMapStringA) \
 X("kernel32.dll","GetStdHandle",GetStdHandle) X("kernel32.dll","SetStdHandle",SetStdHandle) X("kernel32.dll","GetFileType",GetFileType) \
 X("kernel32.dll","IsDebuggerPresent",IsDebuggerPresent) X("kernel32.dll","IsProcessorFeaturePresent",IsProcessorFeaturePresent) \
 X("kernel32.dll","SetUnhandledExceptionFilter",SetUnhandledExceptionFilter) X("kernel32.dll","UnhandledExceptionFilter",UnhandledExceptionFilter) \
 X("kernel32.dll","TerminateProcess",TerminateProcess) X("kernel32.dll","GetModuleHandleW",GetModuleHandleW) \
 X("kernel32.dll","CompareFileTime",CompareFileTime) X("kernel32.dll","FileTimeToSystemTime",FileTimeToSystemTime) \
 X("kernel32.dll","SystemTimeToFileTime",SystemTimeToFileTime) X("kernel32.dll","FileTimeToLocalFileTime",FileTimeToLocalFileTime) \
 X("kernel32.dll","LocalFileTimeToFileTime",LocalFileTimeToFileTime) X("kernel32.dll","GetLocalTime",GetLocalTime) \
 X("kernel32.dll","GetTimeZoneInformation",GetTimeZoneInformation) X("kernel32.dll","GetDriveTypeA",GetDriveTypeA) \
 X("kernel32.dll","lstrcpyA",lstrcpyA) X("kernel32.dll","lstrcatA",lstrcatA) X("kernel32.dll","InterlockedExchangeAdd",InterlockedExchangeAdd) \
 X("kernel32.dll","DebugBreak",DebugBreak) X("kernel32.dll","LocalAlloc",LocalAlloc) X("kernel32.dll","LocalFree",LocalFree) \
 X("kernel32.dll","EncodePointer",EncodePointer) X("kernel32.dll","DecodePointer",DecodePointer) \
 X("kernel32.dll","FormatMessageA",FormatMessageA) \
 X("kernel32.dll","GetVersionExA",GetVersionExA) X("kernel32.dll","GetComputerNameA",GetComputerNameA) \
 X("kernel32.dll","GetConsoleCP",GetConsoleCP) X("kernel32.dll","GetConsoleOutputCP",GetConsoleOutputCP) \
 X("kernel32.dll","GetConsoleMode",GetConsoleMode) X("kernel32.dll","WriteConsoleA",WriteConsoleA) \
 X("kernel32.dll","WriteConsoleW",WriteConsoleW) X("kernel32.dll","EnumSystemLocalesA",EnumSystemLocalesA) \
 X("kernel32.dll","SystemTimeToTzSpecificLocalTime",SystemTimeToTzSpecificLocalTime) \
 X("kernel32.dll","IsBadWritePtr",IsBadWritePtr) X("kernel32.dll","GlobalMemoryStatusEx",GlobalMemoryStatusEx) \
 X("kernel32.dll","GetPrivateProfileStringA",GetPrivateProfileStringA) \
 X("kernel32.dll","GetPrivateProfileIntA",GetPrivateProfileIntA) \
 X("kernel32.dll","WritePrivateProfileStringA",WritePrivateProfileStringA) \
 X("kernel32.dll","GetFileInformationByHandle",GetFileInformationByHandle) \
 X("kernel32.dll","GetFileTime",GetFileTime) X("kernel32.dll","SetFileTime",SetFileTime) \
 X("kernel32.dll","MoveFileA",MoveFileA) X("kernel32.dll","SetEnvironmentVariableA",SetEnvironmentVariableA) \
 X("kernel32.dll","GetDiskFreeSpaceExA",GetDiskFreeSpaceExA) X("kernel32.dll","SleepEx",SleepEx) \
 X("kernel32.dll","SetThreadIdealProcessor",SetThreadIdealProcessor) \
 X("kernel32.dll","PeekNamedPipe",PeekNamedPipe) X("kernel32.dll","ReadFileEx",ReadFileEx) \
 X("kernel32.dll","WriteFileEx",WriteFileEx) X("kernel32.dll","RaiseException",RaiseException) \
 X("kernel32.dll","RtlUnwind",RtlUnwind) \
 X("kernel32.dll","VirtualQuery",VirtualQuery) X("kernel32.dll","GetVersion",GetVersion) \
 X("kernel32.dll","AreFileApisANSI",AreFileApisANSI) X("kernel32.dll","SetErrorMode",SetErrorMode) \
 X("kernel32.dll","GetEnvironmentVariableA",GetEnvironmentVariableA) \
 X("kernel32.dll","ExpandEnvironmentStringsA",ExpandEnvironmentStringsA) \
 X("kernel32.dll","GetTempPathA",GetTempPathA) X("kernel32.dll","IsBadReadPtr",IsBadReadPtr) \
 X("kernel32.dll","CreateFileW",CreateFileW) X("kernel32.dll","DeleteFileW",DeleteFileW) \
 X("kernel32.dll","MoveFileW",MoveFileW) X("kernel32.dll","GetFullPathNameW",GetFullPathNameW) \
 X("kernel32.dll","GetModuleFileNameW",GetModuleFileNameW) X("kernel32.dll","GetTempPathW",GetTempPathW) \
 X("kernel32.dll","GetTempFileNameA",GetTempFileNameA) X("kernel32.dll","GetTempFileNameW",GetTempFileNameW) \
 X("kernel32.dll","GetWindowsDirectoryA",GetWindowsDirectoryA) X("kernel32.dll","GetSystemDirectoryA",GetSystemDirectoryA) \
 X("kernel32.dll","IsDBCSLeadByte",IsDBCSLeadByte) X("kernel32.dll","DisableThreadLibraryCalls",DisableThreadLibraryCalls) \
 X("kernel32.dll","LoadLibraryExA",LoadLibraryExA) X("kernel32.dll","LoadLibraryExW",LoadLibraryExW) \
 X("kernel32.dll","GetModuleHandleExW",GetModuleHandleExW) X("kernel32.dll","GetStartupInfoW",GetStartupInfoW) \
 X("kernel32.dll","OutputDebugStringW",OutputDebugStringW) X("kernel32.dll","ReadConsoleW",ReadConsoleW) \
 X("d3d9.dll","D3DPERF_SetOptions",D3DPERF_SetOptions) \
 X("kernel32.dll","IsBadCodePtr",IsBadCodePtr) X("kernel32.dll","GlobalMemoryStatus",GlobalMemoryStatus) \
 X("winmm.dll","timeGetTime",timeGetTime) X("winmm.dll","timeBeginPeriod",timeBeginPeriod) \
 X("winmm.dll","timeEndPeriod",timeEndPeriod)
#define C(X) \
 X("msvcrt.dll","malloc",malloc) X("msvcrt.dll","calloc",calloc) X("msvcrt.dll","realloc",realloc) X("msvcrt.dll","free",free) \
 X("msvcrt.dll","memcpy",memcpy) X("msvcrt.dll","memmove",memcpy) X("msvcrt.dll","memset",memset) X("msvcrt.dll","memcmp",memcmp) \
 X("msvcrt.dll","strlen",strlen) X("msvcrt.dll","strcmp",strcmp) X("msvcrt.dll","_stricmp",stricmp) \
 X("msvcrt.dll","strcpy",strcpy) X("msvcrt.dll","_strdup",strdup)
#define DEF(d,n,f) {d,n,s_##f},
static const struct {const char *dll,*name;GuestFn fn;} defs[]={K(DEF) C(DEF) BINK(DEF)};
#undef DEF
#define NDEFS (sizeof defs/sizeof defs[0])
static FnEntry entries[NDEFS];
const FnEntry *win32_lookup(uint32_t addr){
    if(!mem || addr<WIN32_THUNK_BASE || (addr&3))return NULL;
    const FnEntry *syn=win32_sync_lookup(addr);if(syn)return syn;
    const FnEntry *thr=win32_thread_lookup(addr);if(thr)return thr;
    const FnEntry *inp=win32_input_lookup(addr);if(inp)return inp;
    const FnEntry *usr=win32_user_lookup(addr);if(usr)return usr;
    const FnEntry *loc=win32_locale_lookup(addr);if(loc)return loc;
    const FnEntry *sh=win32_shell_lookup(addr);if(sh)return sh;
    const FnEntry *aud=win32_audio_lookup(addr);if(aud)return aud;
    const FnEntry *steam=win32_steam_lookup(addr);if(steam)return steam;
    const FnEntry *reg=win32_registry_lookup(addr);if(reg)return reg;
    unsigned i=(addr-WIN32_THUNK_BASE)/4;
    if(i<NDEFS){entries[i]=(FnEntry){addr,defs[i].fn};return &entries[i];}
    unsigned t=i-NDEFS;
    if(t>=trap_count)return NULL;
    trap_index=(int)t;
    trap_entries[t]=(FnEntry){addr,trap_fn};
    return &trap_entries[t];
}
uint32_t win32_resolve(const PeImport *e,void *ctx){
    (void)ctx;if(!e || !e->dll)return 0;
    if(e->name){
        for(unsigned i=0;i<NDEFS;++i)if(!icmp(e->dll,defs[i].dll)&&!strcmp(e->name,defs[i].name))return WIN32_THUNK_BASE+4*i;
        uint32_t syn=win32_sync_resolve(e->dll,e->name);if(syn)return syn;
        uint32_t thr=win32_thread_resolve(e->dll,e->name);if(thr)return thr;
        uint32_t inp=win32_input_resolve(e->dll,e->name);if(inp)return inp;
        uint32_t usr=win32_user_resolve(e->dll,e->name);if(usr)return usr;
        uint32_t loc=win32_locale_resolve(e->dll,e->name);if(loc)return loc;
        uint32_t sh=win32_shell_resolve(e->dll,e->name);if(sh)return sh;
        uint32_t aud=win32_audio_resolve(e->dll,e->name);if(aud)return aud;
        uint32_t steam=win32_steam_resolve(e->dll,e->name);if(steam)return steam;
        uint32_t reg=win32_registry_resolve(e->dll,e->name);if(reg)return reg;
    } else {
        uint32_t inp=win32_input_resolve_ordinal(e->dll,e->ordinal);if(inp)return inp;
    }
    uint32_t ext=external_resolver?external_resolver(e,external_context):0;
    if(ext)return ext;
    for(unsigned i=0;i<MAX_EXTRA_RESOLVERS;++i)
        if(extra_resolvers[i]&&(ext=extra_resolvers[i](e,extra_context[i]))!=0)return ext;
    if(!trap_enabled)return 0;
    if(trap_count>=MAX_TRAPS)return 0;
    char text[192];
    if(e->name)snprintf(text,sizeof text,"unresolved import %s!%s",e->dll,e->name);
    else snprintf(text,sizeof text,"unresolved ordinal import %s!#%u",e->dll,(unsigned)e->ordinal);
    for(unsigned i=0;i<trap_count;++i)if(!strcmp(traps[i].text,text))return WIN32_THUNK_BASE+4*(NDEFS+i);
    memcpy(traps[trap_count].text,text,sizeof text);
    return WIN32_THUNK_BASE+4*(NDEFS+trap_count++);
}
