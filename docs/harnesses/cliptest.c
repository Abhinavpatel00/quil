#include <GLFW/glfw3.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){
    char big[200000]; int i;
    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(320,200,"t",NULL,NULL);
    if(!w) return 1;
    for(i=0;i<(int)sizeof big-1;i++) big[i]='a'+(i%26);
    for(i=0;i<1000;i++) big[64*i]='\n'; big[sizeof big-1]=0;
    glfwPollEvents();
    double t0=now(); glfwSetClipboardString(w,"hello"); double t1=now();
    const char* s=glfwGetClipboardString(w); double t2=now();
    printf("small  set %.1f ms   get %.1f ms   got=%s\n",(t1-t0)*1000,(t2-t1)*1000, s?s:"(null)");
    t0=now(); glfwSetClipboardString(w,big); t1=now();
    s=glfwGetClipboardString(w); t2=now();
    printf("200KB  set %.1f ms   get %.1f ms   len=%zu\n",(t1-t0)*1000,(t2-t1)*1000, s?strlen(s):0);
    glfwTerminate(); return 0;
}
