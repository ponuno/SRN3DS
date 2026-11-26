#include "d3d9_vertex.h"
#include <string.h>
#include <math.h>

_Static_assert(sizeof(D9VertexElement)==8,"D3DVERTEXELEMENT9 layout");
static const uint8_t sizes[]={4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
static const uint8_t components[]={1,2,3,4,4,4,2,4,4,2,4,2,4,3,3,2,4};
unsigned d9_decl_type_size(unsigned t){return t<sizeof sizes?sizes[t]:0;}
unsigned d9_decl_components(unsigned t){return t<sizeof components?components[t]:0;}
static unsigned rd16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static float f32(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static float half(unsigned h){uint32_t sign=(uint32_t)(h&0x8000)<<16,m=h&1023,e=(h>>10)&31;
    if(!e){if(!m)return f32(sign);unsigned exp=113;while(!(m&1024)){m<<=1;--exp;}return f32(sign|(exp<<23)|((m&1023)<<13));}
    if(e==31)return f32(sign|0x7f800000u|(m<<13));return f32(sign|((e+112)<<23)|((m&1023)<<13));}
static int signed16(unsigned x){return x&0x8000?(int)x-65536:(int)x;}
int d9_decl_decode(unsigned t,const void *data,size_t bytes,float out[4]){
    unsigned size=d9_decl_type_size(t),n=d9_decl_components(t);if(!size||!data||!out||bytes<size)return -1;
    const uint8_t *p=data;out[0]=out[1]=out[2]=0;out[3]=1;
    if(t<=D9_DECL_FLOAT4){for(unsigned i=0;i<n;++i)out[i]=f32(rd32(p+4*i));}
    else if(t==D9_DECL_COLOR){out[0]=p[2]/255.0f;out[1]=p[1]/255.0f;out[2]=p[0]/255.0f;out[3]=p[3]/255.0f;}
    else if(t==D9_DECL_UBYTE4||t==D9_DECL_UBYTE4N){for(unsigned i=0;i<4;++i)out[i]=t==D9_DECL_UBYTE4N?p[i]/255.0f:(float)p[i];}
    else if(t==D9_DECL_SHORT2||t==D9_DECL_SHORT4||t==D9_DECL_SHORT2N||t==D9_DECL_SHORT4N){
        for(unsigned i=0;i<n;++i){int v=signed16(rd16(p+2*i));out[i]=(t==D9_DECL_SHORT2N||t==D9_DECL_SHORT4N)?fmaxf(-1.0f,v/32767.0f):(float)v;}}
    else if(t==D9_DECL_USHORT2N||t==D9_DECL_USHORT4N){for(unsigned i=0;i<n;++i)out[i]=rd16(p+2*i)/65535.0f;}
    else if(t==D9_DECL_UDEC3||t==D9_DECL_DEC3N){uint32_t v=rd32(p);for(unsigned i=0;i<3;++i){unsigned x=(v>>(10*i))&1023;int s=x&512?(int)x-1024:(int)x;
        out[i]=t==D9_DECL_UDEC3?(float)x:fmaxf(-1.0f,s/511.0f);}}
    else if(t==D9_DECL_FLOAT16_2||t==D9_DECL_FLOAT16_4){for(unsigned i=0;i<n;++i)out[i]=half(rd16(p+2*i));}
    else return -1;return 0;
}
/* Colour must remain data until a colour-capable backend consumes it.  The
 * previous code rejected COLOR outright, which hid the reason behind a generic
 * declaration failure. */
static int color_type(unsigned t){return t==D9_DECL_COLOR||t==D9_DECL_UBYTE4||t==D9_DECL_UBYTE4N||t==D9_DECL_FLOAT4;}
int d9_decl_init(D9Decl *d,const D9VertexElement *e,unsigned n){
    if(!d||!e||!n||n>D9_MAX_DECL)return -1;
    D9Decl tmp={.count=n,.position=UINT32_MAX,.uv=UINT32_MAX,.color=UINT32_MAX,.normal=UINT32_MAX};
    unsigned position_seen=0,positiont_seen=0;
    for(unsigned i=0;i<n;++i){const D9VertexElement *x=e+i;unsigned size=d9_decl_type_size(x->type),end=(unsigned)x->offset+size;
        if(x->stream>=D9_MAX_STREAMS||!size||x->method||x->usage>13||x->usage_index>15||end>4096)return -1;
        if(i&&x->stream<e[i-1].stream)return -1;
        if(x->offset<tmp.required[x->stream])return -1;tmp.required[x->stream]=end;tmp.stream_mask|=1u<<x->stream;
        for(unsigned j=0;j<i;++j)if(e[j].usage==x->usage&&e[j].usage_index==x->usage_index)return -1;
        if(x->usage==D9_USAGE_POSITION&&!x->usage_index){if(d9_decl_components(x->type)<2)return -1;tmp.position=i;++position_seen;}
        if(x->usage==D9_USAGE_POSITIONT&&!x->usage_index){
            /* XYZRHW carries an explicit w; the current PICA pass-through
             * shader has a fixed w=1, so only FLOAT4 is honest here. */
            if(x->type!=D9_DECL_FLOAT4)return -1;tmp.position=i;tmp.position_t=1;++positiont_seen;}
        if(x->usage==D9_USAGE_TEXCOORD&&!x->usage_index){if(d9_decl_components(x->type)<2)return -1;tmp.uv=i;}
        if(x->usage==D9_USAGE_COLOR&&!x->usage_index){if(!color_type(x->type))return -1;tmp.color=i;}
        /* NORMAL is decoded only by the test-only CPU T&L prototype.  It is
         * admitted here so a declaration carrying it is not misreported as
         * malformed; an unlit draw still ignores it. */
        if(x->usage==D9_USAGE_NORMAL&&!x->usage_index){if(d9_decl_components(x->type)<3)return -1;tmp.normal=i;}
        tmp.elements[i]=*x;
    }
    if(tmp.position==UINT32_MAX||position_seen>1||positiont_seen>1||(position_seen&&positiont_seen))return -1;
    *d=tmp;return 0;
}
static int push_element(D9Decl *d,unsigned stream,unsigned type,unsigned usage,unsigned offset,const char **why){
    if(d->count>=4){if(why)*why="declaration exceeds four elements";return -1;}
    d->elements[d->count++]=(D9VertexElement){(uint16_t)stream,(uint16_t)offset,(uint8_t)type,0,(uint8_t)usage,0};
    return 0;
}
int d9_decl_from_fvf(unsigned fvf,int allow_normal,D9Decl *decl,const char **why){
    if(why)*why=NULL;
    if(!decl){if(why)*why="null declaration output";return D9_INVALID;}
    D9Decl d={0};
    unsigned offset=0;
    if(fvf&D9_FVF_XYZRHW){
        if(push_element(&d,0,D9_DECL_FLOAT4,D9_USAGE_POSITIONT,offset,why))return D9_INVALID;
        offset+=16;
    }else if(fvf&D9_FVF_XYZ){
        if(push_element(&d,0,D9_DECL_FLOAT3,D9_USAGE_POSITION,offset,why))return D9_INVALID;
        offset+=12;
    }else{
        if(why)*why="FVF has no XYZ/XYZRHW position";
        return D9_UNSUPPORTED;
    }
    if(fvf&D9_FVF_NORMAL){
        /* CPU T&L prototype only: the production path has no user-visible
         * normal semantics yet, so it must not admit the bit. */
        if(!allow_normal){
            if(why)*why="FVF contains NORMAL, which is unsupported without the CPU T&L prototype";
            return D9_UNSUPPORTED;
        }
        if(push_element(&d,0,D9_DECL_FLOAT3,D9_USAGE_NORMAL,offset,why))return D9_INVALID;
        offset+=12;
    }
    if(fvf&D9_FVF_DIFFUSE){
        if(push_element(&d,0,D9_DECL_COLOR,D9_USAGE_COLOR,offset,why))return D9_INVALID;
        offset+=4;
    }
    if(fvf&D9_FVF_TEX1){
        if(push_element(&d,0,D9_DECL_FLOAT2,D9_USAGE_TEXCOORD,offset,why))return D9_INVALID;
        offset+=8;
    }
    unsigned allowed=D9_FVF_XYZ|D9_FVF_XYZRHW|D9_FVF_DIFFUSE|D9_FVF_TEX1;
    if(allow_normal)allowed|=D9_FVF_NORMAL;
    if(fvf&~allowed){
        if(why)*why="FVF contains unsupported bits (specular/stencil/size or extra texcoords)";return D9_UNSUPPORTED;}
    if(d9_decl_init(decl,d.elements,d.count)){if(why)*why="FVF maps to a declaration the core cannot admit";return D9_UNSUPPORTED;}
    return D9_OK;
}
