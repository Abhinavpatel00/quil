/* Measures each subsystem's real memory floor, one stage per process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "font.h"
#include "tree_sitter/api.h"
extern const TSLanguage* tree_sitter_c(void);
extern const TSLanguage* tree_sitter_rust(void);
extern const TSLanguage* tree_sitter_python(void);
extern const TSLanguage* tree_sitter_javascript(void);
extern const char quill_query_c[];

static void rollup(const char* tag){
  FILE*f=fopen("/proc/self/smaps_rollup","r");char l[256];
  long rss=0,pss=0,priv=0;
  while(f&&fgets(l,sizeof l,f)){
    if(!strncmp(l,"Rss:",4)) sscanf(l+4,"%ld",&rss);
    else if(!strncmp(l,"Pss:",4)) sscanf(l+4,"%ld",&pss);
    else if(!strncmp(l,"Private_Dirty:",14)) sscanf(l+14,"%ld",&priv);
  }
  if(f)fclose(f);
  fprintf(stderr,"%-10s RSS %7.2f MiB   PSS %7.2f MiB   privDirty %7.2f MiB\n",
          tag, rss/1024.0, pss/1024.0, priv/1024.0);
}

int main(int argc,char**argv){
  const char* stage = argc>1?argv[1]:"none";
  if(!strcmp(stage,"none")){ rollup(stage); return 0; }
  if(!strcmp(stage,"font")){ FontSystem*fs=font_create(16,1); if(!fs)return 1; rollup(stage); return 0; }
  if(!strcmp(stage,"ts")){
    TSParser*p=ts_parser_new();
    ts_parser_set_language(p,tree_sitter_c());
    uint32_t eo=0; TSQueryError ec;
    TSQuery*q=ts_query_new(tree_sitter_c(),quill_query_c,(uint32_t)strlen(quill_query_c),&eo,&ec);
    /* parse a real file so tree nodes exist */
    FILE*f=fopen("assets/sample.c","rb"); char buf[65536]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
    TSTree*t=ts_parser_parse_string(p,NULL,buf,(uint32_t)n);
    rollup(stage); (void)q;(void)t; return 0;
  }
  if(!strcmp(stage,"ts4")){ /* all four grammars resident */
    TSParser*p=ts_parser_new(); uint32_t eo=0; TSQueryError ec; TSQuery*q[4];
    q[0]=ts_query_new(tree_sitter_c(),quill_query_c,(uint32_t)strlen(quill_query_c),&eo,&ec);
    q[1]=ts_query_new(tree_sitter_rust(),quill_query_c,(uint32_t)strlen(quill_query_c),&eo,&ec);
    q[2]=ts_query_new(tree_sitter_python(),quill_query_c,(uint32_t)strlen(quill_query_c),&eo,&ec);
    q[3]=ts_query_new(tree_sitter_javascript(),quill_query_c,(uint32_t)strlen(quill_query_c),&eo,&ec);
    ts_parser_set_language(p,tree_sitter_c());
    FILE*f=fopen("assets/sample.c","rb"); char buf[65536]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
    TSTree*t=ts_parser_parse_string(p,NULL,buf,(uint32_t)n);
    rollup(stage); (void)q;(void)t; return 0;
  }
  if(!strcmp(stage,"font_ts")){
    FontSystem*fs=font_create(16,1); if(!fs)return 1;
    TSParser*p=ts_parser_new(); ts_parser_set_language(p,tree_sitter_c());
    FILE*f=fopen("assets/sample.c","rb"); char buf[65536]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
    TSTree*t=ts_parser_parse_string(p,NULL,buf,(uint32_t)n);
    rollup(stage); (void)t; return 0;
  }
  rollup("?");
  return 0;
}
