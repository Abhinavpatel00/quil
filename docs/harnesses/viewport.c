/* Realistic workload: redraw a 256-row viewport many times while scrolling.
 * Measures whether the smaller shape cache costs throughput. */
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void gen(char*d,uint32_t i){int k;for(k=0;k<6;k++)d[k]='a'+((i>>(5*k))&15);for(k=6;k<40;k++)d[k]="int x = 0;"[k%10];}
int main(void){
  FontSystem*fs=font_create(16,1); if(!fs)return 1;
  const ShapedGlyph*o; char ln[64]; unsigned frame,row,glyphs=0;
  double t0=now();
  for(frame=0;frame<600;frame++){          /* 600 frames of scrolling */
    for(row=0;row<256;row++){
      gen(ln,frame*3+row);
      glyphs+=font_shape_line(fs,(const uint8_t*)ln,40,4,&o);
    }
  }
  double dt=now()-t0;
  printf("600 frames x 256 rows: %.3f s total, %.3f ms/frame, %u glyph refs\n",
         dt,dt*1000/600,glyphs);
  return 0;
}
