/* Regression for a cold basic block placed before the callable entry.
 *
 * Discovery can omit a backward conditional-branch target when it lies before
 * the entry.  The container must still pack and execute that block in the same
 * function frame; it is not a separate callable function/import.
 */
#include "ir_container.h"
#include "ir_core.h"
#include "guest.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE  0x00001000u
#define COLD_BLOCK 0x00001000u
#define ENTRY      0x00001010u
#define FUNC_END   0x00001015u
#define STACK_TOP  0x00003000u

static const uint8_t code[] = {
    0x8d, 0x42, 0xff,             /* 1000: lea eax, [edx-1] */
    0x5b,                         /* 1003: pop ebx */
    0xc3,                         /* 1004: ret */
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x53,                         /* 1010: push ebx */
    0x31, 0xc0,                   /* 1011: xor eax, eax */
    0x74, 0xeb,                   /* 1013: je 1000 */
};

static GuestMemory mem;
static N3dsIrc *irc;
static jmp_buf fault_jb;
static int fault_armed;
static char fault_text[256];

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    void *p = guest_ptr(&mem, addr, bytes);
    if (!p) rt_unimpl(NULL, addr, "unmapped guest memory");
    return p;
}

void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    snprintf(fault_text, sizeof fault_text, "Guest fault at %08lx: %s",
             (unsigned long)addr, what ? what : "unknown");
    if (fault_armed) longjmp(fault_jb, 1);
    fprintf(stderr, "%s\n", fault_text);
    abort();
}

void rt_int3(CPU *c, uint32_t addr) { rt_unimpl(c, addr, "int3"); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) {
    (void)c; (void)a; (void)b; (void)cc; (void)d;
}
uint64_t rt_rdtsc(void) { return 1; }
int g_a43f_trace_on = 0;
void n3ds_a43f_trace_prime(void) {}
uint32_t g_ir_ip;
uint32_t n3ds_ir_cur_func_end;
void n3ds_lift_set_ir_ip(uint32_t ip) { (void)ip; }
void n3ds_a43f_trace_pre(CPU *c, uint32_t block_ip, uint32_t func_addr,
                         int is_irc) {
    (void)c; (void)block_ip; (void)func_addr; (void)is_irc;
}
void n3ds_a43f_trace_block(CPU *c, uint32_t block_ip, uint32_t func_addr,
                           uint32_t next, int is_irc) {
    (void)c; (void)block_ip; (void)func_addr; (void)next; (void)is_irc;
}
void n3ds_a43f_trace_dispatch(CPU *c, uint32_t addr) { (void)c; (void)addr; }
void n3ds_a43f_trace_aot_pre(CPU *c) { (void)c; }
void n3ds_a43f_trace_aot_post(CPU *c) { (void)c; }
void n3ds_a43f_trace_periodic(CPU *c) { (void)c; }

int n3ds_rt_dispatch(CPU *c, uint32_t addr) {
    int rc = n3ds_irc_exec_status(irc, c, addr, 0);
    return rc == N3DS_IRC_TIER_CONTAINER ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s CONTAINER.IRC [preentry|missing]\n", argv[0]);
        return 2;
    }
    guest_init(&mem, 1u << 20);
    if (guest_map(&mem, CODE_BASE, 0x1000) ||
        guest_map(&mem, STACK_TOP - 0x1000, 0x1000)) {
        fprintf(stderr, "guest map failed\n");
        return 2;
    }
    memcpy(n3ds_guest_ptr(CODE_BASE, sizeof code), code, sizeof code);

    int status = 0;
    irc = n3ds_irc_open_ex(argv[1], &status);
    if (!irc || n3ds_irc_cache_init(irc, 64u << 10, 4) != N3DS_IRC_OK) {
        fprintf(stderr, "container open/cache failed: %s (%d)\n",
                n3ds_irc_strstatus(status), status);
        return 2;
    }
    N3dsIrcEntry e;
    if (n3ds_irc_lookup(irc, COLD_BLOCK, &e) != N3DS_IRC_ERR_NOT_FOUND) {
        fprintf(stderr, "cold block was incorrectly exposed as a callable entry\n");
        return 1;
    }
    int expect_preentry = argc < 3 || strcmp(argv[2], "missing") != 0;
    int has_preentry = n3ds_irc_has_preentry(irc, ENTRY);
    if (has_preentry != expect_preentry ||
        n3ds_irc_has_preentry(irc, COLD_BLOCK)) {
        fprintf(stderr, "pre-entry index flag does not match the fixture\n");
        return 1;
    }

    CPU c;
    memset(&c, 0, sizeof c);
    c.r[4] = STACK_TOP;
    c.r[2] = 0;                 /* lea eax, [edx-1] -> 0xffffffff */
    c.r[3] = 0x13579bdfu;       /* pop ebx must restore this value */
    uint32_t stack_base = STACK_TOP - 4;
    M32(stack_base) = 0xffffffffu;
    c.r[4] = stack_base;

    IrCacheStats before, after;
    n3ds_ir_cache_stats(&before);
    fault_armed = 1;
    int trapped = setjmp(fault_jb);
    int rc;
    if (trapped) {
        rc = -1;
        n3ds_irc_recover(irc);
    } else {
        rc = n3ds_irc_exec_forced(irc, &c, ENTRY);
    }
    fault_armed = 0;
    n3ds_ir_cache_stats(&after);

    if (trapped) {
        fprintf(stderr, "FAIL: pre-entry branch faulted: %s\n", fault_text);
        return 1;
    }
    if (rc != N3DS_IRC_TIER_CONTAINER || c.r[0] != 0xffffffffu ||
        c.r[3] != 0x13579bdfu || c.r[4] != stack_base + 4) {
        fprintf(stderr, "FAIL: rc=%d eax=%08lx ebx=%08lx esp=%08lx\n", rc,
                (unsigned long)c.r[0], (unsigned long)c.r[3],
                (unsigned long)c.r[4]);
        return 1;
    }
    if (before.blocks != after.blocks) {
        fprintf(stderr, "FAIL: container execution invoked the runtime decoder\n");
        return 1;
    }
    N3dsIrcStats st;
    n3ds_irc_stats(irc, &st);
    if (st.block_hits < 2) {
        fprintf(stderr, "FAIL: expected entry and cold block, got %llu block hits\n",
                (unsigned long long)st.block_hits);
        return 1;
    }
    printf("IR container pre-entry block PASS (entry %08lx, cold %08lx, hits %llu)\n",
           (unsigned long)ENTRY, (unsigned long)COLD_BLOCK,
           (unsigned long long)st.block_hits);
    n3ds_irc_close(irc);
    guest_destroy(&mem);
    return 0;
}
