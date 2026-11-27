/* Host smoke test for the user32/GDI compatibility thunks.
 *
 * No window or message queue exists, so the test drives the GuestFn pointers
 * directly with a synthetic CPU stack. Guest memory is mapped with the normal
 * GuestMemory helpers; no recompiled code is required. */
#include "win32_user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW_USEDEFAULT 0x80000000u
#define MAP 0x01000000u
#define MAP_SIZE 0x10000u
#define TEB 0x02000000u
#define TEB_SIZE 0x1000u
#define WNDCLASS MAP+0x0000u
#define RECT_IN MAP+0x0100u
#define RECT_OUT MAP+0x0120u
#define STRINGS MAP+0x1000u
#define CLASS_NAME STRINGS+0x000u
#define OTHER_NAME STRINGS+0x040u
#define MISSING_NAME STRINGS+0x080u
#define HELLO STRINGS+0x0c0u
#define TITLE MAP+0x0200u
#define LONG_TITLE MAP+0x0300u
#define WNDTEXT MAP+0x0600u
#define MBOX MAP+0x0800u
#define MSG MAP+0x0900u
#define STACK MAP+0x8000u

static GuestMemory m;
static CPU cpu;
static int failures;

void rt_unimpl(CPU *c,uint32_t addr,const char *what) {
    (void)c;
    fprintf(stderr,"FAIL: runtime trap at %08lx: %s\n",(unsigned long)addr,what?what:"?");
    exit(3);
}
static void put32(uint32_t a,uint32_t v) {
    if(guest_write32(&m,a,v)){fprintf(stderr,"FAIL: put32 at %08lx\n",(unsigned long)a);exit(3);}
}
static uint32_t get32(uint32_t a) {
    uint32_t v=0;
    if(guest_read32(&m,a,&v)){fprintf(stderr,"FAIL: get32 at %08lx\n",(unsigned long)a);exit(3);}
    return v;
}
static int32_t geti32(uint32_t a) {return (int32_t)get32(a);}
static void put_i32(uint32_t a,int32_t v) {put32(a,(uint32_t)v);}
static void putstr(uint32_t a,const char *s) {
    size_t n=strlen(s)+1;
    if(guest_write(&m,a,s,n)){fprintf(stderr,"FAIL: putstr at %08lx\n",(unsigned long)a);exit(3);}
}
static void put_data(uint32_t a,const void *p,size_t n) {
    if(guest_write(&m,a,p,n)){fprintf(stderr,"FAIL: put_data at %08lx\n",(unsigned long)a);exit(3);}
}
static uint32_t last_error(void) {return get32(TEB+0x34);}

static int call(const char *dll,const char *name,const uint32_t *args,unsigned n,uint32_t *out) {
    uint32_t addr=win32_user_resolve(dll,name);
    const FnEntry *e=addr?win32_user_lookup(addr):NULL;
    if(!addr || !e){fprintf(stderr,"FAIL: resolve %s!%s\n",dll,name);++failures;return -1;}
    uint32_t base=STACK-(n+1)*4;
    put32(base,0xffffffffu);
    for(unsigned i=0;i<n;++i)put32(base+4+i*4,args[i]);
    cpu.r[4]=base;
    e->fn(&cpu);
    uint32_t expected=base+4+n*4;
    if(cpu.r[4]!=expected){
        fprintf(stderr,"FAIL: %s did not pop %u arguments\n",name,n);
        ++failures;cpu.r[4]=STACK;return -1;
    }
    if(out)*out=cpu.r[0];
    return 0;
}
#define CALL(dll,name,args,n,out) do{if(call(dll,name,args,n,out))goto done;}while(0)
#define CHECK(x,desc) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,desc);++failures;}}while(0)

int main(void) {
    guest_init(&m,0x40000);
    if(guest_map(&m,MAP,MAP_SIZE) || guest_map(&m,TEB,TEB_SIZE)){fprintf(stderr,"FAIL: guest_map\n");return 2;}
    memset(&cpu,0,sizeof cpu);
    cpu.r[4]=STACK;
    cpu.fs_base=TEB;

    /* ---- registry ---- */
    unsigned ndefs=0;
    const Win32UserDef *defs=win32_user_defs(&ndefs);
    CHECK(defs && ndefs>0,"defs table");
    for(unsigned i=0;i<ndefs;++i)
        CHECK(defs[i].dll && defs[i].name && defs[i].fn,"defs entry");
    CHECK(win32_user_resolve("USER32.DLL","RegisterClassA")!=0,"case-insensitive dll");
    CHECK(win32_user_resolve("user32","PeekMessageA")!=0,"dll without suffix");
    CHECK(win32_user_resolve("gdi32.dll","GetStockObject")!=0,"gdi32 resolve");
    CHECK(win32_user_resolve("user32.dll","NoSuchFunction")==0,"unknown name");
    CHECK(win32_user_resolve(NULL,"PeekMessageA")==0,"null dll");
    CHECK(win32_user_init(&m)==0,"init");
    CHECK(win32_user_init(&m)==-1,"double init");
    const FnEntry *first=win32_user_lookup(WIN32_USER_BASE);
    CHECK(first && first->addr==WIN32_USER_BASE && first->fn,"lookup first thunk");
    CHECK(win32_user_lookup(WIN32_USER_BASE+4*ndefs)==NULL,"lookup past end");
    CHECK(win32_user_lookup(WIN32_USER_BASE+1)==NULL,"lookup unaligned");
    CHECK(win32_user_lookup(WIN32_USER_BASE-4)==NULL,"lookup below range");

    /* ---- classes ---- */
    putstr(CLASS_NAME,"TestClass");
    putstr(OTHER_NAME,"OtherClass");
    putstr(MISSING_NAME,"NoSuchClass");
    putstr(HELLO,"Hello");
    putstr(MBOX,"guest text");
    memset(guest_ptr(&m,WNDCLASS,40),0,40);
    put32(WNDCLASS+0,0x20);
    put32(WNDCLASS+4,0x12345678);
    put32(WNDCLASS+36,CLASS_NAME);
    uint32_t a[12],r=0;
    a[0]=WNDCLASS;
    CALL("user32.dll","RegisterClassA",a,1,&r);
    CHECK(r==0xC000,"class atom");
    CHECK(last_error()==0,"class success clears error");
    CALL("user32.dll","RegisterClassA",a,1,&r);
    CHECK(r==0,"duplicate class rejected");
    CHECK(last_error()==1410,"duplicate class error");
    put32(WNDCLASS+36,OTHER_NAME);
    CALL("user32.dll","RegisterClassA",a,1,&r);
    CHECK(r==0xC001,"second class atom");

    /* ---- windows ---- */
    a[0]=0;a[1]=MISSING_NAME;a[2]=0;a[3]=0;a[4]=0;a[5]=0;a[6]=100;a[7]=100;a[8]=0;a[9]=0;a[10]=0;a[11]=0;
    CALL("user32.dll","CreateWindowExA",a,12,&r);
    CHECK(r==0,"unknown class rejected");
    CHECK(last_error()==1407,"unknown class error");
    a[0]=0;a[1]=CLASS_NAME;a[2]=TITLE;a[3]=0x00CF0000u;
    a[4]=CW_USEDEFAULT;a[5]=10;a[6]=CW_USEDEFAULT;a[7]=50;a[8]=0;a[9]=0;a[10]=0;a[11]=0;
    putstr(TITLE,"Hello Window");
    CALL("user32.dll","CreateWindowExA",a,12,&r);
    uint32_t win=r;
    CHECK(win==0x00010000u,"window handle");
    CHECK(last_error()==0,"window success clears error");

    a[0]=win;a[1]=RECT_OUT;
    CALL("user32.dll","GetWindowRect",a,2,&r);
    CHECK(r==1,"GetWindowRect");
    CHECK(geti32(RECT_OUT)==0 && geti32(RECT_OUT+4)==0 &&
          geti32(RECT_OUT+8)==400 && geti32(RECT_OUT+12)==50,"CW_USEDEFAULT rect 400x50");
    CALL("user32.dll","GetClientRect",a,2,&r);
    CHECK(r==1 && geti32(RECT_OUT+8)==400 && geti32(RECT_OUT+12)==50,"GetClientRect");

    put_i32(RECT_IN,-10);put_i32(RECT_IN+4,-20);put_i32(RECT_IN+8,290);put_i32(RECT_IN+12,180);
    a[0]=RECT_IN;a[1]=0;a[2]=0;
    CALL("user32.dll","AdjustWindowRect",a,3,&r);
    CHECK(r==1,"AdjustWindowRect");
    CHECK(geti32(RECT_IN)==-10 && geti32(RECT_IN+4)==-20 &&
          geti32(RECT_IN+8)==290 && geti32(RECT_IN+12)==180,"AdjustWindowRect keeps the in/out rect");
    a[0]=RECT_IN;a[1]=0x10;a[2]=0;a[3]=0;
    CALL("user32.dll","AdjustWindowRectEx",a,4,&r);
    CHECK(r==1 && geti32(RECT_IN)==-10 && geti32(RECT_IN+12)==180,"AdjustWindowRectEx keeps the in/out rect");

    a[0]=win;a[1]=0;a[2]=5;a[3]=6;a[4]=320;a[5]=240;a[6]=0;
    CALL("user32.dll","SetWindowPos",a,7,&r);
    CHECK(r==1,"SetWindowPos");
    a[0]=win;a[1]=RECT_OUT;
    CALL("user32.dll","GetWindowRect",a,2,&r);
    CHECK(geti32(RECT_OUT+8)==320 && geti32(RECT_OUT+12)==240,"SetWindowPos resizes");
    a[0]=win;a[1]=0;a[2]=999;a[3]=999;a[4]=999;a[5]=999;a[6]=3;
    CALL("user32.dll","SetWindowPos",a,7,&r);
    a[0]=win;a[1]=RECT_OUT;
    CALL("user32.dll","GetWindowRect",a,2,&r);
    CHECK(geti32(RECT_OUT+8)==320 && geti32(RECT_OUT+12)==240,"NOMOVE|NOSIZE keeps size");
    a[0]=win;a[1]=0;a[2]=999;a[3]=999;a[4]=100;a[5]=60;a[6]=2;
    CALL("user32.dll","SetWindowPos",a,7,&r);
    a[0]=win;a[1]=RECT_OUT;
    CALL("user32.dll","GetWindowRect",a,2,&r);
    CHECK(geti32(RECT_OUT+8)==100 && geti32(RECT_OUT+12)==60,"NOMOVE still resizes");

    a[0]=win;a[1]=HELLO;
    CALL("user32.dll","SetWindowTextA",a,2,&r);
    CHECK(r==1,"SetWindowTextA");
    memset(guest_ptr(&m,WNDTEXT,64),0x5a,64);
    a[0]=win;a[1]=WNDTEXT;a[2]=64;
    CALL("user32.dll","GetWindowTextA",a,3,&r);
    CHECK(r==5 && !strcmp(guest_cstr(&m,WNDTEXT,64),"Hello"),"GetWindowTextA length");
    putstr(WNDTEXT,"xxxxxxxx");
    a[0]=win;a[1]=WNDTEXT;a[2]=4;
    CALL("user32.dll","GetWindowTextA",a,3,&r);
    CHECK(r==3 && !strcmp(guest_cstr(&m,WNDTEXT,4),"Hel"),"GetWindowTextA truncates");
    char longtitle[400];
    for(unsigned i=0;i<399;++i)longtitle[i]=(char)('A'+i%26);
    longtitle[399]=0;
    putstr(LONG_TITLE,longtitle);
    a[0]=win;a[1]=LONG_TITLE;
    CALL("user32.dll","SetWindowTextA",a,2,&r);
    CHECK(r==1,"SetWindowTextA long");
    memset(guest_ptr(&m,WNDTEXT,300),0x5a,300);
    a[0]=win;a[1]=WNDTEXT;a[2]=300;
    CALL("user32.dll","GetWindowTextA",a,3,&r);
    CHECK(r==255 && strlen(guest_cstr(&m,WNDTEXT,300))==255 &&
          ((char*)guest_ptr(&m,WNDTEXT,300))[255]==0,"title bounded to 255");
    putstr(TITLE,"Hello Window");
    a[0]=win;a[1]=TITLE;
    CALL("user32.dll","SetWindowTextA",a,2,&r);

    /* ---- per-window storage ---- */
    a[0]=win;a[1]=(uint32_t)-21;a[2]=0;
    CALL("user32.dll","SetWindowLongA",a,3,&r);
    CHECK(r==0 && last_error()==0,"SetWindowLongA initial");
    a[0]=win;a[1]=(uint32_t)-21;a[2]=0xDEADBEEFu;
    CALL("user32.dll","SetWindowLongA",a,3,&r);
    CHECK(r==0,"SetWindowLongA returns old zero");
    a[0]=win;a[1]=(uint32_t)-21;
    CALL("user32.dll","GetWindowLongA",a,2,&r);
    CHECK(r==0xDEADBEEFu,"GetWindowLongA userdata");
    a[0]=win;a[1]=(uint32_t)-21;a[2]=0x1111;
    CALL("user32.dll","SetWindowLongA",a,3,&r);
    CHECK(r==0xDEADBEEFu,"SetWindowLongA returns previous");
    CALL("user32.dll","GetWindowLongA",a,2,&r);
    CHECK(r==0x1111,"GetWindowLongA updated");
    a[0]=win;a[1]=(uint32_t)-21;
    CALL("user32.dll","GetClassLongA",a,2,&r);
    CHECK(r==0x1111,"GetClassLongA userdata");
    a[0]=win;a[1]=0;
    CALL("user32.dll","GetClassLongA",a,2,&r);
    CHECK(r==0,"GetClassLongA other index");
    a[0]=win;a[1]=0;a[2]=123;
    CALL("user32.dll","SetWindowLongA",a,3,&r);
    CHECK(r==0 && last_error()==87,"SetWindowLongA unsupported index");

    /* ---- messages and metrics ---- */
    put32(MSG,0xAAAAAAAAu);
    a[0]=MSG;a[1]=win;a[2]=0;a[3]=0;a[4]=1;
    CALL("user32.dll","PeekMessageA",a,5,&r);
    CHECK(r==0,"PeekMessageA never fabricates");
    CHECK(get32(MSG)==0xAAAAAAAAu,"PeekMessageA leaves MSG untouched");
    a[0]=MSG;
    CALL("user32.dll","TranslateMessage",a,1,&r);
    CHECK(r==0,"TranslateMessage");
    a[0]=win;
    CALL("user32.dll","DefWindowProcA",a,4,&r);
    CHECK(r==0,"DefWindowProcA");

    const struct {uint32_t index,value;} metrics[]={
        {0,400},{1,240},{76,400},{77,240},{80,1},{99,0}
    };
    for(unsigned i=0;i<sizeof metrics/sizeof metrics[0];++i){
        a[0]=metrics[i].index;
        CALL("user32.dll","GetSystemMetrics",a,1,&r);
        CHECK(r==metrics[i].value,"GetSystemMetrics");
    }

    a[0]=0;a[1]=MBOX;a[2]=MBOX+0x40;a[3]=0;
    putstr(MBOX+0x40,"caption");
    CALL("user32.dll","MessageBoxA",a,4,&r);
    CHECK(r==1,"MessageBoxA returns IDOK");
    a[0]=0;a[1]=0;a[2]=0;a[3]=0;
    CALL("user32.dll","MessageBoxA",a,4,&r);
    CHECK(r==1,"MessageBoxA NULL text");

    /* ---- cursor and input stubs ---- */
    a[0]=0;a[1]=0x7f00;
    CALL("user32.dll","LoadIconA",a,2,&r);
    CHECK(r==0x00020001u,"LoadIconA fake handle");
    a[0]=0;a[1]=0x7f01;
    CALL("user32.dll","LoadCursorA",a,2,&r);
    CHECK(r==0x00020002u,"LoadCursorA fake handle");
    a[0]=0x00020001u;
    CALL("user32.dll","SetCursor",a,1,&r);
    CHECK(r==0,"SetCursor initial previous");
    a[0]=0;
    CALL("user32.dll","SetCursor",a,1,&r);
    CHECK(r==0x00020001u,"SetCursor returns previous");
    a[0]=1;
    CALL("user32.dll","ShowCursor",a,1,&r);
    CHECK((int32_t)r==1,"ShowCursor increments");
    CALL("user32.dll","ShowCursor",a,1,&r);
    CHECK((int32_t)r==2,"ShowCursor increments again");
    a[0]=0;
    CALL("user32.dll","ShowCursor",a,1,&r);
    CHECK((int32_t)r==1,"ShowCursor decrements");
    CALL("user32.dll","ShowCursor",a,1,&r);
    CHECK((int32_t)r==0,"ShowCursor back to zero");
    a[0]=0x10;
    CALL("user32.dll","GetKeyState",a,1,&r);
    CHECK(r==0,"GetKeyState no input");
    CALL("user32.dll","GetAsyncKeyState",a,1,&r);
    CHECK(r==0,"GetAsyncKeyState no input");

    /* ---- activation ---- */
    CALL("user32.dll","GetActiveWindow",NULL,0,&r);
    CHECK(r==0,"initial active window");
    a[0]=win;
    CALL("user32.dll","SetActiveWindow",a,1,&r);
    CHECK(r==0,"SetActiveWindow returns old");
    CALL("user32.dll","GetActiveWindow",NULL,0,&r);
    CHECK(r==win,"GetActiveWindow");
    a[0]=0;
    CALL("user32.dll","SetActiveWindow",a,1,&r);
    CHECK(r==win,"SetActiveWindow null returns old");
    a[0]=0x9999;
    CALL("user32.dll","SetActiveWindow",a,1,&r);
    CHECK(r==0 && last_error()==1400,"SetActiveWindow invalid");
    CALL("user32.dll","GetForegroundWindow",NULL,0,&r);
    CHECK(r==0,"initial foreground window");
    a[0]=win;
    CALL("user32.dll","SetForegroundWindow",a,1,&r);
    CHECK(r==1,"SetForegroundWindow");
    CALL("user32.dll","GetForegroundWindow",NULL,0,&r);
    CHECK(r==win,"GetForegroundWindow");
    a[0]=0;
    CALL("user32.dll","SetForegroundWindow",a,1,&r);
    CHECK(r==0 && last_error()==1400,"SetForegroundWindow zero invalid");

    /* ---- a shown window is the active/foreground window ---------------
     * The retail WinMain only calls its frame pump (0x69cbd0) when
     * GetActiveWindow() equals the handle it stored.  A single-window virtual
     * desktop must therefore activate the game's own shown window. */
    a[0]=win;a[1]=5; /* SW_SHOW */
    CALL("user32.dll","ShowWindow",a,2,&r);
    CHECK(r==0,"ShowWindow first show returns hidden state");
    CALL("user32.dll","GetActiveWindow",NULL,0,&r);
    CHECK(r==win,"shown window becomes active");
    CALL("user32.dll","GetForegroundWindow",NULL,0,&r);
    CHECK(r==win,"shown window becomes foreground");
    a[0]=0;a[1]=CLASS_NAME;a[2]=TITLE;a[3]=0x10000000u|0x00CF0000u;
    a[4]=0;a[5]=0;a[6]=64;a[7]=64;a[8]=0;a[9]=0;a[10]=0;a[11]=0;
    CALL("user32.dll","CreateWindowExA",a,12,&r);
    uint32_t win2=r;
    CHECK(win2==0x00010001u,"second window handle");
    CALL("user32.dll","GetActiveWindow",NULL,0,&r);
    CHECK(r==win,"WS_VISIBLE does not steal an existing active window");
    a[0]=win2;
    CALL("user32.dll","DestroyWindow",a,1,&r);
    CHECK(r==1,"DestroyWindow second");

    /* ---- FindWindow and GDI ---- */
    a[0]=CLASS_NAME;a[1]=TITLE;
    CALL("user32.dll","FindWindowA",a,2,&r);
    CHECK(r==0,"FindWindowA returns NULL");
    a[0]=0;a[1]=0;
    CALL("user32.dll","FindWindowA",a,2,&r);
    CHECK(r==0,"FindWindowA null names");

    a[0]=win;
    CALL("user32.dll","GetDC",a,1,&r);
    uint32_t dc=r;
    CHECK(dc==0x00030001u,"GetDC fake handle");
    a[0]=0;
    CALL("gdi32.dll","GetStockObject",a,1,&r);
    CHECK(r==0x00040000u,"GetStockObject 0");
    a[0]=19;
    CALL("gdi32.dll","GetStockObject",a,1,&r);
    CHECK(r==0x00040013u,"GetStockObject 19");
    a[0]=20;
    CALL("gdi32.dll","GetStockObject",a,1,&r);
    CHECK(r==0 && last_error()==87,"GetStockObject out of range");
    a[0]=dc;a[1]=0x00040001u;
    CALL("gdi32.dll","SelectObject",a,2,&r);
    CHECK(r==0x00040001u,"SelectObject stock");
    a[0]=0x9999;a[1]=0x00040001u;
    CALL("gdi32.dll","SelectObject",a,2,&r);
    CHECK(r==0 && last_error()==87,"SelectObject invalid DC");
    a[0]=dc;a[1]=0x12345678u;
    CALL("gdi32.dll","SelectObject",a,2,&r);
    CHECK(r==0 && last_error()==87,"SelectObject invalid object");
    a[0]=0x00040000u;
    CALL("gdi32.dll","DeleteObject",a,1,&r);
    CHECK(r==1,"DeleteObject stock");
    a[0]=0x00020001u;
    CALL("gdi32.dll","DeleteObject",a,1,&r);
    CHECK(r==1,"DeleteObject icon");
    a[0]=0x12345678u;
    CALL("gdi32.dll","DeleteObject",a,1,&r);
    CHECK(r==0 && last_error()==87,"DeleteObject invalid");
    a[0]=win;a[1]=dc;
    CALL("user32.dll","ReleaseDC",a,2,&r);
    CHECK(r==1,"ReleaseDC");
    a[0]=win;a[1]=0x1234;
    CALL("user32.dll","ReleaseDC",a,2,&r);
    CHECK(r==0 && last_error()==87,"ReleaseDC invalid");

    /* ---- invalid handles ---- */
    a[0]=0x9999;a[1]=5;
    CALL("user32.dll","ShowWindow",a,2,&r);
    CHECK(r==0 && last_error()==1400,"ShowWindow invalid");
    a[0]=0x9999;
    CALL("user32.dll","UpdateWindow",a,1,&r);
    CHECK(r==0 && last_error()==1400,"UpdateWindow invalid");
    a[0]=0x9999;a[1]=HELLO;
    CALL("user32.dll","SetWindowTextA",a,2,&r);
    CHECK(r==0 && last_error()==1400,"SetWindowTextA invalid");
    a[0]=0x9999;a[1]=WNDTEXT;a[2]=64;
    CALL("user32.dll","GetWindowTextA",a,3,&r);
    CHECK(r==0 && last_error()==1400,"GetWindowTextA invalid");
    a[0]=0x9999;a[1]=RECT_OUT;
    CALL("user32.dll","GetClientRect",a,2,&r);
    CHECK(r==0 && last_error()==1400,"GetClientRect invalid");
    a[0]=0x9999;a[1]=0;a[2]=0;a[3]=0;a[4]=1;a[5]=1;a[6]=0;
    CALL("user32.dll","SetWindowPos",a,7,&r);
    CHECK(r==0 && last_error()==1400,"SetWindowPos invalid");
    a[0]=0x9999;
    CALL("user32.dll","GetDC",a,1,&r);
    CHECK(r==0 && last_error()==1400,"GetDC invalid");

    /* ---- class/window lifetime ---- */
    a[0]=0;a[1]=OTHER_NAME;a[2]=0;a[3]=0;a[4]=0;a[5]=0;a[6]=100;a[7]=100;a[8]=0;a[9]=0;a[10]=0;a[11]=0;
    CALL("user32.dll","CreateWindowExA",a,12,&r);
    uint32_t other=r;
    CHECK(other==0x00010001u,"second window handle");
    a[0]=OTHER_NAME;a[1]=0;
    CALL("user32.dll","UnregisterClassA",a,2,&r);
    CHECK(r==0 && last_error()==1414,"class with window cannot unregister");
    a[0]=other;
    CALL("user32.dll","DestroyWindow",a,1,&r);
    CHECK(r==1,"destroy second window");
    a[0]=OTHER_NAME;a[1]=0;
    CALL("user32.dll","UnregisterClassA",a,2,&r);
    CHECK(r==1,"unregister second class");

    a[0]=win;
    CALL("user32.dll","DestroyWindow",a,1,&r);
    CHECK(r==1,"DestroyWindow");
    CHECK(get32(TEB+0x34)==0,"destroy clears error");
    CALL("user32.dll","GetForegroundWindow",NULL,0,&r);
    CHECK(r==0,"destroy clears foreground");
    a[0]=win;a[1]=RECT_OUT;
    CALL("user32.dll","GetWindowRect",a,2,&r);
    CHECK(r==0 && last_error()==1400,"destroyed window invalid");
    a[0]=win;
    CALL("user32.dll","DestroyWindow",a,1,&r);
    CHECK(r==0 && last_error()==1400,"double destroy");
    a[0]=MISSING_NAME;a[1]=0;
    CALL("user32.dll","UnregisterClassA",a,2,&r);
    CHECK(r==0 && last_error()==1412,"unregister missing class");
    a[0]=CLASS_NAME;a[1]=0;
    CALL("user32.dll","UnregisterClassA",a,2,&r);
    CHECK(r==1,"unregister class");
    CALL("user32.dll","UnregisterClassA",a,2,&r);
    CHECK(r==0 && last_error()==1412,"unregister twice");

    /* ---- lifecycle ---- */
    win32_user_shutdown();
    CHECK(win32_user_lookup(WIN32_USER_BASE)==NULL,"lookup after shutdown");
    CHECK(win32_user_init(&m)==0,"re-init");
    CHECK(win32_user_lookup(WIN32_USER_BASE)!=NULL,"lookup after re-init");
    win32_user_shutdown();

done:
    guest_destroy(&m);
    if(failures){
        printf("FAIL: %d check(s) failed\n",failures);
        return 1;
    }
    puts("PASS: win32_user smoke test");
    return 0;
}
