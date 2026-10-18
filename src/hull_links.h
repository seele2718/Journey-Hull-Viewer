#ifndef HV_HULL_LINKS_H
#define HV_HULL_LINKS_H
#include "index.h"
/* Authored relationships only. A path does not prove runtime activation.
 * Work is bounded and runs in the panel on selection, never in Journey. */
#define HV_HULL_PATH_DEPTH 12
#define HV_HULL_PATH_LIMIT 64
enum { HV_HULL_MEMBERS, HV_HULL_REFERENCES, HV_HULL_SOURCES };
typedef struct hv_hull_path {
  int object[HV_HULL_PATH_DEPTH+1], edge[HV_HULL_PATH_DEPTH];
  unsigned char reverse[HV_HULL_PATH_DEPTH];
  unsigned length;
} hv_hull_path;
typedef struct hv_hull_paths {
  hv_hull_path paths[HV_HULL_PATH_LIMIT];
  unsigned count, limited, missing;
  int ok;
} hv_hull_paths;
static int hv_link_control(const hv_object *o) {
  return !strcmp(o->kind,"Trigger")||!strcmp(o->kind,"Timeline")||!strcmp(o->kind,"Clump");
}
static int hv_link_member(const hv_reference *r) {
  return !strncmp(r->field,"Objects[",8);
}
/* One shortest path per hull. Sources follows incoming control references,
 * then EventFromRegion.Vars.hulls and Clump.Objects toward its region hulls.
 * It never crosses a creature/marker as a shortcut between unrelated triggers. */
static void hv_find_hulls(const hv_index *index,int root,int mode,hv_hull_paths *out) {
  memset(out,0,sizeof(*out));
  if(root<0||(unsigned)root>=index->count||index->count>30000||index->ref_count>200000)return;
  unsigned n=index->count,states=n*2;
  int *heads=malloc(n*2*sizeof(int)),*next=malloc((index->ref_count?index->ref_count:1)*2*sizeof(int));
  int *queue=malloc(states*sizeof(int)),*parent=malloc(states*sizeof(int)),*via=malloc(states*sizeof(int));
  unsigned char *depth=calloc(states,1),*direction=calloc(states,1),*found=calloc(n,1);
  if(!heads||!next||!queue||!parent||!via||!depth||!direction||!found)goto done;
  for(unsigned i=0;i<n*2;i++)heads[i]=parent[i]=-1;
  for(unsigned i=0;i<index->ref_count;i++) {
    const hv_reference *r=index->refs+i;
    if(r->source<0||(unsigned)r->source>=n)continue;
    next[2*i]=heads[r->source];heads[r->source]=(int)i;
    if(r->target>=0&&(unsigned)r->target<n) {next[2*i+1]=heads[n+r->target];heads[n+r->target]=(int)i;}
  }
  unsigned read=0,write=1;queue[0]=root;parent[root]=root;
  while(read<write) {
    int state=queue[read++],object=state%(int)n,phase=state/(int)n;
    const hv_object *o=index->objects+object;
    if(!strcmp(o->kind,"Hull")) {
      if(found[object])continue;
      found[object]=1;
      if(out->count==HV_HULL_PATH_LIMIT){out->limited=1;continue;}
      hv_hull_path *p=out->paths+out->count++;p->length=depth[state];
      for(int at=state,k=(int)p->length;k>=0;k--) {
        p->object[k]=at%(int)n;
        if(k){p->edge[k-1]=via[at];p->reverse[k-1]=direction[at];at=parent[at];}
      }continue;
    }
    if(object!=root&&!hv_link_control(o))continue;
    for(int reverse=0;reverse<=1;reverse++) {
      if(reverse&&(mode!=HV_HULL_SOURCES||phase))continue;
      for(int e=heads[reverse?(int)n+object:object];e>=0;e=next[2*e+reverse]) {
        const hv_reference *r=index->refs+e;int target=reverse?r->source:r->target,target_phase=phase;
        if(mode==HV_HULL_MEMBERS&& (strcmp(o->kind,"Clump")||!hv_link_member(r)))continue;
        if(mode==HV_HULL_SOURCES&&!reverse) {
          if(!phase) {
            if(strcmp(o->type,"EventFromRegion")||strcmp(r->field,"Vars.hulls"))continue;
            target_phase=1;
          } else if(strcmp(o->kind,"Clump")||!hv_link_member(r))continue;
        }
        if(target<0||(unsigned)target>=n){out->missing++;continue;}
        const hv_object *t=index->objects+target;
        if(reverse&&!hv_link_control(t))continue;
        if((mode==HV_HULL_MEMBERS||target_phase)&&strcmp(t->kind,"Hull")&&strcmp(t->kind,"Clump"))continue;
        if(!reverse&&strcmp(t->kind,"Hull")&&!hv_link_control(t))continue;
        int to=target+target_phase*(int)n;if(parent[to]>=0)continue;
        if(depth[state]==HV_HULL_PATH_DEPTH){out->limited=1;continue;}
        parent[to]=state;via[to]=e;direction[to]=(unsigned char)reverse;depth[to]=depth[state]+1;
        queue[write++]=to;
      }
    }
  }
  out->ok=1;
done:
  free(heads);free(next);free(queue);free(parent);free(via);free(depth);free(direction);free(found);
}
#endif
