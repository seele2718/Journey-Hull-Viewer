#ifndef HV_INDEX_H
#define HV_INDEX_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct hv_object {
  char *hash,*kind,*name,*type,*detail;
} hv_object;
typedef struct hv_reference {
  int source,target;
  char *target_hash,*field;
} hv_reference;
typedef struct hv_index {
  char *buffer;
  hv_object *objects;
  hv_reference *refs;
  unsigned count,ref_count;
} hv_index;
static void hv_index_free(hv_index *index) {
  free(index->buffer);free(index->objects);free(index->refs);memset(index,0,sizeof(*index));
}
static int hv_hash_valid(const char *h) {
  if(!h||strlen(h)!=32)return 0;
  for(int i=0;i<32;i++)if(!((h[i]>='0'&&h[i]<='9')||(h[i]>='a'&&h[i]<='f')))return 0;
  return 1;
}
static int hv_index_find(const hv_index *index,const char *hash) {
  unsigned lo=0,hi=index->count;
  while(lo<hi) {
    unsigned mid=lo+(hi-lo)/2;int compare=strcmp(index->objects[mid].hash,hash);
    if(compare<0)lo=mid+1;else hi=mid;
  }
  return lo<index->count&&!strcmp(index->objects[lo].hash,hash)?(int)lo:-1;
}
static unsigned hv_fields(char *line,char **out,unsigned cap) {
  unsigned count=0;
  for(;;) {
    if(count==cap)return 0;
    out[count++]=line;char *end=strchr(line,'\t');
    if(!end)return count;
    *end=0;line=end+1;
  }
}
/* Parse only the generated data grammar, never Lua/JSON execution. All strings
 * point into one owned, bounded buffer. Replace the old index only on success. */
static int hv_index_parse(hv_index *out,const char *bytes,size_t size,const char *level) {
  hv_index next={0};
  if(!bytes||!size||size>32*1024*1024||memchr(bytes,0,size)||bytes[size-1]!='\n')return 0;
  unsigned objects=0,refs=0;
  for(size_t i=0;i<size;i++)if(i==0||bytes[i-1]=='\n') {
    if(bytes[i]=='O')objects++;
    if(bytes[i]=='R')refs++;
  }
  if(objects>30000||refs>200000)return 0;
  next.buffer=malloc(size+1);next.objects=calloc(objects?objects:1,sizeof(hv_object));
  next.refs=calloc(refs?refs:1,sizeof(hv_reference));
  if(!next.buffer||!next.objects||!next.refs)goto fail;
  memcpy(next.buffer,bytes,size);next.buffer[size]=0;
  char *line=next.buffer,*end=strchr(line,'\n'),*fields[6];
  if(!end)goto fail;
  if(end>line&&end[-1]=='\r')end[-1]=0;
  *end=0;
  if(hv_fields(line,fields,6)!=3||strcmp(fields[0],"HVI1")||strcmp(fields[1],level)||strlen(fields[2])!=64)goto fail;
  line=end+1;
  /* Objects must precede references, and be strictly sorted by hash. */
  while(*line) {
    end=strchr(line,'\n');if(!end||end-line>65535)goto fail;
    if(end>line&&end[-1]=='\r')end[-1]=0;
    *end=0;
    unsigned n=hv_fields(line,fields,6);
    if(n==6&&!strcmp(fields[0],"O")&&next.ref_count==0) {
      if(next.count>=objects||!hv_hash_valid(fields[1]))goto fail;
      if(next.count&&strcmp(next.objects[next.count-1].hash,fields[1])>=0)goto fail;
      next.objects[next.count++]=(hv_object){fields[1],fields[2],fields[3],fields[4],fields[5]};
    }else if(n==4&&!strcmp(fields[0],"R")) {
      if(next.ref_count>=refs||!hv_hash_valid(fields[1])||!hv_hash_valid(fields[2]))goto fail;
      int source=hv_index_find(&next,fields[1]);if(source<0)goto fail;
      next.refs[next.ref_count++]=(hv_reference){source,hv_index_find(&next,fields[2]),fields[2],fields[3]};
    }else goto fail;
    line=end+1;
  }
  hv_index_free(out);*out=next;return 1;
fail:
  hv_index_free(&next);return 0;
}
#endif
