/* Cost of one VFX-bearing frame: update + render into a DrawList. */
#include "vfx.h"
#include "frame.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){
    static DrawList dl; static VfxState v; static MotionConfig cfg;
    float base[4]={1,1,1,1}, fw=9.6f, fh=16.0f;
    config_defaults(&cfg);
    cfg.vfx_modes = VFX_RAILGUN|VFX_TORPEDO|VFX_PIXIEDUST|VFX_COMET|VFX_EMBER|
                    VFX_AURORA|VFX_GLITCH|VFX_SNOW|VFX_CONFETTI|VFX_SPARK|VFX_SMOKE|
                    VFX_SONICBOOM|VFX_RIPPLE|VFX_WIREFRAME|VFX_PULSE|VFX_SHOCKWAVE|VFX_HALO;
    vfx_init(&v); dl.nvfx=0;
    /* warm: sweep the cursor across the viewport like a `p` jump would */
    uint32_t f; double tu=0,tr=0; uint32_t peak=0; uint64_t total=0;
    for(f=0;f<240;f++){
        float x=(float)((f*37)%1200), y=(float)((f*37)%780);
        double a=now(); vfx_update(&v,&cfg,0.016f,x,y,fw,fh,false,base); double b=now();
        dl.nvfx=0; vfx_render(&v,&cfg,&dl,fw,fh); double c=now();
        tu+=b-a; tr+=c-b; total+=dl.nvfx; if(dl.nvfx>peak)peak=dl.nvfx;
    }
    printf("QUILL_VFX_POOL=%u  QUILL_MAX_VFX=%u\n", (unsigned)QUILL_VFX_POOL, (unsigned)QUILL_MAX_VFX);
    printf("  17 modes, 240 frames @60fps:\n");
    printf("    vfx_update  %6.3f ms/frame\n", tu*1000/240);
    printf("    vfx_render  %6.3f ms/frame   (%.1f quads avg, peak %u)\n", tr*1000/240, (double)total/240, peak);
    printf("    total       %6.3f ms/frame\n", (tu+tr)*1000/240);
    return 0;
}
