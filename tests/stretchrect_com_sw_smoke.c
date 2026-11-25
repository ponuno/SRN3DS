#include "stretchrect_com_harness.h"
#include "stretchrect_sw.h"

static void software_path(void){
    D9SwCtx sw;assert(!d9sw_start(&sw,400,240,D9_X8R8G8B8));
    D9SrHarness h;assert(!d9sr_start(&h,&d9sw_ops,&sw,4u<<20,0));
    uint32_t device=d9sr_device(&h),back;
    uint32_t getrt[]={0,D9SR_OUT2};assert(d9sr_call(&h,device,38,getrt,2)==0);back=d9sr_rd(&h,D9SR_OUT2);assert(back);
    assert(d9sr_unref(&h,back)==1);
    uint32_t a=d9sr_create_target(&h,device,16,16,D9_X8R8G8B8,1);D9SwTarget *ta=sw.last_created;
    uint32_t b=d9sr_create_target(&h,device,16,16,D9_X8R8G8B8,1);D9SwTarget *tb=sw.last_created;

    /* Clear A red and B green through the real Clear/BeginScene path. */
    uint32_t setrt[]={0,a};assert(d9sr_call(&h,device,37,setrt,2)==0);
    uint32_t clear[]={0,0,1,0xff102030u,0,0};assert(d9sr_call(&h,device,43,clear,6)==0);
    assert(d9sr_call(&h,device,41,NULL,0)==0&&d9sr_call(&h,device,42,NULL,0)==0);
    uint32_t present[]={0,0,0,0};assert(d9sr_call(&h,device,17,present,4)==0);
    setrt[1]=b;assert(d9sr_call(&h,device,37,setrt,2)==0);
    clear[3]=0xff445566u;assert(d9sr_call(&h,device,43,clear,6)==0);
    assert(d9sr_call(&h,device,41,NULL,0)==0&&d9sr_call(&h,device,42,NULL,0)==0&&d9sr_call(&h,device,17,present,4)==0);
    setrt[1]=back;assert(d9sr_call(&h,device,37,setrt,2)==0);

    /* Partial A->B copy through COM, verified by locking B. */
    int32_t s1[]={4,4,12,12},d1[]={0,0,8,8};d9sr_wr(&h,D9SR_RECT1,s1,16);d9sr_wr(&h,D9SR_RECT2,d1,16);
    assert(d9sr_stretch(&h,device,a,D9SR_RECT1,b,D9SR_RECT2,D9_FILTER_NONE)==0);
    uint32_t lock[]={D9SR_OUT2,0,0x10};assert(d9sr_call(&h,b,13,lock,3)==0);
    uint32_t pitch=d9sr_rd(&h,D9SR_OUT2),ptr=d9sr_rd(&h,D9SR_OUT2+4);assert(pitch==64&&ptr);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
        uint32_t got=d9sr_rd(&h,ptr+(size_t)y*pitch+x*4);
        assert(got==((x<8&&y<8)?0xff102030u:0xff445566u));
    }
    assert(d9sr_call(&h,b,14,NULL,0)==0);

    /* A locked surface is rejected by name before the backend sees it. */
    assert(d9sr_call(&h,a,13,lock,3)==0);
    assert(d9sr_stretch(&h,device,a,0,b,0,D9_FILTER_NONE)==D9SR_INVALIDCALL);
    assert(strstr(d9sr_last_reject(),"locked"));
    assert(d9sr_call(&h,a,14,NULL,0)==0);
    assert(d9sr_stretch(&h,device,a,0,b,0,D9_FILTER_NONE)==0);

    /* Back-buffer restore: a copy to the back buffer leaves the selected
     * target (and the backend's native binding) on A. */
    setrt[1]=a;assert(d9sr_call(&h,device,37,setrt,2)==0);
    assert(d9sr_call(&h,device,41,NULL,0)==0);
    sw.bind_on_blit=1; /* pretend the backend changed its native binding */
    assert(d9sr_stretch(&h,device,b,0,back,0,D9_FILTER_NONE)==0);
    assert(sw.active==ta&&d9sr_call(&h,device,38,getrt,2)==0&&d9sr_rd(&h,D9SR_OUT2)==a);assert(d9sr_unref(&h,a)==2);
    sw.bind_on_blit=0;
    assert(d9sr_call(&h,device,42,NULL,0)==0&&d9sr_call(&h,device,17,present,4)==0);

    /* Inside-scene back-buffer -> explicit copy still works and restores A. */
    assert(d9sr_call(&h,device,37,setrt,2)==0&&d9sr_call(&h,device,41,NULL,0)==0);
    sw.bind_on_blit=1;
    assert(d9sr_stretch(&h,device,back,D9SR_RECT1,b,D9SR_RECT2,D9_FILTER_POINT)==0);
    assert(sw.active==ta&&d9sr_call(&h,device,38,getrt,2)==0&&d9sr_rd(&h,D9SR_OUT2)==a);assert(d9sr_unref(&h,a)==2);
    sw.bind_on_blit=0;
    assert(d9sr_call(&h,device,42,NULL,0)==0&&d9sr_call(&h,device,17,present,4)==0);

    /* A PICA-style rejection reaches COM as D3DERR_NOTAVAILABLE with the
     * backend's named reason, never as fake success. */
    sw.reject_code=D9_BLIT_BACK_BUFFER_SRC;
    assert(d9sr_stretch(&h,device,a,0,b,0,D9_FILTER_NONE)==D9SR_NOTAVAILABLE);
    assert(strstr(d9sr_last_reject(),"implicit back buffer"));
    sw.reject_code=0;
    assert(tb->w==16&&ta->w==16);
    d9sr_stop(&h);
    d9sw_stop(&sw);
}

int main(void){
    software_path();
    puts("D3D9 StretchRect COM(sw): partial copy pixels, locked-surface reject, in-scene restore and named backend rejection passed");
    return 0;
}
