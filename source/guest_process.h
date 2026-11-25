#pragma once
#include "pe_imports.h"

#define GP_MAX_MODULES 8
#define GP_MAX_CALLBACKS 64
#define GP_STACK_BASE UINT32_C(0x10000000)
#define GP_TEB UINT32_C(0x18000000)
#define GP_PEB UINT32_C(0x18100000)
#define GP_TLS UINT32_C(0x18200000)
#define GP_COMMAND_A (GP_PEB+0x1000)
#define GP_COMMAND_W (GP_PEB+0x1200)
#define GP_ENV_A (GP_PEB+0x1600)
#define GP_ENV_W (GP_PEB+0x1700)
#define GP_PARAMS (GP_PEB+0x200)
typedef struct {
    uint32_t image_base,index_addr,slot,block,bytes,callbacks[GP_MAX_CALLBACKS];
    unsigned callback_count;
} GuestTls;
typedef struct {
    GuestMemory *memory;
    uint32_t stack_top,teb,peb;
    unsigned tls_count;
    GuestTls tls[GP_MAX_MODULES];
    uint32_t mapped[4];
    size_t mapped_count;
} GuestProcess;
/* Minimal x86 process structures; module 0 is the executable. */
int guest_process_init(GuestProcess *p,GuestMemory *m,const PeModule *modules,size_t count,uint32_t stack_bytes);
void guest_process_destroy(GuestProcess *p);
