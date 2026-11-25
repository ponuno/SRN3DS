#include "n3ds_pilot.h"
#include "lift_prelude.h"
#include "pilot_guard.h"
#include <stdio.h>
/* Handwritten analogue of our own synthetic DLL, NOT lifted game code. */
static void scaling(CPU *c){
    n3ds_pilot_tick();uint32_t sp=c->r[4],p=M32(sp+4);
    for(unsigned i=0;i<16;i++)M32(p+4*i)=0;
    M32(p)=M32(sp+8);M32(p+20)=M32(sp+12);M32(p+40)=M32(sp+16);M32(p+60)=0x3f800000;
    c->r[0]=p;c->r[4]+=20;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    PilotSpec s={"D3DXMatrixScaling",2,0x08001000,0x08000000,0,0};
    const FnEntry t[]={{0x08001000,scaling}};PilotResult r;
    if(n3ds_pilot_identity(argv[1],&s.file_bytes,&s.fnv64))return 2;
    s.fnv64^=1;if(!n3ds_pilot_run(argv[1],&s,t,1,4u<<20,&r))return 1;s.fnv64^=1;
    int rc=n3ds_pilot_run(argv[1],&s,t,1,4u<<20,&r);puts(r.message);
    return rc||r.passed!=3;
}
