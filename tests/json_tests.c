#include "../src/json_view.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(int argc,char **argv) {
  const char *input="{\"source\":\"Base/Scripts/Level_Cave/__TriggerInstances.lua.gz\",\"authored\":{\"Vars\":{\"delay\":-1.25e2,\"enabled\":true,\"position\":[1,2,3],\"empty\":{},\"text\":\"x, \\\"y\\\" {z} \\n test\",\"ref\":{\"$ref\":\"8dc1645e2ed9ee33aa75297e01b94d0c\"}}}}";
  hv_json doc={0};CHECK(hv_json_parse(&doc,input));CHECK(doc.nodes[0].kind==HV_J_OBJECT);
  int source=hv_json_field(&doc,0,"source");CHECK(source>=0);
  char text[200];hv_json_display(&doc,doc.nodes[source].start,doc.nodes[source].end,text,sizeof(text));
  CHECK(!strcmp(text,"Base/Scripts/Level_Cave/__TriggerInstances.lua.gz"));
  CHECK(hv_json_field(&doc,hv_json_field(&doc,0,"authored"),"Vars")>=0);
  CHECK(hv_json_field(&doc,0,"does-not-exist")==-1);
  char *pretty=hv_json_pretty(input);CHECK(pretty&&strstr(pretty,"\r\n  \"source\": ")&&strstr(pretty,"x, \\\"y\\\" {z} \\n test"));
  hv_json next={0};CHECK(hv_json_parse(&next,pretty));CHECK(next.count==doc.count);
  for(unsigned i=0;i<doc.count;i++) {
    CHECK(doc.nodes[i].kind==next.nodes[i].kind);
    if(doc.nodes[i].kind==HV_J_STRING||doc.nodes[i].kind==HV_J_VALUE) {
      unsigned size=doc.nodes[i].end-doc.nodes[i].start;
      CHECK(size==next.nodes[i].end-next.nodes[i].start);
      CHECK(!memcmp(input+doc.nodes[i].start,pretty+next.nodes[i].start,size));
    }
  }
  free(pretty);hv_json_free(&next);hv_json_free(&doc);
  const char *bad[]={"{\"a\":}","[1,]","{\"a\":01}","{\"a\":1.}","[1e]","true false","{\"a\":\"\\q\"}","[","",NULL};
  for(unsigned i=0;bad[i];i++){CHECK(!hv_json_parse(&doc,bad[i]));CHECK(!hv_json_pretty(bad[i]));}
  char deep[140];memset(deep,'[',66);memset(deep+66,']',66);deep[132]=0;CHECK(!hv_json_parse(&doc,deep));
  CHECK(hv_json_parse(&doc,"{\"x\": [ ], \"y\": { }, \"u\": \"\\u65e5\"}"));hv_json_free(&doc);
  if(argc==2) {
    const char *levels[]={"Graveyard","Barrens","Desert","Canyon","Cave","Ruins","Mountain","Summit","Credits","Bryan","Matt","Chris"};
    unsigned objects=0;char *line=malloc(65538);CHECK(line);
    for(unsigned i=0;i<12;i++) {
      char path[4096];snprintf(path,sizeof(path),"%s/Level_%s.hvi",argv[1],levels[i]);
      FILE *file=fopen(path,"rb");CHECK(file);
      while(fgets(line,65538,file))if(line[0]=='O'&&line[1]=='\t') {
        char *json=line;for(unsigned field=0;field<5;field++){json=strchr(json,'\t');CHECK(json);json++;}
        CHECK(hv_json_parse(&doc,json));hv_json_free(&doc);
        char *formatted=hv_json_pretty(json);CHECK(formatted);free(formatted);objects++;
      }
      fclose(file);
    }
    free(line);printf("PASS: structured/pretty JSON for %u actual index records across12 levels\n",objects);
  }
  puts("PASS: bounded display JSON, source extraction, formatted CRLF, exact scalar/string preservation, invalid/deep input refusal");
  return 0;
}
