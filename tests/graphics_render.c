/* CPU reference rasterizer for the fixed demo scene, not a game renderer. */
#include "d3d9_demo_scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 400
#define H 240
typedef struct {unsigned w,h;uint8_t *pixels;} Tex;
typedef struct {uint8_t rgb[W*H*3];float depth[W*H];unsigned draws,painted;} Frame;
static int tex_new(void *ctx,unsigned w,unsigned h,unsigned levels,void **out){(void)ctx;if(levels!=1)return -1;Tex *t=malloc(sizeof *t);if(!t)return -1;
    t->w=w;t->h=h;t->pixels=malloc((size_t)w*h*4);if(!t->pixels){free(t);return -1;}*out=t;return 0;}
static int upload(void *ctx,void *n,unsigned level,const void *data,size_t bytes){(void)ctx;Tex *t=n;if(level||bytes!=(size_t)t->w*t->h*4)return -1;memcpy(t->pixels,data,bytes);return 0;}
static void tex_del(void *ctx,void *n){(void)ctx;Tex *t=n;free(t->pixels);free(t);}
static int begin(void *ctx,uint32_t argb){Frame *f=ctx;for(unsigned i=0;i<W*H;++i){f->rgb[i*3]=argb>>16;f->rgb[i*3+1]=argb>>8;f->rgb[i*3+2]=argb;f->depth[i]=1.0f;}f->painted=0;return 0;}
typedef struct {float x,y,z,iw,u,v;} Screen;
static Screen project(const D9Vertex *v,const D9Matrix *m){float p[4]={v->x,v->y,v->z,1},c[4]={0};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)c[i]+=m->m[i][j]*p[j];
    float iw=1/c[3];return (Screen){(1-c[1]*iw)*W*.5f,(1-c[0]*iw)*H*.5f,1+c[2]*iw,iw,v->u,v->v};}
static float edge(Screen a,Screen b,float x,float y){return (x-a.x)*(b.y-a.y)-(y-a.y)*(b.x-a.x);}
static void sample(Tex *t,float u,float v,uint8_t *out){if(!t){memset(out,255,3);return;}
    unsigned x=(unsigned)(fminf(1,fmaxf(0,u))*(t->w-1)+.5f),y=t->h-1-(unsigned)(fminf(1,fmaxf(0,v))*(t->h-1)+.5f);
    unsigned code=0;for(unsigned i=0;i<3;++i){code|=((x>>i)&1)<<(2*i);code|=((y>>i)&1)<<(2*i+1);}
    size_t pos=(((size_t)(y/8)*(t->w/8)+x/8)*64+code)*4;out[0]=t->pixels[pos+3];out[1]=t->pixels[pos+2];out[2]=t->pixels[pos+1];}
static int draw(void *ctx,const D9Vertex *v,size_t n,const D9Matrix *m,void *texture,const D9DrawState *state){Frame *f=ctx;++f->draws;(void)state;
    for(size_t i=0;i<n;i+=3){Screen a=project(v+i,m),b=project(v+i+1,m),c=project(v+i+2,m);
        if(a.iw<=0||b.iw<=0||c.iw<=0)continue;
        float area=edge(a,b,c.x,c.y);if(fabsf(area)<1e-5f)continue;
        int minx=(int)fmaxf(0,floorf(fminf(a.x,fminf(b.x,c.x)))),maxx=(int)fminf(W-1,ceilf(fmaxf(a.x,fmaxf(b.x,c.x))));
        int miny=(int)fmaxf(0,floorf(fminf(a.y,fminf(b.y,c.y)))),maxy=(int)fminf(H-1,ceilf(fmaxf(a.y,fmaxf(b.y,c.y))));
        for(int y=miny;y<=maxy;++y)for(int x=minx;x<=maxx;++x){float px=x+.5f,py=y+.5f,wa=edge(b,c,px,py)/area,wb=edge(c,a,px,py)/area,wc=1-wa-wb;
            if(wa<0||wb<0||wc<0)continue;
            float depth=wa*a.z+wb*b.z+wc*c.z;unsigned p=(unsigned)(y*W+x);
            if(depth<0||depth>f->depth[p])continue;
            float iw=wa*a.iw+wb*b.iw+wc*c.iw;
            float u=(wa*a.u*a.iw+wb*b.u*b.iw+wc*c.u*c.iw)/iw,vv=(wa*a.v*a.iw+wb*b.v*b.iw+wc*c.v*c.iw)/iw;
            f->depth[p]=depth;sample(texture,u,vv,f->rgb+p*3);++f->painted;
        }
    }return 0;}
static void end(void *ctx){(void)ctx;}
int main(int argc,char **argv){if(argc!=2&&argc!=3){fprintf(stderr,"usage: graphics_render output.ppm [dxt1|dxt3|dxt5]\n");return 1;}
    static Frame f;static const D9Backend ops={tex_new,upload,tex_del,begin,draw,end};
    D9Device *d=d9_create_device(&ops,&f,4u<<20);D9Demo s;
    if(!d||d9_demo_init(d,&s)){d9_destroy_device(d);return 1;}
    if(argc==3){unsigned fmt=!strcmp(argv[2],"dxt1")?D9_DXT1:!strcmp(argv[2],"dxt3")?D9_DXT3:!strcmp(argv[2],"dxt5")?D9_DXT5:0;
        D9Handle tex;size_t pitch,bytes,actual;void *p;
        if(d9_texture_layout(64,64,fmt,&pitch,&bytes)||d9_create_texture(d,64,64,fmt,&tex)||d9_texture_lock(d,tex,&p,&actual)||
            pitch!=actual||d9_demo_fill_bc(p,pitch,bytes,fmt)||d9_texture_unlock(d,tex)||d9_set_texture(d,tex)){d9_destroy_device(d);return 1;}}
    if(d9_demo_frame(d,&s,0.7f)||f.painted<1000){d9_destroy_device(d);return 1;}
    FILE *out=fopen(argv[1],"wbx");if(!out){d9_destroy_device(d);return 1;}
    int ok=fprintf(out,"P6\n%d %d\n255\n",W,H)>0&&fwrite(f.rgb,1,sizeof f.rgb,out)==sizeof f.rgb;
    if(fclose(out))ok=0;d9_destroy_device(d);printf("Reference frame: %u pixels drawn\n",f.painted);return ok?0:1;}
