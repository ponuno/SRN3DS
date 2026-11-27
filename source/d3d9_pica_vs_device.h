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
    const D9PicaVsProgram *loaded_program;   /* identity checked by bind */
    int program_ready;
    int open;
} D9PicaVsDevice;

int d9_pica_vs_device_open(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           char *why, size_t why_size);
void d9_pica_vs_device_close(D9PicaVsDevice *device);
/* Bind and declare.  `rows` is the bank in PICA row order; `row_mask` is the
 * D3D dirty mask and only written rows are uploaded.  `packed` is the already
 * repacked native-layout stream (`stride` bytes per vertex) built by
 * d9_pica_vs_pack_vertex(); the declaration's original offsets are checked
 * before AttrInfo/BufInfo are touched. */
int d9_pica_vs_device_bind(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           const D9PicaVsDecl *decl,
                           const float (*rows)[4], const uint32_t *row_mask,
                           unsigned row_count,
                           const void *packed, size_t vertex_count,
                           size_t stride, char *why, size_t why_size);
#endif
