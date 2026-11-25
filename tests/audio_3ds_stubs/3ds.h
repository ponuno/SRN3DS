#pragma once
/* Minimal libctru surface used by the host regression for win32_audio_3ds.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint32_t Result;

#define R_SUCCEEDED(r) ((r) == 0u)
#define R_FAILED(r) ((r) != 0u)
#define SYSCLOCK_ARM11 1000000u
#define DSP_RAM_VADDR 0x1FF40000u

#define MEMPERM_READ 1u
#define MEMSTATE_FREE 0

typedef struct {
    u32 base_addr;
    u32 size;
    u32 perm;
    u32 state;
} MemInfo;
typedef struct { u32 flags; } PageInfo;

#define NDSP_FORMAT_STEREO_PCM16 0u
#define NDSP_INTERP_LINEAR 1u
#define NDSP_OUTPUT_STEREO 1
#define NDSP_CLIP_SOFT 1

enum {
    NDSP_WBUF_FREE = 0,
    NDSP_WBUF_QUEUED = 1,
    NDSP_WBUF_PLAYING = 2,
    NDSP_WBUF_DONE = 3,
};

typedef struct {
    const void *data_vaddr;
    u32 nsamples;
    void *adpcm_data;
    u32 offset;
    bool looping;
    u8 status;
    u16 sequence_id;
    struct ndspWaveBuf *next;
} ndspWaveBuf;

uint64_t svcGetSystemTick(void);
Result svcQueryMemory(MemInfo *info, PageInfo *page, u32 address);
void DSP_FlushDataCache(const void *address, u32 size);
void *linearMemAlign(size_t size, size_t alignment);
void linearFree(void *pointer);

void ndspUseComponent(const void *binary, u32 size, u16 progMask, u16 dataMask);
Result ndspInit(void);
void ndspExit(void);
void ndspSetOutputMode(int mode);
void ndspSetClippingMode(int mode);
void ndspSetMasterVol(float volume);
void ndspChnReset(int channel);
void ndspChnSetFormat(int channel, u16 format);
void ndspChnSetRate(int channel, float rate);
void ndspChnSetInterp(int channel, int interpolation);
void ndspChnSetMix(int channel, float mix[12]);
void ndspChnGetMix(int channel, float out_mix[12]);
void ndspChnSetPaused(int channel, bool paused);
bool ndspChnIsPaused(int channel);
void ndspChnWaveBufClear(int channel);
void ndspChnWaveBufAdd(int channel, ndspWaveBuf *buffer);
