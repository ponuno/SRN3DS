#include "guest_process.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <string.h>

enum { TEB_BYTES=0x2000,PEB_BYTES=0x4000,TLS_LIMIT=0x200000 };
typedef struct {uint32_t start,length,zero,align,index,callbacks[GP_MAX_CALLBACKS];unsigned n;} TlsPlan;
static int in_image(const GuestImage *im,uint32_t address,uint32_t bytes){
    return address>=im->base && bytes<=im->size && address-im->base<=im->size-bytes;
}
static int tls_plan(GuestMemory *m,const GuestImage *im,TlsPlan *t){
    memset(t,0,sizeof *t);
    if(!im->tls_rva&&!im->tls_size)return 0;
    if(!im->tls_rva||im->tls_size<24||im->tls_rva>im->size||im->tls_size>im->size-im->tls_rva)return -1;
    uint32_t d=im->base+im->tls_rva,v[6];
    for(unsigned i=0;i<6;++i)if(guest_read32(m,d+4*i,&v[i]))return -1;
    if(v[1]<v[0] || !in_image(im,v[2],4) || !guest_ptr(m,v[2],4) || (v[2]&3))return -1;
    t->start=v[0];t->length=v[1]-v[0];t->zero=v[4];t->index=v[2];
    if((t->length && (!in_image(im,t->start,t->length)||!guest_ptr(m,t->start,t->length))) ||
       (uint64_t)t->length+t->zero>TLS_LIMIT)return -1;
    unsigned a=(v[5]>>20)&15;if(a>13)return -1;
    t->align=a?1u<<(a-1):16;if(t->align<16)t->align=16;
    if(v[3]){
        if(v[3]&3)return -1;
        /* Windows stops the TLS callback array at the first NULL. The array
         * pointer in this image lands after two leading NULLs, so the loader
         * (correctly) runs no callback here; the shim invokes the two
         * non-NULL entries itself after the CRT heap is initialised. */
        for(unsigned i=0;;++i){
            uint64_t at=(uint64_t)v[3]+i*4;uint32_t cb;
            if(at>UINT32_MAX||!in_image(im,(uint32_t)at,4)||guest_read32(m,(uint32_t)at,&cb))return -1;
            if(!cb)break;
            if(i==GP_MAX_CALLBACKS||!in_image(im,cb,1))return -1;
            t->callbacks[t->n++]=cb;
        }
    }
    return 1;
}
static int map(GuestProcess *p,uint32_t base,uint32_t size){
    if(guest_map(p->memory,base,size))return -1;
    p->mapped[p->mapped_count++]=base;return 0;
}
static void w32(GuestProcess *p,uint32_t a,uint32_t v){(void)guest_write32(p->memory,a,v);}
static void w16(GuestProcess *p,uint32_t a,uint16_t v){uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)};(void)guest_write(p->memory,a,b,2);}
static void wide(GuestProcess *p,uint32_t addr,const char *s){for(size_t i=0;i<=strlen(s);++i)w16(p,addr+(uint32_t)i*2,(uint8_t)s[i]);}
static void ustr(GuestProcess *p,uint32_t desc,uint32_t text,const char *s){
    uint16_t bytes=(uint16_t)(strlen(s)*2);w16(p,desc,bytes);w16(p,desc+2,bytes+2);w32(p,desc+4,text);wide(p,text,s);
}
static void list(GuestProcess *p,uint32_t head,uint32_t offset,size_t n){
    const uint32_t first=GP_PEB+0x2000;const uint32_t stride=0x80;
    w32(p,head,n?first+offset:head);w32(p,head+4,n?first+(uint32_t)(n-1)*stride+offset:head);
    for(size_t i=0;i<n;++i){uint32_t curr=first+(uint32_t)i*stride+offset;
        w32(p,curr,i+1<n?curr+stride:head);w32(p,curr+4,i?curr-stride:head);}
}
static void guest_backing_fault(GuestMemory *m,uint32_t addr,const char *text){
    (void)m;rt_unimpl(NULL,addr,text);
}
int guest_process_init(GuestProcess *p,GuestMemory *m,const PeModule *modules,size_t n,uint32_t stack_bytes){
    memset(p,0,sizeof *p);if(!m||!modules||!n||n>GP_MAX_MODULES||stack_bytes<4096||stack_bytes>0x400000||(stack_bytes&4095))return -1;
    TlsPlan plans[GP_MAX_MODULES];uint32_t offsets[GP_MAX_MODULES],total=0;unsigned slots=0;
    for(size_t i=0;i<n;++i){
        const GuestImage *im=modules[i].image;
        if(!im||!modules[i].dll||!*modules[i].dll||strlen(modules[i].dll)>63||!im->size||
           (uint64_t)im->base+im->size>(UINT64_C(1)<<32)||!guest_ptr(m,im->base,im->size))return -1;
        for(size_t j=0;j<i;++j)if(modules[j].image->base==im->base)return -1;
        int rc=tls_plan(m,im,&plans[i]);if(rc<0)return -1;if(!rc){offsets[i]=UINT32_MAX;continue;}
        uint32_t align=plans[i].align;total=(total+align-1)&~(align-1);offsets[i]=total;
        uint32_t bytes=plans[i].length+plans[i].zero;if(!bytes)bytes=16;
        if(total>TLS_LIMIT || bytes>TLS_LIMIT-total)return -1;
        total+=bytes;++slots;
    }
    p->memory=m;
    guest_set_fault_handler(m,guest_backing_fault);
    if(map(p,GP_STACK_BASE,stack_bytes)||map(p,GP_TEB,TEB_BYTES)||map(p,GP_PEB,PEB_BYTES)||
       (total&&map(p,GP_TLS,(total+4095)&~4095u))){guest_process_destroy(p);return -1;}
    p->stack_top=GP_STACK_BASE+stack_bytes-64;p->teb=GP_TEB;p->peb=GP_PEB;
    w32(p,GP_TEB,UINT32_MAX);w32(p,GP_TEB+4,p->stack_top);w32(p,GP_TEB+8,GP_STACK_BASE);
    w32(p,GP_TEB+0x18,GP_TEB);w32(p,GP_TEB+0x20,1);w32(p,GP_TEB+0x24,1);
    w32(p,GP_TEB+0x2c,GP_TEB+0x1000);w32(p,GP_TEB+0x30,GP_PEB);
    w32(p,GP_TEB+0xf94,GP_TEB+0x1800); /* dynamic TLS expansion storage */
    w32(p,GP_PEB+8,modules[0].image->base);w32(p,GP_PEB+0xc,GP_PEB+0x100);
    w32(p,GP_PEB+0x10,GP_PARAMS);w32(p,GP_PEB+0x18,1);w32(p,GP_PEB+0x64,1); /* one logical CPU */
    w32(p,GP_PEB+0x100,0x30);w32(p,GP_PEB+0x104,1);
    list(p,GP_PEB+0x10c,0,n);list(p,GP_PEB+0x114,8,n);list(p,GP_PEB+0x11c,16,n);
    char path[128],cmd[132];snprintf(path,sizeof path,"C:\\Skyrim\\%s",modules[0].dll);snprintf(cmd,sizeof cmd,"\"%s\"",path);
    w32(p,GP_PARAMS,0x100);w32(p,GP_PARAMS+4,0x100);w32(p,GP_PARAMS+8,1);
    ustr(p,GP_PARAMS+0x24,GP_PEB+0x1900,"C:\\Skyrim");
    ustr(p,GP_PARAMS+0x38,GP_PEB+0x1a00,path);ustr(p,GP_PARAMS+0x40,GP_COMMAND_W,cmd);
    w32(p,GP_PARAMS+0x48,GP_ENV_W);
    guest_write(m,GP_COMMAND_A,cmd,strlen(cmd)+1);
    guest_write(m,GP_ENV_A,"OS=Windows_NT\0",15);wide(p,GP_ENV_W,"OS=Windows_NT");
    for(size_t i=0;i<n;++i){
        const GuestImage *im=modules[i].image;uint32_t e=GP_PEB+0x2000+(uint32_t)i*0x80;
        w32(p,e+0x18,im->base);w32(p,e+0x1c,im->entry);w32(p,e+0x20,im->size);
        snprintf(path,sizeof path,"C:\\Skyrim\\%s",modules[i].dll);
        uint32_t strings=GP_PEB+0x2800+(uint32_t)i*0x280;
        ustr(p,e+0x24,strings,path);ustr(p,e+0x2c,strings+0x100,modules[i].dll);
        if(offsets[i]==UINT32_MAX)continue;
        TlsPlan *t=&plans[i];GuestTls *g=&p->tls[p->tls_count++];
        g->image_base=im->base;g->index_addr=t->index;g->slot=p->tls_count-1;g->block=GP_TLS+offsets[i];g->bytes=t->length+t->zero;
        g->callback_count=t->n;memcpy(g->callbacks,t->callbacks,t->n*sizeof(uint32_t));
        if(t->length)guest_write(m,g->block,guest_ptr(m,t->start,t->length),t->length);
        w32(p,t->index,g->slot);w32(p,GP_TEB+0x1000+g->slot*4,g->block);
    }
    return p->tls_count==slots?0:-1;
}
void guest_process_destroy(GuestProcess *p){
    if(p->memory)while(p->mapped_count)guest_unmap(p->memory,p->mapped[--p->mapped_count]);
    memset(p,0,sizeof *p);
}
