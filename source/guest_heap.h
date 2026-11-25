#pragma once
#include "guest.h"
#define HEAP_MAX_BLOCKS 512
typedef struct { uint32_t offset, size; int free; } HeapBlock;
typedef struct {
    GuestMemory *memory;
    uint32_t base, size;
    size_t count;
    HeapBlock blocks[HEAP_MAX_BLOCKS];
} GuestHeap;
int heap_create(GuestHeap *h, GuestMemory *m, uint32_t base, uint32_t size);
void heap_destroy(GuestHeap *h);
uint32_t heap_alloc(GuestHeap *h, uint32_t size);
int heap_free(GuestHeap *h, uint32_t addr);
uint32_t heap_realloc(GuestHeap *h, uint32_t addr, uint32_t size);
int heap_self_test(void);
