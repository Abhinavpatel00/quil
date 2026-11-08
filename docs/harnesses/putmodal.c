/* Time the real modal 'p' path: yank -> put, repeated, to catch growth. */
#include "document.h"
#include "syntax.h"
#include "modal.h"
#include "selection_set.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

int main(void){
    static Document d; static ModalState m; static SelectionSet ss;
    const char* f = "src/render_vk.c";
    if(!doc_init(&d,64u<<20)) return 1;
    if(!doc_load_file(&d,f)){printf("load fail\n");return 1;}
    doc_attach_syntax(&d, syntax_create_for_path(f));
    if(d.syntax) syntax_parse(d.syntax,&d.q);
    memset(&m,0,sizeof m); m.window=NULL; modal_init(&m);
    ss_single(&ss, 1000u);
    ss_sync_doc(&ss,&d);

    /* yank 200 lines into the unnamed register using the real key path */
    double t0=now();
    modal_key(&m,&d,&ss,GLFW_KEY_Y,0,0); modal_key(&m,&d,&ss,GLFW_KEY_Y,0,0);
    double yank=(now()-t0)*1000.0;
    printf("yank 200 lines:        %7.2f ms\n", yank);

    printf("%-8s %10s %12s %14s\n","put #","total(ms)","put_only(ms)","text_len");
    int i;
    for(i=1;i<=8;i++){
        uint32_t before=d.q.text_len;
        double a=now();
        modal_key(&m,&d,&ss,GLFW_KEY_P,0,0);
        double ms=(now()-a)*1000.0;
        printf("%-8d %10.2f %12.2f %14u\n", i, ms, ms, d.q.text_len-before);
    }
    /* now measure the undo vector + payload arena growth */
    printf("\nundo entries: %zu   payload used: %.2f MiB\n",
           (size_t)mu_small_vec_count(&d.undo),
           mu_vm_arena_used(&d.payload)/1048576.0);
    return 0;
}
