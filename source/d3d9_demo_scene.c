#include "d3d9_demo_scene.h"
#include "d3d9_bc.h"
#include <math.h>
#include <string.h>

void d9_demo_mesh(D9Vertex vertices[24],uint16_t indices[36]){
    static const float corners[8][3]={{-0.7f,-0.7f,-0.7f},{0.7f,-0.7f,-0.7f},{0.7f,0.7f,-0.7f},{-0.7f,0.7f,-0.7f},
        {-0.7f,-0.7f,0.7f},{0.7f,-0.7f,0.7f},{0.7f,0.7f,0.7f},{-0.7f,0.7f,0.7f}};
    static const unsigned faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    static const float uv[4][2]={{0,1},{1,1},{1,0},{0,0}};
    for(unsigned f=0;f<6;++f){for(unsigned i=0;i<4;++i){const float *p=corners[faces[f][i]];
        vertices[f*4+i]=(D9Vertex){p[0],p[1],p[2],uv[i][0],uv[i][1]};}
        const unsigned order[]={0,1,2,0,2,3};for(unsigned i=0;i<6;++i)indices[f*6+i]=(uint16_t)(f*4+order[i]);}
}
void d9_demo_fill_texture(void *p,size_t pitch){
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        uint8_t *c=(uint8_t *)p+y*pitch+x*4;unsigned tone=((x/8+y/8)&1)?220:90;
        c[0]=(uint8_t)(y<32?tone:35);c[1]=(uint8_t)(x<32?tone:35);c[2]=(uint8_t)(x>=32?tone:35);c[3]=255;
        if(x<3||y<3||x>60||y>60)c[0]=c[1]=c[2]=255;
    }
}
int d9_demo_fill_bc_level(void *pixels,size_t pitch,size_t bytes,unsigned w,unsigned h,unsigned f,unsigned level){
    unsigned bb=d9_bc_block_bytes(f),cols=(w+3)/4,rows=(h+3)/4;size_t row=(size_t)cols*bb;
    if(!pixels||!bb||!w||!h||w>1024||h>1024||pitch<row||(rows>1&&pitch>(SIZE_MAX-row)/(rows-1))||bytes<(rows-1)*pitch+row)return -1;
    static const uint16_t bright[]={0x07ff,0xf81f,0x07e0,0xf800};
    for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<cols;++x){uint8_t *b=(uint8_t *)pixels+y*pitch+x*bb;memset(b,0,bb);
        unsigned a=((x+y)&1)?128:255;
        if(f==D9_DXT3)memset(b,a==255?255:0x88,8);else if(f==D9_DXT5)b[0]=b[1]=(uint8_t)a;
        uint8_t *c=b+(f==D9_DXT1?0:8);uint16_t color=level?bright[(level-1)%4]:bright[(y>=rows/2)*2+(x>=cols/2)];
        if(!level&&(x==0||x+1==cols||y==0||y+1==rows))color=0xffff;
        c[0]=(uint8_t)color;c[1]=(uint8_t)(color>>8); /* c1=black, c0>c1: four-color BC1 */
        memset(c+4,((x+y)&1)?0xaa:0,4); /* palette index 2 or 0, opaque in each format */
    }return 0;
}
int d9_demo_fill_bc(void *pixels,size_t pitch,size_t bytes,unsigned f){return d9_demo_fill_bc_level(pixels,pitch,bytes,64,64,f,0);}
void d9_demo_camera(D9Matrix *view,D9Matrix *projection){
    d9_matrix_identity(view);view->m[3][2]=3.0f;memset(projection,0,sizeof *projection);
    const float near=0.1f,far=20.0f,f=1.0f/tanf(0.55f);
    projection->m[0][0]=f/(400.0f/240.0f);projection->m[1][1]=f;projection->m[2][2]=far/(far-near);
    projection->m[2][3]=1;projection->m[3][2]=-near*far/(far-near);
}
void d9_demo_world(D9Matrix *world,float angle){
    D9Matrix x,y;d9_matrix_identity(&x);d9_matrix_identity(&y);
    float c=cosf(angle),sn=sinf(angle);y.m[0][0]=y.m[2][2]=c;y.m[0][2]=-sn;y.m[2][0]=sn;
    c=cosf(angle*.6f+0.3f);sn=sinf(angle*.6f+0.3f);x.m[1][1]=x.m[2][2]=c;x.m[1][2]=sn;x.m[2][1]=-sn;
    d9_matrix_multiply(world,&x,&y);
}
int d9_demo_init(D9Device *d,D9Demo *s){
    memset(s,0,sizeof *s);D9Vertex vertices[24];uint16_t indices[36];d9_demo_mesh(vertices,indices);
    void *p;size_t pitch;
    if(d9_create_vertex_buffer(d,sizeof vertices,&s->vb)||d9_buffer_lock(d,s->vb,0,sizeof vertices,&p))return -1;
    memcpy(p,vertices,sizeof vertices);if(d9_buffer_unlock(d,s->vb))return -1;
    if(d9_create_index_buffer(d,sizeof indices,D9_INDEX16,&s->ib)||d9_buffer_lock(d,s->ib,0,sizeof indices,&p))return -1;
    memcpy(p,indices,sizeof indices);if(d9_buffer_unlock(d,s->ib))return -1;
    if(d9_create_texture(d,64,64,D9_A8R8G8B8,&s->texture)||d9_texture_lock(d,s->texture,&p,&pitch))return -1;
    /* Original procedural checker: colored quadrants reveal flip/channel mistakes. */
    d9_demo_fill_texture(p,pitch);
    if(d9_texture_unlock(d,s->texture)||d9_set_stream_source(d,s->vb,0,sizeof(D9Vertex))||d9_set_indices(d,s->ib)||d9_set_texture(d,s->texture))return -1;
    D9Matrix view,projection;d9_demo_camera(&view,&projection);
    return d9_set_transform(d,D9_VIEW,&view)||d9_set_transform(d,D9_PROJECTION,&projection)?-1:0;
}
int d9_demo_frame(D9Device *d,const D9Demo *s,float angle){
    (void)s;D9Matrix world;d9_demo_world(&world,angle);
    if(d9_set_transform(d,D9_WORLD,&world)||d9_begin_scene(d,0xff14243c))return -1;
    int rc=d9_draw_indexed_primitive(d,D9_TRIANGLELIST,0,0,12);int present=d9_present(d);return rc||present?-1:0;
}
