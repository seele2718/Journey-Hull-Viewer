#ifndef HV_JSON_VIEW_H
#define HV_JSON_VIEW_H
/* Bounded, display-only JSON reader. Nothing from an inspection index executes. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
enum { HV_J_OBJECT, HV_J_ARRAY, HV_J_STRING, HV_J_VALUE };
typedef struct hv_json_node {
  unsigned start,end,key_start,key_end;
  int first,next,kind;
} hv_json_node;
typedef struct hv_json {
  const char *text;size_t length,at;
  hv_json_node *nodes;unsigned count;
} hv_json;
static void hv_json_free(hv_json *doc) { free(doc->nodes);memset(doc,0,sizeof(*doc)); }
static void hv_json_space(hv_json *doc) {
  while(doc->at<doc->length&&strchr(" \t\r\n",doc->text[doc->at]))doc->at++;
}
static int hv_json_hex(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
}
static int hv_json_quote(hv_json *doc) {
  if(doc->at>=doc->length||doc->text[doc->at++]!='"')return 0;
  while(doc->at<doc->length) {
    unsigned char c=(unsigned char)doc->text[doc->at++];
    if(c=='"')return 1;
    if(c<32)return 0;
    if(c=='\\') {
      if(doc->at==doc->length)return 0;
      c=(unsigned char)doc->text[doc->at++];
      if(c=='u') {for(int i=0;i<4;i++)if(doc->at==doc->length||hv_json_hex(doc->text[doc->at++])<0)return 0;}
      else if(!strchr("\"\\/bfnrt",c))return 0;
    }
  }
  return 0;
}
static int hv_json_value(hv_json *doc,unsigned depth) {
  hv_json_space(doc);
  if(depth>64||doc->at>=doc->length||doc->count==16384)return -1;
  int index=(int)doc->count++;hv_json_node *node=doc->nodes+index;
  *node=(hv_json_node){.start=(unsigned)doc->at,.first=-1,.next=-1};
  char c=doc->text[doc->at];
  if(c=='{'||c=='[') {
    node->kind=c=='{'?HV_J_OBJECT:HV_J_ARRAY;char close=c=='{'?'}':']';doc->at++;
    hv_json_space(doc);int last=-1;
    if(doc->at<doc->length&&doc->text[doc->at]==close){doc->at++;node->end=(unsigned)doc->at;return index;}
    for(;;) {
      unsigned key_start=0,key_end=0;
      if(c=='{') {
        hv_json_space(doc);key_start=(unsigned)doc->at;
        if(!hv_json_quote(doc))return -1;
        key_end=(unsigned)doc->at;hv_json_space(doc);
        if(doc->at>=doc->length||doc->text[doc->at++]!=':')return -1;
      }
      int child=hv_json_value(doc,depth+1);if(child<0)return -1;
      doc->nodes[child].key_start=key_start;doc->nodes[child].key_end=key_end;
      if(last<0)node->first=child;else doc->nodes[last].next=child;last=child;
      hv_json_space(doc);if(doc->at>=doc->length)return -1;
      if(doc->text[doc->at]==close){doc->at++;break;}
      if(doc->text[doc->at++]!=',')return -1;
    }
  } else if(c=='"') {
    node->kind=HV_J_STRING;if(!hv_json_quote(doc))return -1;
  } else {
    node->kind=HV_J_VALUE;
    if(c=='t'||c=='f'||c=='n') {
      const char *literal=c=='t'?"true":c=='f'?"false":"null";size_t size=strlen(literal);
      if(doc->length-doc->at<size||memcmp(doc->text+doc->at,literal,size))return -1;
      doc->at+=size;
    } else {
      if(c=='-')doc->at++;
      if(doc->at>=doc->length)return -1;
      if(doc->text[doc->at]=='0')doc->at++;
      else {
        if(doc->text[doc->at]<'1'||doc->text[doc->at]>'9')return -1;
        while(doc->at<doc->length&&isdigit((unsigned char)doc->text[doc->at]))doc->at++;
      }
      if(doc->at<doc->length&&doc->text[doc->at]=='.') {
        doc->at++;size_t first=doc->at;
        while(doc->at<doc->length&&isdigit((unsigned char)doc->text[doc->at]))doc->at++;
        if(first==doc->at)return -1;
      }
      if(doc->at<doc->length&&(doc->text[doc->at]=='e'||doc->text[doc->at]=='E')) {
        doc->at++;if(doc->at<doc->length&&(doc->text[doc->at]=='+'||doc->text[doc->at]=='-'))doc->at++;
        size_t first=doc->at;
        while(doc->at<doc->length&&isdigit((unsigned char)doc->text[doc->at]))doc->at++;
        if(first==doc->at)return -1;
      }
    }
  }
  node->end=(unsigned)doc->at;return index;
}
static int hv_json_parse(hv_json *out,const char *text) {
  hv_json doc={.text=text,.length=text?strlen(text):0};
  if(!doc.length||doc.length>65535)return 0;
  doc.nodes=calloc(16384,sizeof(hv_json_node));if(!doc.nodes)return 0;
  if(hv_json_value(&doc,0)!=0){hv_json_free(&doc);return 0;}
  hv_json_space(&doc);if(doc.at!=doc.length){hv_json_free(&doc);return 0;}
  hv_json_free(out);*out=doc;return 1;
}
/* Decode display strings; control escapes stay visible, not layout commands.
 * Unicode escapes are retained verbatim; generated indexes store Unicode UTF-8. */
static void hv_json_display(const hv_json *doc,unsigned start,unsigned end,char *out,size_t cap) {
  if(!cap)return;
  if(end>doc->length||start>end){out[0]=0;return;}
  if(end>start+1&&doc->text[start]=='"'){start++;end--;}
  size_t size=end-start;int shortened=size>=cap;
  if(shortened) {
    size=cap>4?cap-4:cap-1;
    while(size&&((unsigned char)doc->text[start+size]&0xc0)==0x80)size--;
  }
  memcpy(out,doc->text+start,size);out[size]=0;
  if(shortened&&cap>4)strcat(out,"...");
}
static int hv_json_field(const hv_json *doc,int parent,const char *key) {
  if(parent<0||(unsigned)parent>=doc->count)return -1;
  for(int child=doc->nodes[parent].first;child>=0;child=doc->nodes[child].next) {
    const hv_json_node *node=doc->nodes+child;char name[128];
    hv_json_display(doc,node->key_start,node->key_end,name,sizeof(name));
    if(!strcmp(name,key))return child;
  }
  return -1;
}
static char *hv_json_pretty(const char *text) {
  hv_json doc={0};if(!hv_json_parse(&doc,text))return NULL;hv_json_free(&doc);
  size_t cap=1024*1024,at=0;unsigned depth=0;int quoted=0,escaped=0;
  char *out=malloc(cap);if(!out)return NULL;
  for(size_t i=0;text[i];i++) {
    char c=text[i];
    if(quoted) {
      if(at+2>=cap)goto fail;
      out[at++]=c;
      if(escaped)escaped=0;else if(c=='\\')escaped=1;else if(c=='"')quoted=0;
      continue;
    }
    if(strchr(" \r\n\t",c))continue;
    if(c=='"')quoted=1;
    if(c=='}'||c==']') {
      if(depth)depth--;
      if(i&&text[i-1]!='{'&&text[i-1]!='[') {
        if(at+2+depth*2+2>=cap)goto fail;
        out[at++]='\r';out[at++]='\n';for(unsigned k=0;k<depth*2;k++)out[at++]=' ';
      }
    }
    if(at+4>=cap)goto fail;
    out[at++]=c;
    if(c==':')out[at++]=' ';
    if(c=='{'||c=='[')depth++;
    size_t next=i+1;while(text[next]&&strchr(" \r\n\t",text[next]))next++;
    if(c==','||((c=='{'||c=='[')&&text[next]!='}'&&text[next]!=']')) {
      if(at+2+depth*2+2>=cap)goto fail;
      out[at++]='\r';out[at++]='\n';for(unsigned k=0;k<depth*2;k++)out[at++]=' ';
    }
  }
  out[at]=0;return out;
fail:free(out);return NULL;
}
#endif
