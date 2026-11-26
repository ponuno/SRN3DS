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
 * through the same slot.  Some exports put the real body at entry+11; others
 * (for example D3DXVec3TransformCoord) put another wrapper there, and the
 * one-time initializer copies the real body pointer from a 74-entry table.
 * Resolve that table when present, then fall back to entry+11 only when it is
 * a real function body and not another wrapper.  Every byte read is bounded by
 * in_image(); otherwise the caller keeps the raw export address. */
static uint32_t le32(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
/* True when [addr, addr+n) is inside the image.  Compare in 64-bit so a tiny
 * image cannot underflow im->size-n and accept a read past the image. */
static int in_image(const GuestImage *im,uint32_t addr,uint32_t n){
    return addr>=im->base && (uint64_t)addr+(uint64_t)n<=(uint64_t)im->base+(uint64_t)im->size;
}
/* addr+4*index without wrapping guest arithmetic. */
static int in_image4(const GuestImage *im,uint32_t addr,uint32_t index){
    uint64_t a=(uint64_t)addr+4ull*index;
    return a<=UINT32_MAX && in_image(im,(uint32_t)a,4);
}
/* A real MSVC frame body starts 8B FF 55 8B EC.  The lazy-ILT guard wrapper
 * starts with the same five bytes, so require the full prologue and reject the
 * wrapper's `push 1; call` tail before accepting a candidate. */
static int real_prologue(GuestMemory *m,const GuestImage *im,uint32_t addr){
    uint8_t b[8];
    if(!in_image(im,addr,sizeof b)||guest_read(m,addr,b,sizeof b))return 0;
    if(!(b[0]==0x8b&&b[1]==0xff&&b[2]==0x55&&b[3]==0x8b&&b[4]==0xec))return 0;
    return !(b[5]==0x6a&&b[6]==0x01&&b[7]==0xe8);
}
typedef struct {uint32_t src,dst,count;} InitTable;
/* Find the `want`-th `mov esi,src; mov edi/ebx,dst; ... rep movsd` table copy
 * in the one-time initializer.  The immediates are relocated guest addresses.
 * The copied dword count comes from a preceding `mov ecx,imm32`; without one,
 * use the largest count whose src and dst entries are both readable, capped at
 * 256.  Callers iterate the patterns because the first memcpy is not always the
 * real-body table. */
static int init_table_at(GuestMemory *m,const GuestImage *im,uint32_t init,unsigned want,InitTable *t){
    uint8_t b[128];
    if(!in_image(im,init,sizeof b)||guest_read(m,init,b,sizeof b))return 0;
    for(unsigned i=0,seen=0;i+10<sizeof b;++i){
        if(b[i]!=0xbe||(b[i+5]!=0xbf&&b[i+5]!=0xbb))continue;
        uint32_t s=le32(b+i+1),d=le32(b+i+6);
        if(!in_image(im,s,4)||!in_image(im,d,4))continue;
        int rep=-1;
        for(unsigned j=i+10;j+1<sizeof b&&j<i+48;++j)
            if(b[j]==0xf3&&b[j+1]==0xa5){rep=(int)j;break;}
        if(rep<0)continue;
        uint32_t count=0;
        for(unsigned j=i+10;j<(unsigned)rep;++j)
            if(b[j]==0xb9){uint32_t c=le32(b+j+1);if(c>=1&&c<=256)count=c;break;}
        if(!count)while(count<256&&in_image4(im,s,count)&&in_image4(im,d,count))++count;
        if(!count||seen++!=want)continue;
        t->src=s;t->dst=d;t->count=count;return 1;
    }
    return 0;
}
static uint32_t hotpatch_body(GuestMemory *m,const GuestImage *im,uint32_t entry){
    if(!m||!im||!in_image(im,entry,11))return 0;
    uint8_t e[11];
    if(guest_read(m,entry,e,sizeof e))return 0;
    if(e[0]!=0xff||e[1]!=0x25)return 0;
    for(unsigned i=6;i<11;++i)if(e[i]!=0xcc)return 0;
    uint32_t slot=le32(e+2);
    if(!in_image(im,slot,4))return 0;
    uint32_t wrapper;
    if(guest_read32(m,slot,&wrapper))return 0;
    if(wrapper>=entry||!in_image(im,wrapper,19))return 0;
    uint8_t w[19];
    if(guest_read(m,wrapper,w,sizeof w))return 0;
    /* 8B FF 55 8B EC | 6A 01 | E8 rel32 | 5D | FF 25 <same slot> */
    if(!(w[0]==0x8b&&w[1]==0xff&&w[2]==0x55&&w[3]==0x8b&&w[4]==0xec))return 0;
    if(w[5]!=0x6a||w[6]!=0x01||w[7]!=0xe8||w[12]!=0x5d||w[13]!=0xff||w[14]!=0x25)return 0;
    uint32_t rel=(uint32_t)w[8]|((uint32_t)w[9]<<8)|((uint32_t)w[10]<<16)|((uint32_t)w[11]<<24);
    uint32_t init=(uint32_t)(wrapper+12+rel);
    if(!in_image(im,init,1))return 0;
    uint32_t wslot=le32(w+15);
    if(wslot!=slot)return 0;
    InitTable t;
    for(unsigned n=0;init_table_at(m,im,init,n,&t);++n){
        if(slot<t.dst||((slot-t.dst)&3u))continue;
        uint32_t idx=(slot-t.dst)/4u;
        if(idx>=t.count||!in_image4(im,t.src,idx))continue;
        uint32_t cand;
        if(guest_read32(m,(uint32_t)((uint64_t)t.src+4ull*idx),&cand))continue;
        if(cand==entry||cand==wrapper)continue;
        if(real_prologue(m,im,cand))return cand;
    }
    uint64_t body64=(uint64_t)entry+11u;
    if(body64>UINT32_MAX)return 0;
    uint32_t body=(uint32_t)body64;
    if(!real_prologue(m,im,body))return 0;
    return body;
}
static int same_dll(const char *a,const char *b){
    while(*a && lower((unsigned char)*a)==lower((unsigned char)*b)){++a;++b;}
    return lower((unsigned char)*a)==lower((unsigned char)*b);
}
/* Diagnostic only: N3DS_IMPORT_TRACE=1 records every binding, including host
 * shims resolved before the PE modules.  This is the first source of a guest
 * function pointer, so the trace must not stop at module exports. */
static int import_trace(void){
    static int state;
    if(!state){const char *v=getenv("N3DS_IMPORT_TRACE");state=(v&&v[0]&&strcmp(v,"0"))?2:1;}
    return state==2;
}
uint32_t pe_link_resolve(const PeImport *entry,void *ctx) {
    PeLinker *l=ctx;if(!l || !entry || !entry->dll)return 0;
    if(l->builtin){
        uint32_t addr=l->builtin(entry,l->builtin_ctx);
        if(addr){
            if(import_trace())fprintf(stderr,"[import] %s!%s -> %08lx (host shim)\n",
                entry->dll,entry->name?entry->name:"#ordinal",(unsigned long)addr);
            return addr;
        }
    }
    for(size_t i=0;i<l->count;++i)if(l->modules[i].dll && same_dll(l->modules[i].dll,entry->dll)) {
        uint32_t addr=pe_export_find(l->memory,l->modules[i].image,entry->name,entry->ordinal);
        if(addr){
            uint32_t body=hotpatch_body(l->memory,l->modules[i].image,addr);
            if(import_trace()){
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
