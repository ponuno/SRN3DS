#include "guest_heap.h"
#include <string.h>

/* Single-threaded first-fit heap; metadata lives outside guest memory. */
int heap_create(GuestHeap *h, GuestMemory *m, uint32_t base, uint32_t size) {
    memset(h,0,sizeof *h);
    if (!base || (base&15) || !size || (size&15) || guest_map(m,base,size)) return -1;
    h->memory=m; h->base=base; h->size=size; h->count=1;
    h->blocks[0]=(HeapBlock){0,size,1}; return 0;
}
void heap_destroy(GuestHeap *h) {
    if (h->memory) guest_unmap(h->memory,h->base);
    memset(h,0,sizeof *h);
}
uint32_t heap_alloc(GuestHeap *h, uint32_t size) {
    if (!h->memory || !size || size>UINT32_MAX-15) return 0;
    size=(size+15)&~15u;
    for (size_t i=0;i<h->count;++i) {
        HeapBlock *b=&h->blocks[i];
        if (!b->free || b->size<size) continue;
        if (b->size>size && h->count<HEAP_MAX_BLOCKS) {
            memmove(b+2,b+1,(h->count-i-1)*sizeof *b);
            b[1]=(HeapBlock){b->offset+size,b->size-size,1};
            b->size=size; ++h->count;
        }
        b->free=0; return h->base+b->offset;
    }
    return 0;
}
static size_t find_block(GuestHeap *h, uint32_t addr) {
    if (addr>=h->base) for(size_t i=0;i<h->count;++i)
        if (!h->blocks[i].free && h->blocks[i].offset==addr-h->base) return i;
    return h->count;
}
int heap_free(GuestHeap *h, uint32_t addr) {
    if (!addr) return 0;
    size_t i=find_block(h,addr);
    if(i==h->count) return -1;
    h->blocks[i].free=1;
    if (i && h->blocks[i-1].free) --i;
    while(i+1<h->count && h->blocks[i+1].free) {
        h->blocks[i].size+=h->blocks[i+1].size;
        memmove(h->blocks+i+1,h->blocks+i+2,(h->count-i-2)*sizeof h->blocks[0]);
        --h->count;
    }
    return 0;
}
uint32_t heap_realloc(GuestHeap *h, uint32_t addr, uint32_t size) {
    if (!addr) return heap_alloc(h,size);
    if (!size) {heap_free(h,addr);return 0;}
    size_t i=find_block(h,addr);
    if(i==h->count) return 0;
    uint32_t old=h->blocks[i].size;
    if(size<=old) return addr;
    uint32_t next=heap_alloc(h,size);
    if(!next) return 0;
    memcpy(guest_ptr(h->memory,next,old),guest_ptr(h->memory,addr,old),old);
    guest_watch_check(h->memory,"heap_realloc");
    heap_free(h,addr);return next;
}
int heap_self_test(void) {
    GuestMemory m; GuestHeap h; int rc=-1;
    guest_init(&m,4096);
    if(heap_create(&h,&m,0x20000000,4096)) goto done;
    uint32_t a=heap_alloc(&h,19), b=heap_alloc(&h,40), v;
    if(!a || !b || a==b || (a&15) || (b&15) || guest_write32(&m,a,0x12345678)) goto done;
    uint32_t c=heap_realloc(&h,a,80);
    if(!c || guest_read32(&m,c,&v) || v!=0x12345678 || heap_free(&h,b) || heap_free(&h,c)) goto done;
    if(h.count!=1 || h.blocks[0].size!=4096 || heap_free(&h,c)!=-1) goto done;
    if(heap_alloc(&h,4096)!=h.base || heap_alloc(&h,1)!=0) goto done;
    rc=0;
done:
    heap_destroy(&h);guest_destroy(&m);return rc;
}
