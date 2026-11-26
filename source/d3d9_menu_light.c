#include "d3d9_menu_light.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ML_MAX_INS 32
#define ML_REG_TEMP 0
#define ML_REG_INPUT 1
#define ML_REG_CONST 2
#define ML_REG_COLOROUT 8
#define ML_REG_SAMPLER 10

#define ML_SW_IDENTITY 0xE4u
#define ML_SW_X 0x00u
#define ML_SW_Y 0x55u
#define ML_SW_Z 0xAAu
#define ML_SW_W 0xFFu
#define ML_SW_XZYW 0xD8u

typedef struct {
    unsigned op;
    uint32_t dst,src[3];
    unsigned nsrc;
    float literal[4];
} MlIns;

static unsigned ml_type(uint32_t t){return ((t>>28)&7u)|((t&0x1800u)>>8);}
static unsigned ml_reg(uint32_t t){return t&0x7FFu;}
static unsigned ml_mask(uint32_t t){return (t>>16)&0xFu;}
static unsigned ml_dstmod(uint32_t t){return (t>>20)&0xFu;}
static unsigned ml_swizzle(uint32_t t){return (t>>16)&0xFFu;}
static unsigned ml_srcmod(uint32_t t){return (t>>24)&0xFu;}

static void ml_reject(char *why,size_t cap,const char *text){
    if(why&&cap)snprintf(why,cap,"%s",text);
}

static int ml_parse(const uint32_t *code,size_t words,MlIns ins[ML_MAX_INS],
                    unsigned *count,char *why,size_t cap){
    if(!code||words<2||(code[0]>>16)!=0xFFFFu||((code[0]>>8)&255u)!=3u||(code[0]&255u)!=0u){
        ml_reject(why,cap,"not a ps_3_0 program");return -1;
    }
    size_t pos=1;unsigned n=0;
    while(pos<words){
        uint32_t token=code[pos],op=token&0xFFFFu;
        if(op==0xFFFFu){*count=n;return 0;}
        if(op==0xFFFEu){
            size_t skip=(token>>16)&0x7FFFu;
            if(skip>words-pos-1){ml_reject(why,cap,"truncated comment");return -1;}
            pos+=1+skip;continue;
        }
        size_t total=1u+((token>>24)&15u);
        if(total>words-pos){ml_reject(why,cap,"truncated instruction");return -1;}
        if(n>=ML_MAX_INS){ml_reject(why,cap,"too many instructions for menu-light signature");return -1;}
        MlIns *in=&ins[n++];memset(in,0,sizeof *in);in->op=op;
        const uint32_t *arg=code+pos+1;
        if(op==0x51u){
            if(total!=6){ml_reject(why,cap,"def does not have four literal operands");return -1;}
            in->dst=arg[0];memcpy(in->literal,arg+1,sizeof in->literal);
        }else if(op==0x1Fu){
            if(total!=3){ml_reject(why,cap,"dcl does not have two operands");return -1;}
            in->dst=arg[1];in->src[0]=arg[0];in->nsrc=1;
        }else{
            if(total<2){char b[96];snprintf(b,sizeof b,"instruction opcode 0x%02x at word %zu has no destination",(unsigned)op,pos);ml_reject(why,cap,b);return -1;}
            in->dst=arg[0];in->nsrc=(unsigned)total-2u;
            if(in->nsrc>3){ml_reject(why,cap,"instruction has more than three sources");return -1;}
            for(unsigned i=0;i<in->nsrc;++i)in->src[i]=arg[1+i];
        }
        pos+=total;
    }
    ml_reject(why,cap,"missing END token");return -1;
}

static int ml_dst(const MlIns *in,unsigned type,unsigned reg,unsigned mask,unsigned mod){
    return ml_type(in->dst)==type&&ml_reg(in->dst)==reg&&ml_mask(in->dst)==mask&&ml_dstmod(in->dst)==mod;
}
static int ml_src(const MlIns *in,unsigned slot,unsigned type,unsigned reg,unsigned sw,unsigned mod){
    return slot<in->nsrc&&ml_type(in->src[slot])==type&&ml_reg(in->src[slot])==reg&&
           ml_swizzle(in->src[slot])==sw&&ml_srcmod(in->src[slot])==mod;
}
static int ml_dcl(const MlIns *in,unsigned usage,unsigned usage_index,unsigned type,unsigned reg,unsigned tex_type){
    if(in->op!=0x1Fu)return 0;
    uint32_t decl=in->src[0];
    return (decl&0x1Fu)==usage&&((decl>>16)&0xFu)==usage_index&&
           ((decl>>27)&0xFu)==tex_type&&ml_type(in->dst)==type&&ml_reg(in->dst)==reg;
}

static int ml_expect_common(const MlIns *ins,unsigned n,unsigned *at,char *why,size_t cap){
    unsigned i=*at;
    if(n<24){ml_reject(why,cap,"program is too short for the base-lit menu signature");return -1;}
    if(ins[0].op!=0x51u||!ml_dst(&ins[0],ML_REG_CONST,8,0xF,0)){
        ml_reject(why,cap,"signature must begin with def c8");return -1;
    }
    if(!ml_dcl(&ins[1],5,0,ML_REG_INPUT,0,0)||
       !ml_dcl(&ins[2],10,0,ML_REG_INPUT,1,0)||
       !ml_dcl(&ins[3],10,1,ML_REG_INPUT,2,0)||
       !ml_dcl(&ins[4],0,0,ML_REG_SAMPLER,0,2)||
       !ml_dcl(&ins[5],0,0,ML_REG_SAMPLER,1,2)){
        ml_reject(why,cap,"dcl signature must be texcoord0 v0, color0 v1, color1 v2, 2d s0, 2d s1");return -1;
    }
    i=6;
#define NEED(condition,text) do{if(i>=n||!(condition)){ml_reject(why,cap,(text));return -1;}}while(0)
    NEED(ins[i].op==0x42&&ml_dst(&ins[i],ML_REG_TEMP,0,0xF,0)&&
         ml_src(&ins[i],0,ML_REG_INPUT,0,ML_SW_IDENTITY,0)&&
         ml_src(&ins[i],1,ML_REG_SAMPLER,0,ML_SW_IDENTITY,0),"texld diffuse mismatch");++i;
    NEED(ins[i].op==0x05&&ml_dst(&ins[i],ML_REG_TEMP,0,0x8,0)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_W,0)&&
         ml_src(&ins[i],1,ML_REG_CONST,5,ML_SW_Z,0),"material-alpha mul mismatch");++i;
    NEED(ins[i].op==0x05&&ml_dst(&ins[i],ML_REG_COLOROUT,0,0x8,2)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_W,0)&&
         ml_src(&ins[i],1,ML_REG_INPUT,1,ML_SW_W,0),"output-alpha mul mismatch");++i;
    NEED(ins[i].op==0x42&&ml_dst(&ins[i],ML_REG_TEMP,1,0xF,0)&&
         ml_src(&ins[i],0,ML_REG_INPUT,0,ML_SW_IDENTITY,0)&&
         ml_src(&ins[i],1,ML_REG_SAMPLER,1,ML_SW_IDENTITY,0),"texld normal mismatch");++i;
    *at=i;return 0;
#undef NEED
}

static int ml_expect_tail(const MlIns *ins,unsigned n,unsigned *at,char *why,size_t cap){
    unsigned i=*at;
#define NEED(condition,text) do{if(i>=n||!(condition)){ml_reject(why,cap,(text));return -1;}}while(0)
    NEED(ins[i].op==0x09&&ml_dst(&ins[i],ML_REG_TEMP,2,0x1,0)&&
         ml_src(&ins[i],0,ML_REG_CONST,0,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,1,ML_SW_IDENTITY,0),
         "dp4 basis-row-0 mismatch");++i;
    NEED(ins[i].op==0x09&&ml_dst(&ins[i],ML_REG_TEMP,2,0x2,0)&&
         ml_src(&ins[i],0,ML_REG_CONST,1,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,1,ML_SW_IDENTITY,0),
         "dp4 basis-row-1 mismatch");++i;
    NEED(ins[i].op==0x09&&ml_dst(&ins[i],ML_REG_TEMP,2,0x4,0)&&
         ml_src(&ins[i],0,ML_REG_CONST,2,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,1,ML_SW_IDENTITY,0),
         "dp4 basis-row-2 mismatch");++i;
    NEED(ins[i].op==0x08&&ml_dst(&ins[i],ML_REG_TEMP,0,0x8,1)&&
         ml_src(&ins[i],0,ML_REG_TEMP,1,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_CONST,3,ML_SW_IDENTITY,0),
         "dp3 light-direction mismatch");++i;
    NEED(ins[i].op==0x02&&ml_dst(&ins[i],ML_REG_TEMP,1,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_TEMP,2,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_CONST,6,ML_SW_IDENTITY,0),
         "emissive add mismatch");++i;
    NEED(ins[i].op==0x04&&ml_dst(&ins[i],ML_REG_TEMP,1,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_CONST,4,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,0,ML_SW_W,0)&&
         ml_src(&ins[i],2,ML_REG_TEMP,1,ML_SW_IDENTITY,0),"directional-light mad mismatch");++i;
    NEED(ins[i].op==0x05&&ml_dst(&ins[i],ML_REG_TEMP,0,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,1,ML_SW_IDENTITY,0),
         "diffuse-lighting multiply mismatch");++i;
    NEED(ins[i].op==0x05&&ml_dst(&ins[i],ML_REG_TEMP,1,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_INPUT,1,ML_SW_IDENTITY,0),
         "vertex-colour multiply mismatch");++i;
    NEED(i+2<=n&&ins[i].op==0x01&&ml_dst(&ins[i],ML_REG_TEMP,2,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_INPUT,1,ML_SW_IDENTITY,0),"vertex-colour copy mismatch");++i;
    NEED(ins[i].op==0x04&&ml_dst(&ins[i],ML_REG_TEMP,0,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_TEMP,2,ML_SW_IDENTITY,1)&&
         ml_src(&ins[i],2,ML_REG_INPUT,2,ML_SW_IDENTITY,0),"secondary-colour subtract mismatch");++i;
    NEED(ins[i].op==0x04&&ml_dst(&ins[i],ML_REG_TEMP,0,0x7,0)&&
         ml_src(&ins[i],0,ML_REG_INPUT,2,ML_SW_W,0)&&ml_src(&ins[i],1,ML_REG_TEMP,0,ML_SW_IDENTITY,0)&&
         ml_src(&ins[i],2,ML_REG_TEMP,1,ML_SW_IDENTITY,0),"secondary-colour blend mismatch");++i;
    NEED(ins[i].op==0x05&&ml_dst(&ins[i],ML_REG_COLOROUT,0,0x7,2)&&
         ml_src(&ins[i],0,ML_REG_TEMP,0,ML_SW_IDENTITY,0)&&ml_src(&ins[i],1,ML_REG_CONST,7,ML_SW_W,0),
         "final output multiply mismatch");++i;
    *at=i;return 0;
#undef NEED
}

int d9_menu_light_match(const uint32_t *code,size_t words,D9MenuLightMatch *out,
                        char *why,size_t why_size){
    if(out)memset(out,0,sizeof *out);
    if(why&&why_size)why[0]=0;
    MlIns ins[ML_MAX_INS];unsigned n=0,at=0;
    if(ml_parse(code,words,ins,&n,why,why_size)<0)return 0;
    if(ml_expect_common(ins,n,&at,why,why_size)<0)return 0;

    int signature=D9_MENU_LIGHT_NONE;
    if(n==24&&ins[at].op==0x04&&ml_dst(&ins[at],ML_REG_TEMP,1,0x7,0)&&
       ml_src(&ins[at],0,ML_REG_TEMP,1,ML_SW_XZYW,0)&&
       ml_src(&ins[at],1,ML_REG_CONST,8,ML_SW_X,0)&&ml_src(&ins[at],2,ML_REG_CONST,8,ML_SW_Y,0)){
        signature=D9_MENU_LIGHT_P5_MAD_NORMAL;++at;
        if(!(at<n&&ins[at].op==0x01&&ml_dst(&ins[at],ML_REG_TEMP,1,0x8,0)&&
             ml_src(&ins[at],0,ML_REG_CONST,8,ML_SW_Z,0))){
            ml_reject(why,why_size,"normal.w mov mismatch");return 0;
        }
        ++at;
    }else if(n==25&&ins[at].op==0x02&&ml_dst(&ins[at],ML_REG_TEMP,1,0x7,0)&&
             ml_src(&ins[at],0,ML_REG_TEMP,1,ML_SW_XZYW,0)&&ml_src(&ins[at],1,ML_REG_CONST,8,ML_SW_X,0)){
        signature=D9_MENU_LIGHT_P1200005_ADD_NORMAL;++at;
        if(!(at<n&&ins[at].op==0x02&&ml_dst(&ins[at],ML_REG_TEMP,1,0x7,0)&&
             ml_src(&ins[at],0,ML_REG_TEMP,1,ML_SW_IDENTITY,0)&&ml_src(&ins[at],1,ML_REG_TEMP,1,ML_SW_IDENTITY,0))){
            ml_reject(why,why_size,"normal double-add mismatch");return 0;
        }
        ++at;
        if(!(at<n&&ins[at].op==0x01&&ml_dst(&ins[at],ML_REG_TEMP,1,0x8,0)&&
             ml_src(&ins[at],0,ML_REG_CONST,8,ML_SW_Y,0))){
            ml_reject(why,why_size,"normal.w mov mismatch");return 0;
        }
        ++at;
    }else{
        ml_reject(why,why_size,"normal-decode signature is neither p5 MAD nor p1200005 ADD");return 0;
    }
    if(ml_expect_tail(ins,n,&at,why,why_size)<0)return 0;
    if(at!=n){ml_reject(why,why_size,"trailing instructions after final oC0 write");return 0;}
    if(signature==D9_MENU_LIGHT_P5_MAD_NORMAL){
        if(ins[0].literal[0]!=2.0f||ins[0].literal[1]!=-1.0f||ins[0].literal[2]!=1.0f||ins[0].literal[3]!=0.0f){
            ml_reject(why,why_size,"def c8 must be {2,-1,1,0} for p5 MAD");return 0;
        }
    }else{
        if(ins[0].literal[0]!=-0.5f||ins[0].literal[1]!=1.0f||ins[0].literal[2]!=0.0f||ins[0].literal[3]!=0.0f){
            ml_reject(why,why_size,"def c8 must be {-0.5,1,0,0} for p1200005 ADD");return 0;
        }
    }
    if(out){
        out->signature=signature;out->instructions=n;memcpy(out->normal_decode,ins[0].literal,sizeof out->normal_decode);
    }
    return 1;
}

const char *d9_menu_light_signature_name(int signature){
    switch(signature){
    case D9_MENU_LIGHT_P5_MAD_NORMAL:return "p5 MAD normal (840 B)";
    case D9_MENU_LIGHT_P1200005_ADD_NORMAL:return "p1200005 ADD normal (852 B)";
    default:return "none";
    }
}

static float ml_clamp(float v){if(!(v>0.0f))return 0.0f;if(v>1.0f)return 1.0f;return v;}
static float ml_dot4(const float a[4],const float b[4]){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];}

int d9_menu_light_eval_exact(const D9MenuLightMatch *signature,
                             const D9MenuLightInputs *in,D9MenuLightResult *out){
    if(!signature||!in||!out||signature->signature==D9_MENU_LIGHT_NONE)return -1;
    memset(out,0,sizeof *out);
    float diffuse[4];memcpy(diffuse,in->diffuse,sizeof diffuse);
    diffuse[3]*=in->c5[2];
    float alpha=diffuse[3]*in->vertex_color[3];
    float normal[4]={in->normal_raw[0],in->normal_raw[2],in->normal_raw[1],1.0f};
    if(signature->signature==D9_MENU_LIGHT_P5_MAD_NORMAL){
        for(int i=0;i<3;++i)normal[i]=normal[i]*signature->normal_decode[0]+signature->normal_decode[1];
    }else{
        for(int i=0;i<3;++i)normal[i]=(normal[i]+signature->normal_decode[0])*2.0f;
    }
    float transformed[3]={ml_dot4(in->c0,normal),ml_dot4(in->c1,normal),ml_dot4(in->c2,normal)};
    float ndotl=ml_clamp(normal[0]*in->c3[0]+normal[1]*in->c3[1]+normal[2]*in->c3[2]);
    float light[3];
    for(int i=0;i<3;++i)light[i]=in->c4[i]*ndotl+(transformed[i]+in->c6[i]);
    float color[3];for(int i=0;i<3;++i)color[i]=diffuse[i]*light[i];
    float vertex_color[3];for(int i=0;i<3;++i)vertex_color[i]=color[i]*in->vertex_color[i];
    float secondary[3];for(int i=0;i<3;++i)secondary[i]=in->secondary_color[i]-color[i]*in->vertex_color[i];
    for(int i=0;i<3;++i)secondary[i]=in->secondary_color[3]*secondary[i]+vertex_color[i];
    out->rgba[0]=ml_clamp(secondary[0]*in->c7[3]);
    out->rgba[1]=ml_clamp(secondary[1]*in->c7[3]);
    out->rgba[2]=ml_clamp(secondary[2]*in->c7[3]);
    out->rgba[3]=ml_clamp(alpha);
    out->ndotl=ndotl;memcpy(out->transformed_normal,transformed,sizeof transformed);
    memcpy(out->lighting,light,sizeof light);memcpy(out->vertex_modulated,vertex_color,sizeof vertex_color);
    return 0;
}

static uint32_t ml_pack(const float v[4]){return d9_tev_pack_rgba8(v);}
static void ml_unpack(uint32_t packed,float v[4]){
    for(int i=0;i<4;++i)v[i]=(float)((packed>>(8*i))&0xFFu)/255.0f;
}

int d9_menu_light_plan(const D9MenuLightMatch *signature,D9TevProgram *out,
                       char *why,size_t why_size){
    if(!signature||!out||signature->signature==D9_MENU_LIGHT_NONE){
        if(why&&why_size)snprintf(why,why_size,"no accepted menu-light signature");
        return -1;
    }
    memset(out,0,sizeof *out);out->version=0xFFFF0300u;out->stage_limit=D9TEV_MAX_STAGES;
    out->backend_flags=D9TEV_PLAN_MENU_LIGHT;
    out->instructions=signature->instructions;out->textures=2;
    out->texture[0].texcoord_reg=0;out->texture[0].sampler_reg=0;out->texture[0].sampler_type=2;
    out->texture[1].texcoord_reg=0;out->texture[1].sampler_reg=1;out->texture[1].sampler_type=2;
    out->inputs=2;
    out->input[0].usage=D9TEV_USAGE_COLOR;out->input[0].usage_index=0;out->input[0].reg_index=1;out->input[0].tev_source=D9TEV_SRC_PRIMARY;
    out->input[1].usage=D9TEV_USAGE_COLOR;out->input[1].usage_index=1;out->input[1].reg_index=2;out->input[1].tev_source=D9TEV_SRC_FRAG_PRIMARY;
    out->uses_vertex_color=1;

    D9TevStage *s;
    /* 0: NdotL = dot3(pre-baked normal, c3). */
    s=&out->stage[out->stages++];s->func_rgb=s->func_a=D9TEV_FUNC_DOT3_RGBA;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_TEX1;s->op_rgb[0]=s->op_a[0]=D9TEV_RGB_COLOR;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_CONST;s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[1]=D9TEV_A_ALPHA;
    s->const_reg=3;s->const_is_literal=0;
    /* 1: light = NdotL*c4 + (ambient+emissive) varying. */
    s=&out->stage[out->stages++];s->func_rgb=s->func_a=D9TEV_FUNC_MULTIPLY_ADD;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_PREV;s->op_rgb[0]=D9TEV_RGB_COLOR;s->op_a[0]=D9TEV_A_ALPHA;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_CONST;s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[1]=D9TEV_A_ALPHA;
    s->src_rgb[2]=s->src_a[2]=D9TEV_SRC_FRAG_PRIMARY;s->op_rgb[2]=D9TEV_RGB_COLOR;s->op_a[2]=D9TEV_A_ALPHA;
    s->const_reg=4;s->const_is_literal=0;
    /* 2: rgb = diffuse*light; alpha = diffuse.a*vertex.a. */
    s=&out->stage[out->stages++];s->func_rgb=s->func_a=D9TEV_FUNC_MODULATE;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_TEX0;s->op_rgb[0]=D9TEV_RGB_COLOR;s->op_a[0]=D9TEV_A_ALPHA;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_PREV;s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[1]=D9TEV_A_ALPHA;
    /* Alpha needs a different second source; encode it with the alpha source
     * slots while RGB keeps PREV. */
    s->src_a[1]=D9TEV_SRC_PRIMARY;s->op_a[1]=D9TEV_A_ALPHA;
    /* 3: rgb *= vertex.rgb; alpha *= c4.a (material alpha). */
    s=&out->stage[out->stages++];s->func_rgb=s->func_a=D9TEV_FUNC_MODULATE;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_PREV;s->op_rgb[0]=D9TEV_RGB_COLOR;s->op_a[0]=D9TEV_A_ALPHA;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_PRIMARY;s->op_rgb[1]=D9TEV_RGB_COLOR;s->op_a[1]=D9TEV_A_ALPHA;
    s->src_a[1]=D9TEV_SRC_CONST;s->op_a[1]=D9TEV_A_ALPHA;
    s->const_reg=4;s->const_is_literal=0;
    /* 4: rgb *= c7.a; alpha passes through.  This matches the retail final
     * secondary-colour multiply; c7.rgb is not used by that evaluator. */
    s=&out->stage[out->stages++];s->func_rgb=D9TEV_FUNC_MODULATE;s->func_a=D9TEV_FUNC_REPLACE;
    s->src_rgb[0]=s->src_a[0]=D9TEV_SRC_PREV;s->op_rgb[0]=D9TEV_RGB_COLOR;s->op_a[0]=D9TEV_A_ALPHA;
    s->src_rgb[1]=s->src_a[1]=D9TEV_SRC_CONST;s->op_rgb[1]=D9TEV_RGB_ALPHA;s->op_a[1]=D9TEV_A_ALPHA;
    s->src_rgb[2]=s->src_a[2]=D9TEV_SRC_PREV;s->op_rgb[2]=D9TEV_RGB_COLOR;s->op_a[2]=D9TEV_A_ALPHA;
    s->const_reg=7;s->const_is_literal=0;
    out->constants=3;out->constant_reg[0]=3;out->constant_reg[1]=4;out->constant_reg[2]=7;
    return 0;
}

int d9_menu_light_eval_plan(const D9TevProgram *plan,
                            const D9MenuLightInputs *in,D9MenuLightResult *out){
    if(!plan||!in||!out||plan->stages!=5)return -1;
    memset(out,0,sizeof *out);
    float c3[4],c4[4],c7[4];
    ml_unpack(ml_pack(in->c3),c3);ml_unpack(ml_pack(in->c4),c4);ml_unpack(ml_pack(in->c7),c7);
    float ndotl=ml_clamp(in->normal_raw[0]*c3[0]+in->normal_raw[1]*c3[1]+in->normal_raw[2]*c3[2]);
    float light[3];for(int i=0;i<3;++i)light[i]=ml_clamp(ndotl*c4[i]+in->secondary_color[i]);
    float color[3];for(int i=0;i<3;++i)color[i]=ml_clamp(in->diffuse[i]*light[i]);
    float alpha=ml_clamp(in->diffuse[3]*in->vertex_color[3]);
    for(int i=0;i<3;++i)color[i]=ml_clamp(color[i]*in->vertex_color[i]);
    alpha=ml_clamp(alpha*c4[3]);
    memcpy(out->vertex_modulated,color,sizeof color);
    for(int i=0;i<3;++i)color[i]=ml_clamp(color[i]*c7[3]);
    memcpy(out->rgba,color,sizeof color);out->rgba[3]=alpha;
    out->ndotl=ndotl;memcpy(out->lighting,light,sizeof light);
    return 0;
}
