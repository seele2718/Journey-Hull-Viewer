#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/index.h"
#include "../src/game_data.h"
#include "../src/json_view.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void grammar(void) {
  const char *good="-- test\nT={ObjectType='T', {ObjectName='a', Vars={v=Names['b'], a={1,-2,.3}, s='a\\n\\065', yes=true}}};";
  hd_lua d={0};CHECK(hd_parse(&d,good,strlen(good),"T"));
  hd_text json={.ok=1};hd_json(&d,0,&json);CHECK(json.ok);
  hv_json doc={0};CHECK(hv_json_parse(&doc,json.p));CHECK(hv_json_field(&doc,0,"ObjectType")>=0);
  char value[64];hv_json_display(&doc,0,1,value,sizeof(value));
  char *pretty=hv_json_pretty(json.p);CHECK(pretty);free(pretty);hv_json_free(&doc);free(json.p);hd_lua_free(&d);
  const char *bad[]={"T={ObjectType='T'};os.execute('bad')","T={ObjectType='T', a=function() end}",
    "T={ObjectType='T', a=game:cameraSystem()}","T={ObjectType='T', a=1+2}","T={ObjectType='T', a=1,a=2}",
    "T={ObjectType='T', a='unterminated}","T={ObjectType='T',a=Names[2]}","T={ObjectType='X'}","T={ObjectType='T', a=1e999}","T={ObjectType='T'} --[["};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){CHECK(!hd_parse(&d,bad[i],strlen(bad[i]),"T"));hd_lua_free(&d);}
  const char *hex="1f8b08000000000002ff5dcd310fc2201086e1ff7273077a2dc20d5ddc5c74212ec601b8a36a6263a08b31fc77313a393ff9decfe5eb3c4bde2d65f54b9432bd0ee1267175cf874ce0fe14ba1feffdbdb1e5d86f462d284c22c3e0bdd14846541f686415a1fb66b6b9ad2fd01d7d6e0792522b94e9d32827084c6c299231c25a69b4187040545ea7d0936238d75adfd53bfa86a7000000";
  unsigned char compressed[512];size_t n=strlen(hex)/2;
  for(size_t i=0;i<n;i++){unsigned byte;CHECK(sscanf(hex+i*2,"%2x",&byte)==1);compressed[i]=(unsigned char)byte;}
  size_t size=0;unsigned char *raw=hd_gunzip(compressed,n,&size);CHECK(raw&&size==167);
  CHECK(hd_parse(&d,(const char *)raw,size,"TriggerInstances"));hd_lua_free(&d);free(raw);
  CHECK(!hd_gunzip(compressed,n-1,&size));compressed[n-8]^=1;CHECK(!hd_gunzip(compressed,n,&size));compressed[n-8]^=1;
  compressed[n-1]=255;CHECK(!hd_gunzip(compressed,n,&size));compressed[n-1]=0;
  compressed[3]|=32;CHECK(!hd_gunzip(compressed,n,&size));compressed[3]=0;
  puts("PASS: generated Lua-only grammar, JSON conversion, calls/statements/duplicates/NaN refusal, gzip CRC/size/truncation/flags");
}
int wmain(int argc,wchar_t **argv) {
  grammar();if(argc<2)return 0;
  const char *levels[]={"Graveyard","Barrens","Desert","Canyon","Cave","Ruins","Mountain","Summit","Credits","Bryan","Matt","Chris"};
  unsigned total=0;ULONGLONG started=GetTickCount64();
  for(unsigned k=0;k<12;k++) {
    char level[80],error[512];snprintf(level,sizeof(level),"Level_%s",levels[k]);hv_index index={0};unsigned files=0,missing=0;
    ULONGLONG load_start=GetTickCount64();int ok=hd_load_game(&index,argv[1],level,error,&files,&missing);
    ULONGLONG load_time=GetTickCount64()-load_start;
    if(!ok)fprintf(stderr,"%s: %s\n",level,error);
    CHECK(ok);
    for(unsigned i=0;i<index.count;i++){hv_json json={0};CHECK(hv_json_parse(&json,index.objects[i].detail));hv_json_free(&json);}
    printf("%s: %u objects, %u references, %u files, %u missing, lookup %llu ms\n",level,index.count,index.ref_count,files,missing,(unsigned long long)load_time);fflush(stdout);
    if(argc>2) {
      wchar_t path[512];_snwprintf(path,512,L"%ls\\%hs.hvi",argv[2],level);path[511]=0;
      FILE *f=_wfopen(path,L"wb");CHECK(f);
      fprintf(f,"HVI1\t%s\t0000000000000000000000000000000000000000000000000000000000000000\n",level);
      for(unsigned i=0;i<index.count;i++){const hv_object *o=index.objects+i;fprintf(f,"O\t%s\t%s\t%s\t%s\t%s\n",o->hash,o->kind,o->name,o->type,o->detail);}
      for(unsigned i=0;i<index.ref_count;i++){const hv_reference *r=index.refs+i;fprintf(f,"R\t%s\t%s\t%s\n",index.objects[r->source].hash,r->target_hash,r->field);}
      CHECK(!ferror(f));CHECK(!fclose(f));
    }
    total+=index.count;hv_index_free(&index);
  }
  printf("PASS: %u authored objects from12 levels, in %llu ms; no script execution or Data writes\n",total,(unsigned long long)(GetTickCount64()-started));return 0;
}
