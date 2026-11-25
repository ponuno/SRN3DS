#pragma once
#include <3ds.h>

#define OPENDSP_CAP_FIRMWARE (1u << 0)
#define OPENDSP_CAP_AAC_ARM11 (1u << 16)
#define OPENDSP_CAP_MP3_ARM11 (1u << 17)
#define OPENDSP_ERR_NO_IMAGE 1u

Result opendspInit(void);
void opendspExit(void);
Result opendspGetCapabilities(u32 *caps);
