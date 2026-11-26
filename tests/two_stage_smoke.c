#include "d3d9_null.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checked_uploads, checked_draws, checked_menu_draws;
static int menu_draw(void *ctx,const D9ColorVertex *v,size_t n,const D9Matrix *clip,
                     void *tex0,void *tex1,const D9DrawState *st,
                     const float (*constants)[4]){
    (void)ctx;(void)clip;
    assert(v&&n==3&&tex0&&tex1&&st&&st->pixel_shader);
    assert(st->pixel_shader->textures==2&&st->pixel_shader->uses_vertex_color);
    assert(constants&&constants[7][0]==0.25f&&constants[7][1]==0.5f);
    ++checked_menu_draws;
    return 0;
}
static D9TevProgram menu_plan(void){
    D9TevProgram p;memset(&p,0,sizeof p);
    p.version=0xFFFF0300u;p.stage_limit=D9TEV_MAX_STAGES;p.backend_flags=D9TEV_PLAN_MENU_FIXED;
    p.textures=2;p.texture[0]=(D9TevTexture){0,0,2};p.texture[1]=(D9TevTexture){0,1,2};
    p.inputs=1;p.input[0]=(D9TevInput){D9TEV_USAGE_COLOR,0,1,D9TEV_SRC_PRIMARY};
    p.uses_vertex_color=1;p.stages=2;
    D9TevStage *s=&p.stage[0];
    s->func_rgb=s->func_a=D9TEV_FUNC_MODULATE;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_TEX0;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_PRIMARY;
    s->op_rgb[0]=s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[0]=s->op_a[1]=D9TEV_A_ALPHA;
    s=&p.stage[1];
    s->func_rgb=s->func_a=D9TEV_FUNC_ADD;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_PREV;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_CONST;
    s->op_rgb[0]=s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[0]=s->op_a[1]=D9TEV_A_ALPHA;
    s->const_is_literal=0;s->const_reg=7;
    p.constants=1;p.constant_reg[0]=7;
    return p;
}
static int upload(void *ctx,void *texture,unsigned level,const void *data,size_t bytes){
    uint32_t logical[64];
    assert(level==0 && bytes==sizeof logical);
    assert(d9_unswizzle_rgba8(logical,32,data,8,8,D9_A8R8G8B8,0,0,8,8)==D9_OK);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)
        assert(logical[y*8+x]==(x<4?0xff000011u:0xff002200u)+checked_uploads);
    ++checked_uploads;
    return d9_null_backend()->texture_upload(ctx,texture,level,data,bytes);
}
static int draw(void *ctx,const D9ColorVertex *vertices,size_t n,const D9Matrix *clip,
                void *tex0,const D9DrawState *a,void *tex1,const D9DrawState *b){
    assert(tex0 && tex1 && tex0!=tex1 && a->address_u==1 && b->address_u==3);
    assert(b->stage_colorop==D9_TOP_MODULATE && b->stage_colorarg1==D9_TA_CURRENT);
    assert(b->stage_colorarg2==D9_TA_TEXTURE && b->resident_mips==1);
    ++checked_draws;
    return d9_null_backend()->draw_two(ctx,vertices,n,clip,tex0,a,tex1,b);
}
int main(void){
    D9Backend ops=*d9_null_backend();
    ops.draw_two=draw;ops.texture_upload=upload;ops.draw_menu=menu_draw;
    D9Device *d=d9_create_device(&ops,NULL,4096);
    assert(d && d9_stage_count(d)==2);
    D9Handle textures[2]={0,0};
    for(unsigned stage=0;stage<2;stage++){
        D9Handle tex;void *p;size_t pitch,gpu,guest;unsigned w,h;
        assert(!d9_create_texture_ex(d,2,1,D9_A8R8G8B8,1,D9_POOL_MANAGED,0,0,&tex));
        assert(!d9_texture_level_info(d,tex,0,&w,&h,&pitch,NULL) && w==2 && h==1 && pitch==8);
        assert(!d9_texture_resource_info(d,tex,NULL,NULL,NULL,NULL,&guest,&gpu,NULL,NULL));
        assert(guest==8 && gpu==256);
        assert(!d9_texture_lock_level(d,tex,0,&p,&pitch));
        uint32_t pixels[]={0xff000011u+stage,0xff002200u+stage};
        memcpy(p,pixels,sizeof pixels);
        assert(!d9_texture_unlock_level(d,tex,0));
        assert(!d9_bind_texture(d,stage,tex));
        textures[stage]=tex;
    }
    assert(d9_bind_texture(d,2,0)==D9_UNSUPPORTED);
    assert(!d9_set_unit_sampler(d,1,1,3));
    assert(d9_set_texture_stage_state(d,1,D9_TSS_COLOROP,999)==D9_UNSUPPORTED);
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_COLOROP,D9_TOP_MODULATE));
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_COLORARG1,D9_TA_CURRENT));
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_COLORARG2,D9_TA_TEXTURE));
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_ALPHAOP,D9_TOP_SELECTARG2));
    D9Vertex v[3]={{0,0,0,0,0},{1,0,0,1,0},{0,1,0,0,1}};
    assert(!d9_begin_scene(d,0));
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0])==D9_UNSUPPORTED);
    assert(!d9_set_texture_stage_state(d,1,D9_TSS_TEXCOORDINDEX,0));
    assert(!d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0]));
    assert(!d9_set_unit_sampler(d,1,5,2));
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0])==D9_UNSUPPORTED);
    assert(!d9_bind_texture(d,1,0));
    assert(d9_draw_primitive_up(d,D9_TRIANGLELIST,1,v,sizeof v[0])==D9_UNSUPPORTED);

    /* Menu TEV submission: two textures + color0 + external c7 through the
     * core's draw_menu route, not the fixed two-stage path. */
    assert(!d9_bind_texture(d,0,textures[0]));
    assert(!d9_bind_texture(d,1,textures[1]));
    assert(!d9_set_unit_sampler(d,1,5,D9_FILTER_POINT));
    D9TevProgram plan=menu_plan();
    assert(!d9_set_pixel_shader(d,&plan));
    D9VertexElement elems[3]={{0,0,D9_DECL_FLOAT3,0,D9_USAGE_POSITION,0},
        {0,12,D9_DECL_FLOAT2,0,D9_USAGE_TEXCOORD,0},
        {0,20,D9_DECL_COLOR,0,D9_USAGE_COLOR,0}};
    D9Decl decl;assert(!d9_decl_init(&decl,elems,3));
    assert(!d9_set_vertex_declaration(d,&decl));
    float bank[9][4]={{0}};bank[7][0]=0.25f;bank[7][1]=0.5f;bank[7][2]=0.75f;bank[7][3]=1.0f;
    assert(!d9_set_tev_constants(d,bank));
    D9ColorVertex cv[3]={{0,0,0,0,0,0.5f,0.5f,0.5f,1},
                         {1,0,0,1,0,0.5f,0.5f,0.5f,1},
                         {0,1,0,0,1,0.5f,0.5f,0.5f,1}};
    int menu_rc=d9_draw_primitive_up(d,D9_TRIANGLELIST,1,cv,sizeof cv[0]);
    if(menu_rc)fprintf(stderr,"menu draw rc=%d: %s\n",menu_rc,d9_last_draw_diagnostic(d));
    assert(!menu_rc);
    assert(checked_menu_draws==1);
    assert(!d9_present(d) && checked_draws==1 && checked_uploads==2);
    d9_destroy_device(d);
    puts("two-stage draw and padded texture: PASS");
}
