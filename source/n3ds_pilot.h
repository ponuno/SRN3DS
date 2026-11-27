#pragma once
#include "n3ds_lift_runtime.h"
typedef struct {
    const char *name;unsigned test_case;uint32_t entry,base;
    uint64_t file_bytes,fnv64;
} PilotSpec;
typedef struct {unsigned passed;size_t memory,limit;char message[256];} PilotResult;
int n3ds_pilot_identity(const char *path,uint64_t *bytes,uint64_t *hash);
int n3ds_pilot_run(const char *path,const PilotSpec *spec,const FnEntry *table,unsigned count,size_t budget,PilotResult *result);
/* Shared basic-block guard used by generated slices and the pilot. */
void n3ds_pilot_set_steps(uint64_t steps);
