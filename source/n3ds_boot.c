#include "n3ds_boot.h"
#include "win32_shims.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef __3DS__
/* libctru's 3dsx crate ships a weak 32 KiB main-thread stack.  The menu path's
 * lifted call chain (n3ds_guest_ptr, TEV/menu helpers) overflows it: the first
 * Azahar fault was a push at SP 0x07FFFFA4 just below the 0x08000000 stack
 * top, i.e. stack exhaustion, not a guest/VRAM fault.  A strong definition is
 * read by 3dsxtool/libctru when sizing the main thread stack. */
unsigned int __stacksize__ = 128u * 1024u;
#endif

static int error(BootSession *b,const char *fmt,...){va_list a;va_start(a,fmt);vsnprintf(b->message,sizeof b->message,fmt,a);va_end(a);return -1;}
void n3ds_boot_init(BootSession *b,size_t budget){memset(b,0,sizeof *b);guest_init(&b->memory,budget);}
void n3ds_boot_set_unresolved_traps(BootSession *b,int enabled){
    if(!b)return;b->traps=enabled?1:0;win32_set_unresolved_trap(b->traps);
}
/* win32_resolve() can manufacture diagnostic trap thunks. Keep those out of
 * the implemented-builtin phase so they cannot mask real PE exports. */
static uint32_t boot_builtin(const PeImport *e,void *ctx){
    BootSession *b=ctx;win32_set_unresolved_trap(0);
    uint32_t address=win32_resolve(e,NULL);
    win32_set_unresolved_trap(b->traps);return address;
}
static uint32_t boot_fallback(const PeImport *e,void *ctx){
    BootSession *b=ctx;if(!b->traps)return 0;
    win32_set_unresolved_trap(1);uint32_t address=win32_resolve(e,NULL);
    win32_set_unresolved_trap(b->traps);return address;
}
int n3ds_boot_add_mapped(BootSession *b,const char *name,const GuestImage *image){
    if(b->bound||b->ready||b->shims||b->count>=GP_MAX_MODULES||!name||!*name||strlen(name)>63||!image||
       !image->size||!guest_range_mapped(&b->memory,image->base,image->size))return error(b,"invalid module registration");
    for(size_t j=0;j<b->count;++j)if(b->images[j].base==image->base)return error(b,"duplicate image address");
    size_t i=b->count++;b->images[i]=*image;strcpy(b->names[i],name);b->modules[i]=(PeModule){b->names[i],&b->images[i]};return 0;
}
int n3ds_boot_add_file(BootSession *b,const char *name,const char *path,uint32_t base){
    if(b->bound||b->ready||b->shims||b->count>=GP_MAX_MODULES)return error(b,"module limit or session already prepared");
    GuestImage image;int rc=pe_image_load(&b->memory,path,base,&image);if(rc)return error(b,"%s: %s",name,pe_error(rc));
    if(n3ds_boot_add_mapped(b,name,&image)){pe_image_unload(&b->memory,image.base);return -1;}return 0;
}
int n3ds_boot_add_data_file(BootSession *b,const char *name,const char *path,uint32_t base){
    int rc=n3ds_boot_add_file(b,name,path,base);
    if(!rc)b->data_only[b->count-1]=1;
    return rc;
}
static int check_import(const PeImport *e,void *ctx){
    BootSession *b=ctx;if(pe_link_resolve(e,&b->linker))return 0;
    if(e->name)error(b,"Unresolved import %s!%s",e->dll,e->name);else error(b,"Unresolved import %s!#%u",e->dll,(unsigned)e->ordinal);
    return PE_IMPORT_UNRESOLVED;
}
int n3ds_boot_prepare(BootSession *b,const FnModule *tables,unsigned n,uint32_t stack_bytes,uint32_t heap_bytes,const char *root){
    if(!b->count||b->bound||b->ready||b->shims||!b->images[0].entry)return error(b,"entry or session missing");
    b->linker=(PeLinker){.memory=&b->memory,.modules=b->modules,.count=b->count,
        .builtin=boot_builtin,.builtin_ctx=b,.fallback=boot_fallback,.fallback_ctx=b};
    for(size_t i=0;i<b->count;++i){
        const GuestImage *im=&b->images[i];
        if(im->entry && (im->entry<im->base||im->entry-im->base>=im->size))return error(b,"Invalid entry for %s",b->names[i]);
        int rc=pe_import_walk(&b->memory,im,check_import,b);
        if(rc==PE_IMPORT_UNRESOLVED)return -1;
        if(rc)return error(b,"Invalid import table for %s",b->names[i]);
    }
    if(guest_process_init(&b->process,&b->memory,b->modules,b->count,stack_bytes))return error(b,"PEB/TEB/TLS setup failed (format or memory)");
    if(win32_init(&b->memory,heap_bytes,root))return error(b,"guest heap setup failed");
    b->shims=1;win32_set_linker(&b->linker);
    for(size_t i=0;i<b->count;++i)if(pe_import_bind(&b->memory,&b->images[i],pe_link_resolve,&b->linker,NULL))return error(b,"IAT binding failed for %s",b->names[i]);
    if(n3ds_lift_bind_modules(&b->memory,tables,n))return error(b,"invalid generated function tables or runtime already used");
    b->bound=1;
    for(size_t i=0;i<b->count;++i)if(b->images[i].entry&&!b->data_only[i]&&!n3ds_lift_has_function(b->images[i].entry))return error(b,"Missing lifted/IR entry %s at %08lx",b->names[i],(unsigned long)b->images[i].entry);
    for(unsigned i=0;i<b->process.tls_count;++i){
        int skip=0;for(size_t k=0;k<b->count;++k)if(b->images[k].base==b->process.tls[i].image_base&&b->data_only[k])skip=1;
        if(skip)continue;
        for(unsigned j=0;j<b->process.tls[i].callback_count;++j){uint32_t cb=b->process.tls[i].callbacks[j];
            if(!n3ds_lift_has_function(cb))return error(b,"Missing lifted TLS callback %08lx",(unsigned long)cb);}
    }
    n3ds_lift_init_cpu(&b->cpu,b->process.stack_top,b->process.teb);
    b->ready=1;
    pe_image_touch_report(stderr, "prepared");
    snprintf(b->message,sizeof b->message,"Entry and %u TLS modules ready",b->process.tls_count);return 0;}
int n3ds_boot_prepare_skyrim(BootSession *b,const char *root,const FnModule *tables,unsigned n,uint32_t stack_bytes,uint32_t heap_bytes){
    if(!root||!*root||b->count)return error(b,"Expected empty Skyrim session and game root");
    static const char *const files[]={"TESV_Unpacked.exe","d3dx9_42.dll","binkw32.dll","X3DAudio1_7.dll"};
    static const char *const names[]={"TESV.exe","d3dx9_42.dll","binkw32.dll","X3DAudio1_7.dll"};
    static const uint32_t bases[]={0,0x08000000,0x09000000,0x0a000000};
    for(unsigned i=0;i<4;++i){char path[1024];int len=snprintf(path,sizeof path,"%s/%s",root,files[i]);
        if(len<0||(size_t)len>=sizeof path)return error(b,"Game path too long");
        if(n3ds_boot_add_file(b,names[i],path,bases[i]))return -1;
    }
    return n3ds_boot_prepare(b,tables,n,stack_bytes,heap_bytes,root);
}
/* 0 = normal call, 1 = ExitProcess, -1 = fault/invalid call. */
static int invoke(BootSession *b,uint32_t address,const uint32_t *args,size_t count,uint32_t *result){
    int rc=n3ds_lift_invoke(&b->cpu,address,args,count,result);
    if(rc==N3DS_INVOKE_EXIT)return 1;
    if(rc==N3DS_INVOKE_TRAP)return error(b,"%s",n3ds_lift_fault());
    if(rc)return error(b,"Call failed at %08lx",(unsigned long)address);
    return 0;
}
static int callbacks(BootSession *b,uint32_t base,uint32_t *result){
    uint32_t args[]={base,1,0};
    for(unsigned i=0;i<b->process.tls_count;++i)if(b->process.tls[i].image_base==base)
        for(unsigned j=0;j<b->process.tls[i].callback_count;++j){int rc=invoke(b,b->process.tls[i].callbacks[j],args,3,result);if(rc)return rc;}
    return 0;
}
int n3ds_boot_run(BootSession *b,uint32_t *status){
    if(!b->ready||b->ran||!status)return error(b,"session not ready or already used");b->ran=1;
    uint32_t r=0;int rc;
    for(size_t i=1;i<b->count;++i){
        if(b->data_only[i])continue;
        GuestImage *im=&b->images[i];rc=callbacks(b,im->base,&r);if(rc)goto finish;
        if(im->entry){uint32_t args[]={im->base,1,0};rc=invoke(b,im->entry,args,3,&r);if(rc)goto finish;
            if(!r)return error(b,"DllMain failed: %s",b->names[i]);}
    }
    rc=callbacks(b,b->images[0].base,&r);if(rc)goto finish;
    rc=invoke(b,b->images[0].entry,NULL,0,&r);
finish:
    if(rc<0)return -1;
    *status=r;snprintf(b->message,sizeof b->message,"Guest %s with status %lu",rc==1?"ExitProcess":"returned",(unsigned long)r);return 0;
}
void n3ds_boot_destroy(BootSession *b){
    if(!b)return;
    pe_image_touch_report(stderr, "exit");
    if(b->traps)win32_set_unresolved_trap(0);b->traps=0;
    if(b->bound)n3ds_lift_release();if(b->shims)win32_shutdown();
    guest_process_destroy(&b->process);guest_destroy(&b->memory);b->bound=b->shims=b->ready=0;b->count=0;
}
