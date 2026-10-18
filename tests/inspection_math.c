#include <assert.h>
#include <stdio.h>
#include "../src/index.h"
#include "../src/pick_math.h"
int main(int argc,char **argv) {
  const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  const float a[3]={-.5f,0,.5f},b[3]={.5f,0,.5f};float depth=0;
  assert(hv_edge_distance(a,b,identity,500,500,1000,1000,&depth)<.001f);
  assert(fabsf(depth-.5f)<.001f);
  assert(fabsf(hv_edge_distance(a,b,identity,500,507,1000,1000,&depth)-7)<.001f);
  const float behind[3]={0,0,-1};
  assert(hv_edge_distance(behind,behind,identity,500,500,1000,1000,&depth)>1e10f);
  const float far[3]={0,0,2};
  assert(hv_edge_distance(far,far,identity,500,500,1000,1000,&depth)>1e10f);
  /* Clipping a near-plane crossing must preserve its visible part. */
  assert(hv_edge_distance(behind,b,identity,700,500,1000,1000,&depth)<.001f);
  float matrix[16];memcpy(matrix,identity,sizeof(matrix));matrix[12]=.25f;
  assert(hv_edge_distance(a,b,matrix,875,500,1000,1000,&depth)<.001f);
  float invalid[3]={NAN,0,.5f};
  assert(hv_edge_distance(invalid,b,identity,500,500,1000,1000,&depth)>1e10f);
  const float t0[3]={-.8f,-.8f,.4f},t1[3]={.8f,-.8f,.4f},t2[3]={0,.8f,.4f};
  assert(hv_triangle_depth(t0,t1,t2,identity,0,0,&depth));
  assert(fabsf(depth-.4f)<.00001f);
  assert(hv_triangle_depth(t2,t1,t0,identity,0,0,&depth));
  assert(!hv_triangle_depth(t0,t1,t2,identity,.9f,.9f,&depth));
  const float t3[3]={0,.8f,-.4f};
  assert(hv_triangle_depth(t0,t1,t3,identity,0,-.6f,&depth));
  assert(!hv_triangle_depth(behind,behind,behind,identity,0,0,&depth));
  assert(!hv_triangle_depth(invalid,t1,t2,identity,0,0,&depth));
  hv_index index={0};
  const char *valid="HVI1\tLevel_Cave\t0000000000000000000000000000000000000000000000000000000000000000\n"
    "O\t11111111111111111111111111111111\tHull\tHull A\t\t{}\n"
    "O\t22222222222222222222222222222222\tTrigger\tEnter\tEventFromRegion\t{}\n"
    "R\t22222222222222222222222222222222\t11111111111111111111111111111111\tVars.hulls\n"
    "R\t22222222222222222222222222222222\t33333333333333333333333333333333\tVars.effects\n";
  assert(hv_index_parse(&index,valid,strlen(valid),"Level_Cave"));
  assert(index.count==2&&index.ref_count==2);
  assert(index.refs[0].source==1&&index.refs[0].target==0&&index.refs[1].target==-1);
  assert(hv_index_find(&index,"11111111111111111111111111111111")==0);
  char *crlf=malloc(strlen(valid)*2+1),*write=crlf;assert(crlf);
  for(const char *read=valid;*read;read++){if(*read=='\n')*write++='\r';*write++=*read;}*write=0;
  assert(hv_index_parse(&index,crlf,strlen(crlf),"Level_Cave"));free(crlf);
  assert(!hv_index_parse(&index,valid,strlen(valid),"Level_Ruins"));
  assert(index.count==2); /* A bad replacement preserves the previous index. */
  assert(!hv_index_parse(&index,valid,strlen(valid)-1,"Level_Cave"));
  assert(!hv_hash_valid("1111111111111111111111111111111x"));
  assert(!hv_index_parse(&index,"HVI1\0bad\n",9,"Level_Cave"));
  hv_index_free(&index);
  assert(!index.buffer&&!index.count);
  if(argc==2) {
    const char *levels[]={"Graveyard","Barrens","Desert","Canyon","Cave","Ruins","Mountain","Summit","Credits","Bryan","Matt","Chris"};
    for(unsigned i=0;i<12;i++) {
      char path[2048],level[80];snprintf(level,sizeof(level),"Level_%s",levels[i]);
      snprintf(path,sizeof(path),"%s/%s.hvi",argv[1],level);FILE *file=fopen(path,"rb");assert(file);
      assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0&&size<32*1024*1024);
      rewind(file);char *bytes=malloc((size_t)size);assert(bytes);
      assert(fread(bytes,1,(size_t)size,file)==(size_t)size);fclose(file);
      assert(hv_index_parse(&index,bytes,(size_t)size,level));free(bytes);
      assert(index.count>1000&&index.ref_count>0);
      if(i==4) {
        int hull=hv_index_find(&index,"8dc1645e2ed9ee33aa75297e01b94d0c");assert(hull>=0);
        unsigned regions=0;
        for(unsigned j=0;j<index.ref_count;j++)if(index.refs[j].target==hull&&
          !strcmp(index.objects[index.refs[j].source].type,"EventFromRegion"))regions++;
        assert(regions==2);
      }
      printf("%s: %u objects, %u references\n",level,index.count,index.ref_count);
      hv_index_free(&index);
    }
  }
  puts("PASS: projection, near/far clipping, screen tolerance, NaN rejection, bounded index grammar, reverse links, missing targets and level isolation");
  return 0;
}
