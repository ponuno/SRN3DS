/* Minimal user32/GDI compatibility thunks.
 *
 * A static recompiled Windows game expects a window, a message queue and a
 * device context. This module provides the bookkeeping only: no host window is
 * created, PeekMessageA never invents a message, and input is not implemented.
 * The guest is single-threaded, so all state is one fixed static table.
 *
 * The intended display is 400x240 (3DS), hence GetSystemMetrics values.
 * Unsupported functions/indices fail with a Win32 error; they do not claim
 * success. See win32_user.h for the limitations and the registry contract. */
#include "win32_user.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 240

#define ERR_INVALID_PARAMETER 87u
#define ERR_INSUFFICIENT_BUFFER 122u
#define ERR_NOT_ENOUGH_MEMORY 8u
#define ERR_INVALID_WINDOW_HANDLE 1400u
#define ERR_CANNOT_FIND_WND_CLASS 1407u
#define ERR_CLASS_ALREADY_EXISTS 1410u
#define ERR_CLASS_DOES_NOT_EXIST 1412u
#define ERR_CLASS_HAS_WINDOWS 1414u

#define MAX_CLASSES 16
#define MAX_WINDOWS 32
#define MAX_CLASS_NAME 128
#define MAX_TITLE 256

#define HWND_BASE 0x00010000u
#define ATOM_BASE 0xC000u
#define CW_USEDEFAULT 0x80000000u
#define GWL_USERDATA (-21)
#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define FAKE_ICON 0x00020001u
#define FAKE_CURSOR 0x00020002u
#define FAKE_DC 0x00030001u
#define STOCK_BASE 0x00040000u
#define MAX_STOCK 20u
#define MSG_BYTES 28u

typedef struct {
    int used;
    char name[MAX_CLASS_NAME];
    uint32_t wndproc;
} UserClass;

typedef struct {
    int used;
    unsigned cls;
    int32_t x,y,w,h;
    char title[MAX_TITLE];
    uint32_t userdata;
} UserWindow;

static GuestMemory *mem;
static UserClass classes[MAX_CLASSES];
static UserWindow windows[MAX_WINDOWS];
static uint32_t active_window,foreground_window,cursor;
static int show_count;

static uint32_t arg(CPU *c,unsigned n) {
    uint32_t addr=c->r[4]+4+n*4,v;
    if(guest_read32(mem,addr,&v))rt_unimpl(c,addr,"invalid user32 argument frame");
    return v;
}
#define A(n) arg(c,n)
#define RET(v,n) do{c->r[0]=(uint32_t)(v);c->r[4]+=4+(n)*4;return;}while(0)
#define FAIL(err,v,n) do{set_error(c,err);RET(v,n);}while(0)

static void set_error(CPU *c,uint32_t v) {
    if(!c->fs_base)return;
    if(guest_write32(mem,c->fs_base+0x34,v))rt_unimpl(c,c->fs_base+0x34,"invalid TEB last-error slot");
}
static void *checked(CPU *c,uint32_t addr,uint32_t n) {
    void *p=guest_ptr(mem,addr,n);
    if(!p)rt_unimpl(c,addr,"invalid user32 shim memory range");
    return p;
}
static const char *str(CPU *c,uint32_t addr) {
    const char *p=guest_cstr(mem,addr,1024*1024);
    if(!p)rt_unimpl(c,addr,"invalid user32 string");
    return p;
}
static unsigned lower_ascii(unsigned ch) {return ch>='A'&&ch<='Z'?ch+32:ch;}
static int icmp(const char *a,const char *b) {
    while(*a && lower_ascii((unsigned char)*a)==lower_ascii((unsigned char)*b)){++a;++b;}
    return (int)lower_ascii((unsigned char)*a)-(int)lower_ascii((unsigned char)*b);
}
static UserWindow *win_for(uint32_t hwnd) {
    if(hwnd<HWND_BASE || hwnd>=HWND_BASE+MAX_WINDOWS)return NULL;
    UserWindow *w=&windows[hwnd-HWND_BASE];
    return w->used?w:NULL;
}
static int class_by_name(const char *name) {
    for(unsigned i=0;i<MAX_CLASSES;++i)
        if(classes[i].used && !icmp(classes[i].name,name))return (int)i;
    return -1;
}
static int known_object(uint32_t h) {
    return (h>=FAKE_ICON && h<=FAKE_CURSOR) || (h>=STOCK_BASE && h<STOCK_BASE+MAX_STOCK);
}
static void write_rect(CPU *c,uint32_t p,int32_t l,int32_t t,int32_t r,int32_t b) {
    uint8_t *d=checked(c,p,16);
    /* Guest RECTs may be unaligned and this code also builds for ARM. */
    memcpy(d,&l,4);memcpy(d+4,&t,4);memcpy(d+8,&r,4);memcpy(d+12,&b,4);
}

/* ---- window classes ---- */
static void s_RegisterClassA(CPU *c) {
    uint32_t p=A(0);
    const uint8_t *wc=checked(c,p,40);
    uint32_t proc;
    memcpy(&proc,wc+4,4);
    uint32_t namep;
    memcpy(&namep,wc+36,4);
    if(!namep)FAIL(ERR_INVALID_PARAMETER,0,1);
    const char *name=str(c,namep);
    if(strlen(name)>=MAX_CLASS_NAME)FAIL(ERR_INVALID_PARAMETER,0,1);
    if(class_by_name(name)>=0)FAIL(ERR_CLASS_ALREADY_EXISTS,0,1);
    unsigned i;
    for(i=0;i<MAX_CLASSES && classes[i].used;++i){}
    if(i==MAX_CLASSES)FAIL(ERR_NOT_ENOUGH_MEMORY,0,1);
    classes[i].used=1;
    memcpy(classes[i].name,name,strlen(name)+1);
    classes[i].wndproc=proc;
    set_error(c,0);
    RET(ATOM_BASE+i,1);
}
static void s_UnregisterClassA(CPU *c) {
    const char *name=str(c,A(0));
    int i=class_by_name(name);
    if(i<0)FAIL(ERR_CLASS_DOES_NOT_EXIST,0,2);
    for(unsigned k=0;k<MAX_WINDOWS;++k)
        if(windows[k].used && windows[k].cls==(unsigned)i)FAIL(ERR_CLASS_HAS_WINDOWS,0,2);
    classes[i].used=0;
    classes[i].name[0]=0;
    classes[i].wndproc=0;
    set_error(c,0);
    RET(1,2);
}

/* ---- windows ---- */
static void s_CreateWindowExA(CPU *c) {
    uint32_t namep=A(1),titlep=A(2);
    if(!namep)FAIL(ERR_CANNOT_FIND_WND_CLASS,0,12);
    const char *name=str(c,namep);
    int ci=class_by_name(name);
    if(ci<0)FAIL(ERR_CANNOT_FIND_WND_CLASS,0,12);
    unsigned i;
    for(i=0;i<MAX_WINDOWS && windows[i].used;++i){}
    if(i==MAX_WINDOWS)FAIL(ERR_NOT_ENOUGH_MEMORY,0,12);
    UserWindow *w=&windows[i];
    memset(w,0,sizeof *w);
    w->used=1;w->cls=(unsigned)ci;
    int32_t x=(int32_t)A(4),y=(int32_t)A(5),cx=(int32_t)A(6),cy=(int32_t)A(7);
    w->x=(uint32_t)x==CW_USEDEFAULT?0:x;
    w->y=(uint32_t)y==CW_USEDEFAULT?0:y;
    /* No host screen exists; the virtual screen is 400x240. */
    w->w=(uint32_t)cx==CW_USEDEFAULT?SCREEN_W:cx;
    w->h=(uint32_t)cy==CW_USEDEFAULT?SCREEN_H:cy;
    const char *title=titlep?str(c,titlep):"";
    size_t n=strlen(title);
    if(n>=MAX_TITLE)n=MAX_TITLE-1;
    memcpy(w->title,title,n);
    w->title[n]=0;
    set_error(c,0);
    RET(HWND_BASE+i,12);
}
static void s_DestroyWindow(CPU *c) {
    uint32_t hwnd=A(0);
    UserWindow *w=win_for(hwnd);
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    w->used=0;
    if(active_window==hwnd)active_window=0;
    if(foreground_window==hwnd)foreground_window=0;
    set_error(c,0);
    RET(1,1);
}
static void s_DefWindowProcA(CPU *c) {RET(0,4);}
static void s_GetClientRect(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    write_rect(c,A(1),0,0,w->w,w->h);
    RET(1,2);
}
static void s_GetWindowRect(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    /* Deliberately origin-relative: there is no desktop coordinate space. */
    write_rect(c,A(1),0,0,w->w,w->h);
    RET(1,2);
}
static void s_AdjustWindowRect(CPU *c) {
    /* BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu): the
     * rectangle is the single in/out parameter.  This window has no non-client
     * area, so the rectangle is validated and left unchanged. */
    (void)checked(c,A(0),16);
    set_error(c,0);
    RET(1,3);
}
static void s_AdjustWindowRectEx(CPU *c) {
    /* BOOL AdjustWindowRectEx(LPRECT, DWORD style, BOOL menu, DWORD exStyle). */
    (void)checked(c,A(0),16);
    set_error(c,0);
    RET(1,4);
}
static void s_SetWindowPos(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,7);
    uint32_t flags=A(6);
    if(!(flags&SWP_NOMOVE)){w->x=(int32_t)A(2);w->y=(int32_t)A(3);}
    if(!(flags&SWP_NOSIZE)){w->w=(int32_t)A(4);w->h=(int32_t)A(5);}
    set_error(c,0);
    RET(1,7);
}
static void s_SetWindowTextA(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    const char *s=A(1)?str(c,A(1)):"";
    size_t n=strlen(s);
    if(n>=MAX_TITLE)n=MAX_TITLE-1;
    memcpy(w->title,s,n);
    w->title[n]=0;
    set_error(c,0);
    RET(1,2);
}
static void s_GetWindowTextA(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,3);
    int32_t cap=(int32_t)A(2);
    if(cap<0)FAIL(ERR_INVALID_PARAMETER,0,3);
    if(!cap)RET(0,3);
    char *buf=checked(c,A(1),(uint32_t)cap);
    size_t len=strlen(w->title),copy=len<(size_t)cap-1?len:(size_t)cap-1;
    memcpy(buf,w->title,copy);
    buf[copy]=0;
    set_error(c,0);
    RET(copy,3);
}
static void s_GetClassNameA(CPU *c) {
    UserWindow *w=win_for(A(0));
    uint32_t buf=A(1),n=A(2);
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,3);
    if(!buf||!n)FAIL(ERR_INVALID_PARAMETER,0,3);
    const char *name=classes[w->cls].name;
    size_t len=strlen(name);
    uint8_t *out=checked(c,buf,n);
    size_t copy=len<n-1?len:n-1;
    memcpy(out,name,copy);out[copy]=0;
    set_error(c,0);
    RET((uint32_t)copy,3);
}
static void s_ClientToScreen(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    /* Windows are origin-relative: there is no desktop coordinate space. */
    (void)checked(c,A(1),8);
    set_error(c,0);
    RET(1,2);
}
static void s_ScreenToClient(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    checked(c,A(1),8);
    set_error(c,0);
    RET(1,2);
}
static void s_IsIconic(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    RET(0,1);
}
static void s_IsWindowVisible(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    RET(1,1);
}
static void s_GetWindowInfo(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w||!guest_ptr(mem,A(1),60))FAIL(ERR_INVALID_PARAMETER,0,2);
    uint32_t size;
    if(guest_read32(mem,A(1),&size)||size<60)FAIL(ERR_INSUFFICIENT_BUFFER,0,2);
    for(uint32_t off=0;off<60;off+=4)(void)guest_write32(mem,A(1)+off,0);
    guest_write32(mem,A(1),60);
    guest_write32(mem,A(1)+24,(uint32_t)w->w);
    guest_write32(mem,A(1)+28,(uint32_t)w->h);
    guest_write32(mem,A(1)+32,0x10000000u); /* WS_VISIBLE */
    set_error(c,0);
    RET(1,2);
}
static void s_GetTopWindow(CPU *c) {
    for(unsigned i=0;i<MAX_WINDOWS;++i)if(windows[i].used)RET(HWND_BASE+i,1);
    RET(0,1);
}
static void s_GetWindow(CPU *c) { (void)A(0);(void)A(1);RET(0,2); }
static void s_GetWindowThreadProcessId(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    if(A(1)){checked(c,A(1),4);guest_write32(mem,A(1),0x40);}
    RET(1,2);
}
static void s_ShowWindow(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    set_error(c,0);
    RET(1,2);
}
static void s_UpdateWindow(CPU *c) {
    if(!win_for(A(0)))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    set_error(c,0);
    RET(1,1);
}
static void s_GetActiveWindow(CPU *c) {RET(active_window,0);}
static void s_SetActiveWindow(CPU *c) {
    uint32_t hwnd=A(0);
    if(hwnd && !win_for(hwnd))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    uint32_t old=active_window;
    active_window=hwnd;
    set_error(c,0);
    RET(old,1);
}
static void s_GetForegroundWindow(CPU *c) {RET(foreground_window,0);}
static void s_SetForegroundWindow(CPU *c) {
    uint32_t hwnd=A(0);
    if(!win_for(hwnd))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    foreground_window=hwnd;
    set_error(c,0);
    RET(1,1);
}
/* No desktop to search: always NULL, no error. */
static void s_FindWindowA(CPU *c) {
    if(A(0))str(c,A(0));
    if(A(1))str(c,A(1));
    RET(0,2);
}
static void s_GetSystemMetrics(CPU *c) {
    switch(A(0)) {
    case 0:RET(SCREEN_W,1);        /* SM_CXSCREEN */
    case 1:RET(SCREEN_H,1);        /* SM_CYSCREEN */
    case 76:RET(SCREEN_W,1);       /* SM_CXVIRTUALSCREEN */
    case 77:RET(SCREEN_H,1);       /* SM_CYVIRTUALSCREEN */
    case 80:RET(1,1);              /* SM_CMONITORS */
    default:RET(0,1);              /* not modelled */
    }
}
static void s_MessageBoxA(CPU *c) {
    uint32_t textp=A(1);
    const char *text="";
    if(textp)text=str(c,textp);
    fprintf(stderr,"[messagebox] %s\n",text);
    set_error(c,0);
    RET(1,4); /* IDOK */
}

/* ---- cursor, input, messages ---- */
static void s_LoadIconA(CPU *c) {set_error(c,0);RET(FAKE_ICON,2);}
static void s_LoadCursorA(CPU *c) {set_error(c,0);RET(FAKE_CURSOR,2);}
static void s_SetCursor(CPU *c) {
    uint32_t old=cursor;
    cursor=A(0);
    set_error(c,0);
    RET(old,1);
}
static void s_ShowCursor(CPU *c) {
    show_count+=A(0)?1:-1;
    set_error(c,0);
    RET((uint32_t)show_count,1);
}
static void s_GetKeyState(CPU *c) {RET(0,1);}      /* fallback: win32_input's real thunk resolves first */
static void s_GetAsyncKeyState(CPU *c) {RET(0,1);}
/* Never fabricate WM_QUIT or any other message: the queue is permanently empty. */
static void s_PeekMessageA(CPU *c) {
    if(A(0))checked(c,A(0),MSG_BYTES);
    RET(0,5);
}
static void s_TranslateMessage(CPU *c) {
    if(A(0))checked(c,A(0),MSG_BYTES);
    RET(0,1);
}

/* ---- per-window/class storage ---- */
static void s_GetWindowLongA(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    RET((int32_t)A(1)==GWL_USERDATA?w->userdata:0,2);
}
static void s_SetWindowLongA(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,3);
    if((int32_t)A(1)!=GWL_USERDATA)FAIL(ERR_INVALID_PARAMETER,0,3);
    uint32_t old=w->userdata;
    w->userdata=A(2);
    set_error(c,0);
    RET(old,3);
}
static void s_GetClassLongA(CPU *c) {
    UserWindow *w=win_for(A(0));
    if(!w)FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    RET((int32_t)A(1)==GWL_USERDATA?w->userdata:0,2);
}

/* ---- GDI stubs ---- */
static void s_GetDC(CPU *c) {
    uint32_t hwnd=A(0);
    if(hwnd && !win_for(hwnd))FAIL(ERR_INVALID_WINDOW_HANDLE,0,1);
    set_error(c,0);
    RET(FAKE_DC,1);
}
static void s_ReleaseDC(CPU *c) {
    uint32_t hwnd=A(0);
    if(hwnd && !win_for(hwnd))FAIL(ERR_INVALID_WINDOW_HANDLE,0,2);
    if(A(1)!=FAKE_DC)FAIL(ERR_INVALID_PARAMETER,0,2);
    set_error(c,0);
    RET(1,2);
}
static void s_GetStockObject(CPU *c) {
    uint32_t i=A(0);
    if(i>=MAX_STOCK)FAIL(ERR_INVALID_PARAMETER,0,1);
    RET(STOCK_BASE+i,1);
}
static void s_DeleteObject(CPU *c) {
    if(!known_object(A(0)))FAIL(ERR_INVALID_PARAMETER,0,1);
    set_error(c,0);
    RET(1,1);
}
static void s_SelectObject(CPU *c) {
    if(A(0)!=FAKE_DC)FAIL(ERR_INVALID_PARAMETER,0,2);
    if(!known_object(A(1)))FAIL(ERR_INVALID_PARAMETER,0,2);
    set_error(c,0);
    RET(A(1),2);
}

#define D(dll,name,fn) {dll,name,s_##fn},
static const Win32UserDef defs[]={
    D("user32.dll","RegisterClassA",RegisterClassA)
    D("user32.dll","UnregisterClassA",UnregisterClassA)
    D("user32.dll","CreateWindowExA",CreateWindowExA)
    D("user32.dll","DestroyWindow",DestroyWindow)
    D("user32.dll","DefWindowProcA",DefWindowProcA)
    D("user32.dll","GetClientRect",GetClientRect)
    D("user32.dll","GetWindowRect",GetWindowRect)
    D("user32.dll","GetClassNameA",GetClassNameA)
    D("user32.dll","ClientToScreen",ClientToScreen)
    D("user32.dll","ScreenToClient",ScreenToClient)
    D("user32.dll","IsIconic",IsIconic)
    D("user32.dll","IsWindowVisible",IsWindowVisible)
    D("user32.dll","GetWindowInfo",GetWindowInfo)
    D("user32.dll","GetTopWindow",GetTopWindow)
    D("user32.dll","GetWindow",GetWindow)
    D("user32.dll","GetWindowThreadProcessId",GetWindowThreadProcessId)
    D("user32.dll","AdjustWindowRect",AdjustWindowRect)
    D("user32.dll","AdjustWindowRectEx",AdjustWindowRectEx)
    D("user32.dll","SetWindowPos",SetWindowPos)
    D("user32.dll","SetWindowTextA",SetWindowTextA)
    D("user32.dll","GetWindowTextA",GetWindowTextA)
    D("user32.dll","ShowWindow",ShowWindow)
    D("user32.dll","UpdateWindow",UpdateWindow)
    D("user32.dll","GetActiveWindow",GetActiveWindow)
    D("user32.dll","SetActiveWindow",SetActiveWindow)
    D("user32.dll","GetForegroundWindow",GetForegroundWindow)
    D("user32.dll","SetForegroundWindow",SetForegroundWindow)
    D("user32.dll","FindWindowA",FindWindowA)
    D("user32.dll","GetSystemMetrics",GetSystemMetrics)
    D("user32.dll","MessageBoxA",MessageBoxA)
    D("user32.dll","LoadIconA",LoadIconA)
    D("user32.dll","LoadCursorA",LoadCursorA)
    D("user32.dll","SetCursor",SetCursor)
    D("user32.dll","ShowCursor",ShowCursor)
    D("user32.dll","GetKeyState",GetKeyState)
    D("user32.dll","GetAsyncKeyState",GetAsyncKeyState)
    D("user32.dll","PeekMessageA",PeekMessageA)
    D("user32.dll","TranslateMessage",TranslateMessage)
    D("user32.dll","GetWindowLongA",GetWindowLongA)
    D("user32.dll","SetWindowLongA",SetWindowLongA)
    D("user32.dll","GetClassLongA",GetClassLongA)
    D("user32.dll","GetDC",GetDC)
    D("user32.dll","ReleaseDC",ReleaseDC)
    D("gdi32.dll","GetStockObject",GetStockObject)
    D("gdi32.dll","DeleteObject",DeleteObject)
    D("gdi32.dll","SelectObject",SelectObject)
};
#undef D
#define NDEFS (sizeof defs/sizeof defs[0])

int win32_user_init(GuestMemory *m) {
    if(mem || !m)return -1;
    mem=m;
    memset(classes,0,sizeof classes);
    memset(windows,0,sizeof windows);
    active_window=foreground_window=cursor=0;
    show_count=0;
    return 0;
}
void win32_user_shutdown(void) {
    mem=NULL;
    memset(classes,0,sizeof classes);
    memset(windows,0,sizeof windows);
    active_window=foreground_window=cursor=0;
    show_count=0;
}
const Win32UserDef *win32_user_defs(unsigned *count) {
    if(count)*count=(unsigned)NDEFS;
    return defs;
}
static int dll_match(const char *a,const char *b) {
    if(!icmp(a,b))return 1;
    size_t n=strlen(b);
    return n>4 && !icmp(b+n-4,".dll") && !strncmp(a,b,n-4) && !a[n-4];
}
uint32_t win32_user_resolve(const char *dll,const char *name) {
    if(!dll || !name)return 0;
    for(unsigned i=0;i<NDEFS;++i)
        if(dll_match(dll,defs[i].dll) && !strcmp(name,defs[i].name))return WIN32_USER_BASE+4*i;
    return 0;
}
static FnEntry entries[NDEFS];
const FnEntry *win32_user_lookup(uint32_t addr) {
    if(!mem || addr<WIN32_USER_BASE || (addr&3))return NULL;
    unsigned i=(addr-WIN32_USER_BASE)/4;
    if(i>=NDEFS)return NULL;
    entries[i]=(FnEntry){addr,defs[i].fn};
    return &entries[i];
}
