/* CPU-reference tests for the pre-specialised simple-lit menu/logo family.
 * Signature streams are assembled from semantic fields; no retail bytecode is
 * embedded.  The independent D3D interpreter below executes the assembled
 * tokens and is the reference for d9_menu_light_eval_exact.
 */

#include "d3d9_menu_light.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DI(op, operands) ((uint32_t)(op) | ((uint32_t)(operands) << 24))
#define SW_ID 0xE4u
#define SW_X  0x00u
#define SW_Y  0x55u
#define SW_Z  0xAAu
#define SW_W  0xFFu
#define SW_XZYW 0xD8u

static uint32_t dr(unsigned type,unsigned reg,unsigned mask,unsigned mod){
    return 0x80000000u|((type&7u)<<28)|((type&0x18u)<<8)|(reg&0x7FFu)|((mask&0xFu)<<16)|((mod&0xFu)<<20);
}
static uint32_t sr(unsigned type,unsigned reg,unsigned sw,unsigned mod){
    return 0x80000000u|((type&7u)<<28)|((type&0x18u)<<8)|(reg&0x7FFu)|((sw&0xFFu)<<16)|((mod&0xFu)<<24);
}
static uint32_t fbits(float f){union{float f;uint32_t u;}x;x.f=f;return x.u;}

static void put_alu(uint32_t *c,size_t *n,unsigned op,uint32_t dst,
                    uint32_t a,uint32_t b,uint32_t d,unsigned sources){
    c[(*n)++]=DI(op,1+sources);c[(*n)++]=dst;c[(*n)++]=a;
    if(sources>1)c[(*n)++]=b;if(sources>2)c[(*n)++]=d;
}
static void put_texld(uint32_t *c,size_t *n,unsigned dst,unsigned sampler){
    c[(*n)++]=DI(0x42,3);c[(*n)++]=dr(0,dst,0xF,0);c[(*n)++]=sr(1,0,SW_ID,0);c[(*n)++]=sr(10,sampler,SW_ID,0);
}
static size_t assemble(uint32_t *c,int add_normal){
    size_t n=0;c[n++]=0xFFFF0300u;
    c[n++]=DI(0x51,5);c[n++]=dr(2,8,0xF,0);
    c[n++]=fbits(add_normal?-0.5f:2.0f);c[n++]=fbits(add_normal?1.0f:-1.0f);
    c[n++]=fbits(add_normal?0.0f:1.0f);c[n++]=0;
    c[n++]=DI(0x1F,2);c[n++]=0x80000005u;c[n++]=dr(1,0,0x3,0);
    c[n++]=DI(0x1F,2);c[n++]=0x8000000Au;c[n++]=dr(1,1,0xF,0);
    c[n++]=DI(0x1F,2);c[n++]=0x8001000Au;c[n++]=dr(1,2,0xF,0);
    c[n++]=DI(0x1F,2);c[n++]=0x90000000u;c[n++]=dr(10,0,0xF,0);
    c[n++]=DI(0x1F,2);c[n++]=0x90000000u;c[n++]=dr(10,1,0xF,0);
    put_texld(c,&n,0,0);
    put_alu(c,&n,0x05,dr(0,0,0x8,0),sr(0,0,SW_W,0),sr(2,5,SW_Z,0),0,2);
    put_alu(c,&n,0x05,dr(8,0,0x8,2),sr(0,0,SW_W,0),sr(1,1,SW_W,0),0,2);
    put_texld(c,&n,1,1);
    if(add_normal){
        put_alu(c,&n,0x02,dr(0,1,0x7,0),sr(0,1,SW_XZYW,0),sr(2,8,SW_X,0),0,2);
        put_alu(c,&n,0x02,dr(0,1,0x7,0),sr(0,1,SW_ID,0),sr(0,1,SW_ID,0),0,2);
        put_alu(c,&n,0x01,dr(0,1,0x8,0),sr(2,8,SW_Y,0),0,0,1);
    }else{
        put_alu(c,&n,0x04,dr(0,1,0x7,0),sr(0,1,SW_XZYW,0),sr(2,8,SW_X,0),sr(2,8,SW_Y,0),3);
        put_alu(c,&n,0x01,dr(0,1,0x8,0),sr(2,8,SW_Z,0),0,0,1);
    }
    put_alu(c,&n,0x09,dr(0,2,0x1,0),sr(2,0,SW_ID,0),sr(0,1,SW_ID,0),0,2);
    put_alu(c,&n,0x09,dr(0,2,0x2,0),sr(2,1,SW_ID,0),sr(0,1,SW_ID,0),0,2);
    put_alu(c,&n,0x09,dr(0,2,0x4,0),sr(2,2,SW_ID,0),sr(0,1,SW_ID,0),0,2);
    put_alu(c,&n,0x08,dr(0,0,0x8,1),sr(0,1,SW_ID,0),sr(2,3,SW_ID,0),0,2);
    put_alu(c,&n,0x02,dr(0,1,0x7,0),sr(0,2,SW_ID,0),sr(2,6,SW_ID,0),0,2);
    put_alu(c,&n,0x04,dr(0,1,0x7,0),sr(2,4,SW_ID,0),sr(0,0,SW_W,0),sr(0,1,SW_ID,0),3);
    put_alu(c,&n,0x05,dr(0,0,0x7,0),sr(0,0,SW_ID,0),sr(0,1,SW_ID,0),0,2);
    put_alu(c,&n,0x05,dr(0,1,0x7,0),sr(0,0,SW_ID,0),sr(1,1,SW_ID,0),0,2);
    put_alu(c,&n,0x01,dr(0,2,0x7,0),sr(1,1,SW_ID,0),0,0,1);
    put_alu(c,&n,0x04,dr(0,0,0x7,0),sr(0,0,SW_ID,0),sr(0,2,SW_ID,1),sr(1,2,SW_ID,0),3);
    put_alu(c,&n,0x04,dr(0,0,0x7,0),sr(1,2,SW_W,0),sr(0,0,SW_ID,0),sr(0,1,SW_ID,0),3);
    put_alu(c,&n,0x05,dr(8,0,0x7,2),sr(0,0,SW_ID,0),sr(2,7,SW_W,0),0,2);
    c[n++]=0xFFFFu;return n;
}

/* Independent token interpreter for the accepted signature streams. */
typedef struct {float r[16][4],c[16][4],v[4][4],out[4];int saw_out;} Ref;
static unsigned rt(uint32_t t){return ((t>>28)&7u)|((t&0x1800u)>>8);}
static unsigned rn(uint32_t t){return t&0x7FFu;}
static float *rv(Ref *s,uint32_t t){unsigned ty=rt(t),i=rn(t);return ty==2?s->c[i]:ty==1?s->v[i]:s->r[i];}
static void src(Ref *s,uint32_t t,float o[4]){float *v=rv(s,t);unsigned sw=(t>>16)&255u;for(int i=0;i<4;++i)o[i]=v[(sw>>(2*i))&3u];if(((t>>24)&15u)==1)for(int i=0;i<4;++i)o[i]=-o[i];}
static void dst(Ref *s,uint32_t t,const float v[4]){unsigned ty=rt(t),i=rn(t),m=(t>>16)&15u;if(!m)m=15;float *d=ty==8?s->out:s->r[i];float q[4];memcpy(q,v,16);if(((t>>20)&15u)==1)for(int k=0;k<4;++k){if(q[k]<0)q[k]=0;if(q[k]>1)q[k]=1;}for(int k=0;k<4;++k)if(m&(1u<<k))d[k]=q[k];if(ty==8)s->saw_out=1;}
static int run_ref(const uint32_t *code,size_t words,const D9MenuLightInputs *in,float out[4]){
    Ref s;memset(&s,0,sizeof s);memcpy(s.v[1],in->vertex_color,16);memcpy(s.v[2],in->secondary_color,16);memcpy(s.c,in->c0,8*16);
    const float tex[2][4]={{in->diffuse[0],in->diffuse[1],in->diffuse[2],in->diffuse[3]},
                           {in->normal_raw[0],in->normal_raw[1],in->normal_raw[2],in->normal_raw[3]}};
    size_t p=1;
    while(p<words){uint32_t t=code[p],op=t&0xFFFFu;if(op==0xFFFFu)break;if(op==0xFFFEu){p+=1+((t>>16)&0x7FFFu);continue;}size_t total=1+((t>>24)&15u);const uint32_t *a=code+p+1;
        if(op==0x1Fu){p+=total;continue;}
        if(op==0x51u){memcpy(s.c[rn(a[0])],a+1,16);p+=total;continue;}
        if(op==0x42u){dst(&s,a[0],tex[rn(a[2])]);p+=total;continue;}
        float x[4],y[4],z[4],r[4];src(&s,a[1],x);if(total>3)src(&s,a[2],y);if(total>4)src(&s,a[3],z);
        switch(op){case 1:memcpy(r,x,16);break;case 2:for(int i=0;i<4;++i)r[i]=x[i]+y[i];break;case 3:for(int i=0;i<4;++i)r[i]=x[i]-y[i];break;
        case 4:for(int i=0;i<4;++i)r[i]=x[i]*y[i]+z[i];break;case 5:for(int i=0;i<4;++i)r[i]=x[i]*y[i];break;
        case 8:r[0]=r[1]=r[2]=r[3]=x[0]*y[0]+x[1]*y[1]+x[2]*y[2];break;
        case 9:r[0]=r[1]=r[2]=r[3]=x[0]*y[0]+x[1]*y[1]+x[2]*y[2]+x[3]*y[3];break;default:return -1;}
        dst(&s,a[0],r);p+=total;
    }
    if(!s.saw_out)return -1;for(int i=0;i<4;++i)out[i]=s.out[i]<0?0:s.out[i]>1?1:s.out[i];return 0;
}

static unsigned rng=0xC001D00Du;
static float rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return (float)(rng&0xFFFFu)/65535.0f;}
static D9MenuLightInputs random_inputs(void){D9MenuLightInputs in;float *p=(float*)&in;for(unsigned i=0;i<sizeof in/sizeof *p;++i)p[i]=rnd();return in;}
static uint32_t pack8(float f){if(f<0)f=0;if(f>1)f=1;return d9_tev_pack_rgba8((float[4]){f,f,f,f})&255u;}
static float unpack8(uint32_t q){return (float)q/255.0f;}

static void plan_reference(const D9MenuLightInputs *in,float out[4]){
    float c3[4],c4[4],c7[4];for(int i=0;i<4;++i){c3[i]=unpack8(pack8(in->c3[i]));c4[i]=unpack8(pack8(in->c4[i]));c7[i]=unpack8(pack8(in->c7[i]));}
    float n=in->normal_raw[0]*c3[0]+in->normal_raw[1]*c3[1]+in->normal_raw[2]*c3[2];if(n<0)n=0;if(n>1)n=1;
    float light[3];for(int i=0;i<3;++i){light[i]=n*c4[i]+in->secondary_color[i];if(light[i]<0)light[i]=0;if(light[i]>1)light[i]=1;}
    float color[3];for(int i=0;i<3;++i){color[i]=in->diffuse[i]*light[i];if(color[i]<0)color[i]=0;if(color[i]>1)color[i]=1;}
    float alpha=in->diffuse[3]*in->vertex_color[3];
    for(int i=0;i<3;++i){color[i]*=in->vertex_color[i];if(color[i]<0)color[i]=0;if(color[i]>1)color[i]=1;}
    alpha*=c4[3];if(alpha<0)alpha=0;if(alpha>1)alpha=1;
    for(int i=0;i<3;++i){color[i]*=c7[3];if(color[i]<0)color[i]=0;if(color[i]>1)color[i]=1;}
    memcpy(out,color,12);out[3]=alpha;
}

static int test_fade_contract(const D9MenuLightMatch *match){
    D9TevProgram plan;char why[192];
    if(d9_menu_light_plan(match,&plan,why,sizeof why)){fprintf(stderr,"fade plan: %s\n",why);return 1;}
    int failures=0;
#define CHECK(condition,text) do{if(!(condition)){fprintf(stderr,"FAIL: %s\n",(text));++failures;}}while(0)
    const D9TevStage *fade=&plan.stage[4];
    CHECK(fade->func_rgb==D9TEV_FUNC_MODULATE,"fade RGB is MODULATE");
    CHECK(fade->func_a==D9TEV_FUNC_REPLACE,"fade alpha is passthrough REPLACE");
    CHECK(fade->src_rgb[0]==D9TEV_SRC_PREV&&fade->op_rgb[0]==D9TEV_RGB_COLOR,
          "fade first RGB source is PREVIOUS");
    CHECK(fade->src_rgb[1]==D9TEV_SRC_CONST&&fade->op_rgb[1]==D9TEV_RGB_ALPHA,
          "fade second RGB source is CONSTANT.a");
    CHECK(fade->src_a[0]==D9TEV_SRC_PREV&&fade->op_a[0]==D9TEV_A_ALPHA,
          "fade alpha passes PREVIOUS through");
    CHECK(fade->const_reg==7&&!fade->const_is_literal,"fade uses external c7");

    D9MenuLightInputs in;memset(&in,0,sizeof in);
    in.diffuse[0]=0.8f;in.diffuse[1]=0.6f;in.diffuse[2]=0.4f;in.diffuse[3]=0.5f;
    in.normal_raw[2]=1.0f;
    in.vertex_color[0]=in.vertex_color[1]=in.vertex_color[2]=in.vertex_color[3]=0.5f;
    /* Pre-baked ambient+emissive varying. */
    in.secondary_color[0]=in.secondary_color[1]=in.secondary_color[2]=0.2f;
    in.c3[2]=1.0f;
    in.c4[0]=in.c4[1]=in.c4[2]=1.0f;in.c4[3]=0.5f;
    /* RGB is deliberately unrelated: the retail evaluator uses only c7.a. */
    in.c7[0]=1.0f;in.c7[1]=0.0f;in.c7[2]=1.0f;in.c7[3]=0.5f;

    float q7a=unpack8(pack8(in.c7[3])),q4a=unpack8(pack8(in.c4[3]));
    float before[3],expect[4],old[3];
    for(int i=0;i<3;++i){
        before[i]=in.diffuse[i]*in.vertex_color[i];
        expect[i]=before[i]*q7a;
        old[i]=in.c7[i]*q7a+before[i]*(1.0f-q7a);
    }
    expect[3]=in.diffuse[3]*in.vertex_color[3]*q4a;
    printf("menu fade regression: retail secondary*c7.a=(%.6f,%.6f,%.6f); "
           "former c7.rgb interpolation=(%.6f,%.6f,%.6f)\n",
           expect[0],expect[1],expect[2],old[0],old[1],old[2]);

    D9MenuLightResult got;
    CHECK(!d9_menu_light_eval_plan(&plan,&in,&got),"fade plan evaluator succeeds");
    for(int i=0;i<4;++i)CHECK(fabsf(got.rgba[i]-expect[i])<1e-6f,
                               "fade plan evaluator multiplies by c7.a");
    for(int i=0;i<3;++i)CHECK(fabsf(got.vertex_modulated[i]-before[i])<1e-6f,
                               "fade plan metadata is pre-fade vertex modulation");
    float tex[3][4]={{0}},stage_const[D9TEV_MAX_STAGES][4]={{0}},out[4];
    memcpy(tex[0],in.diffuse,16);memcpy(tex[1],in.normal_raw,16);
    for(int i=0;i<4;++i){
        stage_const[0][i]=unpack8(pack8(in.c3[i]));
        stage_const[1][i]=unpack8(pack8(in.c4[i]));
        stage_const[3][i]=stage_const[1][i];
        stage_const[4][i]=unpack8(pack8(in.c7[i]));
    }
    CHECK(!d9_tev_eval_pixel_varyings(&plan,tex,stage_const,in.vertex_color,
                                     in.secondary_color,NULL,out),
          "fade TEV evaluator succeeds");
    for(int i=0;i<4;++i){
        if(fabsf(out[i]-expect[i])>=1e-6f)
            fprintf(stderr,"fade TEV lane=%d got=%.9f expect=%.9f stage4=(%u,%u,%u,%u)\n",
                    i,out[i],expect[i],fade->func_rgb,fade->func_a,fade->src_rgb[1],fade->op_rgb[1]);
        CHECK(fabsf(out[i]-expect[i])<1e-6f,"fade TEV descriptors multiply by c7.a");
    }
#undef CHECK
    return failures;
}

int main(void){
    uint32_t p5[128],p120[128];size_t n5=assemble(p5,0),n120=assemble(p120,1);
    assert(n5==97&&n120==100); /* token words for 24/25 instructions plus END */
    D9MenuLightMatch m;char why[192];
    if(!d9_menu_light_match(p5,n5,&m,why,sizeof why)){fprintf(stderr,"p5 match: %s\n",why);return 1;}
    assert(m.signature==D9_MENU_LIGHT_P5_MAD_NORMAL&&m.instructions==24);
    if(!d9_menu_light_match(p120,n120,&m,why,sizeof why)){fprintf(stderr,"p120 match: %s\n",why);return 1;}
    assert(m.signature==D9_MENU_LIGHT_P1200005_ADD_NORMAL&&m.instructions==25);

    p5[3]=fbits(1.5f);assert(!d9_menu_light_match(p5,n5,&m,why,sizeof why));assert(strstr(why,"def c8"));p5[3]=fbits(2.0f);
    p5[25]=sr(10,2,SW_ID,0);assert(!d9_menu_light_match(p5,n5,&m,why,sizeof why));assert(strstr(why,"texld diffuse"));p5[25]=sr(10,0,SW_ID,0);
    p5[26]=DI(0x02,3);assert(!d9_menu_light_match(p5,n5,&m,why,sizeof why));assert(strstr(why,"material-alpha"));p5[26]=DI(0x05,3);

    assert(d9_menu_light_match(p5,n5,&m,why,sizeof why));
    if(test_fade_contract(&m))return 1;

    double exact_worst=0.0,plan_worst=0.0;
    for(int variant=0;variant<2;++variant){
        const uint32_t *code=variant?p120:p5;size_t words=variant?n120:n5;
        assert(d9_menu_light_match(code,words,&m,why,sizeof why));
        D9TevProgram plan;assert(!d9_menu_light_plan(&m,&plan,why,sizeof why));assert(plan.stages==5&&plan.textures==2);
        for(int k=0;k<2000;++k){
            D9MenuLightInputs in=random_inputs();D9MenuLightResult got;float expect[4];
            assert(!run_ref(code,words,&in,expect));
            assert(!d9_menu_light_eval_exact(&m,&in,&got));
            for(int i=0;i<4;++i){double e=fabs(expect[i]-got.rgba[i]);if(e>exact_worst)exact_worst=e;if(e>1e-6){fprintf(stderr,"exact mismatch variant=%d k=%d lane=%d expect=%.6f got=%.6f ndotl=%.6f diffuse=%g,%g,%g,%g normal=%g,%g,%g,%g v1=%g,%g,%g,%g v2=%g,%g,%g,%g\n",variant,k,i,expect[i],got.rgba[i],got.ndotl,in.diffuse[0],in.diffuse[1],in.diffuse[2],in.diffuse[3],in.normal_raw[0],in.normal_raw[1],in.normal_raw[2],in.normal_raw[3],in.vertex_color[0],in.vertex_color[1],in.vertex_color[2],in.vertex_color[3],in.secondary_color[0],in.secondary_color[1],in.secondary_color[2],in.secondary_color[3]);return 1;}}
            plan_reference(&in,expect);assert(!d9_menu_light_eval_plan(&plan,&in,&got));
            for(int i=0;i<4;++i){double e=fabs(expect[i]-got.rgba[i]);if(e>plan_worst)plan_worst=e;}
            float tex[3][4]={{0}},stage_const[D9TEV_MAX_STAGES][4]={{0}};
            memcpy(tex[0],in.diffuse,16);memcpy(tex[1],in.normal_raw,16);
            for(int c=0;c<4;++c){stage_const[0][c]=unpack8(pack8(in.c3[c]));stage_const[1][c]=unpack8(pack8(in.c4[c]));stage_const[3][c]=stage_const[1][c];stage_const[4][c]=unpack8(pack8(in.c7[c]));}
            assert(!d9_tev_eval_pixel_varyings(&plan,tex,stage_const,in.vertex_color,in.secondary_color,NULL,got.rgba));
            for(int i=0;i<4;++i){double e=fabs(expect[i]-got.rgba[i]);if(e>plan_worst)plan_worst=e;}
        }
    }
    fprintf(stderr,"menu light: exact signature CPU-reference max_abs_error=%.9f\n",exact_worst);
    fprintf(stderr,"menu light: five-stage pre-baked plan max_abs_error=%.9f\n",plan_worst);
    assert(exact_worst<1e-6&&plan_worst<1e-6);
    puts("menu-light signatures, rejection reasons, exact CPU reference and five-stage prototype passed");
    return 0;
}
