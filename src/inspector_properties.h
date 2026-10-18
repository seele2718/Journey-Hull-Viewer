#include "json_view.h"
static BOOL g_hide_zero_false;
/* Display emphasis, not runtime activity or a comparison with game defaults.
 * Read the mantissa directly: -0 and 0e10 are zero, 1e-999 stays non-zero. */
static int property_emphasis(const hv_json *doc,int index) {
  const hv_json_node *node=doc->nodes+index;
  if(node->kind!=HV_J_VALUE)return 0;
  const char *s=doc->text+node->start;unsigned n=node->end-node->start;
  if(n==4&&!memcmp(s,"true",4))return 1;
  if(n==5&&!memcmp(s,"false",5))return -1;
  if(!n||(*s!='-'&&(*s<'0'||*s>'9')))return 0;
  for(unsigned i=0;i<n&&s[i]!='e'&&s[i]!='E';i++)if(s[i]>='1'&&s[i]<='9')return 1;
  return -1;
}
static void property_style(HTREEITEM item,int emphasis) {
  TVITEMW value={.mask=TVIF_STATE,.hItem=item,.stateMask=TVIS_BOLD|TVIS_CUT,
    .state=emphasis>0?TVIS_BOLD:emphasis<0?TVIS_CUT:0};
  TreeView_SetItem(GetDlgItem(g_window,ID_PROPERTIES),&value);
}
static LRESULT property_draw(NMTVCUSTOMDRAW *draw) {
  if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
  if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT&&!(draw->nmcd.uItemState&CDIS_SELECTED)) {
    TVITEMW item={.mask=TVIF_STATE,.hItem=(HTREEITEM)draw->nmcd.dwItemSpec,.stateMask=TVIS_BOLD|TVIS_CUT};
    if(TreeView_GetItem(GetDlgItem(g_window,ID_PROPERTIES),&item))
      draw->clrText=(item.state&TVIS_BOLD)?RGB(37,76,154):(item.state&TVIS_CUT)?RGB(112,120,132):RGB(35,49,70);
  }
  return CDRF_DODEFAULT;
}
/* Some authored fields (Clump.Objects and timeline TriggerName) store bare hashes. Only
 * treat them as links when the current object's reference table confirms it. */
static int property_reference_value(const hv_json *doc,int index,char hash[33]) {
  if(doc->nodes[index].kind!=HV_J_STRING||g_object<0)return FALSE;
  if(doc->nodes[index].end-doc->nodes[index].start!=34)return FALSE;
  hv_json_display(doc,doc->nodes[index].start,doc->nodes[index].end,hash,33);
  if(!hv_hash_valid(hash))return FALSE;
  for(unsigned i=0;i<g_index.ref_count;i++)
    if(g_index.refs[i].source==g_object&&!strcmp(g_index.refs[i].target_hash,hash))return TRUE;
  return FALSE;
}
static HTREEITEM property_item(HTREEITEM parent,const char *text,LPARAM target) {
  wchar_t wide[2200];int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,wide,2200);
  if(!n)lstrcpyW(wide,L"See Raw Data for the full value.");
  TVINSERTSTRUCTW insert={.hParent=parent,.hInsertAfter=TVI_LAST,
    .item={.mask=TVIF_TEXT|TVIF_PARAM,.pszText=wide,.lParam=target}};
  return TreeView_InsertItem(GetDlgItem(g_window,ID_PROPERTIES),&insert);
}
static void property_node(const hv_json *doc,int index,HTREEITEM parent,unsigned ordinal) {
  const hv_json_node *node=doc->nodes+index;
  int emphasis=property_emphasis(doc,index);
  if(g_hide_zero_false&&emphasis<0)return;
  char key[300],value[1500],label[1900];
  if(node->key_end)hv_json_display(doc,node->key_start,node->key_end,key,sizeof(key));
  else snprintf(key,sizeof(key),"[%u]",ordinal);
  int ref=hv_json_field(doc,index,"$ref");
  if(ref>=0&&doc->nodes[ref].next<0) {
    const hv_json_node *target=doc->nodes+ref;
    hv_json_display(doc,target->start,target->end,value,sizeof(value));
    snprintf(label,sizeof(label),"%s  →  %s",key,value);
    int object=hv_hash_valid(value)?hv_index_find(&g_index,value):-1;
    property_item(parent,label,object>=0?(LPARAM)object+1:0);return;
  }
  if(node->kind==HV_J_ARRAY||node->kind==HV_J_OBJECT) {
    unsigned count=0;BOOL compact=node->kind==HV_J_ARRAY;
    for(int child=node->first;child>=0;child=doc->nodes[child].next) {
      count++;if(doc->nodes[child].kind==HV_J_ARRAY||doc->nodes[child].kind==HV_J_OBJECT)compact=FALSE;
      char hash[33];if(property_reference_value(doc,child,hash))compact=FALSE;
    }
    if(compact&&count<=4&&node->end-node->start<200) {
      hv_json_display(doc,node->start,node->end,value,sizeof(value));
      snprintf(label,sizeof(label),"%s:  %s",key,value);property_item(parent,label,0);return;
    }
    snprintf(label,sizeof(label),"%s  ·  %u %s",!strcmp(key,"authored")?"Game File Properties":key,
      count,node->kind==HV_J_ARRAY?"items":"fields");
    HTREEITEM item=property_item(parent,label,0);unsigned i=1;
    for(int child=node->first;child>=0;child=doc->nodes[child].next)property_node(doc,child,item,i++);
    if(!strcmp(key,"authored")||!strcmp(key,"Vars"))TreeView_Expand(GetDlgItem(g_window,ID_PROPERTIES),item,TVE_EXPAND);
  } else {
    char hash[33];
    if((!node->key_end||!strcmp(key,"TriggerName"))&&property_reference_value(doc,index,hash)) {
      int object=hv_index_find(&g_index,hash);
      snprintf(label,sizeof(label),"%s  →  %s%s",key,hash,object<0?" (not found in loaded data)":"");
      property_item(parent,label,object>=0?(LPARAM)object+1:0);return;
    }
    hv_json_display(doc,node->start,node->end,value,sizeof(value));
    snprintf(label,sizeof(label),"%s:  %s",!strcmp(key,"source")?"Data Source":key,
      !strcmp(key,"positionNote")&&!strcmp(value,"Authored hull bounds centre")?"Bounds Center":value);
    property_style(property_item(parent,label,0),emphasis);
  }
}
static void property_finish(HWND tree) {
  SendMessageW(tree,WM_SETREDRAW,TRUE,0);
  /* WM_SETREDRAW(TRUE) may add WS_VISIBLE; preserve the selected data view. */
  ShowWindow(tree,inspector_view()==0?SW_SHOW:SW_HIDE);
  InvalidateRect(tree,NULL,TRUE);
}
static void inspect_properties(const hv_object *object) {
  inspector_copy_clear();
  HWND tree=GetDlgItem(g_window,ID_PROPERTIES);SendMessageW(tree,WM_SETREDRAW,FALSE,0);TreeView_DeleteAllItems(tree);
  SetWindowTextW(GetDlgItem(g_window,ID_RAW),L"");
  if(!object){property_finish(tree);return;}
  for(unsigned i=0;i<g_hits_count;i++)if(!strcmp(g_hits[i].name,object->hash)) {
    const hv_pick_hit *hit=g_hits+i;char row[256];
    HTREEITEM snapshot=property_item(TVI_ROOT,"Runtime State · captured when picked",0);
    snprintf(row,sizeof(row),"State:  %s",hit->active?"Active":"Inactive");property_item(snapshot,row,0);
    snprintf(row,sizeof(row),"Hull Type Bits:  %u",hit->mask);property_item(snapshot,row,0);
    snprintf(row,sizeof(row),"Bounds min:  %.6g, %.6g, %.6g",hit->bounds[0],hit->bounds[1],hit->bounds[2]);property_item(snapshot,row,0);
    snprintf(row,sizeof(row),"Bounds max:  %.6g, %.6g, %.6g",hit->bounds[3],hit->bounds[4],hit->bounds[5]);property_item(snapshot,row,0);
    TreeView_Expand(tree,snapshot,TVE_EXPAND);break;
  }
  hv_json doc={0};
  if(hv_json_parse(&doc,object->detail)) {
    HTREEITEM metadata=property_item(TVI_ROOT,"Source Information",0);
    for(int child=doc.nodes[0].first;child>=0;child=doc.nodes[child].next) {
      const hv_json_node *node=doc.nodes+child;char key[128];
      hv_json_display(&doc,node->key_start,node->key_end,key,sizeof(key));
      BOOL meta=!strcmp(key,"source")||!strcmp(key,"positionNote");
      property_node(&doc,child,meta?metadata:TVI_ROOT,0);
    }
    hv_json_free(&doc);
  } else property_item(TVI_ROOT,"Properties unavailable. See Raw Data.",0);
  char *pretty=hv_json_pretty(object->detail);
  inspect_text(GetDlgItem(g_window,ID_RAW),pretty?pretty:object->detail);free(pretty);
  property_finish(tree);
}
