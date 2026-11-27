#include "d3d9_bc.h"
#include "d3d9_core.h"
#include <string.h>

unsigned d9_bc_block_bytes(unsigned f){
    if(f==D9_DXT1||f==D9_ATI1||f==D9_BC4U)return 8;
    if(f==D9_DXT3||f==D9_DXT5||f==D9_ATI2||f==D9_BC5U)return 16;
    return 0;
}
unsigned d9_rgb24_pitch(unsigned w){return (w*3u+3u)&~3u;}
/* The 24-bit DDS pixel masks are R=0x00ff0000, G=0x0000ff00, B=0x000000ff, so
 * each pixel is three bytes in B,G,R order; the missing alpha is opaque.
 * A8B8G8R8 is the byte-swapped 32-bit layout: R,G,B,A in memory. */
static int unpack_rows(void *dst,size_t dst_bytes,const void *src,size_t src_pitch,
                       size_t src_bytes,unsigned w,unsigned h,unsigned bytes_per_pixel){
    if(!dst||!src||!w||!h||src_pitch<(size_t)w*bytes_per_pixel||
       dst_bytes<(size_t)w*h*4u||
       (h>1u&&src_pitch>(SIZE_MAX-(size_t)w*bytes_per_pixel)/(h-1u))||
       (size_t)(h-1u)*src_pitch+(size_t)w*bytes_per_pixel>src_bytes)return D9_INVALID;
    uint8_t *out=dst;const uint8_t *in=src;
    for(unsigned y=0;y<h;++y){
        for(unsigned x=0;x<w;++x){
            const uint8_t *s=in+(size_t)y*src_pitch+(size_t)x*bytes_per_pixel;
            uint8_t *d=out+((size_t)y*w+x)*4u;
            if(bytes_per_pixel==3u){d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=255;}
            else{d[0]=s[2];d[1]=s[1];d[2]=s[0];d[3]=s[3];}
        }
    }
    return D9_OK;
}
int d9_rgb24_to_bgra8(void *dst,size_t dst_bytes,const void *src,size_t src_pitch,
                      size_t src_bytes,unsigned w,unsigned h){
    return unpack_rows(dst,dst_bytes,src,src_pitch,src_bytes,w,h,3u);
}
int d9_abgr8_to_bgra8(void *dst,size_t dst_bytes,const void *src,size_t src_pitch,
                      size_t src_bytes,unsigned w,unsigned h){
    return unpack_rows(dst,dst_bytes,src,src_pitch,src_bytes,w,h,4u);
}
static unsigned rd16(const uint8_t *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void rgb565(uint8_t c[4],unsigned n){unsigned r=n>>11,g=(n>>5)&63,b=n&31;
    c[0]=(uint8_t)((r<<3)|(r>>2));c[1]=(uint8_t)((g<<2)|(g>>4));c[2]=(uint8_t)((b<<3)|(b>>2));c[3]=255;}
/* One BC4/BC3-alpha 8-byte block expanded to 16 unsigned bytes.  BC5 uses the
 * same two-mode palette for each of its two channels. */
static void bc4_block(const uint8_t *b,uint8_t out[16]){
    unsigned a0=b[0],a1=b[1];uint8_t a[8];a[0]=(uint8_t)a0;a[1]=(uint8_t)a1;
    if(a0>a1)for(unsigned i=1;i<=6;++i)a[i+1]=(uint8_t)(((7-i)*a0+i*a1)/7);
    else{for(unsigned i=1;i<=4;++i)a[i+1]=(uint8_t)(((5-i)*a0+i*a1)/5);a[6]=0;a[7]=255;}
    uint64_t bits=0;for(unsigned i=0;i<6;++i)bits|=(uint64_t)b[2+i]<<(8*i);
    for(unsigned i=0;i<16;++i)out[i]=a[(bits>>(3*i))&7];
}
int d9_bc_decode_block(unsigned f,const uint8_t *b,size_t bytes,uint8_t rgba[64]){
    unsigned len=d9_bc_block_bytes(f);if(!len||!b||!rgba||bytes<len)return D9_INVALID;
    if(f==D9_ATI2||f==D9_BC5U){
        /* BC5/ATI2 stores two independent BC4 channels: X in R, Y in G.  The
         * D3D9 decode rule leaves B at 0 and A at 255; normal-map shaders
         * reconstruct Z themselves, so replicating R into B would be wrong. */
        uint8_t x[16],y[16];bc4_block(b,x);bc4_block(b+8,y);
        for(unsigned i=0;i<16;++i){uint8_t *o=rgba+4*i;o[0]=x[i];o[1]=y[i];o[2]=0;o[3]=255;}
        return D9_OK;
    }
    if(f==D9_ATI1||f==D9_BC4U){
        /* BC4/ATI1 is one BC4 channel; D3D expands it to (R,0,0,1). */
        uint8_t r[16];bc4_block(b,r);
        for(unsigned i=0;i<16;++i){uint8_t *o=rgba+4*i;o[0]=r[i];o[1]=0;o[2]=0;o[3]=255;}
        return D9_OK;
    }
    const uint8_t *c=b+(f==D9_DXT1?0:8);unsigned c0=rd16(c),c1=rd16(c+2);uint8_t p[4][4];
    rgb565(p[0],c0);rgb565(p[1],c1);
    if(f!=D9_DXT1||c0>c1){for(unsigned k=0;k<3;++k){p[2][k]=(uint8_t)((2*p[0][k]+p[1][k])/3);p[3][k]=(uint8_t)((p[0][k]+2*p[1][k])/3);}p[2][3]=p[3][3]=255;}
    else{for(unsigned k=0;k<3;++k)p[2][k]=(uint8_t)((p[0][k]+p[1][k])/2);p[2][3]=255;memset(p[3],0,4);}
    uint8_t alpha[16];
    if(f==D9_DXT5)bc4_block(b,alpha);
    for(unsigned i=0;i<16;++i){unsigned index=(c[4+i/4]>>(2*(i%4)))&3;uint8_t *o=rgba+4*i;memcpy(o,p[index],4);
        if(f==D9_DXT3)o[3]=(uint8_t)(17*((b[i/2]>>(4*(i%2)))&15));
        else if(f==D9_DXT5)o[3]=alpha[i];}
    return D9_OK;
}
static int dim(unsigned n){return n>=8&&n<=1024&&!(n&(n-1));}
int d9_bc_to_pica(unsigned f,void *dst,size_t dst_bytes,const void *src,size_t src_bytes,unsigned w,unsigned h,size_t pitch){
    unsigned bb=d9_bc_block_bytes(f);if(!bb||!dim(w)||!dim(h)||!dst||!src||dst_bytes<(size_t)w*h*4)return D9_INVALID;
    size_t row=(size_t)(w/4)*bb,rows=h/4;if(pitch<row||pitch>(SIZE_MAX-row)/(rows-1)||(rows-1)*pitch+row>src_bytes)return D9_INVALID;
    uint8_t *out=dst;const uint8_t *in=src;uint8_t rgba[64];
    for(unsigned by=0;by<h/4;++by)for(unsigned bx=0;bx<w/4;++bx){
        if(d9_bc_decode_block(f,in+(size_t)by*pitch+bx*bb,bb,rgba))return D9_INVALID;
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){unsigned px=bx*4+x,py=h-1-(by*4+y),morton=0;
            for(unsigned k=0;k<3;++k){morton|=((px>>k)&1)<<(2*k);morton|=((py>>k)&1)<<(2*k+1);}
            size_t i=((size_t)(py/8)*(w/8)+px/8)*64+morton;uint8_t *d=out+i*4,*s=rgba+(y*4+x)*4;
            d[0]=s[3];d[1]=s[2];d[2]=s[1];d[3]=s[0];}
    }return D9_OK;
}
