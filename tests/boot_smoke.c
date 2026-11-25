/* End-to-end process bootstrap with self-authored static C test functions. */
#include "n3ds_boot.h"
#include "win32_shims.h"
#include <stdio.h>
#include <string.h>

#define EXE 0x00400000u
#define DLL 0x00800000u
#define EXE_ENTRY (EXE+0x1000)
#define EXE_TLS (EXE+0x1100)
#define EXE_FAULT (EXE+0x1200)
#define DLL_ENTRY (DLL+0x1000)
#define DLL_TLS (DLL+0x1100)
#define IAT (EXE+0x220)

static void require(CPU *c,int ok,const char *why){if(!ok)rt_unimpl(c,c->r[4],why);}
static uint32_t tls_block(CPU *c,uint32_t base){uint32_t idx=M32(base+0x350);return M32(M32(c->fs_base+0x2c)+idx*4);}
static void check_attach(CPU *c,uint32_t base){require(c,M32(c->r[4]+4)==base&&M32(c->r[4]+8)==1,"attach ABI");}
static void dll_tls(CPU *c){check_attach(c,DLL);uint32_t p=tls_block(c,DLL);require(c,M32(p)==0x1122 && M32(p+4)==0,"DLL TLS template/zero fill");M32(DLL+0x500)=1;c->r[4]+=16;}
static void dll_entry(CPU *c){check_attach(c,DLL);require(c,M32(DLL+0x500)==1,"DLL TLS order");M32(DLL+0x500)=2;c->r[0]=1;c->r[4]+=16;}
static void exe_tls(CPU *c){check_attach(c,EXE);require(c,M32(DLL+0x500)==2,"DLL init order");uint32_t p=tls_block(c,EXE);require(c,M32(p)==0x3344&&M32(p+4)==0,"EXE TLS template/zero fill");M32(EXE+0x500)=1;c->r[4]+=16;}
static uint32_t shim(CPU *c,uint32_t addr,const uint32_t *args,unsigned n){uint32_t sp=c->r[4];
    for(unsigned i=n;i>0;--i){c->r[4]-=4;M32(c->r[4])=args[i-1];}
    c->r[4]-=4;M32(c->r[4])=0xfeed0000;rt_call(c,addr);
    require(c,c->r[4]==sp,"stdcall shim cleanup");return c->r[0];}
static uint32_t named(CPU *c,const char *name,const uint32_t *args,unsigned n){PeImport e={.dll="kernel32.dll",.name=name};return shim(c,win32_resolve(&e,NULL),args,n);}
static void exe_entry(CPU *c){
    require(c,c->fs_base==GP_TEB && M32(c->fs_base+0x18)==GP_TEB && M32(c->fs_base+0x30)==GP_PEB,"TEB layout");
    require(c,M32(GP_PEB+8)==EXE && M32(GP_PEB+0x10)==GP_PARAMS && M32(EXE+0x500)==1,"PEB and main TLS order");
    require(c,M32(GP_PEB+0x10c)==GP_PEB+0x2000 && M32(GP_PEB+0x2000+0x18)==EXE,"loader module list");
    uint32_t cmd=shim(c,M32(IAT),NULL,0);
    require(c,cmd==GP_COMMAND_A&&!strcmp((char *)n3ds_guest_ptr(cmd,strlen("\"C:\\Skyrim\\TESV.exe\"")+1),"\"C:\\Skyrim\\TESV.exe\""),"command line");
    uint32_t args[]={0};require(c,shim(c,M32(IAT+4),args,1)==EXE,"GetModuleHandleA(NULL)");
    uint32_t pair[]={EXE+0x700,0};memcpy(n3ds_guest_ptr(pair[0],9),"test.dll",9);
    uint32_t handle=named(c,"GetModuleHandleA",pair,1);require(c,handle==DLL,"GetModuleHandleA DLL");
    pair[0]=handle;pair[1]=1;require(c,named(c,"GetProcAddress",pair,2)==DLL_ENTRY,"ordinal GetProcAddress");
    pair[1]=EXE+0x720;memcpy(n3ds_guest_ptr(pair[1],9),"TestFunc",9);
    require(c,named(c,"GetProcAddress",pair,2)==DLL_ENTRY,"named GetProcAddress");
    uint32_t env=named(c,"GetEnvironmentStringsW",NULL,0);require(c,env==GP_ENV_W&&M16(env)=='O',"wide environment");
    for(unsigned i=0;i<=64;++i)require(c,named(c,"TlsAlloc",NULL,0)==i,"dynamic TLS allocation");
    pair[0]=64;pair[1]=0x1234;require(c,named(c,"TlsSetValue",pair,2)==1 && M32(GP_TEB+0x1800)==0x1234,"TLS expansion");
    require(c,named(c,"TlsGetValue",pair,1)==0x1234,"TLS get");
    pair[0]=123;named(c,"SetLastError",pair,1);require(c,M32(GP_TEB+0x34)==123&&named(c,"GetLastError",NULL,0)==123,"TEB last error");
    args[0]=42;shim(c,M32(IAT+8),args,1);rt_unimpl(c,EXE_ENTRY,"ExitProcess returned");
}
static void fault_entry(CPU *c){c->r[0]=M32(0xdeadbeefu);c->r[4]+=4;}
static void w(GuestMemory *m,uint32_t a,uint32_t v){guest_write32(m,a,v);}
static void s(GuestMemory *m,uint32_t a,const char *v){guest_write(m,a,v,strlen(v)+1);}
static void tls_fixture(GuestMemory *m,uint32_t base,uint32_t value,uint32_t callback){
    w(m,base+0x100,base+0x300);w(m,base+0x104,base+0x304);w(m,base+0x108,base+0x350);
    w(m,base+0x10c,base+0x400);w(m,base+0x110,4);w(m,base+0x114,0);
    w(m,base+0x300,value);w(m,base+0x350,0xffffffff);w(m,base+0x400,callback);
}
int n3ds_lift_self_test(void){
    static BootSession b;int rc=-1;uint32_t v;
    static const FnEntry exe_table[]={{EXE_ENTRY,exe_entry},{EXE_TLS,exe_tls},{EXE_FAULT,fault_entry}};
    static const FnEntry dll_table[]={{DLL_ENTRY,dll_entry},{DLL_TLS,dll_tls}};
    static const FnModule tables[]={{"exe",exe_table,3},{"dll",dll_table,2}};
    n3ds_boot_init(&b,0x4000000) /* 64 MiB: VirtualAlloc commit test */;
    if(guest_map(&b.memory,EXE,0x2000)||guest_map(&b.memory,DLL,0x2000))goto done;
    GuestImage exe={.base=EXE,.size=0x2000,.entry=EXE_ENTRY,.tls_rva=0x100,.tls_size=24,.import_rva=0x180,.import_size=40};
    GuestImage dll={.base=DLL,.size=0x2000,.entry=DLL_ENTRY,.tls_rva=0x100,.tls_size=24,.export_rva=0x800,.export_size=0x100};
    tls_fixture(&b.memory,EXE,0x3344,EXE_TLS);tls_fixture(&b.memory,DLL,0x1122,DLL_TLS);
    w(&b.memory,DLL+0x810,1);w(&b.memory,DLL+0x814,1);w(&b.memory,DLL+0x818,1);w(&b.memory,DLL+0x81c,0x840);
    w(&b.memory,DLL+0x820,0x850);w(&b.memory,DLL+0x824,0x860);w(&b.memory,DLL+0x840,0x1000);w(&b.memory,DLL+0x850,0x870);
    s(&b.memory,DLL+0x870,"TestFunc");
    w(&b.memory,EXE+0x180,0x200);w(&b.memory,EXE+0x18c,0x280);w(&b.memory,EXE+0x190,0x220);
    s(&b.memory,EXE+0x280,"kernel32.dll");s(&b.memory,EXE+0x602,"GetCommandLineA");
    s(&b.memory,EXE+0x632,"GetModuleHandleA");s(&b.memory,EXE+0x662,"NotImplemented");
    for(unsigned i=0;i<3;++i){w(&b.memory,EXE+0x200+4*i,0x600+i*0x30);w(&b.memory,IAT+4*i,0x600+i*0x30);}
    if(n3ds_boot_add_mapped(&b,"TESV.exe",&exe)||n3ds_boot_add_mapped(&b,"test.dll",&dll))goto done;
    if(n3ds_boot_prepare(&b,tables,2,0x10000,0x8000,".")==0 || !strstr(b.message,"NotImplemented"))goto done;
    if(guest_read32(&b.memory,IAT,&v)||v!=0x600 || b.memory.count!=2)goto done;
    s(&b.memory,EXE+0x662,"ExitProcess");
    if(n3ds_boot_prepare(&b,tables,2,0x10000,0x8000,"."))goto done;
    if(b.process.tls_count!=2 || M32(EXE+0x350)!=0 || M32(DLL+0x350)!=1 || M32(EXE+0x500)||M32(DLL+0x500))goto done;
    uint32_t original_sp=b.cpu.r[4];
    if(n3ds_lift_invoke(&b.cpu,EXE_FAULT,NULL,0,&v)!=N3DS_INVOKE_TRAP || b.cpu.r[4]!=original_sp || !strstr(n3ds_lift_fault(),"unmapped"))goto done;
    if(n3ds_boot_run(&b,&v)||v!=42 || !strstr(b.message,"ExitProcess"))goto done;
    rc=0;
done:
    if(rc)fprintf(stderr,"Boot test failure: %s\n",b.message);else puts("PEB/TEB/TLS, IAT, DLL attach, entry and ExitProcess passed");
    n3ds_boot_destroy(&b);return rc;
}
#ifndef __3DS__
int main(int argc,char **argv){
    if(argc>1){BootSession b;n3ds_boot_init(&b,0x20000);int rc=n3ds_boot_add_file(&b,"fixture.exe",argv[1],0);
        if(!rc&&(b.count!=1||b.images[0].entry!=EXE_ENTRY))rc=-1;
        if(rc)fprintf(stderr,"File-backed image test failed: %s\n",b.message);else puts("File-backed boot image passed");
        n3ds_boot_destroy(&b);if(rc)return 1;}
    return n3ds_lift_self_test()?1:0;
}
#endif
