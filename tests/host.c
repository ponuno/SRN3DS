#include "probe.h"
#include "pe_image.h"
#include "guest_heap.h"
#include "guest_call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(unsigned char *p,unsigned v) {p[0]=v;p[1]=v>>8;}
static void put32(unsigned char *p,uint32_t v) {p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
/* Synthetic data fixture: PE32 header + two sections, no executable code. */
static int make_fixture(const char *path) {
    unsigned char b[0x600]={0};
    put16(b,0x5a4d);put32(b+60,0x80);
    memcpy(b+0x80,"PE\0\0",4);put16(b+0x84,0x14c);put16(b+0x86,2);put16(b+0x94,224);
    unsigned char *o=b+0x98;
    put16(o,0x10b);put32(o+16,0x1000);put32(o+28,0x400000);
    put32(o+56,0x3000);put32(o+60,0x200);put32(o+92,16);
    put32(o+136,0x2000);put32(o+140,12);
    unsigned char *s=o+224;
    memcpy(s,".data",5);put32(s+8,0x400);put32(s+12,0x1000);put32(s+16,0x200);put32(s+20,0x200);
    s+=40;memcpy(s,".reloc",6);put32(s+8,0x200);put32(s+12,0x2000);put32(s+16,0x200);put32(s+20,0x400);
    put32(b+0x200,0x401234);put32(b+0x400,0x1000);put32(b+0x404,12);put16(b+0x408,0x3000);
    FILE *f=fopen(path,"wbx");if(!f)return -1;
    int ok=fwrite(b,1,sizeof b,f)==sizeof b;
    if(fclose(f))ok=0;
    return ok?0:-1;
}
int main(int argc, char **argv) {
    assert(guest_memory_test() == 0);
    assert(heap_self_test() == 0);
    assert(guest_call_self_test() == 0);
    PeInfo i;
    assert(pe_probe("/nonexistent/skyrim.exe",&i) == -1);
    const char *path="test_fixture.pe";
    assert(make_fixture(path)==0);
    GuestMemory m;guest_init(&m,0x6000);
    GuestImage im;uint32_t val;
    assert(pe_image_load(&m,path,0x800000,&im)==0);
    /* Paged image: reserved pages are charged, not the whole SizeOfImage. */
    assert(m.used < 0x3000);
    assert(im.entry==0x801000 && guest_read32(&m,0x801000,&val)==0 && val==0x801234);
    assert(guest_read32(&m,0x801300,&val)==0 && val==0);
    assert(pe_image_load(&m,path,0x800000,&im)==PE_MEMORY);
    assert(pe_image_load(&m,path,0,&im)==0);
    assert(guest_read32(&m,0x401000,&val)==0 && val==0x401234);
    {   /* A budget smaller than the page tables rejects the image. */
        GuestMemory small; guest_init(&small,0x40);
        assert(pe_image_load(&small,path,0x900000,&im)==PE_MEMORY);
        guest_destroy(&small);
    }
    guest_destroy(&m);
    /* A truncated section must fail without leaving a mapped slab or region. */
    FILE *f=fopen(path,"r+b");assert(f);
    unsigned char offset[4]={0xff,0xff,0xff,0x7f};
    assert(fseek(f,0x98+224+20,SEEK_SET)==0 && fwrite(offset,1,4,f)==4 && fclose(f)==0);
    guest_init(&m,0x6000);
    assert(pe_image_load(&m,path,0,&im)==PE_FORMAT && m.used==0 && m.count==0);
    for (unsigned i=0;i<GUEST_MAX_SLABS;++i) assert(m.slabs[i].state==NULL);
    guest_destroy(&m);
    assert(remove(path)==0);
    puts("PE mapping, zero fill, relocation & budget tests passed");
    if (argc > 1) {
        if (pe_probe(argv[1],&i) != 0) return 2;
        printf("PE32: %u sections, image %u bytes\n",i.sections,i.image_size);
    }
    puts("Guest memory self-test passed");
    return 0;
}
