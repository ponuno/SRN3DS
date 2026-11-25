#include "stretchrect_sw.h"
#include <stdio.h>

int main(void){
    D9SwCtx sw;assert(!d9sw_start(&sw,16,16,D9_A8R8G8B8));
    D9Device *d=d9_create_device(&d9sw_ops,&sw,1u<<20);assert(d);
    assert(d9_set_back_buffer_info(d,16,16,D9_A8R8G8B8)==D9_OK);
    unsigned w=0,h=0,fmt=0;assert(!d9_get_back_buffer_info(d,&w,&h,&fmt)&&w==16&&h==16&&fmt==D9_A8R8G8B8);
    assert(d9_set_back_buffer_info(d,0,16,D9_A8R8G8B8)==D9_INVALID);
    assert(d9_set_back_buffer_info(d,16,16,D9_DXT1)==D9_INVALID);

    D9Handle a=0,b=0,c8=0;
    assert(!d9_create_render_target(d,16,16,D9_A8R8G8B8,0,&a));D9SwTarget *ta=sw.last_created;
    assert(!d9_create_render_target(d,16,16,D9_A8R8G8B8,0,&b));
    assert(!d9_create_render_target(d,16,16,D9_X8R8G8B8,0,&c8));

    /* A is a solid clear, B a sentinel clear. */
    assert(!d9_set_render_target(d,a)&&!d9_begin_scene(d,0xff102030u)&&!d9_present(d));
    assert(!d9_set_render_target(d,b)&&!d9_begin_scene(d,0xff445566u)&&!d9_present(d));
    size_t used=d9_memory_used(d);

    /* Partial, 1:1 copy: the destination rect moves; nothing outside it moves. */
    D9Rect sr={4,4,12,12},dr={2,2,10,10};
    uint32_t out[16*16];
    assert(d9_blit(d,b,&dr,a,&sr,D9_FILTER_NONE)==D9_OK);
    assert(sw.blits==1&&sw.last_dx==2&&sw.last_dy==2&&sw.last_dw==8&&sw.last_dh==8);
    assert(sw.last_sx==4&&sw.last_sy==4&&sw.last_sw==8&&sw.last_sh==8&&sw.last_filter==D9_FILTER_NONE);
    assert(!d9_render_target_read(d,b,out,64,0,0,16,16));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
        uint32_t want=(x>=2&&x<10&&y>=2&&y<10)?0xff102030u:0xff445566u;
        d9sw_check_pixel(out,16,x,y,want);
    }
    assert(!d9_render_target_read(d,a,out,64,0,0,16,16)); /* source unchanged */
    for(unsigned i=0;i<16*16;++i)assert(out[i]==0xff102030u);

    /* Stretched point copy, full source into an 8x8 destination. */
    d9sw_fill(ta);D9Rect sfull={0,0,16,16},dhalf={0,0,8,8};
    assert(d9_blit(d,b,&dhalf,a,&sfull,D9_FILTER_POINT)==D9_OK);
    assert(sw.last_filter==D9_FILTER_POINT&&sw.last_dw==8&&sw.last_sw==16);
    assert(!d9_render_target_read(d,b,out,64,0,0,16,16));
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)d9sw_check_pixel(out,16,x,y,d9sw_pattern(x*2,y*2));
    assert(d9_blit(d,b,&dhalf,a,&sfull,D9_FILTER_LINEAR)==D9_OK);
    assert(sw.last_filter==D9_FILTER_LINEAR);

    /* Implicit back buffer as source and destination, with NULL = full surface. */
    d9sw_fill(&sw.back);
    assert(d9_blit(d,b,NULL,0,NULL,D9_FILTER_NONE)==D9_OK);
    assert(sw.last_dw==16&&sw.last_dh==16&&sw.last_dx==0&&sw.last_dy==0);
    assert(!d9_render_target_read(d,b,out,64,0,0,16,16));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)d9sw_check_pixel(out,16,x,y,sw.back.pixels[(size_t)y*16+x]);
    D9Rect backdst={4,4,8,8},backsrc={0,0,4,4};
    assert(d9_blit(d,0,&backdst,b,&backsrc,D9_FILTER_NONE)==D9_OK);
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
        assert(sw.back.pixels[(size_t)(4+y)*16+4+x]==sw.back.pixels[(size_t)y*16+x]);

    /* Inside a scene the backend is allowed to leave its native target on the
     * destination; d9_blit must restore the selected target. */
    assert(!d9_set_render_target(d,a)&&!d9_begin_scene(d,0));
    sw.bind_on_blit=1;
    assert(d9_blit(d,b,&dhalf,a,&sfull,D9_FILTER_POINT)==D9_OK);
    assert(sw.active==ta&&d9_get_render_target(d)==a); /* restored */
    sw.bind_on_blit=0;
    sw.fail_restore=1;
    assert(d9_blit(d,b,&dhalf,a,&sfull,D9_FILTER_POINT)==D9_BACKEND);
    assert(d9_blit_last_error(d)==D9_BLIT_BACKEND);
    sw.fail_restore=0;
    assert(!d9_present(d));

    /* Backend rejection codes survive to the caller unchanged. */
    const int codes[]={D9_BLIT_UNSUPPORTED,D9_BLIT_BACK_BUFFER_SRC,D9_BLIT_NO_SCENE,D9_BLIT_UNALIGNED,D9_BLIT_BACKEND};
    for(unsigned i=0;i<sizeof codes/sizeof *codes;++i){
        sw.reject_code=codes[i];
        assert(d9_blit(d,b,&dr,a,&sr,D9_FILTER_NONE)==D9_BACKEND);
        assert(d9_blit_last_error(d)==codes[i]);
    }
    sw.reject_code=0;

    /* Argument validation. */
    D9Rect empty={4,4,4,8},negative={-1,0,4,4},outside={0,0,17,8},huge={0,0,INT32_MAX,8};
    assert(d9_blit(NULL,b,&dr,a,&sr,D9_FILTER_NONE)==D9_INVALID);
    assert(d9_blit(d,999,&dr,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_ARGUMENT);
    assert(d9_blit(d,b,&dr,999,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_ARGUMENT);
    assert(d9_blit(d,a,&dr,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_SAME_SURFACE);
    assert(d9_blit(d,0,NULL,0,NULL,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_SAME_SURFACE);
    assert(d9_blit(d,b,&dr,c8,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_FORMAT);
    assert(d9_blit(d,b,&empty,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_RECT);
    assert(d9_blit(d,b,&negative,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_RECT);
    assert(d9_blit(d,b,&outside,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_RECT);
    assert(d9_blit(d,b,&huge,a,&sr,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_RECT);
    assert(d9_blit(d,b,&dr,a,&outside,D9_FILTER_NONE)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_RECT);
    assert(d9_blit(d,b,&dr,a,&sr,3)==D9_INVALID&&d9_blit_last_error(d)==D9_BLIT_ARGUMENT);
    assert(d9_memory_used(d)==used); /* no allocation in the copy path */

    /* A device without back-buffer metadata rejects any blit that names it. */
    D9Device *no_back=d9_create_device(&d9sw_ops,&sw,1u<<20);assert(no_back);
    D9Handle na=0,nb=0;
    assert(!d9_create_render_target(no_back,8,8,D9_A8R8G8B8,0,&na));
    assert(!d9_create_render_target(no_back,8,8,D9_A8R8G8B8,0,&nb));
    assert(d9_blit(no_back,nb,NULL,0,NULL,D9_FILTER_NONE)==D9_INVALID);
    assert(d9_blit_last_error(no_back)==D9_BLIT_ARGUMENT);
    assert(d9_blit(no_back,nb,NULL,na,NULL,D9_FILTER_NONE)==D9_OK);
    d9_destroy_device(no_back);

    /* A backend with no target_blit rejects explicitly, never silently. */
    D9Backend no_blit=d9sw_ops;no_blit.target_blit=NULL;
    D9Device *nb_dev=d9_create_device(&no_blit,&sw,1u<<20);assert(nb_dev);
    assert(!d9_set_back_buffer_info(nb_dev,16,16,D9_A8R8G8B8));
    D9Handle x=0,y=0;assert(!d9_create_render_target(nb_dev,16,16,D9_A8R8G8B8,0,&x));
    assert(!d9_create_render_target(nb_dev,16,16,D9_A8R8G8B8,0,&y));
    assert(d9_blit(nb_dev,y,NULL,x,NULL,D9_FILTER_NONE)==D9_BACKEND);
    assert(d9_blit_last_error(nb_dev)==D9_BLIT_UNSUPPORTED);
    d9_destroy_device(nb_dev);

    assert(!d9_release_render_target(d,a)&&!d9_release_render_target(d,b)&&!d9_release_render_target(d,c8));
    d9_destroy_device(d);
    d9sw_stop(&sw);
    puts("D3D9 StretchRect core: partial/stretched/back-buffer copies, restore, rejection codes and validation passed");
    return 0;
}
