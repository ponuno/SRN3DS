#include <3ds.h>
#include <stdio.h>
#include "d3d9_citro3d.h"
#include "d3d9_com_demo.h"

int main(void){
    gfxInitDefault();consoleInit(GFX_BOTTOM,NULL);osSetSpeedupEnable(true);
    printf("Skyrim N3DS v0.10\nGuest D3D9 COM -> Citro3D\nA:texture X:shader Y:mip B:blend\nL:vertex layout START:exit\n");
    D9Citro *gpu=d9_citro_open();D9Device *d=gpu?d9_create_device(d9_citro_backend(),gpu,4u<<20):NULL;
    static D9ComDemo scene;int started=d!=NULL,ready=started&&!d9_com_demo_init(&scene,d);
    printf("%s\n",ready?"IAT / COM / upload: PASS":started?scene.message:"Graphics init failed");
    int bc=ready&&!d9_com_demo_enable_bc(&scene);printf("BC mip chains: %s\n",bc?"PASS":ready?scene.message:"skipped");
    int shader=ready&&!d9_com_demo_enable_shaders(&scene);printf("SM2 templates: %s\n",shader?"PASS":ready?scene.message:"skipped");
    int decl=ready&&!d9_com_demo_enable_decl(&scene);printf("Vertex declaration: %s\n",decl?"PASS":ready?scene.message:"skipped");
    static const char *formats[]={"BGRA8","DXT1","DXT3","DXT5"};unsigned mode=0,mip=0;int blend=0;
    if(ready)d9_com_demo_mip(&scene,0);
    unsigned frames=0;uint64_t start=svcGetSystemTick(),last=start;
    while(aptMainLoop()){
        hidScanInput();if(hidKeysDown()&KEY_START)break;
        if(bc&&(hidKeysDown()&KEY_A)){unsigned next=(mode+1)%4;if(!d9_com_demo_select_texture(&scene,next))mode=next;}
        if(shader&&(hidKeysDown()&KEY_X))d9_com_demo_select_shaders(&scene,!scene.shaders);
        if(bc&&(hidKeysDown()&KEY_Y)){unsigned next=(mip+1)%4;if(!d9_com_demo_mip(&scene,next))mip=next;}
        if((hidKeysDown()&KEY_B)&&!d9_com_demo_blend(&scene,!blend))blend=!blend;
        if(decl&&(hidKeysDown()&KEY_L))d9_com_demo_select_decl(&scene,!scene.declaration);
        if(!ready){gspWaitForVBlank();continue;}
        uint64_t now=svcGetSystemTick();float angle=(float)(now-start)/SYSCLOCK_ARM11;
        if(d9_com_demo_frame(&scene,angle)){printf("Draw failed: %s\n",scene.message);ready=0;continue;}
        ++frames;if(now-last>=SYSCLOCK_ARM11){float seconds=(float)(now-last)/SYSCLOCK_ARM11;
            printf("\x1b[10;1H%5.1f FPS  %-5s  %-5s\nmip %u blend %s  %-5s\n",frames/seconds,formats[mode],scene.shaders?"SM2":"fixed",mip,blend?"on":"off",scene.declaration?"decl":"FVF");frames=0;last=now;}
    }
    if(started)d9_com_demo_destroy(&scene);d9_destroy_device(d);d9_citro_close(gpu);gfxExit();return 0;
}
