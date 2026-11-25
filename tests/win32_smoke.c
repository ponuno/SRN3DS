#include "win32_shims.h"
#include "n3ds_lift_runtime.h"
#include <stdio.h>
#include <string.h>
#ifndef __3DS__
#include <direct.h>
#endif

static int call(CPU *c,const char *dll,const char *name,const uint32_t *args,size_t n,uint32_t *out) {
    PeImport e={.dll=dll,.name=name};uint32_t fn=win32_resolve(&e,NULL);
    return fn?n3ds_lift_invoke(c,fn,args,n,out):-1;
}
#define K(name,args,n) call(cpu,"kernel32.dll",#name,args,n,&r)
#define U(name,args,n) call(cpu,"user32.dll",#name,args,n,&r)
#define O(name,args,n) call(cpu,"ole32.dll",#name,args,n,&r)
#define A32(name,args,n) call(cpu,"advapi32.dll",#name,args,n,&r)
#define C(name,args,n) call(cpu,"msvcrt.dll",#name,args,n,&r)
#define W(name,args,n) call(cpu,"winmm.dll",#name,args,n,&r)
#define CHECK(x) do{if(!(x)){fprintf(stderr,"Win32 test failed at line %d\n",__LINE__);goto done;}}while(0)
static void setw(GuestMemory *m,uint32_t addr,const char *s){
    (void)m;for(size_t i=0;i<=strlen(s);++i)M16(addr+(uint32_t)(i*2))=(uint8_t)s[i];
}
static int test_fault_kind;
static uint32_t test_fault_addr;
static char test_fault_text[128];
static void record_test_fault(GuestMemory *m,uint32_t addr,const char *text){
    test_fault_kind=(int)guest_last_fault(m);test_fault_addr=addr;
    snprintf(test_fault_text,sizeof test_fault_text,"%s",text?text:"");
}
static int failing_fill(void *ctx,uint64_t off,uint8_t *dst,size_t n,size_t *got){
    (void)ctx;(void)off;(void)dst;(void)n;(void)got;return -1;
}
/* Test the import walker with a tiny synthetic PE data image in guest memory. */
int win32_self_test(CPU *cpu,GuestMemory *m) {
    int rc=-1;uint32_t r,allocated=0,va=0;int mapped=0,dll_mapped=0;
    const uint32_t base=0x05000000u,buf=base+0x500;
    const uint32_t dllbase=0x06000000u;
    size_t bound=0;
#ifndef __3DS__
    const char *filename="shim_fixture.bin";int created=0;
    FILE *f=fopen(filename,"wbx");if(!f)return -1;
    created=1;int ok=fwrite("Skyrim",1,6,f)==6;if(fclose(f))ok=0;if(!ok)goto done;
    CHECK(win32_init(m,0x8000,".")==0);
#else
    CHECK(win32_init(m,0x8000,"sdmc:/Skyrim")==0);
#endif
    CHECK(guest_map(m,base,4096)==0);mapped=1;
    GuestImage im={.base=base,.size=4096,.import_rva=0x100,.import_size=80};
    CHECK(guest_map(m,dllbase,4096)==0);dll_mapped=1;
    GuestImage dll_image={.base=dllbase,.size=4096,.export_rva=0x100,.export_size=0x100};
    M32(dllbase+0x110)=1;M32(dllbase+0x114)=2;M32(dllbase+0x118)=1;
    M32(dllbase+0x11c)=0x180;M32(dllbase+0x120)=0x190;M32(dllbase+0x124)=0x198;
    M32(dllbase+0x180)=0x400;M32(dllbase+0x184)=0x1b0; /* forwarded export unsupported */
    M32(dllbase+0x190)=0x1a0;M16(dllbase+0x198)=0;
    memcpy(n3ds_guest_ptr(dllbase+0x1a0,9),"TestFunc",9);
    CHECK(pe_export_find(m,&dll_image,"TestFunc",0)==dllbase+0x400);
    CHECK(pe_export_find(m,&dll_image,NULL,1)==dllbase+0x400);
    CHECK(pe_export_find(m,&dll_image,NULL,2)==0 && pe_export_find(m,&dll_image,"Missing",0)==0);
    PeModule mods[]={{"test.dll",&dll_image}};PeLinker linker={m,mods,1,win32_resolve,NULL,NULL,NULL};
    win32_set_linker(&linker);
    M32(base+0x100)=0x200;M32(base+0x10c)=0x300;M32(base+0x110)=0x220;
    M32(base+0x114)=0x240;M32(base+0x120)=0x320;M32(base+0x124)=0x260;
    M32(base+0x128)=0x280;M32(base+0x134)=0x380;M32(base+0x138)=0x2a0;
    memcpy(n3ds_guest_ptr(base+0x300,13),"kernel32.dll",13);
    memcpy(n3ds_guest_ptr(base+0x320,11),"msvcrt.dll",11);
    memcpy(n3ds_guest_ptr(base+0x380,9),"TEST.dll",9);
    M32(base+0x200)=0x340;M32(base+0x220)=0x340;
    M32(base+0x240)=0x360;M32(base+0x260)=0x360;
    M32(base+0x280)=0x80000001;M32(base+0x2a0)=0x80000001;
    memcpy(n3ds_guest_ptr(base+0x342,15),"GetProcessHeap",15);
    memcpy(n3ds_guest_ptr(base+0x362,7),"strlen",7);
    /* The ordinal is unresolved, so no IAT cell should have changed. */
    M32(base+0x244)=0x80000001;M32(base+0x264)=0x80000001;
    CHECK(pe_import_bind(m,&im,pe_link_resolve,&linker,&bound)==PE_IMPORT_UNRESOLVED);
    CHECK(M32(base+0x220)==0x340 && M32(base+0x260)==0x360 && bound==0);
    M32(base+0x244)=0;M32(base+0x264)=0;
    CHECK(pe_import_bind(m,&im,pe_link_resolve,&linker,&bound)==0 && bound==3 && M32(base+0x2a0)==dllbase+0x400);
    uint32_t heap_handle;
    CHECK(n3ds_lift_invoke(cpu,M32(base+0x220),NULL,0,&heap_handle)==0 && heap_handle==1);
    memcpy(n3ds_guest_ptr(buf,7),"Skyrim",7);uint32_t a[8]={buf};
    CHECK(n3ds_lift_invoke(cpu,M32(base+0x260),a,1,&r)==0 && r==6);
    a[0]=heap_handle;a[1]=8;a[2]=64;CHECK(!K(HeapAlloc,a,3) && r);allocated=r;
    CHECK(M32(allocated)==0);a[0]=allocated;a[1]=buf;a[2]=7;
    CHECK(!C(memcpy,a,3) && r==allocated && !strcmp(guest_cstr(m,allocated,64),"Skyrim"));
    a[0]=allocated;CHECK(!C(strlen,a,1) && r==6);
    a[0]=allocated;a[1]=128;CHECK(!C(realloc,a,2) && r);allocated=r;
    CHECK(!strcmp(guest_cstr(m,allocated,128),"Skyrim"));a[0]=allocated;CHECK(!C(free,a,1));allocated=0;
    /* Separate heap objects (HeapCreate/HeapDestroy), not the process heap. */
    a[0]=1;a[1]=0x10000;a[2]=0;CHECK(!K(HeapCreate,a,3) && r>=0x200);uint32_t extra_heap=r;
    a[0]=extra_heap;a[1]=8;a[2]=64;CHECK(!K(HeapAlloc,a,3) && r);uint32_t extra_mem=r;
    M32(extra_mem)=0x1234;
    a[0]=extra_heap;a[1]=0;a[2]=extra_mem;CHECK(!K(HeapFree,a,3) && r==1);
    a[0]=extra_heap;CHECK(!K(HeapDestroy,a,1) && r==1);
    a[0]=extra_heap;CHECK(!K(HeapDestroy,a,1) && r==0);
    a[0]=0;a[1]=4096;a[2]=0x3000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r);va=r;CHECK(M32(va)==0);
    a[0]=va;CHECK(!K(InterlockedIncrement,a,1) && r==1);a[1]=5;a[2]=1;
    CHECK(!K(InterlockedCompareExchange,a,3) && r==1 && M32(va)==5);
    a[0]=va;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==1);va=0;
    /* VirtualAlloc: a reservation is address space only; MEM_COMMIT records a
     * promise that is backed lazily on first access. */
    Win32VaStats vst;
    a[0]=0;a[1]=256u<<20;a[2]=0x2000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r && !(r&0xffff));
    uint32_t reserve=r;
    win32_va_stats(&vst);
    CHECK(vst.reserved_live==(256u<<20) && vst.committed_live==0 && vst.backed_live==0);
    CHECK(guest_ptr(m,reserve,4)==NULL); /* reserved, not committed */
    a[0]=reserve;a[1]=buf+2200;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28);
    CHECK(M32(buf+2204)==reserve && M32(buf+2208)==1 && M32(buf+2212)==(256u<<20));
    CHECK(M32(buf+2216)==0x2000 && M32(buf+2220)==1 && M32(buf+2224)==0);
    /* An unaligned MEM_COMMIT rounds down/up to 4 KiB and is a promise only:
     * 0x40..0x1b0 becomes the two pages 0x0000..0x2000. */
    a[0]=reserve+0x40;a[1]=0x170;a[2]=0x1000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r==reserve);
    win32_va_stats(&vst);
    CHECK(vst.committed_live==0x2000 && vst.backed_live==0);
    /* Committed pages are zero-filled and become physical on first access. */
    CHECK(guest_ptr(m,reserve,4)!=NULL && M32(reserve)==0);
    M32(reserve)=0x11223344u;
    win32_va_stats(&vst);CHECK(vst.backed_live==0x1000);
    /* Re-committing an already committed page is idempotent and preserves it. */
    a[0]=reserve;a[1]=4096;a[2]=0x1000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r==reserve && M32(reserve)==0x11223344u);
    win32_va_stats(&vst);CHECK(vst.backed_live==0x1000);
    CHECK(guest_ptr(m,reserve+0x1000,4)!=NULL && M32(reserve+0x1000)==0);
    M32(reserve+0x1000)=0x55667788u;
    a[0]=reserve;a[1]=buf+2200;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28 && M32(buf+2212)==0x2000 &&
          M32(buf+2216)==0x1000 && M32(buf+2208)==4);
    /* The measured engine commit: 85 MiB of promise, two pages of RAM. */
    a[0]=reserve;a[1]=85u<<20;a[2]=0x1000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r==reserve);
    win32_va_stats(&vst);
    CHECK(vst.committed_live==(85u<<20) && vst.reserved_live==(256u<<20));
    CHECK(vst.backed_live==0x2000); /* only the two touched pages */
    /* The engine's second per-block commit: unaligned address, inside the
     * 85 MiB promise, and already committed. Must succeed without clearing. */
    a[0]=reserve+0x40;a[1]=0x170;a[2]=0x1000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r==reserve && M32(reserve)==0x11223344u);
    win32_va_stats(&vst);CHECK(vst.backed_live==0x2000);
    /* Decommit one page: backing and promise disappear, the other page stays. */
    a[0]=reserve+0x1000;a[1]=4096;a[2]=0x4000;CHECK(!K(VirtualFree,a,3) && r==1);
    CHECK(guest_ptr(m,reserve+0x1000,4)==NULL && M32(reserve)==0x11223344u);
    a[0]=reserve;a[1]=buf+2200;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28 && M32(buf+2212)==0x1000 && M32(buf+2216)==0x1000);
    /* MEM_RELEASE needs the allocation base and size 0. */
    a[0]=reserve+0x1000;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==487);
    a[0]=reserve;a[1]=4096;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==87);
    a[0]=reserve;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==1);
    win32_va_stats(&vst);
    CHECK(vst.reserved_live==0 && vst.committed_live==0 && vst.backed_live==0);
    /* Large commit inside a large reservation: a promise costs page tables,
     * not RAM (and no longer consumes the 64-entry host-region table). */
    a[0]=0;a[1]=128u<<20;a[2]=0x2000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r);uint32_t big_res=r;
    a[0]=big_res;a[1]=32u<<20;a[2]=0x1000;a[3]=4;
    CHECK(!K(VirtualAlloc,a,4) && r==big_res);
    win32_va_stats(&vst);
    CHECK(vst.committed_live==(32u<<20) && vst.reserved_live==(128u<<20) && vst.backed_live==0);
    /* The uncommitted tail is still a reservation, not free memory. */
    a[0]=big_res+(32u<<20);a[1]=buf+2200;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28 && M32(buf+2216)==0x2000 &&
          M32(buf+2220)==1 && M32(buf+2212)==(96u<<20));
    a[0]=big_res;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==1);
    win32_va_stats(&vst);
    CHECK(vst.reserved_live==0 && vst.committed_live==0 && vst.backed_live==0);
    /* Physical backing is capped by the guest pool, not by the commit promise. */
    a[0]=0;a[1]=1u<<20;a[2]=0x2000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r);uint32_t cap=r;
    a[0]=cap;a[1]=1u<<20;a[2]=0x1000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r==cap);
    size_t saved_limit=m->limit;
    m->limit=m->used+4096;
    CHECK(guest_ptr(m,cap,4)!=NULL);
    test_fault_kind=0;guest_set_fault_handler(m,record_test_fault);
    CHECK(guest_ptr(m,cap+4096,4)==NULL); /* no budget for a second page */
    CHECK(test_fault_kind==GUEST_FAULT_NO_MEMORY && test_fault_addr==cap+4096 &&
          !strcmp(test_fault_text,"guest memory backing failed: out of memory"));
    guest_set_fault_handler(m,NULL);
    m->limit=saved_limit;
    CHECK(guest_ptr(m,cap+4096,4)!=NULL);
    a[0]=cap;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==1);
    /* Cross-page guest accessors: 4 KiB page boundary, multi-page string, and
     * no partial write when the next page is reserved. */
    a[0]=0;a[1]=4u*4096;a[2]=0x2000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r);uint32_t xp=r;
    a[0]=xp;a[1]=3u*4096;a[2]=0x1000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r==xp);
    const uint8_t pat[8]={0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17};
    CHECK(guest_write(m,xp+0xFFE,pat,8)==0);
    uint8_t xrd[8]={0};
    CHECK(guest_read(m,xp+0xFFE,xrd,8)==0 && !memcmp(xrd,pat,8));
    CHECK(guest_write32(m,xp+0xFFE,0xAABBCCDDu)==0);
    uint32_t xv=0;CHECK(guest_read32(m,xp+0xFFE,&xv)==0 && xv==0xAABBCCDDu);
    uint8_t tail[2]={0};
    /* A second adjacent bridge must preserve page 0 from the first bridge. */
    CHECK(guest_write(m,xp+0x1FFE,pat,4)==0 && guest_read(m,xp+0x1FFE,xrd,4)==0 && !memcmp(xrd,pat,4));
    CHECK(guest_read(m,xp+0xFFE,tail,2)==0 && tail[0]==0xDD && tail[1]==0xCC);
    /* Decommit one page of the bridge: the other page must keep its bytes. */
    a[0]=xp+4096;a[1]=4096;a[2]=0x4000;CHECK(!K(VirtualFree,a,3) && r==1);
    memset(tail,0,sizeof tail);CHECK(guest_read(m,xp+0xFFE,tail,2)==0 && tail[0]==0xDD && tail[1]==0xCC);
    CHECK(guest_read(m,xp+8192,tail,2)==0 && tail[0]==0x12 && tail[1]==0x13);
    CHECK(guest_read32(m,xp+0xFFE,&xv)!=0); /* crosses into reserved page 1 */
    a[0]=xp+4096;a[1]=4096;a[2]=0x1000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r==xp+4096);
    CHECK(guest_read32(m,xp+0xFFE,&xv)==0 && xv==0x0000CCDDu);
    const char *xstr="page-spanning-string";
    uint32_t xso=xp+0x1000-6;
    CHECK(guest_write(m,xso,xstr,strlen(xstr)+1)==0);
    CHECK(guest_cstr(m,xso,64) && !strcmp(guest_cstr(m,xso,64),xstr));
    /* The fourth page is reserved, so a 4-byte write starting in the last two
     * bytes of page 3 must fail before touching either byte. */
    const uint8_t sentinel[2]={0x5A,0x5A};
    CHECK(guest_write(m,xp+3*4096-4,sentinel,2)==0);
    CHECK(guest_write(m,xp+3*4096-2,pat,4)!=0);
    uint8_t keep[2]={0};
    CHECK(guest_read(m,xp+3*4096-4,keep,2)==0 && keep[0]==0x5A && keep[1]==0x5A);
    a[0]=xp+3*4096;a[1]=4096;a[2]=0x1000;a[3]=4;CHECK(!K(VirtualAlloc,a,4) && r==xp+3*4096);
    CHECK(guest_write(m,xp+3*4096-2,pat,4)==0 && guest_read(m,xp+3*4096-2,xrd,4)==0 && !memcmp(xrd,pat,4));
    GuestPageStats ps;guest_page_stats(m,&ps);
    CHECK(ps.first_touched>0 && ps.decommitted>0 && ps.anon>0);
    a[0]=xp;a[1]=0;a[2]=0x8000;CHECK(!K(VirtualFree,a,3) && r==1);
    /* File-provider failure is a distinct explicit fault, not "unmapped". */
    CHECK(guest_slab_reserve(m,0x68000000u,4096)!=NULL);
    CHECK(guest_slab_set_provider(m,0x68000000u,4,0,failing_fill,NULL,NULL,0)==0);
    test_fault_kind=0;guest_set_fault_handler(m,record_test_fault);
    CHECK(guest_ptr(m,0x68000000u,1)==NULL);
    CHECK(test_fault_kind==GUEST_FAULT_IO && test_fault_addr==0x68000000u &&
          !strcmp(test_fault_text,"guest file-backed page I/O error"));
    guest_set_fault_handler(m,NULL);
    CHECK(guest_slab_release(m,0x68000000u)==0);
    a[0]=buf+64;CHECK(!K(QueryPerformanceCounter,a,1) && r==1);CHECK(!K(QueryPerformanceFrequency,a,1) && r==1 && M64(buf+64)>0);
    a[0]=123;CHECK(!K(SetLastError,a,1));CHECK(!K(GetLastError,NULL,0) && r==123);
    CHECK(!K(TlsAlloc,NULL,0) && r!=UINT32_MAX);uint32_t tls=r;
    a[0]=tls;a[1]=99;CHECK(!K(TlsSetValue,a,2) && r==1);
    CHECK(!K(TlsGetValue,a,1) && r==99);CHECK(!K(TlsFree,a,1) && r==1);
    CHECK(!K(TlsGetValue,a,1) && r==0);CHECK(!K(GetLastError,NULL,0) && r==87);
    a[0]=0;CHECK(!K(FlsAlloc,a,1) && r!=UINT32_MAX);uint32_t fls=r;
    a[0]=fls;a[1]=77;CHECK(!K(FlsSetValue,a,2) && r==1);
    a[0]=fls;CHECK(!K(FlsGetValue,a,1) && r==77);
    a[0]=fls;CHECK(!K(FlsFree,a,1) && r==1);
    CHECK(!K(FlsGetValue,a,1) && r==0);CHECK(!K(GetLastError,NULL,0) && r==87);
    a[0]=64;CHECK(!K(SetHandleCount,a,1) && r==64);
    a[0]=0;a[1]=2;CHECK(!O(CoInitializeEx,a,2) && r==0);
    CHECK(!O(CoInitializeEx,a,2) && r==1);
    CHECK(!O(CoUninitialize,NULL,0));CHECK(!O(CoUninitialize,NULL,0));
    a[0]=0;a[1]=0;a[2]=0x4ff000;a[3]=0;a[4]=4;a[5]=buf+1700; /* CREATE_SUSPENDED */
    CHECK(!K(CreateThread,a,6) && r>=0x600 && M32(buf+1700)>=2);
    uint32_t thread=r;
    a[0]=thread;a[1]=buf+1990;CHECK(!K(GetExitCodeThread,a,2) && r==1 && M32(buf+1990)==259);
    a[0]=thread;a[1]=-1;CHECK(!K(SetThreadPriority,a,2) && r==1);
    a[0]=thread;CHECK(!K(GetThreadPriority,a,1) && r==(uint32_t)-1);
    a[0]=thread;CHECK(!K(ResumeThread,a,1) && r==1); /* body 0x401000 is not lifted: runs and faults */
    a[0]=thread;a[1]=buf+1990;CHECK(!K(GetExitCodeThread,a,2) && r==1 && M32(buf+1990)==0xC0000001u);
    a[0]=thread;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=0xfffffffeu;CHECK(!K(ResumeThread,a,1) && r==0);
    a[0]=0xfffffffeu;a[1]=0;CHECK(!K(SetThreadPriority,a,2) && r==1);
    a[0]=0x1234;CHECK(!K(ResumeThread,a,1) && r==UINT32_MAX);CHECK(!K(GetLastError,NULL,0) && r==6);
    CHECK(!U(GetActiveWindow,NULL,0) && r==0);
    a[0]=0;CHECK(!U(DispatchMessageA,a,1) && r==0);
    memcpy(n3ds_guest_ptr(buf+1400,7),"Player",7);
    a[0]=buf+1420;a[1]=buf+1440;M32(buf+1440)=64;CHECK(!A32(GetUserNameA,a,2) && r==1 && !strcmp(guest_cstr(m,buf+1420,16),"Player") && M32(buf+1440)==7);
    a[0]=buf+256;CHECK(!K(GetSystemInfo,a,1) && M32(buf+260)==4096 && M32(buf+276)==1);
    a[0]=buf+304;CHECK(!K(GetSystemTimeAsFileTime,a,1) && M64(buf+304)>0);
    a[0]=buf+64;a[1]=buf+68;M32(buf+68)=0;
    CHECK(!K(GetLogicalProcessorInformation,a,2) && r==0 && M32(buf+68)==24);
    CHECK(!K(GetLastError,NULL,0) && r==122);
    M32(buf+68)=24;CHECK(!K(GetLogicalProcessorInformation,a,2) && r==1 && M32(buf+68)==24 && M32(buf+64)==1);
    /* Read-only file handle adapter. */
    memcpy(n3ds_guest_ptr(buf,7),"../bad",7);
    a[0]=buf;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r==UINT32_MAX);
#ifndef __3DS__
    memcpy(n3ds_guest_ptr(buf,17),"shim_fixture.bin",17);
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t handle=r;
    a[0]=handle;CHECK(!K(GetFileType,a,1) && r==1);    a[0]=handle;a[1]=0;CHECK(!K(GetFileSize,a,2) && r==6);
    a[0]=handle;a[1]=1;a[2]=0;a[3]=0;CHECK(!K(SetFilePointer,a,4) && r==1);
    a[0]=handle;a[1]=buf+128;a[2]=5;a[3]=buf+200;a[4]=0;CHECK(!K(ReadFile,a,5) && r==1 && M32(buf+200)==5);
    CHECK(!memcmp(n3ds_guest_ptr(buf+128,5),"kyrim",5));a[0]=handle;CHECK(!K(CloseHandle,a,1) && r==1);
    /* File-backed lazy view: CreateFileMappingA/MapViewOfFile must reserve
     * guest VA without copying the file, fill pages on first access, and write
     * dirty writable pages back on UnmapViewOfFile. */
    {
        FILE *mf=fopen("shim_map.bin","wb");CHECK(mf);
        for (int i=0;i<3*4096;++i) fputc(i&0xff,mf);
        CHECK(fclose(mf)==0);
    }
    memcpy(n3ds_guest_ptr(buf,13),"shim_map.bin",13);
    a[0]=buf;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t map_file=r;
    a[0]=map_file;a[1]=0;a[2]=2;a[3]=0;a[4]=0;a[5]=0;
    CHECK(!K(CreateFileMappingA,a,6) && r>=0x400 && r<0x410);uint32_t map_handle=r;
    size_t used_before=m->used;
    a[0]=map_handle;a[1]=4;a[2]=0;a[3]=0;a[4]=0;
    CHECK(!K(MapViewOfFile,a,5) && r);uint32_t view=r;
    CHECK(m->used-used_before < 0x10000); /* page tables, not a 12 KiB copy */
    CHECK(M8(view)==0 && M8(view+1)==1);
    CHECK(M8(view+4095)==(4095&0xff) && M8(view+4096)==0 && M8(view+8191)==(8191&0xff));
    a[0]=view;a[1]=buf+2200;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28);
    CHECK(M32(buf+2204)==view && M32(buf+2216)==0x1000 && M32(buf+2224)==0x40000);
    a[0]=map_handle;a[1]=4;a[2]=0;a[3]=0x100;a[4]=0;
    CHECK(!K(MapViewOfFile,a,5) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==87);
    a[0]=view;CHECK(!K(UnmapViewOfFile,a,1) && r==1);
    CHECK(guest_ptr(m,view,1)==NULL);
    a[0]=map_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=map_file;CHECK(!K(CloseHandle,a,1) && r==1);
    /* Writable view: dirty page writeback on unmap. */
    memcpy(n3ds_guest_ptr(buf,13),"shim_map.bin",13);
    a[0]=buf;a[1]=0xC0000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);map_file=r;
    a[0]=map_file;a[1]=0;a[2]=4;a[3]=0;a[4]=0;a[5]=0;
    CHECK(!K(CreateFileMappingA,a,6) && r>=0x400);map_handle=r;
    a[0]=map_handle;a[1]=2;a[2]=0;a[3]=0;a[4]=0;
    CHECK(!K(MapViewOfFile,a,5) && r);view=r;
    M8(view+4096)=0xAA;M8(view+4097)=0xBB;
    a[0]=view;CHECK(!K(UnmapViewOfFile,a,1) && r==1);
    a[0]=map_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=map_file;CHECK(!K(CloseHandle,a,1) && r==1);
    {
        FILE *rf=fopen("shim_map.bin","rb");CHECK(rf);
        CHECK(fseek(rf,4096,SEEK_SET)==0 && fgetc(rf)==0xAA && fgetc(rf)==0xBB);
        CHECK(fclose(rf)==0);
    }
    /* Clean file pages are evicted under pressure and reloaded from disk on
     * the next touch (no silent stale copy). */
    memcpy(n3ds_guest_ptr(buf,13),"shim_map.bin",13);
    a[0]=buf;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);map_file=r;
    a[0]=map_file;a[1]=0;a[2]=2;a[3]=0;a[4]=0;a[5]=0;
    CHECK(!K(CreateFileMappingA,a,6) && r>=0x400);map_handle=r;
    a[0]=map_handle;a[1]=4;a[2]=0;a[3]=0;a[4]=0;CHECK(!K(MapViewOfFile,a,5) && r);uint32_t ev1=r;
    a[0]=map_handle;a[1]=4;a[2]=0;a[3]=0;a[4]=0;CHECK(!K(MapViewOfFile,a,5) && r);uint32_t ev2=r;
    CHECK(M8(ev1)==0 && M8(ev2)==0);
    GuestPageStats ps0,ps1;guest_page_stats(m,&ps0);
    saved_limit=m->limit;m->limit=m->used;
    CHECK(M8(ev2+4096)==0xAA); /* forces one clean file page out */
    guest_page_stats(m,&ps1);CHECK(ps1.evicted>ps0.evicted);
    CHECK(M8(ev1)==0 && M8(ev2)==0); /* one of these reloads from disk */
    guest_page_stats(m,&ps1);CHECK(ps1.reloaded>ps0.reloaded);
    m->limit=saved_limit;
    a[0]=ev1;CHECK(!K(UnmapViewOfFile,a,1) && r==1);
    a[0]=ev2;CHECK(!K(UnmapViewOfFile,a,1) && r==1);
    a[0]=map_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=map_file;CHECK(!K(CloseHandle,a,1) && r==1);
    /* Dirty pages are flushed before eviction, then released. */
    memcpy(n3ds_guest_ptr(buf,13),"shim_map.bin",13);
    a[0]=buf;a[1]=0xC0000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);map_file=r;
    a[0]=map_file;a[1]=0;a[2]=4;a[3]=0;a[4]=4u*4096;a[5]=0;
    CHECK(!K(CreateFileMappingA,a,6) && r>=0x400);map_handle=r;
    a[0]=map_handle;a[1]=2;a[2]=0;a[3]=0;a[4]=4u*4096;CHECK(!K(MapViewOfFile,a,5) && r);view=r;
    M8(view)=0x31;M8(view+4096)=0x32;M8(view+8192)=0x33;
    guest_page_stats(m,&ps0);
    m->limit=m->used;
    (void)M8(view+3*4096); /* fourth page forces a dirty writeback+eviction */
    guest_page_stats(m,&ps1);CHECK(ps1.evicted>ps0.evicted && ps1.dirty_written>ps0.dirty_written);
    m->limit=saved_limit;
    a[0]=view;CHECK(!K(UnmapViewOfFile,a,1) && r==1);
    a[0]=map_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=map_file;CHECK(!K(CloseHandle,a,1) && r==1);
    {
        FILE *rf=fopen("shim_map.bin","rb");CHECK(rf);
        CHECK(fseek(rf,0,SEEK_SET)==0 && fgetc(rf)==0x31 && fgetc(rf)==1);
        CHECK(fseek(rf,4096,SEEK_SET)==0 && fgetc(rf)==0x32);
        CHECK(fseek(rf,8192,SEEK_SET)==0 && fgetc(rf)==0x33);
        CHECK(fclose(rf)==0);
    }
    guest_page_stats(m,&ps0);
    fprintf(stderr,"[mapping-test] pages first-touch=%lu (anon=%lu file=%lu), dirty-written=%lu, "
                   "evicted=%lu (clean=%lu), reloads=%lu, failures=%lu\n",
            (unsigned long)ps0.first_touched,(unsigned long)ps0.anon,
            (unsigned long)ps0.file,(unsigned long)ps0.dirty_written,
            (unsigned long)ps0.evicted,(unsigned long)ps0.evicted_clean,
            (unsigned long)ps0.reloaded,(unsigned long)ps0.failures);
    /* Directory, writable file, enumeration and error paths. Guest cwd is the
     * game root (".") here, so all names stay inside the sandbox. */
    memcpy(n3ds_guest_ptr(buf+1800,9),"shim_dir",9);
    a[0]=buf+1800;a[1]=0;CHECK(!K(CreateDirectoryA,a,2) && r==1);
    a[0]=buf+1800;a[1]=0;CHECK(!K(CreateDirectoryA,a,2) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==183);
    a[0]=buf+1800;CHECK(!K(GetFileAttributesA,a,1) && r==0x10);
    a[0]=buf+1800;a[1]=0;a[2]=buf+1760;CHECK(!K(GetFileAttributesExA,a,3) && r==1);
    CHECK(M32(buf+1760)==0x10 && M32(buf+1788)==0 && M32(buf+1792)==0);
    memcpy(n3ds_guest_ptr(buf+1910,21),"shim_dir/missing.bin",21);
    a[0]=buf+1910;CHECK(!K(GetFileAttributesA,a,1) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==2);
    memcpy(n3ds_guest_ptr(buf+1890,10),"../escape",10);
    a[0]=buf+1890;CHECK(!K(GetFileAttributesA,a,1) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==87);
    /* Create/write/seek/truncate/flush/close. */
    memcpy(n3ds_guest_ptr(buf+1810,15),"shim_dir/w.txt",15);
    memcpy(n3ds_guest_ptr(buf+1900,6),"hello",6);
    a[0]=buf+1810;a[1]=0x40000000u;a[2]=0;a[3]=0;a[4]=2;a[5]=0x80;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t wh=r;
    a[0]=wh;a[1]=buf+1720;CHECK(!K(GetFileSizeEx,a,2) && r==1 && M64(buf+1720)==0);
    a[0]=wh;a[1]=buf+1900;a[2]=5;a[3]=buf+1720;a[4]=0;
    CHECK(!K(WriteFile,a,5) && r==1 && M32(buf+1720)==5);
    a[0]=wh;a[1]=buf+1720;CHECK(!K(GetFileSizeEx,a,2) && r==1 && M64(buf+1720)==5);
    a[0]=wh;a[1]=2;a[2]=0;a[3]=buf+1720;a[4]=0;
    CHECK(!K(SetFilePointerEx,a,5) && r==1 && M64(buf+1720)==2);
    a[0]=wh;a[1]=0xfffffffeu;a[2]=0xffffffffu;a[3]=buf+1720;a[4]=2;
    CHECK(!K(SetFilePointerEx,a,5) && r==1 && M64(buf+1720)==3);
    a[0]=wh;CHECK(!K(SetEndOfFile,a,1) && r==1);
    a[0]=wh;CHECK(!K(FlushFileBuffers,a,1) && r==1);
    a[0]=wh;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=buf+1810;CHECK(!K(GetFileAttributesA,a,1) && r==0x80);
    a[0]=buf+1810;a[1]=0;a[2]=buf+1760;CHECK(!K(GetFileAttributesExA,a,3) && r==1);
    CHECK(M32(buf+1760)==0x80 && M32(buf+1788)==0 && M32(buf+1792)==3);
    a[0]=buf+1810;a[1]=0x40000000u;a[2]=0;a[3]=0;a[4]=1;a[5]=0x80;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==80);
    /* Reopen read-only, verify content and that writes/truncation are refused. */
    a[0]=buf+1810;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0x80;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t rh=r;
    a[0]=rh;a[1]=buf+128;a[2]=8;a[3]=buf+1720;a[4]=0;
    CHECK(!K(ReadFile,a,5) && r==1 && M32(buf+1720)==3 && !memcmp(n3ds_guest_ptr(buf+128,3),"hel",3));
    a[0]=rh;a[1]=buf+1900;a[2]=1;a[3]=buf+1720;a[4]=0;
    CHECK(!K(WriteFile,a,5) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==5);
    a[0]=rh;CHECK(!K(SetEndOfFile,a,1) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==5);
    a[0]=rh;a[1]=buf+1720;CHECK(!K(GetFileSizeEx,a,2) && M64(buf+1720)==3);
    a[0]=rh;CHECK(!K(CloseHandle,a,1) && r==1);
    /* OPEN_ALWAYS must not truncate, TRUNCATE_EXISTING must. */
    a[0]=buf+1810;a[1]=0xC0000000u;a[2]=0;a[3]=0;a[4]=4;a[5]=0x80;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t oh=r;
    a[0]=oh;a[1]=buf+1720;CHECK(!K(GetFileSizeEx,a,2) && M64(buf+1720)==3);
    a[0]=oh;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=buf+1810;a[1]=0x40000000u;a[2]=0;a[3]=0;a[4]=5;a[5]=0x80;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t th=r;
    a[0]=th;a[1]=buf+1720;CHECK(!K(GetFileSizeEx,a,2) && M64(buf+1720)==0);
    a[0]=th;CHECK(!K(CloseHandle,a,1) && r==1);
    memcpy(n3ds_guest_ptr(buf+1920,17),"shim_fixture.bin",17);
    memcpy(n3ds_guest_ptr(buf+1950,14),"shim_copy.bin",14);
    a[0]=buf+1920;a[1]=buf+1950;a[2]=0;CHECK(!K(CopyFileA,a,3) && r==1);
    a[0]=buf+1950;CHECK(!K(GetFileAttributesA,a,1) && r==0x80);
    a[0]=buf+1920;a[1]=buf+1950;a[2]=1;CHECK(!K(CopyFileA,a,3) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==80);
    a[0]=buf+1950;CHECK(!K(DeleteFileA,a,1) && r==1);
    a[0]=buf+1810;a[1]=0x80000000u;a[2]=0;a[3]=0;a[4]=5;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==5);
    a[0]=buf+1810;a[1]=0x10000000u;a[2]=0;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==87);
    /* Wildcard enumeration, no match, and end-of-enumeration. */
    memcpy(n3ds_guest_ptr(buf+1830,15),"shim_dir/*.txt",15);
    a[0]=buf+1830;a[1]=buf+1400;
    CHECK(!K(FindFirstFileA,a,2) && r!=UINT32_MAX);uint32_t fh=r;
    /* TRUNCATE_EXISTING above left the file at 0 bytes: metadata must agree. */
    CHECK(M32(buf+1400)==0x80 && M32(buf+1428)==0 && M32(buf+1432)==0);
    CHECK(!strcmp(guest_cstr(m,buf+1444,260),"w.txt"));
    a[0]=fh;a[1]=buf+1400;CHECK(!K(FindNextFileA,a,2) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==18);
    a[0]=fh;CHECK(!K(FindClose,a,1) && r==1);
    memcpy(n3ds_guest_ptr(buf+1850,15),"shim_dir/w?txt",15);
    a[0]=buf+1850;a[1]=buf+1400;CHECK(!K(FindFirstFileA,a,2) && r!=UINT32_MAX);fh=r;
    CHECK(!strcmp(guest_cstr(m,buf+1444,260),"w.txt"));
    a[0]=fh;CHECK(!K(FindClose,a,1) && r==1);
    memcpy(n3ds_guest_ptr(buf+1870,18),"shim_dir/none.bin",18);
    a[0]=buf+1870;a[1]=buf+1400;CHECK(!K(FindFirstFileA,a,2) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==2);
    /* Path composition: relative to cwd, backslash separators, file part. */
    a[0]=buf+1810;a[1]=64;a[2]=buf+2000;a[3]=buf+2320;
    CHECK(!K(GetFullPathNameA,a,4) && r==14);
    CHECK(!strcmp(guest_cstr(m,buf+2000,64),"shim_dir\\w.txt") && M32(buf+2320)==buf+2009);
    a[1]=4;CHECK(!K(GetFullPathNameA,a,4) && r==15);
    /* Wide file/path APIs, temp files, fixed system directories. */
    uint32_t wname=buf+1400,wout=buf+1900,wp=buf+2200;
    setw(m,wname,"shim_wide.bin");
    a[0]=wname;a[1]=0x40000000;a[2]=1;a[3]=0;a[4]=2;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileW,a,7) && r!=UINT32_MAX);
    uint32_t wh2=r;
    a[0]=wh2;a[1]=buf+1900;a[2]=3;a[3]=buf+1720;a[4]=0;
    CHECK(!K(WriteFile,a,5) && r==1 && M32(buf+1720)==3);
    a[0]=wh2;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=wname;a[1]=64;a[2]=wout;a[3]=0;
    CHECK(!K(GetFullPathNameW,a,4) && r>0 && M16(wout)!=0);
    a[0]=0;a[1]=wout;a[2]=128;
    CHECK(!K(GetModuleFileNameW,a,3) && r>0 && M16(wout)=='C' && M16(wout+4)=='\\');
    a[0]=16;a[1]=wout;CHECK(!K(GetTempPathW,a,2) && r==5 && M16(wout)=='T');
    a[0]=32;a[1]=buf+2500;CHECK(!K(GetWindowsDirectoryA,a,2) && r==10 && !strcmp(guest_cstr(m,buf+2500,32),"C:\\Windows"));
    a[0]=64;a[1]=buf+2500;CHECK(!K(GetSystemDirectoryA,a,2) && !strcmp(guest_cstr(m,buf+2500,64),"C:\\Windows\\System32"));
    memcpy(n3ds_guest_ptr(buf,6),"TempW",6);
    a[0]=buf;CHECK(!K(CreateDirectoryA,a,2) && r==1);
    memcpy(n3ds_guest_ptr(buf,6),"TempW",6);
    memcpy(n3ds_guest_ptr(buf+8,4),"TSA",4);
    a[0]=buf;a[1]=buf+8;a[2]=0;a[3]=buf+2300;
    CHECK(!K(GetTempFileNameA,a,4) && r!=0);
    a[0]=buf+2300;CHECK(!K(GetFileAttributesA,a,1) && r!=UINT32_MAX);
    a[0]=buf+2300;CHECK(!K(DeleteFileA,a,1) && r==1);
    setw(m,wname,"TempW");setw(m,wp,"TSW");
    a[0]=wname;a[1]=wp;a[2]=0;a[3]=wout;
    CHECK(!K(GetTempFileNameW,a,4) && r!=0);
    a[0]=wout;CHECK(!K(DeleteFileW,a,1) && r==1);
    a[0]=0x81;CHECK(!K(IsDBCSLeadByte,a,1) && r==0);
    a[0]=0;CHECK(!K(DisableThreadLibraryCalls,a,1) && r==1);
    a[0]=0;a[1]=buf;a[2]=1;a[3]=buf+2700;a[4]=1;
    CHECK(!K(ReadConsoleW,a,5) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==6);
    uint32_t save=0xefff0000u;
    memcpy(n3ds_guest_ptr(buf,13),"kernel32.dll",13);
    a[0]=buf;a[1]=0;a[2]=0;CHECK(!K(LoadLibraryExA,a,3) && r==save);
    setw(m,wname,"kernel32.dll");
    a[0]=0;a[1]=wname;a[2]=buf+2500;
    CHECK(!K(GetModuleHandleExW,a,3) && r==1 && M32(buf+2500)==save);
    a[0]=buf+2700;CHECK(!K(GetStartupInfoW,a,1) && r==0 && M32(buf+2700)==68);
    setw(m,wname,"wide debug line");
    a[0]=wname;CHECK(!K(OutputDebugStringW,a,1) && r==0);
    /* Unknown SEH is refused with a trap that names the exception, never a fake return. */
    PeImport se={.dll="kernel32.dll",.name="RaiseException"};
    uint32_t sefn=win32_resolve(&se,NULL);
    a[0]=0xe06d7363u;a[1]=1;a[2]=0;a[3]=0;
    CHECK(sefn && n3ds_lift_invoke(cpu,sefn,a,4,&r)==N3DS_INVOKE_TRAP);
    /* The documented SetThreadName exception is benign CONTINUE_EXECUTION. */
    uint32_t se_info=buf+1900,se_name=buf+2000;
    M32(se_info)=0x1000u;M32(se_info+4)=se_name;M32(se_info+8)=1u;M32(se_info+12)=0u;
    memcpy(n3ds_guest_ptr(se_name,5),"Main",5);
    a[0]=0x406d1388u;a[1]=0;a[2]=4;a[3]=se_info;
    CHECK(n3ds_lift_invoke(cpu,sefn,a,4,&r)==N3DS_INVOKE_OK && r==0);
    /* Delete and its error paths. */
    a[0]=buf+1810;CHECK(!K(DeleteFileA,a,1) && r==1);
    a[0]=buf+1810;CHECK(!K(DeleteFileA,a,1) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==2);
    a[0]=buf+1830;a[1]=buf+1400;CHECK(!K(FindFirstFileA,a,2) && r==UINT32_MAX);
    CHECK(!K(GetLastError,NULL,0) && r==2);
    _rmdir("shim_dir");
#endif
    /* Critical section bookkeeping (single expected guest thread). */
    uint32_t cs=buf+400;
    a[0]=cs;CHECK(!K(InitializeCriticalSection,a,1) && M32(cs+4)==0xffffffffu && M32(cs+8)==0);
    CHECK(!K(EnterCriticalSection,a,1) && M32(cs+4)==1 && M32(cs+8)==1 && M32(cs+12)==1);
    CHECK(!K(EnterCriticalSection,a,1) && M32(cs+4)==1 && M32(cs+8)==2);
    CHECK(!K(LeaveCriticalSection,a,1) && M32(cs+8)==1 && M32(cs+12)==1);
    CHECK(!K(LeaveCriticalSection,a,1) && M32(cs+4)==0xffffffffu && M32(cs+8)==0 && M32(cs+12)==0);
    a[0]=cs;a[1]=4000;CHECK(!K(InitializeCriticalSectionAndSpinCount,a,2) && M32(cs+20)==4000);
    CHECK(!K(DeleteCriticalSection,a,1) && M32(cs+4)==0xffffffffu && M32(cs+8)==0);
    /* Current directory starts at the configured game root and accepts a relative change. */
    a[0]=64;a[1]=buf+512;CHECK(!K(GetCurrentDirectoryA,a,2) && r>=1);
    const char *initial=guest_cstr(m,buf+512,64);CHECK(initial && (uint32_t)strlen(initial)==r);
    size_t initial_len=strlen(initial)+1;
    a[0]=0;a[1]=0;CHECK(!K(GetCurrentDirectoryA,a,2) && r==initial_len);
    a[0]=1;a[1]=buf+512;CHECK(!K(GetCurrentDirectoryA,a,2) && r==initial_len);
    a[0]=64;a[1]=0;CHECK(!K(GetCurrentDirectoryA,a,2) && r==0);
#ifndef __3DS__
    /* Relative file access must follow SetCurrentDirectoryA. */
    _mkdir("shim_cwd");
    FILE *tf=fopen("shim_cwd/data.bin","wb");
    CHECK(tf);CHECK(fwrite("Z",1,1,tf)==1);CHECK(fclose(tf)==0);
    memcpy(n3ds_guest_ptr(buf+1300,9),"shim_cwd",9);
    a[0]=buf+1300;CHECK(!K(SetCurrentDirectoryA,a,1) && r==1);
    memcpy(n3ds_guest_ptr(buf+1320,9),"data.bin",9);
    a[0]=buf+1320;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t cwd_handle=r;
    a[0]=cwd_handle;CHECK(!K(GetFileType,a,1) && r==1);
    a[0]=cwd_handle;a[1]=buf+1340;a[2]=1;a[3]=buf+1360;a[4]=0;
    CHECK(!K(ReadFile,a,5) && M32(buf+1360)==1 && M8(buf+1340)=='Z');
    a[0]=cwd_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    remove("shim_cwd/data.bin");_rmdir("shim_cwd");
#endif
    memcpy(n3ds_guest_ptr(buf+576,5),"Data",5);
    a[0]=buf+576;CHECK(!K(SetCurrentDirectoryA,a,1) && r==1);
    a[0]=64;a[1]=buf+512;CHECK(!K(GetCurrentDirectoryA,a,2) && r>=5);
    const char *changed=guest_cstr(m,buf+512,64);
    CHECK(changed && strlen(changed)>=5 && !strcmp(changed+strlen(changed)-5,"/Data"));
    /* Code pages and Unicode conversion. */
    CHECK(!K(GetACP,NULL,0) && r==1252);
    CHECK(!K(GetOEMCP,NULL,0) && r==437);
    a[0]=1252;CHECK(!K(IsValidCodePage,a,1) && r==1);
    a[0]=65001;CHECK(!K(IsValidCodePage,a,1) && r==1);
    a[0]=999;CHECK(!K(IsValidCodePage,a,1) && r==0);
    a[0]=1252;a[1]=buf+640;CHECK(!K(GetCPInfo,a,2) && r==1 && M32(buf+640)==1 && M8(buf+644)=='?');
    memcpy(n3ds_guest_ptr(buf+680,5),"Caf\xe9",5);
    a[0]=1252;a[1]=0;a[2]=buf+680;a[3]=(uint32_t)-1;a[4]=0;a[5]=0;
    CHECK(!K(MultiByteToWideChar,a,6) && r==5);
    a[4]=buf+700;a[5]=8;CHECK(!K(MultiByteToWideChar,a,6) && r==5);
    CHECK(M16(buf+700)=='C' && M16(buf+706)==0xe9 && M16(buf+708)==0);
    a[0]=1252;a[1]=0;a[2]=buf+700;a[3]=(uint32_t)-1;a[4]=buf+760;a[5]=8;a[6]=0;a[7]=0;
    CHECK(!K(WideCharToMultiByte,a,8) && r==5 && !memcmp(n3ds_guest_ptr(buf+760,5),"Caf\xe9",5));
    memcpy(n3ds_guest_ptr(buf+780,4),"\xd0\x9f\xd1\x80",4);
    a[0]=65001;a[1]=0;a[2]=buf+780;a[3]=4;a[4]=buf+800;a[5]=8;
    CHECK(!K(MultiByteToWideChar,a,6) && r==2 && M16(buf+800)==0x41f && M16(buf+802)==0x440);
    a[0]=65001;a[1]=0;a[2]=buf+800;a[3]=2;a[4]=buf+820;a[5]=8;a[6]=0;a[7]=0;
    CHECK(!K(WideCharToMultiByte,a,8) && r==4 && !memcmp(n3ds_guest_ptr(buf+820,4),"\xd0\x9f\xd1\x80",4));
    /* CP_ACP (0) must behave like 1252. */
    memcpy(n3ds_guest_ptr(buf+840,2),"A",2);
    a[0]=0;a[1]=0;a[2]=buf+840;a[3]=(uint32_t)-1;a[4]=buf+850;a[5]=4;
    CHECK(!K(MultiByteToWideChar,a,6) && r==2 && M16(buf+850)=='A' && M16(buf+852)==0);
    a[0]=0;a[1]=0;a[2]=buf+850;a[3]=(uint32_t)-1;a[4]=buf+860;a[5]=4;a[6]=0;a[7]=0;
    CHECK(!K(WideCharToMultiByte,a,8) && r==2 && !strcmp(guest_cstr(m,buf+860,4),"A"));
    /* Character classification (CT_CTYPE1). */
    for(unsigned i=0;i<6;++i)M16(buf+1500+i*2)=(uint16_t)"A a0!"[i];
    M16(buf+1512)=1;M16(buf+1514)=0;
    a[0]=1;a[1]=buf+1500;a[2]=(uint32_t)-1;a[3]=buf+1560;
    CHECK(!K(GetStringTypeW,a,4) && r==1);
    CHECK(M16(buf+1560)==0x181 && M16(buf+1562)==0x48 && M16(buf+1564)==0x182);
    CHECK(M16(buf+1566)==0x84 && M16(buf+1568)==0x10 && M16(buf+1570)==0x20);
    memcpy(n3ds_guest_ptr(buf+1580,3),"A0",3);
    a[0]=0x409;a[1]=1;a[2]=buf+1580;a[3]=(uint32_t)-1;a[4]=buf+1600;
    CHECK(!K(GetStringTypeA,a,5) && r==1 && M16(buf+1600)==0x181 && M16(buf+1602)==0x84);
    /* Case mapping (LCMapString). */
    memcpy(n3ds_guest_ptr(buf+1620,4),"abc",4);
    a[0]=0x409;a[1]=0x200;a[2]=buf+1620;a[3]=(uint32_t)-1;a[4]=0;a[5]=0;
    CHECK(!K(LCMapStringA,a,6) && r==4);
    a[4]=buf+1640;a[5]=4;CHECK(!K(LCMapStringA,a,6) && r==4 && !strcmp(guest_cstr(m,buf+1640,8),"ABC"));
    a[5]=3;CHECK(!K(LCMapStringA,a,6) && r==0);CHECK(!K(GetLastError,NULL,0) && r==122);
    a[5]=4;CHECK(!K(LCMapStringA,a,6) && r==4);
    a[1]=0x100;CHECK(!K(LCMapStringA,a,6) && r==4 && !strcmp(guest_cstr(m,buf+1640,8),"abc"));
    /* Standard handles and file type. */
    a[0]=(uint32_t)-11;CHECK(!K(GetStdHandle,a,1) && r==0xfffffff5u);
    a[0]=r;CHECK(!K(GetFileType,a,1) && r==2);
    a[0]=(uint32_t)-11;a[1]=4;CHECK(!K(SetStdHandle,a,2) && r==1);
    a[0]=(uint32_t)-11;CHECK(!K(GetStdHandle,a,1) && r==4);
    a[0]=4;CHECK(!K(GetFileType,a,1) && r==0);
    a[0]=(uint32_t)-11;a[1]=0xfffffff5u;CHECK(!K(SetStdHandle,a,2) && r==1);
    CHECK(!K(IsDebuggerPresent,NULL,0) && r==0);
    a[0]=0x12345678;CHECK(!K(EncodePointer,a,1) && r!=0x12345678);uint32_t enc=r;
    a[0]=enc;CHECK(!K(DecodePointer,a,1) && r==0x12345678);
    a[0]=10;CHECK(!K(IsProcessorFeaturePresent,a,1) && r==0);
    a[0]=0x1234;CHECK(!K(SetUnhandledExceptionFilter,a,1) && r==0);
    a[0]=0;CHECK(!K(SetUnhandledExceptionFilter,a,1) && r==0x1234);
    CHECK(!K(UnhandledExceptionFilter,a,1) && r==0);
    /* Wide module lookup. */
    const char *modname="test.dll";
    for(unsigned i=0;i<=strlen(modname);++i)M16(buf+860+i*2)=(uint16_t)(unsigned char)modname[i];
    a[0]=buf+860;CHECK(!K(GetModuleHandleW,a,1) && r==dllbase);
    /* Time conversions and string helpers. */
    M64(buf+880)=10;M64(buf+888)=20;
    a[0]=buf+880;a[1]=buf+888;CHECK(!K(CompareFileTime,a,2) && r==(uint32_t)-1);
    M64(buf+900)=UINT64_C(116444736000000000); /* 1970-01-01 00:00:00 UTC */
    a[0]=buf+900;a[1]=buf+920;CHECK(!K(FileTimeToSystemTime,a,2) && r==1);
    CHECK(M16(buf+920)==1970 && M16(buf+922)==1 && M16(buf+924)==4 && M16(buf+926)==1);
    a[0]=buf+920;a[1]=buf+940;CHECK(!K(SystemTimeToFileTime,a,2) && r==1 && M64(buf+940)==UINT64_C(116444736000000000));
    a[0]=buf+900;a[1]=buf+950;CHECK(!K(FileTimeToLocalFileTime,a,2) && r==1);
    a[0]=buf+950;a[1]=buf+958;CHECK(!K(LocalFileTimeToFileTime,a,2) && r==1 && M64(buf+958)==M64(buf+900));
    a[0]=buf+960;CHECK(!K(GetLocalTime,a,1) && M16(buf+960)>=2020 && M16(buf+960)<2200);
    a[0]=buf+1000;CHECK(!K(GetTimeZoneInformation,a,1) && (int32_t)M32(buf+1000)>=-1440 && (int32_t)M32(buf+1000)<=1440);
    M32(buf+1180)=10;a[0]=buf+1180;a[1]=5;CHECK(!K(InterlockedExchangeAdd,a,2) && r==10 && M32(buf+1180)==15);
    memcpy(n3ds_guest_ptr(buf+1200,5),"Data",5);
    a[0]=buf+1160;a[1]=buf+1200;CHECK(!K(lstrcpyA,a,2) && r==buf+1160 && !strcmp(guest_cstr(m,buf+1160,64),"Data"));
    memcpy(n3ds_guest_ptr(buf+1210,8),"\\Meshes",8);
    a[0]=buf+1160;a[1]=buf+1210;CHECK(!K(lstrcatA,a,2) && !strcmp(guest_cstr(m,buf+1160,64),"Data\\Meshes"));
    memcpy(n3ds_guest_ptr(buf+1240,4),"C:\\",4);
    a[0]=buf+1240;CHECK(!K(GetDriveTypeA,a,1) && r==3);
    a[0]=0x40;a[1]=32;CHECK(!K(LocalAlloc,a,2) && r);
    uint32_t localmem=r;CHECK(M32(localmem)==0);
    a[0]=localmem;CHECK(!K(LocalFree,a,1) && r==0);
    a[0]=0x1000;a[1]=0;a[2]=0;a[3]=0;a[4]=buf+1280;a[5]=64;
    CHECK(!K(FormatMessageA,a,7) && r>0 && strlen(guest_cstr(m,buf+1280,64))==r);
    /* Emulated Win32 version and machine identity. */
    M32(buf+1900)=148;
    a[0]=buf+1900;CHECK(!K(GetVersionExA,a,1) && r==1);
    CHECK(M32(buf+1904)==5 && M32(buf+1908)==1 && M32(buf+1912)==2600 && M32(buf+1916)==2);
    CHECK(guest_cstr(m,buf+1920,128)[0]==0);
    M32(buf+1900)=100;a[0]=buf+1900;CHECK(!K(GetVersionExA,a,1) && r==0);
    M32(buf+1950)=16;a[0]=buf+1958;a[1]=buf+1950;
    CHECK(!K(GetComputerNameA,a,2) && r==1 && !strcmp(guest_cstr(m,buf+1958,16),"N3DS"));
    CHECK(M32(buf+1950)==4);
    M32(buf+1950)=4;a[0]=buf+1958;a[1]=buf+1950;
    CHECK(!K(GetComputerNameA,a,2) && r==0 && M32(buf+1950)==4);
    CHECK(!K(GetConsoleCP,NULL,0) && r==1252);
    CHECK(!K(GetConsoleOutputCP,NULL,0) && r==1252);
    M32(buf+1960)=0;a[0]=1;a[1]=buf+1960;CHECK(!K(GetConsoleMode,a,2) && r==0);
    /* Pointer probing and guest-visible memory budget. */
    a[0]=buf+1900;a[1]=4;CHECK(!K(IsBadWritePtr,a,2) && r==0);
    a[0]=0x7ffffff0u;a[1]=64;CHECK(!K(IsBadWritePtr,a,2) && r==1);
    M32(buf+1970)=64;a[0]=buf+1970;CHECK(!K(GlobalMemoryStatusEx,a,1) && r==1);
    CHECK(M64(buf+1978)==m->limit && M32(buf+1970)==64);
    /* Single-thread contract. */
    a[0]=0xfffffffeu;a[1]=buf+1990;CHECK(!K(GetExitCodeThread,a,2) && r==1 && M32(buf+1990)==259);
    a[0]=0;a[1]=0;CHECK(!K(SleepEx,a,2) && r==0);
    /* Environment: a real rebuild in the guest PEB block, with an honest
     * ERROR_INSUFFICIENT_BUFFER when the block cannot grow. */
    CHECK(!guest_map(m,0x18100000u,0x2000));
    const uint32_t envA=0x18101600u;
    memcpy(n3ds_guest_ptr(envA,16),"OS=Windows_NT\0\0",16);
    memcpy(n3ds_guest_ptr(buf+2300,8),"TESTVAR",8);
    memcpy(n3ds_guest_ptr(buf+2320,2),"1",2);
    a[0]=buf+2300;a[1]=buf+2320;CHECK(!K(SetEnvironmentVariableA,a,2) && r==1);
    CHECK(!strcmp(guest_cstr(m,envA,64),"OS=Windows_NT"));
    const char *second=(const char*)n3ds_guest_ptr(envA,64);
    CHECK(!strcmp(second+14,"TESTVAR=1"));
    a[0]=buf+2300;a[1]=0;CHECK(!K(SetEnvironmentVariableA,a,2) && r==1);
    CHECK(!strcmp(guest_cstr(m,envA,64),"OS=Windows_NT"));
    CHECK(M8(envA+13)==0); /* the double NUL terminates the block again */
    memset(n3ds_guest_ptr(buf+2340,241),'X',240);M8(buf+2580)=0;
    a[0]=buf+2340;a[1]=buf+2320;CHECK(!K(SetEnvironmentVariableA,a,2) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==122);
    /* Environment lookup and %VAR% expansion read the same block. */
    a[0]=buf+2300;a[1]=buf+2320;CHECK(!K(SetEnvironmentVariableA,a,2) && r==1);
    a[0]=buf+2300;a[1]=buf+2600;a[2]=8;
    CHECK(!K(GetEnvironmentVariableA,a,3) && r==1 && !strcmp(guest_cstr(m,buf+2600,8),"1"));
    memcpy(n3ds_guest_ptr(buf+2620,12),"missing_var",12);
    a[0]=buf+2620;a[1]=buf+2600;a[2]=8;CHECK(!K(GetEnvironmentVariableA,a,3) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==203);
    a[0]=buf+2300;a[1]=buf+2600;a[2]=1;CHECK(!K(GetEnvironmentVariableA,a,3) && r==2);
    memcpy(n3ds_guest_ptr(buf+2640,16),"%TESTVAR%/x",16);
    a[0]=buf+2640;a[1]=buf+2660;a[2]=32;
    CHECK(!K(ExpandEnvironmentStringsA,a,3) && r==4 && !strcmp(guest_cstr(m,buf+2660,32),"1/x"));
    a[0]=buf+2640;a[1]=0;a[2]=0;CHECK(!K(ExpandEnvironmentStringsA,a,3) && r==4);
    /* Version, virtual memory, and module policy. */
    CHECK(!K(GetVersion,NULL,0) && r==0x8a280105u);
    CHECK(!K(AreFileApisANSI,NULL,0) && r==1);
    a[0]=base;a[1]=buf+2700;a[2]=28;
    CHECK(!K(VirtualQuery,a,3) && r==28 && M32(buf+2700)==base && M32(buf+2704)==base && M32(buf+2712)==4096);
    a[0]=base+4096;a[1]=buf+2700;a[2]=28;CHECK(!K(VirtualQuery,a,3) && r==0);
    a[0]=0;a[1]=buf+2700;a[2]=28;CHECK(!K(VirtualQuery,a,3) && r==0);
    M32(buf+2740)=32;a[0]=buf+2740;CHECK(!K(GlobalMemoryStatus,a,1) && r==0 && M32(buf+2740)==32);
    a[0]=0;a[1]=4;CHECK(!K(IsBadReadPtr,a,2) && r==1);
    a[0]=buf;a[1]=4;CHECK(!K(IsBadReadPtr,a,2) && r==0);
    a[0]=0x7ffffff0u;a[1]=4;CHECK(!K(IsBadReadPtr,a,2) && r==1);
    a[0]=0xf568a1;a[1]=0;CHECK(!K(IsBadCodePtr,a,1) && r==1);
    a[0]=16;a[1]=buf+2760;CHECK(!K(GetTempPathA,a,2) && r==5 && !strcmp(guest_cstr(m,buf+2760,16),"Temp\\"));
    a[0]=4;a[1]=buf+2760;CHECK(!K(GetTempPathA,a,2) && r==6);
    {
        uint32_t t1,t2,t3;
        CHECK(!W(timeGetTime,NULL,0));t1=r;
        CHECK(!W(timeGetTime,NULL,0));t2=r;
        CHECK(t2>=t1 && t2-t1<1000); /* immediate calls cannot wrap or take seconds */
        a[0]=20;CHECK(!K(Sleep,a,1));
        CHECK(!W(timeGetTime,NULL,0));t3=r;
        CHECK(t3>=t2 && t3-t2>=10 && t3-t2<2000); /* Win32 milliseconds */
    }
    a[0]=1;CHECK(!W(timeBeginPeriod,a,1) && r==0);
#ifndef __3DS__
    /* Private profile: real file write, read back, default, delete. */
    memcpy(n3ds_guest_ptr(buf+2000,17),"shim_profile.ini",17);
    memcpy(n3ds_guest_ptr(buf+2020,8),"Display",8);
    memcpy(n3ds_guest_ptr(buf+2040,7),"iSizeW",7);
    memcpy(n3ds_guest_ptr(buf+2060,5),"1280",5);
    a[0]=buf+2020;a[1]=buf+2040;a[2]=buf+2060;a[3]=buf+2000;
    CHECK(!K(WritePrivateProfileStringA,a,4) && r==1);
    a[0]=buf+2020;a[1]=buf+2040;a[2]=0;a[3]=buf+2000;
    CHECK(!K(WritePrivateProfileStringA,a,4) && r==1); /* delete key */
    a[0]=buf+2020;a[1]=buf+2040;a[2]=buf+2060;a[3]=buf+2000;
    CHECK(!K(WritePrivateProfileStringA,a,4) && r==1);
    /* The file really exists in the guest-visible directory. */
    a[0]=buf+2000;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);uint32_t ini_handle=r;
    a[0]=ini_handle;a[1]=0;a[2]=0;CHECK(!K(GetFileSize,a,2) && r>0);
    a[0]=ini_handle;a[1]=buf+2400;a[2]=96;a[3]=buf+2500;a[4]=0;
    CHECK(!K(ReadFile,a,5) && M32(buf+2500)>0);
    a[0]=ini_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    CHECK(strstr((char*)n3ds_guest_ptr(buf+2400,M32(buf+2500)+1),"[Display]")!=NULL);
    memcpy(n3ds_guest_ptr(buf+2080,4),"def",4);
    a[0]=buf+2020;a[1]=buf+2040;a[2]=buf+2080;a[3]=buf+2100;a[4]=32;a[5]=buf+2000;
    CHECK(!K(GetPrivateProfileStringA,a,6) && r==4 && !strcmp(guest_cstr(m,buf+2100,32),"1280"));
    memcpy(n3ds_guest_ptr(buf+2120,11),"bFullScreen",11);
    a[0]=buf+2020;a[1]=buf+2120;a[2]=buf+2080;a[3]=buf+2100;a[4]=32;a[5]=buf+2000;
    CHECK(!K(GetPrivateProfileStringA,a,6) && r==3 && !strcmp(guest_cstr(m,buf+2100,32),"def"));
    a[0]=buf+2020;a[1]=buf+2040;a[2]=0;a[3]=buf+2000;
    CHECK(!K(GetPrivateProfileIntA,a,4) && r==1280);
    a[0]=buf+2020;a[1]=buf+2120;a[2]=7;a[3]=buf+2000;
    CHECK(!K(GetPrivateProfileIntA,a,4) && r==7);
    /* Remove the whole section. */
    a[0]=buf+2020;a[1]=0;a[2]=0;a[3]=buf+2000;
    CHECK(!K(WritePrivateProfileStringA,a,4) && r==1);
    a[0]=buf+2000;a[1]=0x80000000u;a[2]=1;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=UINT32_MAX);ini_handle=r;
    a[0]=ini_handle;a[1]=buf+2400;a[2]=96;a[3]=buf+2500;a[4]=0;
    CHECK(!K(ReadFile,a,5) && M32(buf+2500)<96u);
    a[0]=ini_handle;CHECK(!K(CloseHandle,a,1) && r==1);
    char *remain=(char*)n3ds_guest_ptr(buf+2400,M32(buf+2500)+1);
    remain[M32(buf+2500)]=0;
    CHECK(!strstr(remain,"[Display]") && !strstr(remain,"iSizeW"));
    /* MoveFileA: rename succeeds, existing destination fails with 183. */
    memcpy(n3ds_guest_ptr(buf+2140,16),"shim_move_a.bin",16);
    memcpy(n3ds_guest_ptr(buf+2160,16),"shim_move_b.bin",16);
    a[0]=buf+2140;a[1]=0x40000000u;a[2]=0;a[3]=0;a[4]=2;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=0xffffffffu);
    a[0]=r;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=buf+2140;a[1]=buf+2160;CHECK(!K(MoveFileA,a,2) && r==1);
    a[0]=buf+2160;CHECK(!K(GetFileAttributesA,a,1) && r!=0xffffffffu);
    a[0]=buf+2160;a[1]=buf+2160;CHECK(!K(MoveFileA,a,2) && r==0);
    CHECK(!K(GetLastError,NULL,0) && r==183);
    /* File metadata and timestamps on the moved file. */
    a[0]=buf+2160;a[1]=0xC0000000u;a[2]=0;a[3]=0;a[4]=3;a[5]=0;a[6]=0;
    CHECK(!K(CreateFileA,a,7) && r!=0xffffffffu);
    uint32_t mh=r;
    a[0]=mh;a[1]=0;a[2]=0;a[3]=buf+2180;
    CHECK(!K(GetFileTime,a,4) && r==1 && M64(buf+2180)>0);
    M64(buf+2200)=UINT64_C(132223104000000000); /* 2020-01-01 UTC */
    a[0]=mh;a[1]=0;a[2]=buf+2200;a[3]=buf+2200;
    CHECK(!K(SetFileTime,a,4) && r==1);
    a[0]=mh;a[1]=0;a[2]=0;a[3]=buf+2180;
    CHECK(!K(GetFileTime,a,4) && r==1 && M64(buf+2180)==M64(buf+2200));
    a[0]=mh;a[1]=buf+2220;
    CHECK(!K(GetFileInformationByHandle,a,2) && r==1 && M32(buf+2220)==0x80 && M32(buf+2260)==1);
    a[0]=mh;CHECK(!K(CloseHandle,a,1) && r==1);
    a[0]=buf+2000;CHECK(!K(DeleteFileA,a,1) && r==1);
    a[0]=buf+2160;CHECK(!K(DeleteFileA,a,1) && r==1);
#endif
    rc=0;
done:
    (void)allocated;(void)va;
    win32_shutdown();if(mapped)guest_unmap(m,base);if(dll_mapped)guest_unmap(m,dllbase);
#ifndef __3DS__
    if(created)remove(filename);
    remove("shim_dir/w.txt");_rmdir("shim_dir");
#endif
    return rc;
}
