#pragma once
/* 3DS-only PICA VS device load/bind.  Kept out of d3d9_pica_vs.h because
 * <citro3d.h> pulls <sys/socket.h>, whose bind() collides with the guest COM
 * dispatcher's static bind().  Include this only from the backend/module. */
#ifndef __3DS__
#error "d3d9_pica_vs_device.h is for the 3DS backend only"
#endif

#include <citro3d.h>

#include "d3d9_pica_vs.h"

#ifdef __3DS__
#include <citro3d.h>

/* Real device path: DVLB_ParseFile on the embedded bytes, shaderProgramInit,
 * shaderProgramSetVsh, AttrInfo_AddLoader for the three inputs, and the
 * uniform bank written into the vertex float uniforms. */
typedef struct {
    DVLB_s *dvlb;
    shaderProgram_s program;
    int program_ready;
    int open;
} D9PicaVsDevice;

int d9_pica_vs_device_open(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           char *why, size_t why_size);
void d9_pica_vs_device_close(D9PicaVsDevice *device);
/* Bind and declare.  `rows` is the bank in PICA row order. */
int d9_pica_vs_device_bind(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           const D9PicaVsDecl *decl,
                           const float (*rows)[4], unsigned row_count,
                           char *why, size_t why_size);
#endif
