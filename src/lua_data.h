#ifndef HV_LUA_DATA_H
#define HV_LUA_DATA_H
/* A generated-table DATA grammar, not a Lua VM. Calls, operators, bytecode,
 * globals and statements other than a single table assignment are rejected. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct hd_text {char *p;size_t size,cap;int ok;} hd_text;
static void hd_addn(hd_text *b,const char *s,size_t n) {
  if(!b->ok)return;
  if(n>32u*1024u*1024u-b->size){b->ok=0;return;}
  if(b->size+n+1>b->cap){size_t cap=b->cap?b->cap:256;while(cap<b->size+n+1)cap*=2;
    char *p=realloc(b->p,cap);if(!p){b->ok=0;return;}b->p=p;b->cap=cap;}
  memcpy(b->p+b->size,s,n);b->size+=n;b->p[b->size]=0;
}
static void hd_add(hd_text *b,const char *s){hd_addn(b,s,strlen(s));}
static void hd_quote(hd_text *b,const char *s) {
  hd_add(b,"\"");for(;*s;s++) {
    unsigned char c=(unsigned char)*s;
    if(c=='"'||c=='\\'){hd_add(b,"\\");hd_addn(b,s,1);}
    else if(c<32){char escape[8];snprintf(escape,sizeof(escape),"\\u%04x",c);hd_add(b,escape);}
    else hd_addn(b,s,1);
  }hd_add(b,"\"");
}
enum {HD_TABLE,HD_STRING,HD_SCALAR,HD_REF};
typedef struct hd_node {int kind,child,next;char *key,*value;} hd_node;
typedef struct hd_lua {const char *p,*end;hd_node *nodes;unsigned count,cap;size_t strings;int ok;} hd_lua;
static void hd_lua_free(hd_lua *d){for(unsigned i=0;i<d->count;i++){free(d->nodes[i].key);free(d->nodes[i].value);}free(d->nodes);memset(d,0,sizeof(*d));}
static void hd_space(hd_lua *d) {
  while(d->p<d->end) {
    if(isspace((unsigned char)*d->p)){d->p++;continue;}
    if(d->end-d->p>=2&&!memcmp(d->p,"--",2)) {
      d->p+=2;
      if(d->end-d->p>=2&&!memcmp(d->p,"[[",2)){d->p+=2;while(d->end-d->p>=2&&memcmp(d->p,"]]",2))d->p++;
        if(d->end-d->p<2){d->ok=0;return;}d->p+=2;
      }else while(d->p<d->end&&*d->p!='\n')d->p++;
      continue;
    }break;
  }
}
static int hd_take(hd_lua *d,char c){hd_space(d);if(!d->ok||d->p==d->end||*d->p!=c){d->ok=0;return 0;}d->p++;return 1;}
static char *hd_copy(hd_lua *d,const char *p,size_t n) {
  if(n+1>64u*1024u*1024u-d->strings){d->ok=0;return NULL;}
  char *s=malloc(n+1);if(!s){d->ok=0;return NULL;}memcpy(s,p,n);s[n]=0;d->strings+=n+1;return s;
}
static char *hd_identifier(hd_lua *d) {
  hd_space(d);const char *p=d->p;
  if(p==d->end||!(isalpha((unsigned char)*p)||*p=='_')){d->ok=0;return NULL;}
  d->p++;while(d->p<d->end&&(isalnum((unsigned char)*d->p)||*d->p=='_'))d->p++;
  return hd_copy(d,p,(size_t)(d->p-p));
}
static char *hd_string(hd_lua *d) {
  hd_space(d);if(d->p==d->end||(*d->p!='"'&&*d->p!='\'')){d->ok=0;return NULL;}
  char quote=*d->p++;hd_text text={.ok=1};int closed=0;
  while(d->p<d->end) {
    unsigned char c=(unsigned char)*d->p++;
    if(c==(unsigned char)quote){closed=1;break;}
    if(c=='\\') {
      if(d->p==d->end)break;
      c=(unsigned char)*d->p++;
      if(isdigit(c)){unsigned n=c-'0';for(unsigned i=0;i<2&&d->p<d->end&&isdigit((unsigned char)*d->p);i++)n=n*10+(*d->p++-'0');
        if(n>255||!n){text.ok=0;break;}c=(unsigned char)n;
      }else {const char *from="nrtabfv\\\"'",*to="\n\r\t\a\b\f\v\\\"'",*at=strchr(from,c);
        if(!at){text.ok=0;break;}c=(unsigned char)to[at-from];}
    }else if(c<32){text.ok=0;break;}
    char ch=(char)c;hd_addn(&text,&ch,1);
  }
  if(!closed||!text.ok){d->ok=0;free(text.p);return NULL;}
  char *result=hd_copy(d,text.p?text.p:"",text.size);free(text.p);return result;
}
static int hd_new(hd_lua *d,int kind) {
  if(!d->ok||d->count>=500000){d->ok=0;return -1;}
  if(d->count==d->cap){unsigned cap=d->cap?d->cap*2:128;hd_node *p=realloc(d->nodes,(size_t)cap*sizeof(*p));
    if(!p){d->ok=0;return -1;}d->nodes=p;d->cap=cap;}
  int i=(int)d->count++;d->nodes[i]=(hd_node){.kind=kind,.child=-1,.next=-1};return i;
}
static int hd_value(hd_lua *d,unsigned depth) {
  hd_space(d);if(!d->ok||d->p==d->end||depth>80){d->ok=0;return -1;}
  int node=hd_new(d,HD_SCALAR);if(node<0)return -1;
  if(*d->p=='{') {
    d->p++;d->nodes[node].kind=HD_TABLE;int last=-1;unsigned ordinal=1;
    for(;;) {
      hd_space(d);if(!d->ok||d->p==d->end){d->ok=0;break;}if(*d->p=='}'){d->p++;break;}
      char *key=NULL;
      if(*d->p=='['){d->p++;hd_space(d);
        if(d->p<d->end&&(*d->p=='"'||*d->p=='\''))key=hd_string(d);
        else {const char *s=d->p;while(d->p<d->end&&isdigit((unsigned char)*d->p))d->p++;if(s==d->p)d->ok=0;else key=hd_copy(d,s,(size_t)(d->p-s));}
        hd_take(d,']');hd_take(d,'=');
      }else if(isalpha((unsigned char)*d->p)||*d->p=='_') {
        const char *start=d->p;key=hd_identifier(d);hd_space(d);
        if(d->p==d->end||*d->p!='='){free(key);key=NULL;d->p=start;}else d->p++;
      }
      int child=hd_value(d,depth+1);if(child<0){free(key);break;}
      if(!key){char number[24];snprintf(number,sizeof(number),"%u",ordinal++);key=hd_copy(d,number,strlen(number));}
      for(int prior=d->nodes[node].child;prior>=0;prior=d->nodes[prior].next)
        if(key&&d->nodes[prior].key&&!strcmp(key,d->nodes[prior].key)){d->ok=0;break;}
      d->nodes[child].key=key;
      if(last<0)d->nodes[node].child=child;else d->nodes[last].next=child;last=child;
      hd_space(d);if(!d->ok||d->p==d->end){d->ok=0;break;}
      if(*d->p==','||*d->p==';'){d->p++;continue;}if(*d->p!='}'){d->ok=0;break;}
    }
  }else if(*d->p=='"'||*d->p=='\'') {d->nodes[node].kind=HD_STRING;d->nodes[node].value=hd_string(d);}
  else if(*d->p=='-'||*d->p=='.'||isdigit((unsigned char)*d->p)) {
    const char *start=d->p;char *end=NULL;double n=strtod(start,&end);
    if(end==start||end>d->end||!isfinite(n)){d->ok=0;return node;}
    d->p=end;char number[64];snprintf(number,sizeof(number),"%.17g",n);d->nodes[node].value=hd_copy(d,number,strlen(number));
  }else {
    char *id=hd_identifier(d);
    if(id&&!strcmp(id,"Names")){d->nodes[node].kind=HD_REF;hd_take(d,'[');d->nodes[node].value=hd_string(d);hd_take(d,']');}
    else if(id&&(!strcmp(id,"true")||!strcmp(id,"false")||!strcmp(id,"nil")))d->nodes[node].value=hd_copy(d,!strcmp(id,"nil")?"null":id,!strcmp(id,"nil")?4:strlen(id));
    else d->ok=0;
    free(id);
  }
  return node;
}
static int hd_field(const hd_lua *d,int node,const char *key) {
  if(node<0||d->nodes[node].kind!=HD_TABLE)return -1;
  for(int i=d->nodes[node].child;i>=0;i=d->nodes[i].next)if(!strcmp(d->nodes[i].key,key))return i;
  return -1;
}
static const char *hd_string_field(const hd_lua *d,int node,const char *key) {
  int at=hd_field(d,node,key);return at>=0&&d->nodes[at].kind==HD_STRING?d->nodes[at].value:"";
}
static int hd_array(const hd_lua *d,int node) {
  if(d->nodes[node].child<0)return 0;
  unsigned ordinal=1;
  for(int i=d->nodes[node].child;i>=0;i=d->nodes[i].next){char key[24];snprintf(key,sizeof(key),"%u",ordinal++);if(strcmp(key,d->nodes[i].key))return 0;}return 1;
}
static void hd_json(const hd_lua *d,int node,hd_text *out) {
  const hd_node *n=d->nodes+node;
  if(n->kind==HD_STRING)hd_quote(out,n->value?n->value:"");
  else if(n->kind==HD_REF){hd_add(out,"{\"$ref\":");hd_quote(out,n->value?n->value:"");hd_add(out,"}");}
  else if(n->kind==HD_SCALAR)hd_add(out,n->value?n->value:"null");
  else {int array=hd_array(d,node),first=1;hd_add(out,array?"[":"{");
    for(int i=n->child;i>=0;i=d->nodes[i].next){if(!first)hd_add(out,",");first=0;
      if(!array){hd_quote(out,d->nodes[i].key);hd_add(out,":");}hd_json(d,i,out);}
    hd_add(out,array?"]":"}");}
}
static int hd_parse(hd_lua *d,const char *bytes,size_t size,const char *name) {
  memset(d,0,sizeof(*d));d->p=bytes;d->end=bytes+size;d->ok=1;
  if(memchr(bytes,0,size)){d->ok=0;return 0;}
  if(size>=3&&!memcmp(bytes,"\xef\xbb\xbf",3))d->p+=3;
  char *id=hd_identifier(d);if(!id||strcmp(id,name))d->ok=0;free(id);
  hd_take(d,'=');int root=hd_value(d,0);hd_space(d);
  if(d->p<d->end&&*d->p==';'){d->p++;hd_space(d);}
  if(root!=0||d->p!=d->end||strcmp(hd_string_field(d,root,"ObjectType"),name))d->ok=0;
  return d->ok;
}
#endif
