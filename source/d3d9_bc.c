#include "d3d9_bc.h"
#include "d3d9_core.h"
#include <string.h>

unsigned d9_bc_block_bytes(unsigned f){return f==D9_DXT1?8:f==D9_DXT3||f==D9_DXT5?16:0;}
static unsigned rd16(const uint8_t *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void rgb565(uint8_t c[4],unsigned n){unsigned r=n>>11,g=(n>>5)&63,b=n&31;
    c[0]=(uint8_t)((r<<3)|(r>>2));c[1]=(uint8_t)((g<<2)|(g>>4));c[2]=(uint8_t)((b<<3)|(b>>2));c[3]=255;}
int d9_bc_decode_block(unsigned f,const uint8_t *b,size_t bytes,uint8_t rgba[64]){
    unsigned len=d9_bc_block_bytes(f);if(!len||!b||!rgba||bytes<len)return D9_INVALID;
    const uint8_t *c=b+(f==D9_DXT1?0:8);unsigned c0=rd16(c),c1=rd16(c+2);uint8_t p[4][4];
    rgb565(p[0],c0);rgb565(p[1],c1);
    if(f!=D9_DXT1||c0>c1){for(unsigned k=0;k<3;++k){p[2][k]=(uint8_t)((2*p[0][k]+p[1][k])/3);p[3][k]=(uint8_t)((p[0][k]+2*p[1][k])/3);}p[2][3]=p[3][3]=255;}
    else{for(unsigned k=0;k<3;++k)p[2][k]=(uint8_t)((p[0][k]+p[1][k])/2);p[2][3]=255;memset(p[3],0,4);}
    uint8_t alpha[8];uint64_t abits=0;
    if(f==D9_DXT5){alpha[0]=b[0];alpha[1]=b[1];if(alpha[0]>alpha[1])for(unsigned i=1;i<=6;++i)alpha[i+1]=(uint8_t)(((7-i)*alpha[0]+i*alpha[1])/7);
        else{for(unsigned i=1;i<=4;++i)alpha[i+1]=(uint8_t)(((5-i)*alpha[0]+i*alpha[1])/5);alpha[6]=0;alpha[7]=255;}
        for(unsigned i=0;i<6;++i)abits|=(uint64_t)b[2+i]<<(8*i);}
    for(unsigned i=0;i<16;++i){unsigned index=(c[4+i/4]>>(2*(i%4)))&3;uint8_t *o=rgba+4*i;memcpy(o,p[index],4);
        if(f==D9_DXT3)o[3]=(uint8_t)(17*((b[i/2]>>(4*(i%2)))&15));
        else if(f==D9_DXT5)o[3]=alpha[(abits>>(3*i))&7];}
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
