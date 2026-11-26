#include "d3d9_com_demo.h"
#include "d3d9_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough of a backend for d9_create_device/d9_com_demo_init; the adapter
 * checks below never draw. */
typedef struct {size_t base;unsigned levels;uint8_t *data;} Tex;
static int tex_create(void *ctx,unsigned w,unsigned h,unsigned levels,void **native){
    (void)ctx;Tex *t=calloc(1,sizeof *t);if(!t)return -1;t->base=(size_t)w*h*4;t->levels=levels;
    size_t total=0;for(unsigned i=0;i<levels;++i)total+=t->base>>(2*i);
    t->data=malloc(total);if(!t->data){free(t);return -1;}*native=t;return 0;
}
static int tex_upload(void *ctx,void *native,unsigned level,const void *data,size_t n){
    (void)ctx;Tex *t=native;assert(level<t->levels&&n==(t->base>>(2*level)));
    size_t off=0;for(unsigned i=0;i<level;++i)off+=t->base>>(2*i);memcpy(t->data+off,data,n);return 0;
}
static void tex_delete(void *ctx,void *native){(void)ctx;Tex *t=native;free(t->data);free(t);}
static int begin(void *ctx,uint32_t argb){(void)ctx;(void)argb;return 0;}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *t,const D9DrawState *s){
    (void)ctx;(void)v;(void)n;(void)m;(void)t;(void)s;return 0;
}
static void end(void *ctx){(void)ctx;}
static const D9Backend ops={tex_create,tex_upload,tex_delete,begin,draw,end};

#define IDENT 0x10010500u
#define CAPS9 0x10010a00u
#define MODE  0x10010f00u

static uint32_t call(D9ComDemo *s,uint32_t object,unsigned slot,const uint32_t *args,unsigned n){
    uint32_t r=0xdeadbeefu;assert(!d9_com_demo_call(s,object,slot,args,n,&r));return r;
}
static uint32_t rd(D9ComDemo *s,uint32_t address){uint32_t v=0;assert(!guest_read32(&s->memory,address,&v));return v;}
static uint32_t fbits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static void set_fallback(const char *value){
#ifdef _WIN32
    assert(!_putenv_s("TESV_D3D9_HDR_LDR_FALLBACK",value?value:""));
#else
    if(value)assert(!setenv("TESV_D3D9_HDR_LDR_FALLBACK",value,1));
    else assert(!unsetenv("TESV_D3D9_HDR_LDR_FALLBACK"));
#endif
}

int main(void){
    set_fallback(NULL);
    D9Device *d=d9_create_device(&ops,NULL,4u<<20);assert(d);
    D9ComDemo s;
    if(d9_com_demo_init(&s,d)){fprintf(stderr,"COM initialization failed: %s\n",s.message);return 1;}
    uint32_t args[8],r;

    /* The gate starts with Direct3DCreate9's object and asks for one adapter. */
    assert(call(&s,s.factory,4,NULL,0)==1);

    args[0]=0;args[1]=0;args[2]=IDENT;
    assert(call(&s,s.factory,5,args,3)==0);
    assert(!strcmp((const char *)guest_ptr(&s.memory,IDENT,512),D9_ADAPTER_DRIVER_NAME));
    assert(!strcmp((const char *)guest_ptr(&s.memory,IDENT+512,512),D9_ADAPTER_DESCRIPTION));
    assert(!strcmp((const char *)guest_ptr(&s.memory,IDENT+1024,32),D9_ADAPTER_DEVICE_NAME));
    assert(rd(&s,IDENT+1056)==0&&rd(&s,IDENT+1060)==0); /* DriverVersion: not a hardware driver */
    assert(rd(&s,IDENT+1064)==0&&rd(&s,IDENT+1068)==0&&rd(&s,IDENT+1072)==0&&rd(&s,IDENT+1076)==0);
    assert(!memcmp(guest_ptr(&s.memory,IDENT+1080,16),D9_ADAPTER_DEVICE_ID,sizeof D9_ADAPTER_DEVICE_ID));
    assert(rd(&s,IDENT+1096)==0); /* WHQLLevel: not certified */
    args[1]=2;assert(call(&s,s.factory,5,args,3)==0x0876086fu); /* D3DOK_NOAUTOGEN */
    args[1]=1;assert(call(&s,s.factory,5,args,3)==0x8876086cu);
    args[1]=0;args[2]=0;assert(call(&s,s.factory,5,args,3)==0x8876086cu);
    args[2]=IDENT;

    args[0]=0;args[1]=D9_X8R8G8B8;assert(call(&s,s.factory,6,args,2)==1);
    args[1]=D9_A8R8G8B8;assert(call(&s,s.factory,6,args,2)==0);
    args[0]=1;args[1]=D9_X8R8G8B8;assert(call(&s,s.factory,6,args,2)==0);

    args[0]=0;args[1]=D9_X8R8G8B8;args[2]=0;args[3]=MODE;
    assert(call(&s,s.factory,7,args,4)==0);
    assert(rd(&s,MODE)==400&&rd(&s,MODE+4)==240&&rd(&s,MODE+8)==60&&rd(&s,MODE+12)==D9_X8R8G8B8);
    args[2]=1;assert(call(&s,s.factory,7,args,4)==0x8876086cu);
    args[2]=0;args[1]=D9_A8R8G8B8;assert(call(&s,s.factory,7,args,4)==0x8876086cu);
    args[1]=D9_X8R8G8B8;args[0]=1;assert(call(&s,s.factory,7,args,4)==0x8876086cu);

    args[0]=0;args[1]=MODE;assert(call(&s,s.factory,8,args,2)==0);
    assert(rd(&s,MODE)==400&&rd(&s,MODE+4)==240&&rd(&s,MODE+8)==60&&rd(&s,MODE+12)==D9_X8R8G8B8);
    args[0]=1;assert(call(&s,s.factory,8,args,2)==0x8876086cu);

    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=D9_X8R8G8B8;args[4]=1;
    assert(call(&s,s.factory,9,args,5)==0);
    args[3]=D9_A8R8G8B8;assert(call(&s,s.factory,9,args,5)==0);
    args[3]=0;assert(call(&s,s.factory,9,args,5)==0);
    args[3]=D9_X8R8G8B8;args[4]=0;assert(call(&s,s.factory,9,args,5)==0x8876086au);
    args[4]=1;args[0]=1;assert(call(&s,s.factory,9,args,5)==0x8876086cu);
    args[0]=0;args[1]=2;assert(call(&s,s.factory,9,args,5)==0x8876086cu);
    args[1]=1;args[2]=D9_A8R8G8B8;assert(call(&s,s.factory,9,args,5)==0x8876086au);
    args[2]=D9_X8R8G8B8;args[3]=23;assert(call(&s,s.factory,9,args,5)==0x8876086au);

    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=0;args[4]=3;
    const uint32_t good[]={D9_A8R8G8B8,D9_X8R8G8B8,D9_DXT1,D9_DXT3,D9_DXT5};
    for(unsigned i=0;i<sizeof good/sizeof *good;++i){args[5]=good[i];assert(call(&s,s.factory,10,args,6)==0);}
    args[5]=75;assert(call(&s,s.factory,10,args,6)==0x8876086au);            /* D24S8 */
    args[5]=23;assert(call(&s,s.factory,10,args,6)==0x8876086au);            /* R5G6B5 */
    args[5]=0x32545844u;assert(call(&s,s.factory,10,args,6)==0x8876086au);   /* DXT2 */
    args[5]=0x34545844u;assert(call(&s,s.factory,10,args,6)==0x8876086au);   /* DXT4 */
    args[5]=D9_A8R8G8B8;args[3]=1;args[4]=1;assert(call(&s,s.factory,10,args,6)==0); /* LDR render-target surface */
    args[4]=3;assert(call(&s,s.factory,10,args,6)==0); /* LDR render-target texture */
    args[5]=0x71u;assert(call(&s,s.factory,10,args,6)==0x8876086au); /* float RT texture */
    args[5]=0x72u;assert(call(&s,s.factory,10,args,6)==0x8876086au); /* HDR RT texture */
    args[4]=5;assert(call(&s,s.factory,10,args,6)==0x8876086au); /* cube RT is also off by default */
    args[3]=0;
    const unsigned default_desc_types[]={1,3,5};
    for(unsigned i=0;i<sizeof default_desc_types/sizeof *default_desc_types;++i){
        args[4]=default_desc_types[i];args[5]=0x71u;
        assert(call(&s,s.factory,10,args,6)==0x8876086au); /* float descriptor off */
        args[5]=0x72u;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    }
    args[3]=1;args[4]=1;args[5]=D9_DXT1;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    args[5]=D9_A8R8G8B8;args[3]=0x00020001u;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    args[3]=0x00020000u;assert(call(&s,s.factory,10,args,6)==0x8876086au); /* QUERY_FILTER is texture-only */
    args[3]=0x00200000u;args[4]=3;assert(call(&s,s.factory,10,args,6)==0);       /* QUERY_WRAPANDMIP */
    args[3]=0x00220000u;assert(call(&s,s.factory,10,args,6)==0);
    args[3]=0x00010000u;assert(call(&s,s.factory,10,args,6)==0x8876086au); /* QUERY_SRGBREAD */
    args[3]=0;args[4]=1;assert(call(&s,s.factory,10,args,6)==0); /* CPU off-screen plain surface */
    args[4]=3;args[2]=D9_A8R8G8B8;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    args[2]=D9_X8R8G8B8;args[0]=1;assert(call(&s,s.factory,10,args,6)==0x8876086cu);
    args[0]=0;args[1]=2;assert(call(&s,s.factory,10,args,6)==0x8876086cu);

    /* GetDeviceCaps: SM2 and only implemented caps, both IDirect3D9 and the device. */
    args[0]=0;args[1]=1;args[2]=CAPS9;
    assert(call(&s,s.factory,14,args,3)==0);
    assert(rd(&s,CAPS9+0)==1&&rd(&s,CAPS9+4)==0);
    assert(rd(&s,CAPS9+8)==0&&rd(&s,CAPS9+12)==0&&rd(&s,CAPS9+16)==0&&rd(&s,CAPS9+20)==0);
    assert(rd(&s,CAPS9+24)==0);
    assert(rd(&s,CAPS9+28)==0x00082000u); /* DRAWPRIMITIVES2 | HWRASTERIZATION */
    assert((rd(&s,CAPS9+28)&0x00010000u)==0); /* HWTRANSFORMANDLIGHT stays clear */
    assert((rd(&s,CAPS9+28)&0x00008000u)==0); /* DRAWPRIMITIVES2EX stays clear */
    /* Positive and negative capability reasons are explicit and stable. */
    assert(strstr(d9_adapter_devcaps_reason(0x00002000u),"claimed:")==d9_adapter_devcaps_reason(0x00002000u));
    assert(strstr(d9_adapter_devcaps_reason(0x00080000u),"claimed:")==d9_adapter_devcaps_reason(0x00080000u));
    assert(strstr(d9_adapter_devcaps_reason(0x00008000u),"not claimed:")==d9_adapter_devcaps_reason(0x00008000u));
    assert(strstr(d9_adapter_devcaps_reason(0x00010000u),"not claimed:")==d9_adapter_devcaps_reason(0x00010000u));
    assert(strstr(d9_adapter_devcaps_reason(0x00010000u),"lighting")!=NULL);
    assert(strstr(d9_adapter_devcaps_reason(0x00010000u),"DrawPrimitiveUP")==NULL);
    assert(strstr(d9_adapter_devcaps_reason(0x00000010u),"not claimed:")==d9_adapter_devcaps_reason(0x00000010u));
    assert(rd(&s,CAPS9+32)==0x000008f2u);
    assert(rd(&s,CAPS9+36)==0x00000010u);
    assert(rd(&s,CAPS9+40)==0xffu&&rd(&s,CAPS9+44)==0x7ffu&&rd(&s,CAPS9+48)==0x7ffu&&rd(&s,CAPS9+52)==0xffu);
    assert(rd(&s,CAPS9+56)==0);
    assert(rd(&s,CAPS9+60)==0x00004007u);
    assert(rd(&s,CAPS9+64)==0x00030303u);
    assert(rd(&s,CAPS9+68)==0&&rd(&s,CAPS9+72)==0);
    assert(rd(&s,CAPS9+76)==0x17u&&rd(&s,CAPS9+80)==0&&rd(&s,CAPS9+84)==0);
    assert(rd(&s,CAPS9+88)==1024u&&rd(&s,CAPS9+92)==1024u&&rd(&s,CAPS9+96)==0);
    assert(rd(&s,CAPS9+100)==1024u&&rd(&s,CAPS9+104)==128u&&rd(&s,CAPS9+108)==1u);
    assert(rd(&s,CAPS9+112)==fbits(1.0e10f));
    assert(rd(&s,CAPS9+116)==0&&rd(&s,CAPS9+120)==0&&rd(&s,CAPS9+124)==0&&rd(&s,CAPS9+128)==0&&rd(&s,CAPS9+132)==0);
    assert(rd(&s,CAPS9+136)==0);
    assert(rd(&s,CAPS9+140)==1u&&rd(&s,CAPS9+144)==0&&rd(&s,CAPS9+148)==1u&&rd(&s,CAPS9+152)==1u);
    assert(rd(&s,CAPS9+156)==0&&rd(&s,CAPS9+160)==0&&rd(&s,CAPS9+164)==0);
    assert(rd(&s,CAPS9+168)==0&&rd(&s,CAPS9+172)==0&&rd(&s,CAPS9+176)==0);
    assert(rd(&s,CAPS9+180)==1365u&&rd(&s,CAPS9+184)==0xffffu&&rd(&s,CAPS9+188)==16u&&rd(&s,CAPS9+192)==4096u);
    assert(rd(&s,CAPS9+196)==0xfffe0200u&&rd(&s,CAPS9+200)==4u);
    assert(rd(&s,CAPS9+204)==0xffff0200u&&rd(&s,CAPS9+208)==0);
    assert(((rd(&s,CAPS9+196)>>8)&0xff)==2&&((rd(&s,CAPS9+204)>>8)&0xff)==2);
    assert(rd(&s,CAPS9+196)<0xfffe0300u&&rd(&s,CAPS9+204)<0xffff0300u); /* the gate must reject SM3 */
    assert(rd(&s,CAPS9+212)==1u&&rd(&s,CAPS9+216)==0&&rd(&s,CAPS9+220)==0); /* D3DDEVCAPS2_STREAMOFFSET */
    assert(rd(&s,CAPS9+224)==0&&rd(&s,CAPS9+228)==0&&rd(&s,CAPS9+232)==1);
    assert(rd(&s,CAPS9+236)==0x3ffu&&rd(&s,CAPS9+240)==1u&&rd(&s,CAPS9+244)==0);
    assert(rd(&s,CAPS9+248)==0&&rd(&s,CAPS9+252)==0&&rd(&s,CAPS9+256)==0&&rd(&s,CAPS9+260)==0);
    assert(rd(&s,CAPS9+264)==0&&rd(&s,CAPS9+268)==0&&rd(&s,CAPS9+272)==1&&rd(&s,CAPS9+276)==0&&rd(&s,CAPS9+280)==2);
    assert(rd(&s,CAPS9+284)==0&&rd(&s,CAPS9+288)==2u&&rd(&s,CAPS9+292)==2u);
    assert(rd(&s,CAPS9+296)==0&&rd(&s,CAPS9+300)==0);
    args[0]=CAPS9;assert(call(&s,s.device,7,args,1)==0);
    assert(rd(&s,CAPS9+196)==0xfffe0200u&&rd(&s,CAPS9+204)==0xffff0200u);

    /* Fallback changes only the measured format query, never DevCaps. */
    uint8_t caps_before[sizeof(D9Caps9)];
    memcpy(caps_before,guest_ptr(&s.memory,CAPS9,sizeof caps_before),sizeof caps_before);
    set_fallback("1");
    const unsigned rt_types[]={1,3,5}; /* SURFACE, TEXTURE, CUBETEXTURE */
    const unsigned float_formats[]={0x71u,0x72u};
    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=1;
    for(unsigned i=0;i<sizeof rt_types/sizeof *rt_types;++i)
        for(unsigned j=0;j<sizeof float_formats/sizeof *float_formats;++j){
            args[4]=rt_types[i];args[5]=float_formats[j];
            assert(call(&s,s.factory,10,args,6)==0);
        }
    args[3]=0;
    const unsigned desc_types[]={1,3,5}; /* SURFACE, TEXTURE, CUBETEXTURE */
    for(unsigned i=0;i<sizeof desc_types/sizeof *desc_types;++i)
        for(unsigned j=0;j<sizeof float_formats/sizeof *float_formats;++j){
            args[4]=desc_types[i];args[5]=float_formats[j];
            assert(call(&s,s.factory,10,args,6)==0); /* measured descriptor */
        }
    args[3]=1;args[4]=4;args[5]=0x71u;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    args[3]=0;args[4]=5;args[5]=0x73u;assert(call(&s,s.factory,10,args,6)==0x8876086au);
    args[0]=0;args[1]=1;args[2]=CAPS9;assert(call(&s,s.factory,14,args,3)==0);
    assert(!memcmp(caps_before,guest_ptr(&s.memory,CAPS9,sizeof caps_before),sizeof caps_before));
    assert(rd(&s,CAPS9+196)==0xfffe0200u&&rd(&s,CAPS9+204)==0xffff0200u);
    assert((rd(&s,CAPS9+28)&0x00010000u)==0);
    set_fallback(NULL);

    args[0]=1;args[1]=1;args[2]=CAPS9;assert(call(&s,s.factory,14,args,3)==0x8876086cu);
    args[0]=0;args[1]=2;assert(call(&s,s.factory,14,args,3)==0x8876086cu);

    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=1;args[4]=0;args[5]=MODE;
    assert(call(&s,s.factory,11,args,6)==0&&rd(&s,MODE)==1);
    args[4]=2;assert(call(&s,s.factory,11,args,6)==0x8876086au);
    args[4]=0;args[0]=1;assert(call(&s,s.factory,11,args,6)==0x8876086cu);

    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=D9_X8R8G8B8;args[4]=75;
    assert(call(&s,s.factory,12,args,5)==0); /* D24S8 for the 400x240 back buffer */
    args[4]=77;assert(call(&s,s.factory,12,args,5)==0); /* D24X8, same attachment */
    args[4]=80;assert(call(&s,s.factory,12,args,5)==0x8876086au); /* D16 */
    args[4]=75;args[3]=D9_A8R8G8B8;assert(call(&s,s.factory,12,args,5)==0);
    args[3]=D9_X8R8G8B8;args[1]=2;assert(call(&s,s.factory,12,args,5)==0x8876086cu);

    args[0]=0;args[1]=1;args[2]=D9_X8R8G8B8;args[3]=D9_DXT1;
    assert(call(&s,s.factory,13,args,4)==0x8876086au);
    args[1]=2;assert(call(&s,s.factory,13,args,4)==0x8876086cu);

    args[0]=0;assert(call(&s,s.factory,15,args,1)==0); /* no desktop monitor */
    args[0]=MODE;assert(call(&s,s.factory,3,args,1)==0x8876086au); /* D3DERR_NOTAVAILABLE */

    assert(!d9_com_demo_destroy(&s)&&d9_memory_used(d)==0);
    d9_destroy_device(d);
    puts("D3D9 adapter: one 400x240 X8R8G8B8 mode, SM2-only caps, truthful format/device checks passed");
    return 0;
}
