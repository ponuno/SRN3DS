#include <3ds.h>
#include <stdio.h>
#include "d3d9_citro3d.h"
#include "d3d9_demo_scene.h"

int main(void){
    gfxInitDefault();consoleInit(GFX_BOTTOM,NULL);osSetSpeedupEnable(true);
    printf("Skyrim N3DS v0.6 graphics demo\nFixed-function D3D9 subset / PICA200\nSTART: exit\n\n");
    D9Citro *gpu=d9_citro_open();D9Device *device=gpu?d9_create_device(d9_citro_backend(),gpu,4u<<20):NULL;
    D9Demo demo;int ready=device&&d9_demo_init(device,&demo)==0;
    if(!ready)printf("Graphics/resource initialization failed\n");
    unsigned frames=0;uint64_t initial=svcGetSystemTick(),last=initial;
    while(aptMainLoop()){
        hidScanInput();if(hidKeysDown()&KEY_START)break;
        if(!ready){gspWaitForVBlank();continue;}
        uint64_t now=svcGetSystemTick();float angle=(float)(now-initial)/(float)SYSCLOCK_ARM11;
        if(d9_demo_frame(device,&demo,angle)){printf("Draw failed\n");ready=0;continue;}
        ++frames;
        if(now-last>=SYSCLOCK_ARM11){float seconds=(float)(now-last)/(float)SYSCLOCK_ARM11;
            printf("\x1b[6;1H%5.1f FPS   %lu KiB buffers/textures\n",frames/seconds,(unsigned long)(d9_memory_used(device)>>10));last=now;frames=0;}
    }
    d9_destroy_device(device);d9_citro_close(gpu);gfxExit();return 0;
}
