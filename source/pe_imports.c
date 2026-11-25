#include "pe_imports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IMPORTS 8192u
static int range(const GuestImage *i,uint32_t rva,uint32_t size) {return rva<=i->size && size<=i->size-rva;}
static int value(GuestMemory *m,const GuestImage *i,uint32_t rva,uint32_t *v) {
    return !range(i,rva,4) || guest_read32(m,i->base+rva,v);
}
static const char *string_at(GuestMemory *m,const GuestImage *i,uint32_t rva) {
    if(!rva || rva>=i->size)return NULL;
    size_t n=i->size-rva;if(n>512)n=512;
    return guest_cstr(m,i->base+rva,n);
}
int pe_import_walk(GuestMemory *m,const GuestImage *im,PeImportVisit visit,void *ctx) {
    if(!m || !im || !visit || (uint64_t)im->base+im->size>(UINT64_C(1)<<32))return PE_IMPORT_FORMAT;
    if(!im->import_rva && !im->import_size)return PE_IMPORT_OK;
    if(!im->import_rva || im->import_size<20 || !range(im,im->import_rva,im->import_size))return PE_IMPORT_FORMAT;
    size_t n=0;
    for(uint32_t off=0;off<=im->import_size-20;off+=20) {
        uint32_t d=im->import_rva+off,a[5];
        for(unsigned j=0;j<5;++j)if(value(m,im,d+4*j,&a[j]))return PE_IMPORT_FORMAT;
        if(!(a[0]|a[1]|a[2]|a[3]|a[4]))return PE_IMPORT_OK;
        const char *dll=string_at(m,im,a[3]);
        uint32_t ilt=a[0]?a[0]:a[4],iat=a[4];
        if(!dll || !*dll || !ilt || !iat)return PE_IMPORT_FORMAT;
        for(uint32_t j=0;;j+=4) {
            uint32_t token,dummy;
            if(j>UINT32_MAX-ilt || j>UINT32_MAX-iat || value(m,im,ilt+j,&token) || value(m,im,iat+j,&dummy))return PE_IMPORT_FORMAT;
            if(!token)break;
            if(n++>=MAX_IMPORTS)return PE_IMPORT_LIMIT;
            PeImport e={.dll=dll,.iat=im->base+iat+j};
            if(token&0x80000000u) {if(token&0x7fff0000u)return PE_IMPORT_FORMAT;e.ordinal=(uint16_t)token;}
            else {
                if(!range(im,token,3))return PE_IMPORT_FORMAT;
                e.name=string_at(m,im,token+2);if(!e.name || !*e.name)return PE_IMPORT_FORMAT;
            }
            int rc=visit(&e,ctx);if(rc)return rc;
        }
    }
    return PE_IMPORT_FORMAT;
}
typedef struct {uint32_t at,addr;} Patch;
static int by_address(const void *a,const void *b) {
    uint32_t x=((const Patch *)a)->at,y=((const Patch *)b)->at;return (x>y)-(x<y);
}
typedef struct {PeImportResolve resolve;void *ctx;Patch *patches;size_t n,cap;} Bind;
static int collect(const PeImport *e,void *ctx) {
    Bind *b=ctx;uint32_t address=b->resolve(e,b->ctx);if(!address)return PE_IMPORT_UNRESOLVED;
    if(b->n==b->cap) {
        size_t cap=b->cap?2*b->cap:64;if(cap>MAX_IMPORTS)return PE_IMPORT_LIMIT;
        Patch *p=realloc(b->patches,cap*sizeof *p);if(!p)return PE_IMPORT_LIMIT;b->patches=p;b->cap=cap;
    }
    b->patches[b->n++]=(Patch){e->iat,address};return 0;
}
int pe_import_bind(GuestMemory *m,const GuestImage *im,PeImportResolve resolve,void *ctx,size_t *count) {
    if(count)*count=0;
    if(!resolve)return PE_IMPORT_FORMAT;
    Bind b={.resolve=resolve,.ctx=ctx};int rc=pe_import_walk(m,im,collect,&b);
    if(!rc) {
        /* Reject duplicate/overlapping IAT cells before applying changes. */
        if(b.n>1)qsort(b.patches,b.n,sizeof *b.patches,by_address);
        for(size_t i=1;i<b.n;++i)
            if((uint64_t)b.patches[i-1].at+4>b.patches[i].at) {rc=PE_IMPORT_FORMAT;break;}
        if(!rc)for(size_t i=0;i<b.n;++i)guest_write32(m,b.patches[i].at,b.patches[i].addr);
    }
    if(!rc && count)*count=b.n;
    free(b.patches);return rc;
}
static int word(GuestMemory *m,const GuestImage *im,uint32_t rva,uint16_t *v) {
    uint8_t b[2];if(!range(im,rva,2)||guest_read(m,im->base+rva,b,2))return -1;
    *v=(uint16_t)(b[0]|(uint16_t)b[1]<<8);return 0;
}
uint32_t pe_export_find(GuestMemory *m,const GuestImage *im,const char *name,uint16_t ordinal) {
    if(!m || !im || !im->export_rva || im->export_size<40 || !range(im,im->export_rva,im->export_size) ||
       (uint64_t)im->base+im->size>(UINT64_C(1)<<32))return 0;
    uint32_t d=im->export_rva,base,nf,nn,t,n,o;
    if(value(m,im,d+16,&base)||value(m,im,d+20,&nf)||value(m,im,d+24,&nn)||
       value(m,im,d+28,&t)||value(m,im,d+32,&n)||value(m,im,d+36,&o))return 0;
    if(!nf||nf>65536||nn>65536||!range(im,t,nf*4)||!range(im,n,nn*4)||!range(im,o,nn*2))return 0;
    uint32_t index=UINT32_MAX;
    if(name) {
        /* The export name pointer table is sorted lexically in PE; use binary search. */
        uint32_t lo=0,hi=nn;
        while(lo<hi){
            uint32_t mid=lo+(hi-lo)/2,ptr;if(value(m,im,n+4*mid,&ptr))return 0;
            const char *candidate=string_at(m,im,ptr);if(!candidate)return 0;
            int cmp=strcmp(candidate,name);
            if(cmp<0)lo=mid+1;else hi=mid;
        }
        if(lo<nn){uint32_t ptr;uint16_t ord;if(value(m,im,n+4*lo,&ptr)||word(m,im,o+2*lo,&ord))return 0;
            const char *candidate=string_at(m,im,ptr);if(candidate&&!strcmp(candidate,name))index=ord;}
    } else if(ordinal>=base)index=ordinal-base;
    uint32_t rva;
    if(index>=nf || value(m,im,t+4*index,&rva) || !rva || rva>=im->size ||
       (rva>=im->export_rva && rva-im->export_rva<im->export_size) || !guest_ptr(m,im->base+rva,1))return 0;
    return im->base+rva;
}
static unsigned lower(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
/* MSVC /hotpatch lazy-ILT export.  The exported entry is a forwarder
 * (`FF 25 <slot>`) followed by five int3 padding bytes; the slot is
 * initialised to a wrapper that calls a one-time initializer and forwards
 * through the same slot; the real function starts 11 bytes after the export
 * entry.  This is the exact shape prepare_pilot.py validates for a single
 * import.  Return the body only when every byte matches; otherwise the caller
 * keeps the raw export address, so an unknown layout still fails explicitly. */
static uint32_t hotpatch_body(GuestMemory *m,const GuestImage *im,uint32_t entry){
    if(!m||!im||entry<im->base||entry-im->base>im->size-11)return 0;
    uint8_t e[11];
    if(guest_read(m,entry,e,sizeof e))return 0;
    if(e[0]!=0xff||e[1]!=0x25)return 0;
    for(unsigned i=6;i<11;++i)if(e[i]!=0xcc)return 0;
    uint32_t slot=(uint32_t)e[2]|((uint32_t)e[3]<<8)|((uint32_t)e[4]<<16)|((uint32_t)e[5]<<24);
    if(slot<im->base||slot-im->base>im->size-4)return 0;
    uint32_t wrapper;
    if(guest_read32(m,slot,&wrapper))return 0;
    if(wrapper<im->base||wrapper>=entry||wrapper-im->base>im->size-19)return 0;
    uint8_t w[19];
    if(guest_read(m,wrapper,w,sizeof w))return 0;
    /* 8B FF 55 8B EC | 6A 01 | E8 rel32 | 5D | FF 25 <same slot> */
    if(!(w[0]==0x8b&&w[1]==0xff&&w[2]==0x55&&w[3]==0x8b&&w[4]==0xec))return 0;
    if(w[5]!=0x6a||w[6]!=0x01||w[7]!=0xe8||w[12]!=0x5d||w[13]!=0xff||w[14]!=0x25)return 0;
    uint32_t rel=(uint32_t)w[8]|((uint32_t)w[9]<<8)|((uint32_t)w[10]<<16)|((uint32_t)w[11]<<24);
    uint32_t init=(uint32_t)(wrapper+12+rel);
    if(init<im->base||init-im->base>=im->size)return 0;
    uint32_t wslot=(uint32_t)w[15]|((uint32_t)w[16]<<8)|((uint32_t)w[17]<<16)|((uint32_t)w[18]<<24);
    if(wslot!=slot)return 0;
    uint32_t body=entry+11;
    uint8_t b[3];
    if(guest_read(m,body,b,sizeof b))return 0;
    if(!(b[0]==0x8b&&b[1]==0xff&&b[2]==0x55))return 0;
    return body;
}
static int same_dll(const char *a,const char *b){
    while(*a && lower((unsigned char)*a)==lower((unsigned char)*b)){++a;++b;}
    return lower((unsigned char)*a)==lower((unsigned char)*b);
}
uint32_t pe_link_resolve(const PeImport *entry,void *ctx) {
    PeLinker *l=ctx;if(!l || !entry || !entry->dll)return 0;
    if(l->builtin){uint32_t addr=l->builtin(entry,l->builtin_ctx);if(addr)return addr;}
    for(size_t i=0;i<l->count;++i)if(l->modules[i].dll && same_dll(l->modules[i].dll,entry->dll)) {
        uint32_t addr=pe_export_find(l->memory,l->modules[i].image,entry->name,entry->ordinal);
        if(addr){
            uint32_t body=hotpatch_body(l->memory,l->modules[i].image,addr);
            /* Diagnostic only: N3DS_IMPORT_TRACE=1 records every cross-module
             * binding, which is the first source of a guest function pointer. */
            static int trace_state;
            if(!trace_state){
                const char *v=getenv("N3DS_IMPORT_TRACE");
                trace_state=(v&&v[0]&&strcmp(v,"0"))?2:1;
            }
            if(trace_state==2){
                if(body)fprintf(stderr,"[import] %s!%s -> %08lx (export %08lx hotpatch body)\n",
                    entry->dll,entry->name?entry->name:"#ordinal",(unsigned long)body,(unsigned long)addr);
                else fprintf(stderr,"[import] %s!%s -> %08lx\n",
                    entry->dll,entry->name?entry->name:"#ordinal",(unsigned long)addr);
            }
            return body?body:addr;
        }
    }
    if(l->fallback)return l->fallback(entry,l->fallback_ctx);
    return 0;
}
