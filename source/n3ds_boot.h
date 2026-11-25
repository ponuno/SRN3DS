#pragma once
#include "guest_process.h"
#include "n3ds_lift_runtime.h"

typedef struct {
    GuestMemory memory;
    GuestImage images[GP_MAX_MODULES];
    char names[GP_MAX_MODULES][64];
    PeModule modules[GP_MAX_MODULES];
    uint8_t data_only[GP_MAX_MODULES];
    size_t count;
    PeLinker linker;
    GuestProcess process;
    CPU cpu;
    int shims,bound,ready,ran,traps;
    char message[256];
} BootSession;
/* Session owns all guest allocations. Module 0 must be the EXE. Remaining
 * modules must be added in DLL initialization dependency order. */
void n3ds_boot_init(BootSession *b,size_t budget);
/* Optional last-resort named traps for unresolved imports. Normal sessions
 * reject them during preflight; diagnostic sessions may bind explicit traps. */
void n3ds_boot_set_unresolved_traps(BootSession *b,int enabled);
int n3ds_boot_add_file(BootSession *b,const char *name,const char *path,uint32_t base);
int n3ds_boot_add_mapped(BootSession *b,const char *name,const GuestImage *image);
/* Add a dependent DLL for export/import resolution only: its DllMain and TLS
 * callbacks are not executed and its entry need not be lifted. */
int n3ds_boot_add_data_file(BootSession *b,const char *name,const char *path,uint32_t base);
int n3ds_boot_prepare(BootSession *b,const FnModule *tables,unsigned table_count,
                     uint32_t stack_bytes,uint32_t heap_bytes,const char *game_root);
/* Load four local Skyrim modules at the same bases as Skyrim_Android and
 * preflight their supplied static tables. Does not start the game. */
int n3ds_boot_prepare_skyrim(BootSession *b,const char *game_root,const FnModule *tables,unsigned table_count,
                            uint32_t stack_bytes,uint32_t heap_bytes);
/* Executes only linked, preflighted static C functions; never executes PE bytes. */
int n3ds_boot_run(BootSession *b,uint32_t *exit_status);
void n3ds_boot_destroy(BootSession *b);
