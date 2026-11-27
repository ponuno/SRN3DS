#include "d3d9_vertex.h"
#include <stdio.h>
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
/* Colour must remain data until a colour-capable backend consumes it.
 * Four-component forms the decoder produces as RGBA; FLOAT16_4 is the retail
 * menu declaration (the 8 measured CreateVertexDeclaration refusals): a
 * half-float RGBA colour that d9_decl_decode already yields like FLOAT4.
 * Three-component forms remain outside the admitted tint contract. */
static int color_type(unsigned t){return t==D9_DECL_COLOR||t==D9_DECL_UBYTE4||
    t==D9_DECL_UBYTE4N||t==D9_DECL_FLOAT4||t==D9_DECL_FLOAT16_4;}
static const char *decl_type_name(unsigned t){
    static const char *names[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR","UBYTE4",
        "SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N","USHORT4N",
        "UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
    return t<sizeof names/sizeof *names?names[t]:"type?";
}
static const char *decl_usage_name(unsigned u){
    static const char *names[]={"position","blendweight","blendindices","normal",
        "psize","texcoord","tangent","binormal","tessfactor","positiont","color",
        "fog","depth","sample"};
    return u<sizeof names/sizeof *names?names[u]:"usage?";
}
#define DECL_REJECT(...) do{if(why&&cap)snprintf(why,cap,__VA_ARGS__);return -1;}while(0)
int d9_decl_init_why(D9Decl *d,const D9VertexElement *e,unsigned n,char *why,size_t cap){
    if(why&&cap)why[0]=0;
    if(!d||!e||!n||n>D9_MAX_DECL)
        DECL_REJECT("declaration needs 1..%u elements",D9_MAX_DECL);
    D9Decl tmp={.count=n,.position=UINT32_MAX,.uv=UINT32_MAX,.color=UINT32_MAX,.normal=UINT32_MAX,.weight=UINT32_MAX,.index=UINT32_MAX};
    unsigned position_seen=0,positiont_seen=0;
    for(unsigned i=0;i<n;++i){const D9VertexElement *x=e+i;unsigned size=d9_decl_type_size(x->type),end=(unsigned)x->offset+size;
        if(x->stream>=D9_MAX_STREAMS)
            DECL_REJECT("element %u stream %u is outside 0..%u",i,x->stream,D9_MAX_STREAMS-1);
        if(!size)
            DECL_REJECT("element %u type %u is not a D3D9 declaration type",i,x->type);
        if(x->method)
            DECL_REJECT("element %u method %u (tessellator) is not supported",i,x->method);
        if(x->usage>13||x->usage_index>15)
            DECL_REJECT("element %u usage %u index %u is out of range",i,x->usage,x->usage_index);
        if(end>4096)
            DECL_REJECT("element %u offset %u + %u bytes exceeds 4096",i,x->offset,size);
        if(i&&x->stream<e[i-1].stream)
            DECL_REJECT("element %u stream %u is out of order after stream %u",i,x->stream,e[i-1].stream);
        if(x->offset<tmp.required[x->stream])
            DECL_REJECT("element %u offset %u overlaps the previous element in stream %u (needs >= %u)",
                        i,x->offset,x->stream,tmp.required[x->stream]);
        tmp.required[x->stream]=end;tmp.stream_mask|=1u<<x->stream;
        for(unsigned j=0;j<i;++j)if(e[j].usage==x->usage&&e[j].usage_index==x->usage_index)
            DECL_REJECT("element %u duplicates %s%u",i,decl_usage_name(x->usage),x->usage_index);
        if(x->usage==D9_USAGE_POSITION&&!x->usage_index){if(d9_decl_components(x->type)<2)
            DECL_REJECT("element %u position0 %s has fewer than 2 components",i,decl_type_name(x->type));
            tmp.position=i;++position_seen;}
        if(x->usage==D9_USAGE_POSITIONT&&!x->usage_index){
            /* XYZRHW carries an explicit w; the current PICA pass-through
             * shader has a fixed w=1, so only FLOAT4 is honest here. */
            if(x->type!=D9_DECL_FLOAT4)
                DECL_REJECT("element %u positiont0 is %s; only FLOAT4 carries the explicit w",
                            i,decl_type_name(x->type));
            tmp.position=i;tmp.position_t=1;++positiont_seen;}
        if(x->usage==D9_USAGE_TEXCOORD&&!x->usage_index){if(d9_decl_components(x->type)<2)
            DECL_REJECT("element %u texcoord0 %s has fewer than 2 components",i,decl_type_name(x->type));
            tmp.uv=i;}
        if(x->usage==D9_USAGE_COLOR&&!x->usage_index){
            /* 4-component RGBA forms only (measured retail FLOAT16_4 included). */
            if(!color_type(x->type))
                DECL_REJECT("element %u color0 type %s is not COLOR/UBYTE4/UBYTE4N/FLOAT4/FLOAT16_4",
                            i,decl_type_name(x->type));
            tmp.color=i;}
        /* NORMAL is decoded only by the test-only CPU T&L prototype.  It is
         * admitted here so a declaration carrying it is not misreported as
         * malformed; an unlit draw still ignores it. */
        if(x->usage==D9_USAGE_NORMAL&&!x->usage_index){if(d9_decl_components(x->type)<3)
            DECL_REJECT("element %u normal0 %s has fewer than 3 components",i,decl_type_name(x->type));
            tmp.normal=i;}
        /* Skinning inputs.  The generic draw path ignores them; the verified
         * PICA VS gate requires exactly FLOAT4/UBYTE4N.  Admit the element so a
         * skinned declaration is not misreported as malformed. */
        if(x->usage==D9_USAGE_BLENDWEIGHT&&!x->usage_index)tmp.weight=i;
        if(x->usage==D9_USAGE_BLENDINDICES&&!x->usage_index)tmp.index=i;
        tmp.elements[i]=*x;
    }
    if(tmp.position==UINT32_MAX)
        DECL_REJECT("no POSITION or POSITIONT element in the declaration");
    if(position_seen>1||positiont_seen>1||(position_seen&&positiont_seen))
        DECL_REJECT("declaration carries more than one POSITION/POSITIONT element");
    *d=tmp;return 0;
}
int d9_decl_init(D9Decl *d,const D9VertexElement *e,unsigned n){
    return d9_decl_init_why(d,e,n,NULL,0);
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
