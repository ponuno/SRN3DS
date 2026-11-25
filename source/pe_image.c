#include "pe_image.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (uint16_t)b[1]<<8); }
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24; }
static int within(uint32_t pos, uint32_t n, uint32_t limit) { return pos<=limit && n<=limit-pos; }
static int at(FILE *f, uint32_t pos, void *buf, size_t n) {
    return pos>INT32_MAX || fseek(f,(long)pos,SEEK_SET) || fread(buf,1,n,f)!=n;
}
static int relocate(GuestMemory *m, const GuestImage *im, uint32_t rva, uint32_t size) {
    if (im->base==im->preferred_base) return 0;
    if (!rva || !size || !within(rva,size,im->size)) return -1;
    uint8_t *table=guest_ptr(m,im->base+rva,size);
    uint32_t delta=im->base-im->preferred_base, p=0;
    while (p<size) {
        if (size-p<8) return -1;
        uint32_t page=rd32(table+p), span=rd32(table+p+4);
        if (span<8 || span>size-p || (span&1)) return -1;
        for (uint32_t k=8;k<span;k+=2) {
            uint16_t e=rd16(table+p+k);
            unsigned type=e>>12;
            if (!type) continue;
            uint64_t offset=(uint64_t)page+(e&0xfff);
            if (type!=3 || offset>UINT32_MAX || !within((uint32_t)offset,4,im->size)) return -1;
            /* Relocation metadata must remain unchanged during iteration. */
            if (offset<(uint64_t)rva+size && rva<offset+4) return -1;
            uint32_t addr=im->base+(uint32_t)offset, value;
            if (guest_read32(m,addr,&value) || guest_write32(m,addr,value+delta)) return -1;
        }
        p+=span;
    }
    return 0;
}

int pe_image_load(GuestMemory *m, const char *path, uint32_t want_base, GuestImage *out) {
    FILE *f=fopen(path,"rb");
    if (!f) return PE_IO;
    uint8_t dos[64], coff[24], opt[224], sec[40];
    int rc=PE_FORMAT, mapped=0;
    GuestImage im={0};
    if (fseek(f,0,SEEK_END)) { rc=PE_IO; goto done; }
    long end=ftell(f);
    if (end<64 || (uint64_t)end>INT32_MAX) goto done;
    uint32_t file_size=(uint32_t)end;
    if (at(f,0,dos,sizeof dos) || rd16(dos)!=0x5a4d) goto done;
    uint32_t pe=rd32(dos+60);
    if (pe<64 || !within(pe,sizeof coff,file_size) || at(f,pe,coff,sizeof coff) ||
        memcmp(coff,"PE\0\0",4) || rd16(coff+4)!=0x14c) goto done;
    im.sections=rd16(coff+6);
    uint32_t opt_size=rd16(coff+20), opt_at=pe+24;
    if (!im.sections || im.sections>96 || opt_size<96 || !within(opt_at,opt_size,file_size)) goto done;
    memset(opt,0,sizeof opt);
    size_t read_size=opt_size<sizeof opt?opt_size:sizeof opt;
    if (at(f,opt_at,opt,read_size) || rd16(opt)!=0x10b) goto done;
    uint32_t nrva=rd32(opt+92);
    if (nrva>16) nrva=16;
    if (96+nrva*8>opt_size) goto done;
    im.preferred_base=rd32(opt+28); im.base=want_base?want_base:im.preferred_base;
    im.size=rd32(opt+56);
    uint32_t hdr=rd32(opt+60), entry=rd32(opt+16), table=opt_at+opt_size;
    if (!im.size || !hdr || hdr>im.size || hdr>file_size || !within(table,40u*im.sections,hdr) ||
        entry>=im.size || (uint64_t)im.base+im.size>(UINT64_C(1)<<32)) goto done;
    im.entry=entry?im.base+entry:0;
    if (nrva>0) {im.export_rva=rd32(opt+96);im.export_size=rd32(opt+100);}
    if (nrva>1) { im.import_rva=rd32(opt+104); im.import_size=rd32(opt+108); }
    if (nrva>9) { im.tls_rva=rd32(opt+168); im.tls_size=rd32(opt+172); }
    if (!within(im.import_rva,im.import_size,im.size) || !within(im.export_rva,im.export_size,im.size) ||
        !within(im.tls_rva,im.tls_size,im.size)) goto done;
    if (guest_map(m,im.base,im.size)) { rc=PE_MEMORY; goto done; }
    mapped=1;
    if (at(f,0,guest_ptr(m,im.base,hdr),hdr)) { rc=PE_IO; goto done; }
    /* Reject overlapping section footprints; copy directly from SD, not a full file buffer. */
    uint32_t starts[96], sizes[96];
    for (unsigned i=0;i<im.sections;++i) {
        if (at(f,table+40*i,sec,sizeof sec)) { rc=PE_IO; goto done; }
        uint32_t vs=rd32(sec+8), va=rd32(sec+12), raw=rd32(sec+16), off=rd32(sec+20);
        uint32_t extent=vs?vs:raw, copy=raw<extent?raw:extent;
        if (va<hdr || !within(va,extent,im.size) || (copy && !within(off,copy,file_size))) goto done;
        for(unsigned j=0;j<i;++j) if (extent && sizes[j] && va<starts[j]+sizes[j] && starts[j]<va+extent) goto done;
        starts[i]=va; sizes[i]=extent;
        if (copy && at(f,off,guest_ptr(m,im.base+va,copy),copy)) { rc=PE_IO; goto done; }
    }
    if (relocate(m,&im,nrva>5?rd32(opt+136):0,nrva>5?rd32(opt+140):0)) { rc=PE_RELOCATION; goto done; }
    *out=im; rc=PE_OK;
done:
    if (mapped && rc) guest_unmap(m,im.base);
    fclose(f); return rc;
}
const char *pe_error(int code) {
    switch(code) {case PE_OK:return "OK";case PE_IO:return "file I/O";case PE_FORMAT:return "invalid PE32 image";
    case PE_MEMORY:return "guest memory budget/overlap";case PE_RELOCATION:return "unsupported relocations";default:return "unknown";}
}
