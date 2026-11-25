#include "n3ds_pilot.h"
#include "guest_process.h"
#include "guest.h"
#include "win32_thread.h"
#include "pilot_guard.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define DATA 0x21000000u
#define OUTPUT (DATA+0x100)
#define INPUT_A (DATA+0x200)
#define INPUT_B (DATA+0x300)
static unsigned steps;
static uint32_t budget_pc[16][64];
static unsigned budget_pc_count[16];
void n3ds_pilot_tick(void){if(!steps){unsigned t=win32_thread_current_id();unsigned slot=t<16?t:15;fprintf(stderr,"[budget] thread=%u blocks=",t);unsigned n=budget_pc_count[slot];unsigned start=n>64?n-64:0;for(unsigned i=start;i<n;++i)fprintf(stderr," %08x",budget_pc[slot][i&63]);fputc('\n',stderr);rt_unimpl(NULL,0,"pilot basic-block budget exceeded");}--steps;}
void n3ds_pilot_tick_pc(uint32_t pc){unsigned t=win32_thread_current_id();unsigned slot=t<16?t:15;budget_pc[slot][budget_pc_count[slot]++&63]=pc;guest_watch_set_pc(pc,t);n3ds_pilot_tick();}
void n3ds_pilot_set_steps(unsigned n){steps=n;memset(budget_pc,0,sizeof budget_pc);memset(budget_pc_count,0,sizeof budget_pc_count);}
int n3ds_pilot_identity(const char *path,uint64_t *bytes,uint64_t *hash){
    FILE *f=fopen(path,"rb");if(!f)return -1;uint8_t block[4096];uint64_t n=0,h=UINT64_C(0xcbf29ce484222325);size_t got;
    while((got=fread(block,1,sizeof block,f))!=0){n+=got;for(size_t i=0;i<got;++i)h=(h^block[i])*UINT64_C(0x100000001b3);}
    int error=ferror(f);fclose(f);if(error)return -1;*bytes=n;*hash=h;return 0;
}
static uint32_t fbits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static int fail(PilotResult *r,const char *fmt,...){va_list a;va_start(a,fmt);vsnprintf(r->message,sizeof r->message,fmt,a);va_end(a);return -1;}
static void expected(unsigned type,unsigned variant,float a[16],float b[16],float out[16],uint32_t args[4],size_t *argc){
    for(unsigned i=0;i<16;++i){a[i]=(float)((int)((i*3+variant)%17)-8);b[i]=(float)((int)((i*5+variant*2)%13)-6);out[i]=0;}
    args[0]=OUTPUT;args[1]=INPUT_A;args[2]=INPUT_B;*argc=2;
    if(type==1){for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)out[i*4+j]=a[j*4+i];}
    else if(type==4){*argc=3;for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)out[i*4+j]+=a[i*4+k]*b[k*4+j];}
    else if(type==5){*argc=3;for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)out[i*4+j]+=a[j*4+k]*b[k*4+i];}
    else{float x=variant+0.5f,y=-(float)variant-2,z=3.25f;args[1]=fbits(x);args[2]=fbits(y);args[3]=fbits(z);*argc=4;
        out[15]=1;if(type==2){out[0]=x;out[5]=y;out[10]=z;}else{out[0]=out[5]=out[10]=1;out[12]=x;out[13]=y;out[14]=z;}}
}
int n3ds_pilot_run(const char *path,const PilotSpec *spec,const FnEntry *table,unsigned count,size_t budget,PilotResult *r){
    memset(r,0,sizeof *r);r->limit=budget;
    if(!path||!spec||!spec->name||spec->test_case<1||spec->test_case>5||!table||!count)return fail(r,"Invalid pilot configuration");
    uint64_t bytes,hash;if(n3ds_pilot_identity(path,&bytes,&hash))return fail(r,"DLL missing or unreadable: %s",path);
    if(bytes!=spec->file_bytes||hash!=spec->fnv64)return fail(r,"DLL differs from the one used to generate C code");
    GuestMemory m;GuestProcess process={0};GuestImage image;int bound=0,rc=-1;guest_init(&m,budget);
    int pe=pe_image_load(&m,path,spec->base,&image);if(pe){fail(r,"PE mapping: %s",pe_error(pe));goto done;}
    if(spec->entry<image.base||spec->entry-image.base>=image.size){fail(r,"Pilot entry outside DLL");goto done;}
    PeModule mod={"d3dx9_42.dll",&image};
    /* This is an isolated math call, not a DLL load in the Windows sense. No DllMain / TLS callbacks / IAT binding. */
    if(guest_process_init(&process,&m,&mod,1,0x80000)||guest_map(&m,DATA,4096)){fail(r,"Pilot process/stack allocation failed");goto done;}
    if(n3ds_lift_bind(&m,table,count)){fail(r,"Invalid function table");goto done;}bound=1;
    if(!n3ds_lift_has_function(spec->entry)){fail(r,"Export missing from generated function table");goto done;}
    CPU c;for(unsigned i=0;i<3;++i){float a[16],b[16],want[16],out[16];uint32_t args[4],ret;size_t argc;
        expected(spec->test_case,i,a,b,want,args,&argc);
        guest_write(&m,INPUT_A,a,sizeof a);guest_write(&m,INPUT_B,b,sizeof b);
        memset(guest_ptr(&m,OUTPUT-16,96),0xcd,96);
        n3ds_lift_init_cpu(&c,process.stack_top,process.teb);steps=2048;
        int status=n3ds_lift_invoke(&c,spec->entry,args,argc,&ret);
        if(status){fail(r,"%s test %u: %s (status %d)",spec->name,i+1,n3ds_lift_fault(),status);goto done;}
        if(ret!=OUTPUT||guest_read(&m,OUTPUT,out,sizeof out)){fail(r,"Unexpected return pointer in test %u",i+1);goto done;}
        for(unsigned j=0;j<16;++j)if(!isfinite(out[j])||fabsf(out[j]-want[j])>1e-4f){fail(r,"Test %u element %u: got %.5g expected %.5g",i+1,j,out[j],want[j]);goto done;}
        uint8_t canary[16];memset(canary,0xcd,16);
        if(memcmp(guest_ptr(&m,OUTPUT-16,16),canary,16)||memcmp(guest_ptr(&m,OUTPUT+64,16),canary,16)||
           memcmp(guest_ptr(&m,INPUT_A,64),a,64)||memcmp(guest_ptr(&m,INPUT_B,64),b,64)){fail(r,"Unexpected write outside matrix output");goto done;}
        ++r->passed;
    }
    snprintf(r->message,sizeof r->message,"%s: PASS (%u/3)",spec->name,r->passed);rc=0;
done:
    r->memory=m.used;if(bound)n3ds_lift_release();guest_process_destroy(&process);guest_destroy(&m);return rc;
}
