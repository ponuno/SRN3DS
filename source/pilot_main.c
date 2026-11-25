#include "n3ds_pilot.h"
#include "pilot_config.h"
#include <stdio.h>
#ifdef __3DS__
#include <3ds.h>
#endif
extern const FnEntry g_fn_table_pilot[];
extern const unsigned g_fn_count_pilot;
static const PilotSpec spec={PILOT_EXPORT,PILOT_CASE,PILOT_ENTRY,PILOT_BASE,PILOT_FILE_BYTES,PILOT_FNV64};
static void note(char *buf,size_t n){
#if PILOT_ILT_BYPASS
    snprintf(buf,n,"Entry: lazy ILT thunk resolved to body 0x%08lx; wrapper 0x%08lx and one-time initializer 0x%08lx are not executed",
             (unsigned long)PILOT_ENTRY,(unsigned long)PILOT_ILT_WRAPPER,(unsigned long)PILOT_ILT_INIT);
#else
    snprintf(buf,n,"Entry: direct export body (no thunk)");
#endif
}
static int run(const char *path,size_t budget, const char *log){
    PilotResult r;int rc=n3ds_pilot_run(path,&spec,g_fn_table_pilot,g_fn_count_pilot,budget,&r);
    char n[192];note(n,sizeof n);
    printf("%s\n%s\nGuest memory: %lu / %lu KiB\n",r.message,n,(unsigned long)(r.memory/1024),(unsigned long)(r.limit/1024));
    if(log){FILE *f=fopen(log,"w");if(f){fprintf(f,"%s\n%s\nTests: %u/3\nGuest memory: %lu / %lu KiB\n",r.message,n,r.passed,(unsigned long)(r.memory/1024),(unsigned long)(r.limit/1024));fclose(f);}}
    return rc;
}
#ifdef __3DS__
int main(void){
    gfxInitDefault();consoleInit(GFX_TOP,NULL);osSetSpeedupEnable(true);
    printf("Skyrim N3DS v0.11\nIsolated D3DX pilot\n%s\n",PILOT_EXPORT);
    bool n3ds=false;APT_CheckNew3DS(&n3ds);
    size_t free_mem=osGetMemRegionFree(MEMREGION_APPLICATION),budget=free_mem/2;
    if(budget>(48u<<20))budget=48u<<20;
    if(n3ds)run("sdmc:/Skyrim/d3dx9_42.dll",budget,"sdmc:/Skyrim_N3DS_pilot.log");else puts("New Nintendo 3DS required");
    puts("START: exit");while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}gfxExit();return 0;
}
#else
int main(int argc,char **argv){if(argc!=2){fprintf(stderr,"Usage: %s DLL\n",argv[0]);return 2;}return run(argv[1],48u<<20,NULL)?1:0;}
#endif
