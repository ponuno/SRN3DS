#include "d3d9_shader.h"
#include <string.h>

/* Self-authored equivalent of:
 * vs_2_0: dcl_position v0; dcl_texcoord v1; m4x4 oPos,v0,c0; mov oT0,v1
 * ps_2_0: dcl t0.xy; dcl_2d s0; texld r0,t0,s0; mov oC0,r0
 */
const uint32_t d9_demo_vs[]={0xfffe0200,0x0200001f,0x80000000,0x900f0000,
  0x0200001f,0x80000005,0x900f0001,0x03000014,0xc00f0000,0x90e40000,0xa0e40000,
  0x02000001,0xe00f0000,0x90e40001,0x0000ffff};
const uint32_t d9_demo_ps[]={0xffff0200,0x0200001f,0x80000000,0xb0030000,
  0x0200001f,0x90000000,0xa00f0800,0x03000042,0x800f0000,0xb0e40000,0xa0e40800,
  0x02000001,0x800f0800,0x80e40000,0x0000ffff};
const size_t d9_demo_vs_words=sizeof d9_demo_vs/sizeof *d9_demo_vs,d9_demo_ps_words=sizeof d9_demo_ps/sizeof *d9_demo_ps;

/* SM2/SM3 opcode table (Microsoft Direct3D 9 token format, d3d9types.h
 * D3DSHADER_INSTRUCTION_OPCODE_TYPE). min_words/max_words count the
 * instruction token and every operand token; max_words==0 means "not
 * bounded beyond the 16-word limit of the instruction length field". The
 * length field itself is authoritative (D3DSI_INSTLENGTH_MASK); the table is
 * a per-opcode sanity floor so a corrupt token is reported as invalid rather
 * than followed blindly. Numbering is from the Windows SDK d3d9types.h;
 * lengths were cross-checked against fxc 10.0.26100.0 (/Od) output for the
 * opcodes it emits and follow the documented operand counts for the rest.
 * supported marks the four opcodes that appear in the two translated SM2
 * templates. */
typedef struct {const char *name;unsigned char min_words,max_words,supported;} D9Opcode;
static const D9Opcode d9_opcodes[D9_SHADER_OPCODE_COUNT]={
  [0]={"nop",1,1,0},          [1]={"mov",3,3,1},          [2]={"add",4,4,0},
  [3]={"sub",4,4,0},          [4]={"mad",5,5,0},          [5]={"mul",4,4,0},
  [6]={"rcp",3,3,0},          [7]={"rsq",3,3,0},          [8]={"dp3",4,4,0},
  [9]={"dp4",4,4,0},          [10]={"min",4,4,0},         [11]={"max",4,4,0},
  [12]={"slt",4,4,0},         [13]={"sge",4,4,0},         [14]={"exp",3,3,0},
  [15]={"log",3,3,0},         [16]={"lit",3,3,0},         [17]={"dst",4,4,0},
  [18]={"lrp",5,5,0},         [19]={"frc",3,3,0},         [20]={"m4x4",4,4,1},
  [21]={"m4x3",4,4,0},        [22]={"m3x4",4,4,0},        [23]={"m3x3",4,4,0},
  [24]={"m3x2",4,4,0},        [25]={"call",2,2,0},        [26]={"callnz",3,3,0},
  [27]={"loop",2,2,0},        [28]={"ret",1,1,0},         [29]={"endloop",1,1,0},
  [30]={"label",2,2,0},       [31]={"dcl",3,3,1},         [32]={"pow",4,4,0},
  [33]={"crs",4,4,0},         [34]={"sgn",3,5,0},         [35]={"abs",3,3,0},
  [36]={"nrm",3,3,0},         [37]={"sincos",3,5,0},      [38]={"rep",2,2,0},
  [39]={"endrep",1,1,0},      [40]={"if",2,2,0},          [41]={"ifc",3,3,0},
  [42]={"else",1,1,0},        [43]={"endif",1,1,0},       [44]={"break",1,1,0},
  [45]={"breakc",3,3,0},      [46]={"mova",3,3,0},        [47]={"defb",3,3,0},
  [48]={"defi",6,6,0},
  /* 49..63: no opcode defined by D3D9. */
  [64]={"texcoord",2,2,0},   [65]={"texkill",2,2,0},     [66]={"texld",4,4,1},
  [67]={"texbem",1,0,0},      [68]={"texbeml",1,0,0},     [69]={"texreg2ar",1,0,0},
  [70]={"texreg2gb",1,0,0},   [71]={"texm3x2pad",1,0,0},  [72]={"texm3x2tex",1,0,0},
  [73]={"texm3x3pad",1,0,0},  [74]={"texm3x3tex",1,0,0},  [75]={"reserved0",1,0,0},
  [76]={"texm3x3spec",1,0,0}, [77]={"texm3x3vspec",1,0,0},[78]={"expp",3,3,0},
  [79]={"logp",3,3,0},        [80]={"cnd",5,5,0},         [81]={"def",6,6,0},
  [82]={"texreg2rgb",1,0,0},  [83]={"texdp3tex",1,0,0},   [84]={"texm3x2depth",1,0,0},
  [85]={"texdp3",1,0,0},      [86]={"texm3x3",1,0,0},     [87]={"texdepth",1,0,0},
  [88]={"cmp",5,5,0},         [89]={"bem",1,0,0},         [90]={"dp2add",5,5,0},
  [91]={"dsx",3,3,0},         [92]={"dsy",3,3,0},         [93]={"texldd",6,6,0},
  [94]={"setp",4,4,0},        [95]={"texldl",4,4,0},      [96]={"breakp",2,2,0},
};

const char *d9_shader_opcode_name(unsigned opcode){
    if(opcode<D9_SHADER_OPCODE_COUNT&&d9_opcodes[opcode].name)return d9_opcodes[opcode].name;
    switch(opcode){case 0xfffd:return "phase";case 0xfffe:return "comment";case 0xffff:return "end";}
    return "unknown";
}

void d9_shader_census_clear(D9ShaderCensus *census){
    if(!census)return;memset(census,0,sizeof *census);
}

void d9_shader_census_merge(D9ShaderCensus *into,const D9ShaderCensus *from){
    if(!into||!from||into==from)return;
    for(unsigned i=0;i<D9_SHADER_OPCODE_COUNT;++i)into->opcode[i]+=from->opcode[i];
    into->instructions+=from->instructions;into->outside+=from->outside;
}

/* The single token walk shared by d9_shader_analyze and the census API. */
static int d9_shader_scan(const uint32_t *p,size_t n,D9ShaderInfo *out,D9ShaderCensus *census,
                          D9TevProgram *tev_out){
    memset(out,0,sizeof *out);if(census)d9_shader_census_clear(census);
    out->stream=D9_SHADER_STREAM_INVALID;
    if(!p||!n){out->bad_version=1;return D9_SHADER_INVALID;}
    if(n>D9_SHADER_MAX_WORDS)n=D9_SHADER_MAX_WORDS;
    uint32_t ver=p[0],type=ver>>16;
    out->version=ver;out->vertex=type==0xfffe;out->major=(ver>>8)&255;out->minor=ver&255;
    if((type!=0xfffe&&type!=0xffff)||(out->major!=2&&out->major!=3)||out->minor!=0){
        out->bad_version=1;return D9_SHADER_INVALID;
    }
    const uint32_t *pattern=out->vertex?d9_demo_vs:d9_demo_ps;
    size_t words=out->vertex?d9_demo_vs_words:d9_demo_ps_words;
    size_t pos=1,expected=1;
    int same=ver==pattern[0];
    out->signature=2166136261u^ver;out->signature*=16777619u;
    while(pos<n){
        uint32_t token=p[pos],op=token&0xffff;
        if(op==0xffff){
            if(token!=0xffff){out->opcode=op;out->opcode_offset=(unsigned)(pos*4);
                out->bad_length=1;return D9_SHADER_INVALID;}
            out->words=(unsigned)(pos+1);out->saw_end=1;out->stream=D9_SHADER_STREAM_VALID;
            out->kind=same&&expected==words-1?(out->vertex?D9_SHADER_VS_MATRIX_UV:D9_SHADER_PS_TEXTURE):D9_SHADER_UNKNOWN;
            if(out->kind==D9_SHADER_UNKNOWN&&!out->vertex){
                D9TevProgram local;
                D9TevProgram *target=tev_out?tev_out:&local;
                if(!d9_tev_translate_pixel(p,pos+1,target,out->tev_reason,sizeof out->tev_reason)){
                    out->kind=D9_SHADER_TEV;
                    return out->kind;
                }
            }
            if(out->kind==D9_SHADER_UNKNOWN&&!out->opcode_offset&&out->first_opcode_offset){
                out->opcode=out->first_opcode;out->opcode_offset=out->first_opcode_offset;
            }
            return out->kind;
        }
        if(op==0xfffe){
            size_t skip=(size_t)((token>>16)&0x7fff);
            if(skip>n-pos-1){out->opcode=op;out->opcode_offset=(unsigned)(pos*4);
                out->stream=D9_SHADER_STREAM_TRUNCATED;out->need_words=(unsigned)(skip+1);
                out->have_words=(unsigned)(n-pos);return D9_SHADER_INVALID;}
            pos+=skip+1;continue;
        }
        if(op>=D9_SHADER_OPCODE_COUNT||!d9_opcodes[op].name){
            out->opcode=op;out->opcode_offset=(unsigned)(pos*4);out->unknown_opcode=1;
            if(census)++census->outside;
            return D9_SHADER_INVALID;
        }
        const D9Opcode *oi=&d9_opcodes[op];
        size_t total=((token>>24)&15)+1;
        if(total<oi->min_words||(oi->max_words&&total>oi->max_words)){
            out->opcode=op;out->opcode_offset=(unsigned)(pos*4);out->bad_length=1;
            out->need_words=oi->min_words;out->have_words=(unsigned)total;
            return D9_SHADER_INVALID;
        }
        if(total>n-pos){
            out->opcode=op;out->opcode_offset=(unsigned)(pos*4);
            out->stream=D9_SHADER_STREAM_TRUNCATED;out->need_words=(unsigned)total;
            out->have_words=(unsigned)(n-pos);return D9_SHADER_INVALID;
        }
        if(!out->instructions){out->first_opcode=op;out->first_opcode_offset=(unsigned)(pos*4);}
        if(!oi->supported&&!out->opcode_offset){out->opcode=op;out->opcode_offset=(unsigned)(pos*4);}
        ++out->instructions;
        if(census){++census->opcode[op];++census->instructions;}
        for(size_t i=0;i<total;++i){out->signature^=p[pos+i];out->signature*=16777619u;}
        if(same){if(expected>=words-1||total>words-1-expected||memcmp(p+pos,pattern+expected,total*4)){
            same=0;out->first_mismatch=(unsigned)pos;}else expected+=total;}
        pos+=total;
    }
    /* Ran out of words before END: either a partial instruction or a stream
     * that simply stops after a complete one. */
    out->stream=D9_SHADER_STREAM_TRUNCATED;
    out->opcode=0;out->opcode_offset=0;
    return D9_SHADER_INVALID;
}

int d9_shader_analyze(const uint32_t *code,size_t available,D9ShaderInfo *info){
    return d9_shader_analyze_tev(code,available,info,NULL);
}

int d9_shader_analyze_tev(const uint32_t *code,size_t available,D9ShaderInfo *info,
                          D9TevProgram *tev_out){
    if(!info)return D9_SHADER_INVALID;
    if(tev_out)memset(tev_out,0,sizeof *tev_out);
    int result=d9_shader_scan(code,available,info,NULL,tev_out);
    if(result==D9_SHADER_INVALID)info->kind=D9_SHADER_INVALID;
    return result;
}

int d9_shader_census_scan(const uint32_t *code,size_t available,D9ShaderInfo *info,D9ShaderCensus *census){
    if(!info)return D9_SHADER_INVALID;
    int result=d9_shader_scan(code,available,info,census,NULL);
    if(result==D9_SHADER_INVALID)info->kind=D9_SHADER_INVALID;
    return result;
}

/* Small formatter so the shader unit stays free of stdio and allocation. */
typedef struct {char *text;size_t size,len;} D9Text;
static void tx_char(D9Text *t,char c){if(t->len+1<t->size)t->text[t->len]=c;++t->len;}
static void tx_text(D9Text *t,const char *s){while(*s)tx_char(t,*s++);}
static void tx_uint(D9Text *t,unsigned v){char b[10];unsigned n=0;
    do{b[n++]=(char)('0'+v%10);v/=10;}while(v);while(n)tx_char(t,b[--n]);}
static void tx_hex(D9Text *t,uint32_t v,unsigned digits){
    static const char h[]="0123456789abcdef";
    tx_text(t,"0x");for(unsigned i=digits;i;--i)tx_char(t,h[(v>>((i-1)*4))&15]);
}
static int tx_done(D9Text *t){
    if(t->size)t->text[t->len<t->size?t->len:t->size-1]=0;
    return (int)t->len;
}
static void tx_version(D9Text *t,const D9ShaderInfo *info){
    unsigned type=info->version>>16;
    if(type!=0xfffe&&type!=0xffff){tx_text(t,"unknown");return;}
    tx_text(t,type==0xfffe?"vs_":"ps_");tx_uint(t,info->major);tx_char(t,'_');tx_uint(t,info->minor);
}

int d9_shader_version_name(const D9ShaderInfo *info,char *text,size_t size){
    if(!info||!text||!size)return -1;
    D9Text t={text,size,0};tx_version(&t,info);return tx_done(&t);
}

int d9_shader_diagnostic(const D9ShaderInfo *info,char *text,size_t size){
    if(!info||!text||!size)return -1;
    D9Text t={text,size,0};
    if(info->bad_version){
        tx_text(&t,"invalid version token ");tx_hex(&t,info->version,8);
        tx_text(&t," (");tx_version(&t,info);
        tx_text(&t,"); expected vs_2_0, vs_3_0, ps_2_0 or ps_3_0");
        return tx_done(&t);
    }
    tx_version(&t,info);
    if(info->kind==D9_SHADER_VS_MATRIX_UV||info->kind==D9_SHADER_VS_MENU||info->kind==D9_SHADER_PS_TEXTURE||info->kind==D9_SHADER_TEV){
        tx_text(&t,": supported (stream valid, ");tx_uint(&t,info->instructions);tx_text(&t," instructions)");
    }else if(info->stream==D9_SHADER_STREAM_VALID){
        tx_text(&t,": unsupported (stream valid, ");tx_uint(&t,info->instructions);tx_text(&t," instructions)");
        if(info->tev_reason[0]){
            tx_text(&t,"; TEV rejected: ");tx_text(&t,info->tev_reason);
        }else if(info->opcode_offset){tx_text(&t,"; first unsupported opcode '");
            tx_text(&t,d9_shader_opcode_name(info->opcode));tx_text(&t,"' at byte ");
            tx_uint(&t,info->opcode_offset);}
        else tx_text(&t,"; no instructions");
    }else if(info->stream==D9_SHADER_STREAM_TRUNCATED){
        tx_text(&t,": rejected (stream truncated)");
        if(info->opcode_offset){tx_text(&t,"; opcode '");tx_text(&t,d9_shader_opcode_name(info->opcode));
            tx_text(&t,"' at byte ");tx_uint(&t,info->opcode_offset);
            tx_text(&t," needs ");tx_uint(&t,info->need_words);
            tx_text(&t," words, ");tx_uint(&t,info->have_words);tx_text(&t," present");}
        else{tx_text(&t,"; no END token after ");tx_uint(&t,info->instructions);tx_text(&t," instructions");}
    }else{
        tx_text(&t,": rejected (stream invalid)");
        const char *name=d9_shader_opcode_name(info->opcode);
        if(info->unknown_opcode&&!strcmp(name,"unknown")){
            tx_text(&t,"; unknown opcode ");tx_hex(&t,info->opcode&0xffffu,4);
            tx_text(&t," at byte ");tx_uint(&t,info->opcode_offset);}
        else if(info->unknown_opcode){
            tx_text(&t,"; opcode '");tx_text(&t,name);tx_text(&t,"' at byte ");
            tx_uint(&t,info->opcode_offset);tx_text(&t," is not an SM2/SM3 instruction");}
        else if(info->opcode_offset&&info->opcode==0xffff){
            tx_text(&t,"; END token at byte ");tx_uint(&t,info->opcode_offset);
            tx_text(&t," has extra bits set");}
        else if(info->opcode_offset){
            unsigned max=info->opcode<D9_SHADER_OPCODE_COUNT?d9_opcodes[info->opcode].max_words:0;
            tx_text(&t,"; opcode '");tx_text(&t,name);
            tx_text(&t,"' at byte ");tx_uint(&t,info->opcode_offset);
            tx_text(&t," has length ");tx_uint(&t,info->have_words);
            tx_text(&t,", expected ");tx_uint(&t,info->need_words);
            if(max&&max!=info->need_words){tx_text(&t,"..");tx_uint(&t,max);}
            tx_text(&t," words");}
        else tx_text(&t,"; no instructions");
    }
    return tx_done(&t);
}
