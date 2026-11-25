#include <3ds.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdarg.h>
#include "probe.h"
#include "pe_image.h"
#include "guest_heap.h"
#include "guest_call.h"
#include "pe_imports.h"
#include "guest_process.h"

static FILE *log_file;
static void report(const char *fmt, ...) {
    va_list args, copy;
    va_start(args,fmt);
    va_copy(copy,args);
    vprintf(fmt,args);
    if(log_file) {vfprintf(log_file,fmt,copy);fflush(log_file);}
    va_end(copy);va_end(args);
}
#define printf report
typedef struct {PeLinker *linker;unsigned total,resolved,unresolved;} ImportReport;
static int import_count(const PeImport *e,void *ctx) {
    ImportReport *r=ctx;++r->total;
    if(pe_link_resolve(e,r->linker)){++r->resolved;return 0;}
    if(log_file && r->unresolved<256){
        if(e->name)fprintf(log_file,"  unresolved %s!%s\n",e->dll,e->name);
        else fprintf(log_file,"  unresolved %s!#%u\n",e->dll,(unsigned)e->ordinal);
    }
    ++r->unresolved;return 0;
}
#ifdef N3DS_LIFT_TEST
/* The lifted CPU uses its upstream typedefs; keep them out of this TU. */
extern int n3ds_lift_self_test(void);
extern uint32_t win32_resolve(const PeImport *entry,void *ctx);
#endif

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    log_file=fopen("sdmc:/Skyrim_N3DS.log","w");
    bool new3ds = false;
    Result model = APT_CheckNew3DS(&new3ds);
    if (R_SUCCEEDED(model) && new3ds) osSetSpeedupEnable(true);
    printf("Skyrim N3DS - milestone 4\n\n");
    printf("New 3DS: %s\n", R_FAILED(model) ? "unknown" : new3ds ? "yes" : "no");
    printf("Pointer size: %u bits\n", (unsigned)(sizeof(void*) * 8));
    printf("App memory region: %lu MiB\n", (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20));
    printf("App region free: %lu MiB\n\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20));
    printf("Sparse guest memory: %s\n\n", guest_memory_test() == 0 ? "PASS" : "FAIL");
    printf("Guest heap: %s\n",heap_self_test()==0?"PASS":"FAIL");
    printf("Guest call ABI: %s\n",guest_call_self_test()==0?"PASS":"FAIL");
#ifdef N3DS_LIFT_TEST
    printf("Lift ABI: %s\n",n3ds_lift_self_test()==0?"PASS":"FAIL");
#endif
    const char *names[] = {"TESV.exe", "TESV_Unpacked.exe", "d3dx9_42.dll", "binkw32.dll", "X3DAudio1_7.dll"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char path[128];
        snprintf(path, sizeof path, "sdmc:/Skyrim/%s", names[i]);
        PeInfo info;
        int rc = pe_probe(path, &info);
        if (!rc) printf("%s OK\n  base %08" PRIx32 " size %" PRIu32 " KiB\n", names[i], info.image_base, info.image_size >> 10);
        else printf("%s: %s\n", names[i], rc == -1 ? "missing" : "not x86 PE32");
    }
    printf("\nA: map modules  START: exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        if (hidKeysDown() & KEY_A) {
            consoleClear();
            /* Intentional prototype cap: leave space for platform and later generated code. */
            GuestMemory m;
            size_t budget = (size_t)48 << 20;
            size_t free_bytes = osGetMemRegionFree(MEMREGION_APPLICATION);
            if (free_bytes < budget * 2) budget = free_bytes / 2;
            guest_init(&m,budget);
            printf("PE data mapping, budget %lu MiB\n\n",(unsigned long)(budget>>20));
            const char *paths[]={"TESV_Unpacked.exe","d3dx9_42.dll","binkw32.dll","X3DAudio1_7.dll"};
            const char *module_names[]={"TESV.exe","d3dx9_42.dll","binkw32.dll","X3DAudio1_7.dll"};
            const uint32_t bases[]={0,0x08000000,0x09000000,0x0a000000};
            GuestImage images[4];PeModule modules[4];size_t loaded=0;
            for (size_t i=0;i<4;++i) {
                char path[128];
                snprintf(path,sizeof path,"sdmc:/Skyrim/%s",paths[i]);
                GuestImage *image=&images[i];
                int rc=pe_image_load(&m,path,bases[i],image);
                printf("%s: %s\n", paths[i],pe_error(rc));
                if (!rc) {
                    modules[loaded++]=(PeModule){module_names[i],image};
                    printf("  %08" PRIx32 " %" PRIu32 " KiB\n",image->base,image->size>>10);
                }
            }
            PeLinker linker={.memory=&m,.modules=modules,.count=loaded};
#ifdef N3DS_LIFT_TEST
            linker.builtin=win32_resolve;
#endif
            printf("Import audit (dry run):\n");
            for(size_t i=0;i<loaded;++i){
                ImportReport r={.linker=&linker};
                if(log_file)fprintf(log_file,"Audit %s:\n",modules[i].dll);
                int rc=pe_import_walk(&m,modules[i].image,import_count,&r);
                printf("%s %u/%u %s\n",modules[i].dll,r.resolved,r.total,rc?"invalid":"");
            }
            if(loaded && modules[0].image==&images[0]){
                GuestProcess process;
                int pr=guest_process_init(&process,&m,modules,loaded,0x80000);
                if(pr)printf("Process layout: FAIL\n");
                else {
                    unsigned callbacks=0;for(unsigned i=0;i<process.tls_count;++i)callbacks+=process.tls[i].callback_count;
                    printf("Process layout: OK\nTLS modules %u / callbacks %u\n",process.tls_count,callbacks);
                    for(unsigned i=0;i<process.tls_count;++i)if(log_file){
                        GuestTls *t=&process.tls[i];fprintf(log_file,"TLS image %08" PRIx32 " slot %u bytes %" PRIu32 "\n",t->image_base,t->slot,t->bytes);
                    }
                    guest_process_destroy(&process);
                }
            }
            printf("Guest bytes: %lu / %lu\n",(unsigned long)m.used,(unsigned long)m.limit);
            guest_destroy(&m);
            printf("Memory released. A: repeat\nSTART: exit\n");
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    gfxExit();
    if(log_file)fclose(log_file);
    return 0;
}
