#pragma once
#include <stdint.h>
#include "d3d9_core.h"

/* Truthful IDirect3D9 adapter surface for the PICA200 backend.
 * Exactly one display mode (400x240 X8R8G8B8) and caps restricted to what
 * d3d9_core/d3d9_vertex/d3d9_bc/d3d9_citro3d actually implement. */

#define D9_ADAPTER_COUNT 1u
#define D9_DISPLAY_WIDTH 400u
#define D9_DISPLAY_HEIGHT 240u
#define D9_DISPLAY_REFRESH 60u

#define D9_ADAPTER_DRIVER_NAME "Skyrim_N3DS PICA200 D3D9 bridge"
#define D9_ADAPTER_DESCRIPTION "D3D9 to PICA200 host bridge (two exact SM2 shader templates)"
#define D9_ADAPTER_DEVICE_NAME "Nintendo 3DS PICA200"
#define D9_ADAPTER_DEVICE_ID "Skyrim_N3DS_3D9"

enum {D9_ADAPTER_OK=0,D9_ADAPTER_INVALID=-1,D9_ADAPTER_UNAVAILABLE=-2};

typedef struct {uint32_t width,height,refresh,format;} D9DisplayMode;

/* x86 D3DADAPTER_IDENTIFIER9 layout (1100 bytes). */
typedef struct {
    char driver[512],description[512],device_name[32];
    uint32_t driver_version_low,driver_version_high;
    uint32_t vendor_id,device_id,subsys_id,revision;
    uint8_t device_identifier[16];
    uint32_t whql_level;
} D9AdapterIdentifier;

typedef struct {uint32_t caps;int32_t dynamic_flow_control_depth,num_temps,static_flow_control_depth;} D9Vs20Caps;
typedef struct {uint32_t caps;int32_t dynamic_flow_control_depth,num_temps,static_flow_control_depth,num_instruction_slots;} D9Ps20Caps;

/* x86 D3DCAPS9 layout (304 bytes). Floats are IEEE-754 little-endian. */
typedef struct {
    uint32_t device_type,adapter_ordinal,caps,caps2,caps3,presentation_intervals,cursor_caps;
    uint32_t dev_caps,primitive_misc_caps,raster_caps;
    uint32_t z_cmp_caps,src_blend_caps,dest_blend_caps,alpha_cmp_caps,shade_caps;
    uint32_t texture_caps,texture_filter_caps,cube_texture_filter_caps,volume_texture_filter_caps;
    uint32_t texture_address_caps,volume_texture_address_caps,line_caps;
    uint32_t max_texture_width,max_texture_height,max_volume_extent,max_texture_repeat;
    uint32_t max_texture_aspect_ratio,max_anisotropy;
    float max_vertex_w;
    float guard_band_left,guard_band_top,guard_band_right,guard_band_bottom,extents_adjust;
    uint32_t stencil_caps,fvf_caps,texture_op_caps,max_texture_blend_stages,max_simultaneous_textures;
    uint32_t vertex_processing_caps,max_active_lights,max_user_clip_planes;
    uint32_t max_vertex_blend_matrices,max_vertex_blend_matrix_index;
    float max_point_size;
    uint32_t max_primitive_count,max_vertex_index,max_streams,max_stream_stride;
    uint32_t vertex_shader_version,max_vertex_shader_const,pixel_shader_version;
    float pixel_shader_1x_max_value;
    uint32_t dev_caps2;
    float max_npatch_tessellation_level;
    uint32_t reserved5,master_adapter_ordinal,adapter_ordinal_in_group,number_of_adapters_in_group;
    uint32_t decl_types,num_simultaneous_rts,stretch_rect_filter_caps;
    D9Vs20Caps vs20;D9Ps20Caps ps20;
    uint32_t vertex_texture_filter_caps,max_vshader_instructions_executed,max_pshader_instructions_executed;
    uint32_t max_vertex_shader_30_instruction_slots,max_pixel_shader_30_instruction_slots;
} D9Caps9;

int d9_adapter_mode_count(unsigned adapter,unsigned format);
int d9_adapter_display_mode(unsigned adapter,unsigned format,unsigned index,D9DisplayMode *out);
int d9_adapter_identifier(unsigned adapter,D9AdapterIdentifier *out);
int d9_adapter_caps(unsigned adapter,unsigned device_type,D9Caps9 *out);
/* Truthful explanation of one D3DDEVCAPS bit in the adapter's DevCaps.
 * Returns a static NUL-terminated string; never NULL.  Strings starting with
 * "claimed:" describe an implemented capability, "not claimed:" describe why a
 * bit stays clear.  This does not alter d9_adapter_caps(). */
const char *d9_adapter_devcaps_reason(uint32_t bit);
int d9_adapter_check_device_type(unsigned adapter,unsigned device_type,unsigned display_format,unsigned backbuffer_format,int windowed);
int d9_adapter_check_device_format(unsigned adapter,unsigned device_type,unsigned adapter_format,unsigned usage,unsigned resource_type,unsigned format);
int d9_adapter_check_multisample(unsigned adapter,unsigned device_type,unsigned surface_format,int windowed,unsigned multisample_type);
int d9_adapter_check_depth_stencil(unsigned adapter,unsigned device_type,unsigned display_format,unsigned backbuffer_format,unsigned depth_stencil_format);
int d9_adapter_check_format_conversion(unsigned adapter,unsigned device_type,unsigned source_format,unsigned target_format);
