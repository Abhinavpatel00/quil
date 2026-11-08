/* Isolate the cost of the frame that follows an edit (the `p` hitch). */
#include "document.h"
#include "syntax.h"
#include "font.h"
#include "frame.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

static Document d; static FontSystem* fs; static DrawList* dl; static FrameScratch* sc;
static LineLayout lay[QUILL_MAX_VISIBLE]; static uint32_t nlay; static FrameParams p;

static double build_frame(void){
    double t0=now();
    frame_build(&d, fs, &p, dl, sc, lay, &nlay);
    return (now()-t0)*1000.0;
}

int main(void){
    const char* files[] = {"assets/sample.c","src/syntax.c","src/main.c","src/render_vk.c"};
    size_t k;
    fs = font_create(16,1); if(!fs){printf("font fail\n");return 1;}
    dl = (DrawList*)MU_CALLOC(1u,sizeof *dl);
    sc = (FrameScratch*)MU_CALLOC(1u,sizeof *sc);
    memset(&p,0,sizeof p); p.width=1280; p.height=800; p.tab_width=4; p.line_numbers=true;

    printf("%-18s %8s %12s %14s %14s %12s\n","file","lines","rebuild(ms)","after-edit(ms)","reparse(ms)","frame_total");
    for(k=0;k<sizeof files/sizeof files[0];++k){
        if(!doc_init(&d,64u<<20)) return 1;
        if(!doc_load_file(&d,files[k])) { printf("%-18s load fail\n",files[k]); doc_destroy(&d); continue; }
        doc_attach_syntax(&d, syntax_create_for_path(files[k]));
        if(d.syntax) syntax_parse(d.syntax,&d.q);
        p.now_ms=0;
        build_frame();                      /* warm the shape cache */
        int i; double warm=0;
        for(i=0;i<5;i++) warm+=build_frame();
        warm/=5;

        /* one `p`: insert a 300-byte register payload at the cursor */
        char payload[300]; memset(payload,'x',sizeof payload-1); payload[sizeof payload-1]=0;
        payload[0]='\n'; payload[100]='\n';
        uint32_t at = d.q.text_len/3;
        doc_insert(&d, at, payload, (uint32_t)strlen(payload), 0);
        double t0=now(); if(d.syntax) syntax_parse(d.syntax,&d.q); double reparse=(now()-t0)*1000.0;
        double after=build_frame();

        printf("%-18s %8u %12.2f %14.2f %14.2f %12.2f\n", files[k], d.q.nlines, warm, after, reparse, reparse+after);
        doc_destroy(&d);
    }
    return 0;
}
