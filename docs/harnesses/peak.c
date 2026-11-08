#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long rss_kb(void){FILE*f=fopen("/proc/self/status","r");char l[256];long v=0;while(fgets(l,sizeof l,f))if(!strncmp(l,"VmRSS:",6))sscanf(l+6,"%ld",&v);fclose(f);return v;}
int main(int c,char**v){FontSystem*fs=font_create(16,1);if(!fs)return 1;char ln[64];const ShapedGlyph*o;unsigned i,k;long peak=0;
for(i=0;i<20000;i++){for(k=0;k<6;k++)ln[k]='a'+((i>>(5*k))&15);for(k=6;k<40;k++)ln[k]="int x = 0;"[k%10];font_shape_line(fs,(const uint8_t*)ln,40,4,&o);long r=rss_kb();if(r>peak)peak=r;}
printf("peak RSS over 20000 unique lines: %.2f MiB (end %.2f MiB)\n",peak/1024.0,rss_kb()/1024.0);return 0;}
