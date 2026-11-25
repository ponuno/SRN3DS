#pragma once
#include "guest.h"

typedef struct GuestCpu GuestCpu;
typedef int (*GuestFn)(GuestCpu *cpu);
typedef struct { uint32_t addr; GuestFn fn; } GuestFnEntry;
typedef enum { GUEST_EAX, GUEST_ECX, GUEST_EDX, GUEST_EBX, GUEST_ESP, GUEST_EBP, GUEST_ESI, GUEST_EDI } GuestReg;
struct GuestCpu {
    GuestMemory *memory;
    uint32_t r[8];
    uint32_t stack_base, stack_size;
    const GuestFnEntry *table;
    size_t table_count;
};

/* A small single-threaded ABI experiment, not the upstream CPU struct yet. */
int guest_cpu_init(GuestCpu *c, GuestMemory *m, uint32_t stack_base, uint32_t stack_size,
                   const GuestFnEntry *table, size_t table_count);
void guest_cpu_destroy(GuestCpu *c);
int guest_push32(GuestCpu *c, uint32_t value);
int guest_pop32(GuestCpu *c, uint32_t *value);
int guest_arg32(GuestCpu *c, unsigned index, uint32_t *value);
int guest_return(GuestCpu *c, uint32_t value, unsigned callee_args);
int guest_dispatch(GuestCpu *c, uint32_t address);
int guest_invoke(GuestCpu *c, uint32_t address, const uint32_t *args, size_t argc, uint32_t *result);
int guest_call_self_test(void);
