#include "d3d9_cube_coords.h"
#include "d3d9_core.h"

void d9_cube_direction(unsigned face,float u,float v,float out[3]){
    if(!out)return;
    /* Standard cube-face inverse with the PICA bottom-up t axis. */
    float s=u*2.0f-1.0f;
    float t=1.0f-v*2.0f;
    switch(face){
    case D9_CUBE_FACE_POSITIVE_X:out[0]= 1.0f;out[1]=-t;out[2]=-s;break;
    case D9_CUBE_FACE_NEGATIVE_X:out[0]=-1.0f;out[1]=-t;out[2]= s;break;
    case D9_CUBE_FACE_POSITIVE_Y:out[0]= s;out[1]= 1.0f;out[2]= t;break;
    case D9_CUBE_FACE_NEGATIVE_Y:out[0]= s;out[1]=-1.0f;out[2]=-t;break;
    case D9_CUBE_FACE_POSITIVE_Z:out[0]= s;out[1]=-t;out[2]= 1.0f;break;
    default:                     out[0]=-s;out[1]=-t;out[2]=-1.0f;break;
    }
}
