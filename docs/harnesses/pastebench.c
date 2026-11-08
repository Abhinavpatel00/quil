#include "document.h"
#include "syntax.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){
    const char* files[] = {"assets/sample.c","src/main.c","src/render_vk.c","src/syntax.c","src/frame.c"};
    size_t i; char paste[100000];
    memset(paste,'x',sizeof paste); paste[sizeof paste-1]=0;
    for(i=0;i<sizeof paste;i+=64) paste[i]='\n';
    printf("%-20s %8s %11s %15s %13s\n","file","bytes","open+parse","insert100K","reparse");
    for(i=0;i<sizeof files/sizeof files[0];++i){
        Document d; if(!doc_init(&d, 64u<<20)) return 1;
        if(!doc_load_file(&d, files[i])){ printf("%-20s load failed\n",files[i]); doc_destroy(&d); continue; }
        doc_attach_syntax(&d, syntax_create_for_path(files[i]));
        double t0=now(); if(d.syntax) syntax_parse(d.syntax,&d.q); double t1=now();
        uint32_t at=d.q.text_len/2;
        double t2=now(); doc_insert(&d, at, paste, (uint32_t)strlen(paste), 0); double t3=now();
        double t4=now(); if(d.syntax) syntax_parse(d.syntax,&d.q); double t5=now();
        printf("%-20s %8u %11.2f %15.2f %13.2f\n",files[i],d.q.text_len,(t1-t0)*1000,(t3-t2)*1000,(t5-t4)*1000);
        doc_destroy(&d);
    }
    return 0;
}
