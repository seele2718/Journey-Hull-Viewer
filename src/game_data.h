#ifndef HV_GAME_DATA_H
#define HV_GAME_DATA_H
/* External-panel-only, read-only installed-file lookup. Fixed filenames only;
 * no Lua execution, injected commands, writable cache or game-thread file IO. */
#include "gzip_data.h"
#include "lua_data.h"
typedef struct hd_catalog {char **rows;unsigned count,capacity,files,missing;hd_text refs;int ok;} hd_catalog;
static void hd_catalog_free(hd_catalog *c){for(unsigned i=0;i<c->count;i++)free(c->rows[i]);free(c->rows);free(c->refs.p);memset(c,0,sizeof(*c));}
static void hd_clean(hd_text *b,const char *s){for(;*s;s++){char c=*s=='\t'||*s=='\r'||*s=='\n'?' ':*s;hd_addn(b,&c,1);}}
static void hd_object(hd_catalog *c,const char *hash,const char *kind,const char *name,const char *type,hd_text *detail) {
  if(!c->ok||!detail->ok||detail->size>60000||!hv_hash_valid(hash)||c->count==30000){c->ok=0;return;}
  hd_text row={.ok=1};hd_add(&row,"O\t");hd_add(&row,hash);hd_add(&row,"\t");hd_add(&row,kind);hd_add(&row,"\t");
  hd_clean(&row,name);hd_add(&row,"\t");hd_clean(&row,type);hd_add(&row,"\t");hd_add(&row,detail->p);hd_add(&row,"\n");
  if(!row.ok){c->ok=0;free(row.p);return;}
  if(c->count==c->capacity){unsigned cap=c->capacity?c->capacity*2:128;char **p=realloc(c->rows,(size_t)cap*sizeof(*p));
    if(!p){c->ok=0;free(row.p);return;}c->rows=p;c->capacity=cap;}
  c->rows[c->count++]=row.p;
}
static void hd_reference(hd_catalog *c,const char *hash,const char *target,const char *field) {
  if(!hv_hash_valid(target)){c->ok=0;return;}
  hd_add(&c->refs,"R\t");hd_add(&c->refs,hash);hd_add(&c->refs,"\t");hd_add(&c->refs,target);
  hd_add(&c->refs,"\t");hd_clean(&c->refs,field);hd_add(&c->refs,"\n");
}
static void hd_references(hd_catalog *c,const hd_lua *d,int node,const char *hash,const char *prefix) {
  const hd_node *n=d->nodes+node;
  if(n->kind==HD_REF){hd_reference(c,hash,n->value,prefix);return;}
  if(n->kind!=HD_TABLE)return;
  int array=hd_array(d,node);
  for(int i=n->child;i>=0;i=d->nodes[i].next) {
    const hd_node *child=d->nodes+i;char path[2048];
    int size=array?snprintf(path,sizeof(path),"%s[%s]",prefix,child->key):
      snprintf(path,sizeof(path),"%s%s%s",prefix,*prefix?".":"",child->key);
    if(size<0||(size_t)size>=sizeof(path)){c->ok=0;return;}
    if(child->kind==HD_STRING&&!strcmp(child->key,"TriggerName")&&hv_hash_valid(child->value))hd_reference(c,hash,child->value,path);
    else if(child->kind==HD_TABLE&&!strcmp(child->key,"Objects")&&hd_array(d,i)) {
      for(int j=child->child;j>=0;j=d->nodes[j].next)if(d->nodes[j].kind==HD_STRING&&hv_hash_valid(d->nodes[j].value)) {
        char member[2100];snprintf(member,sizeof(member),"%s[%s]",path,d->nodes[j].key);hd_reference(c,hash,d->nodes[j].value,member);
      }
    }else hd_references(c,d,i,hash,path);
  }
}
static int hd_read_file(const wchar_t *path,unsigned char **bytes,size_t *size) {
  DWORD attributes=GetFileAttributesW(path);
  if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND?0:-1;
  if(attributes&FILE_ATTRIBUTE_DIRECTORY)return -1;
  FILE *f=_wfopen(path,L"rb");if(!f)return -1;int result=-1;unsigned char *p=NULL;
  if(!fseek(f,0,SEEK_END)){long n=ftell(f);if(n>0&&n<=64*1024*1024&&!fseek(f,0,SEEK_SET)) {
    p=malloc((size_t)n+1);if(p&&fread(p,1,(size_t)n,f)==(size_t)n){p[n]=0;*bytes=p;*size=(size_t)n;p=NULL;result=1;}
  }}free(p);fclose(f);return result;
}
static int hd_read(const wchar_t *root,const char *level,const char *filename,unsigned char **bytes,size_t *size) {
  wchar_t path[512];int n=_snwprintf(path,512,L"%ls\\Scripts\\%hs\\%hs",root,level,filename);
  if(n<0||n>=512)return -1;
  int result=hd_read_file(path,bytes,size);if(result!=1)return result;
  size_t name_size=strlen(filename);
  if(name_size>=3&&!strcmp(filename+name_size-3,".gz")) {
    size_t length=0;unsigned char *raw=hd_gunzip(*bytes,*size,&length);free(*bytes);*bytes=raw;*size=length;if(!raw)return -1;
  }return 1;
}
static int hd_script(hd_catalog *c,const wchar_t *root,const char *level,const char *table,const char *kind,char error[512]) {
  const char *formats[]={"__%s.lua.gz","__%s.lua","%s.lua.gz","%s.lua"};
  unsigned char *bytes=NULL;size_t size=0;char file[100];int found=0;
  for(unsigned i=0;i<4;i++){snprintf(file,sizeof(file),formats[i],table);found=hd_read(root,level,file,&bytes,&size);if(found)break;}
  if(!found){c->missing++;return 1;}
  if(found<0){snprintf(error,512,"Cannot read game data: %s/%s",level,file);return 0;}
  hd_lua doc={0};int ok=hd_parse(&doc,(const char *)bytes,size,table);
  if(!ok){snprintf(error,512,"Unsupported or invalid Lua DATA table: %s/%s. Scripts are never executed.",level,file);goto done;}
  char source[200];snprintf(source,sizeof(source),"Data/Scripts/%s/%s",level,file);
  unsigned ordinal=1;
  for(int i=doc.nodes[0].child;i>=0;i=doc.nodes[i].next) {
    if(!strcmp(doc.nodes[i].key,"ObjectType"))continue;
    char key[24];snprintf(key,sizeof(key),"%u",ordinal++);
    if(strcmp(key,doc.nodes[i].key)||doc.nodes[i].kind!=HD_TABLE){ok=0;break;}
    const char *hash=hd_string_field(&doc,i,"ObjectName"),*name=hd_string_field(&doc,i,"GardenerName"),*type=hd_string_field(&doc,i,"Type");
    char default_type[80];snprintf(default_type,sizeof(default_type),"%s",table);default_type[strlen(default_type)-9]=0;
    if(hd_field(&doc,i,"Type")<0)type=default_type;
    const char *last=strrchr(name,'|');if(last)name=last+1;if(!*name)name=kind;
    const char *actual_kind=!strcmp(kind,"Creature")&&strcmp(type,"Creature")?"DynamicNode":kind;
    hd_text detail={.ok=1};hd_add(&detail,"{\"source\":");hd_quote(&detail,source);hd_add(&detail,",\"authored\":");hd_json(&doc,i,&detail);hd_add(&detail,"}");
    hd_object(c,hash,actual_kind,name,type,&detail);free(detail.p);
    if(!c->ok){ok=0;break;}hd_references(c,&doc,i,hash,"");
  }
  if(!ok||!c->ok||!c->refs.ok){snprintf(error,512,"Invalid, duplicate or oversized objects in %s/%s",level,file);ok=0;}
  if(ok)c->files++;
done:
  hd_lua_free(&doc);free(bytes);return ok;
}
static uint64_t hd_u64(const unsigned char *p){return hd_u32(p)|(uint64_t)hd_u32(p+4)<<32;}
static int hd_floats(const unsigned char *p,unsigned count,float *out){for(unsigned i=0;i<count;i++){memcpy(out+i,p+i*4,4);if(!isfinite(out[i]))return 0;}return 1;}
static void hd_vector(hd_text *out,const float *v,unsigned count){hd_add(out,"[");for(unsigned i=0;i<count;i++){char number[64];if(i)hd_add(out,",");snprintf(number,sizeof(number),"%.9g",v[i]);hd_add(out,number);}hd_add(out,"]");}
static int hd_hulls(hd_catalog *c,const unsigned char *p,size_t size,const char *source) {
  if(size<16||hd_u64(p)!=16||hd_u64(p+8)!=size)return 0;
  for(size_t at=16;at<size;) {
    if(size-at<208)return 0;
    uint64_t next=hd_u64(p+at+152);if(!next)next=size;
    if(next<at+208||next>size)return 0;
    unsigned triangles=hd_u32(p+at+112),faces=hd_u32(p+at+116),edges=hd_u32(p+at+120),vertices=hd_u32(p+at+124);
    if(!vertices||vertices>256||triangles>100000||edges>100000)return 0;
    uint64_t spans[4]={(uint64_t)triangles*3,faces,(uint64_t)edges*2,(uint64_t)vertices*16};
    for(unsigned k=0;k<4;k++){uint64_t offset=hd_u64(p+at+80+k*8);if(offset<at||offset>next||spans[k]>next-offset)return 0;}
    for(unsigned k=0;k<2;k++){uint64_t offset=hd_u64(p+at+80+k*16);for(uint64_t i=0;i<spans[k*2];i++)if(p[offset+i]>=vertices)return 0;}
    float bounds[6],position[3],point[3];if(!hd_floats(p+at+16,3,bounds)||!hd_floats(p+at+32,3,bounds+3))return 0;
    for(unsigned i=0;i<vertices;i++)if(!hd_floats(p+hd_u64(p+at+104)+i*16,3,point))return 0;
    for(unsigned k=0;k<3;k++){if(bounds[k]>bounds[k+3])return 0;position[k]=bounds[k]*.5f+bounds[k+3]*.5f;}
    char hash[33];memcpy(hash,p+at+160,32);hash[32]=0;
    hd_text detail={.ok=1};hd_add(&detail,"{\"source\":");hd_quote(&detail,source);
    hd_add(&detail,",\"positionNote\":\"Authored hull bounds centre\",\"position\":");hd_vector(&detail,position,3);
    hd_add(&detail,",\"bounds\":[");hd_vector(&detail,bounds,3);hd_add(&detail,",");hd_vector(&detail,bounds+3,3);
    char mask[64];snprintf(mask,sizeof(mask),"],\"mask\":%u}",hd_u32(p+at+132));hd_add(&detail,mask);
    hd_object(c,hash,"Hull","Hull","",&detail);free(detail.p);if(!c->ok)return 0;at=(size_t)next;
  }return 1;
}
static int hd_decorations(hd_catalog *c,const unsigned char *p,size_t size,const char *source) {
  if(size<16||hd_u32(p)!=1)return 0;
  unsigned count=hd_u32(p+4);
  if(count>20000||size!=16+(uint64_t)count*2112)return 0;
  for(unsigned i=0;i<count;i++) {
    const unsigned char *record=p+16+(size_t)i*2112;float matrix[16];if(!hd_floats(record+16,16,matrix))return 0;
    char hash[33],name[65];memcpy(hash,record+112,32);hash[32]=0;memcpy(name,record+176,64);name[64]=0;
    hd_text detail={.ok=1};hd_add(&detail,"{\"source\":");hd_quote(&detail,source);
    hd_add(&detail,",\"positionNote\":\"Authored instance origin, not live state\",\"position\":");hd_vector(&detail,matrix+12,3);hd_add(&detail,"}");
    hd_object(c,hash,"Decoration",name,"",&detail);free(detail.p);if(!c->ok)return 0;
  }return 1;
}
static int hd_row_compare(const void *a,const void *b){return strncmp(*(char *const *)a+2,*(char *const *)b+2,32);}
static int hd_load_game(hv_index *out,const wchar_t *root,const char *level,char error[512],unsigned *files,unsigned *missing) {
  static const char *tables[]={"Trigger","Clump","Rail","Marker","Timeline","DynamicNode","ParticleEmitter","SoundEmitter","CameraKeyFrame","EnvNode","Jet","Sign"};
  static const char *kinds[]={"Trigger","Clump","Rail","Marker","Timeline","Creature","Emitter","Sound","Camera","Environment","Jet","Sign"};
  hd_catalog c={.refs={.ok=1},.ok=1};int ok=0;error[0]=0;
  for(unsigned i=0;i<12;i++){char table[80];snprintf(table,sizeof(table),"%sInstances",tables[i]);if(!hd_script(&c,root,level,table,kinds[i],error))goto done;}
  const char *names[]={"HullInstances.lua.bin","DecorationMeshInstances.lua.bin"};
  for(unsigned i=0;i<2;i++) {
    unsigned char *bytes=NULL;size_t size=0;int found=hd_read(root,level,names[i],&bytes,&size);
    if(!found){c.missing++;continue;}char source[200];snprintf(source,sizeof(source),"Data/Scripts/%s/%s",level,names[i]);
    int valid=found>0&&(i?hd_decorations(&c,bytes,size,source):hd_hulls(&c,bytes,size,source));free(bytes);
    if(!valid){snprintf(error,512,"Unsupported or invalid game data: %s",source);goto done;}c.files++;
  }
  if(!c.count||!c.ok||!c.refs.ok){lstrcpyA(error,"No usable authored objects in the game's Data folder.");goto done;}
  qsort(c.rows,c.count,sizeof(*c.rows),hd_row_compare);
  hd_text all={.ok=1};hd_add(&all,"HVI1\t");hd_add(&all,level);hd_add(&all,"\t0000000000000000000000000000000000000000000000000000000000000000\n");
  for(unsigned i=0;i<c.count;i++)hd_add(&all,c.rows[i]);
  hd_addn(&all,c.refs.p?c.refs.p:"",c.refs.size);
  ok=all.ok&&hv_index_parse(out,all.p,all.size,level);free(all.p);
  if(!ok)lstrcpyA(error,"Game-file lookup exceeds limits or contains duplicate object hashes.");
done:
  *files=c.files;*missing=c.missing;hd_catalog_free(&c);return ok;
}
#endif
