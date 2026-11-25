#include "stretchrect_com_harness.h"

static void null_path(void){
    D9SrHarness h;assert(!d9sr_start(&h,NULL,NULL,0,1));
    uint32_t device=d9sr_device(&h),back;
    uint32_t getrt[]={0,D9SR_OUT2};assert(d9sr_call(&h,device,38,getrt,2)==0);back=d9sr_rd(&h,D9SR_OUT2);assert(back);
    assert(d9sr_unref(&h,back)==1);
    uint32_t a=d9sr_create_target(&h,device,16,16,D9_X8R8G8B8,0);
    uint32_t b=d9sr_create_target(&h,device,16,16,D9_X8R8G8B8,0);
    uint32_t other=d9sr_create_target(&h,device,16,16,D9_A8R8G8B8,0);
    D9NullStats st=*d9_null_stats();unsigned binds=st.target_binds;

    /* Full target -> back buffer stretches; NULL rects mean whole surfaces. */
    assert(d9sr_stretch(&h,device,a,0,back,0,D9_FILTER_NONE)==0);
    st=*d9_null_stats();
    assert(st.blits==1&&st.blit_pixels==400ull*240ull&&st.blit_stretched==1&&st.blit_linear==0);
    assert(st.last_blit_src_back==0&&st.last_blit_dst_back==1);
    assert(d9sr_last_reject()[0]==0);
    assert(d9sr_call(&h,device,38,getrt,2)==0&&d9sr_rd(&h,D9SR_OUT2)==back);assert(d9sr_unref(&h,back)==1);
    assert(d9_null_stats()->target_binds==binds); /* StretchRect did not rebind */

    /* Identical 8x8 rectangles with a partial back-buffer source. */
    int32_t r1[]={100,100,108,108},r2[]={0,0,8,8};d9sr_wr(&h,D9SR_RECT1,r1,16);d9sr_wr(&h,D9SR_RECT2,r2,16);
    assert(d9sr_stretch(&h,device,back,D9SR_RECT1,a,D9SR_RECT2,D9_FILTER_POINT)==0);
    st=*d9_null_stats();
    assert(st.blits==2&&st.blit_pixels==400ull*240ull+64ull&&st.blit_stretched==1&&st.last_blit_src_back==1);

    /* Selected explicit target survives a copy to the back buffer. */
    uint32_t setrt[]={0,a};assert(d9sr_call(&h,device,37,setrt,2)==0);
    assert(d9sr_stretch(&h,device,b,D9SR_RECT2,back,D9SR_RECT2,D9_FILTER_LINEAR)==0);
    assert(d9sr_call(&h,device,38,getrt,2)==0&&d9sr_rd(&h,D9SR_OUT2)==a);assert(d9sr_unref(&h,a)==2);
    assert(d9_null_stats()->target_binds==binds);
    st=*d9_null_stats();
    assert(st.blits==3&&st.blit_pixels==400ull*240ull+128ull&&st.blit_stretched==1&&st.blit_linear==1);

    /* Stretch with a linear filter. */
    int32_t r3[]={4,4,12,12},r4[]={0,0,16,16};d9sr_wr(&h,D9SR_RECT3,r3,16);d9sr_wr(&h,D9SR_RECT4,r4,16);
    assert(d9sr_stretch(&h,device,a,D9SR_RECT3,b,D9SR_RECT4,D9_FILTER_LINEAR)==0);
    st=*d9_null_stats();
    assert(st.blits==4&&st.blit_pixels==400ull*240ull+128ull+256ull&&st.blit_stretched==2&&st.blit_linear==2);
    assert(st.last_blit_dst_back==0&&st.last_blit_filter==D9_FILTER_LINEAR);

    /* Named, non-fatal argument rejects. */
    assert(d9sr_stretch(&h,device,a,D9SR_RECT3,b,D9SR_RECT4,3)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"filter")&&*d9sr_last_reject());
    assert(d9sr_stretch(&h,device,a,0,other,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"format"));
    assert(d9sr_stretch(&h,device,a,0,a,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"different surfaces"));
    uint32_t cpu=d9sr_create_cpu_surface(&h,device,8,8);
    assert(d9sr_stretch(&h,device,cpu,0,b,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"memory pool"));
    assert(d9sr_stretch(&h,device,a,0,cpu,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"destination surface"));
    int32_t badsrc[]={0,0,17,17},baddst[]={0,0,17,17};
    d9sr_wr(&h,D9SR_RECT1,badsrc,16);d9sr_wr(&h,D9SR_RECT2,baddst,16);
    assert(d9sr_stretch(&h,device,a,D9SR_RECT1,b,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"source rectangle"));
    assert(d9sr_stretch(&h,device,a,0,b,D9SR_RECT2,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"destination rectangle"));
    assert(d9sr_stretch(&h,device,a,0x20000000,b,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"outside guest memory"));
    assert(d9sr_stretch(&h,device,0,0,b,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"IDirect3DSurface9"));
    int32_t fullback[]={0,0,401,240};d9sr_wr(&h,D9SR_RECT1,fullback,16);
    assert(d9sr_stretch(&h,device,a,0,back,D9SR_RECT1,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"destination rectangle"));
    assert(d9_null_stats()->blits==4); /* all rejects happened before the backend */

    /* A successful call clears the previous rejection string. */
    uint32_t setrt0[]={0,0};assert(d9sr_call(&h,device,37,setrt0,2)==0);
    assert(d9sr_stretch(&h,device,b,0,back,0,D9_FILTER_NONE)==0&&d9sr_last_reject()[0]==0);
    assert(d9sr_unref(&h,other)==0&&d9sr_unref(&h,cpu)==0&&d9sr_unref(&h,b)==0);
    d9sr_stop(&h);
}

int main(void){
    null_path();
    puts("D3D9 StretchRect COM(null): slot 34 filters/formats/pools/rects rejected by name, partial+stretched copies counted, target binding preserved");
    return 0;
}
