/* Included after panel helpers; UI-only file IO and link traversal never run
 * on a game callback. No raw game pointers cross this boundary. */
#include "index.h"
#include "game_data.h"
#include "hull_links.h"
enum { ID_PICK=1000,ID_HITS,ID_PICK_NOTE,ID_OBJECT,ID_INCOMING,ID_OUTGOING,
       ID_BACK,ID_FORWARD,ID_COPY_HASH,ID_SHOW_HULL,ID_INSPECT_LEVEL,ID_RELOAD_INDEX,
       ID_PROPERTIES,ID_RAW,ID_DATA_TABS,ID_PROVENANCE,ID_IN_LABEL,ID_OUT_LABEL,
       ID_HITS_LABEL,ID_INSPECT_HINT,ID_HULL_LINKS,ID_VALUE_FOCUS,ID_VALUE_NOTE,
       ID_LOOKUP_HASH,ID_LOOKUP_OPEN,ID_TEXT_SELECTION };
#include "inspector_copy.h"
static void inspector_summary_border(HWND window,HDC dc) {
  /* Parent-owned flat outline: independent of native Edit theme/style changes.
   * Paint outside the child, so WS_CLIPCHILDREN cannot clip away this border. */
  const int ids[]={ID_OBJECT,ID_NAMES,ID_LOOKUP_HASH,ID_HITS,ID_PROPERTIES,ID_RAW,ID_INCOMING,ID_OUTGOING,ID_HULL_LINKS,ID_CAM_TARGET,ID_OPACITY};
  for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++) {
    HWND child=GetDlgItem(window,ids[i]);RECT rect;
    if(!child||!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)||!GetWindowRect(child,&rect))continue;
    MapWindowPoints(NULL,window,(POINT *)&rect,2);
    Rectangle(dc,rect.left-1,rect.top-1,rect.right+1,rect.bottom+1);
  }
}
static hv_index g_index;
static const char *g_level_ids[]={"Level_Graveyard","Level_Barrens","Level_Desert","Level_Canyon",
  "Level_Cave","Level_Ruins","Level_Mountain","Level_Summit","Level_Credits","Level_Bryan","Level_Matt","Level_Chris"};
static const wchar_t *g_level_titles[]={L"Chapter Select",L"Broken Bridge",L"Pink Desert",L"Sunken City",L"Underground",
  L"Tower",L"Snow",L"Paradise",L"Level_Credits",L"Level_Bryan",L"Level_Matt",L"Level_Chris"};
static uint32_t g_index_level=UINT32_MAX,g_inspect_request;
static int g_object=-1;
static char g_inspected_hash[33];
static char g_history[64][33];static unsigned g_history_count;
static char g_forward[64][33];static unsigned g_forward_count;
static hv_pick_hit g_hits[HV_PICK_HITS];static unsigned g_hits_count;
static BOOL g_pick_enabled,g_mouse_down;
static BOOL g_highlight_pending;
static char g_highlight_wanted[48];static uint32_t g_highlight_level;
static char g_index_error[512];
static BOOL g_index_from_game;
static hv_hull_paths g_member_hulls;
static int g_links_object=-2;
static void inspect_hull_links(void);
static void inspector_data_view(void);
static int inspector_view(void){return (int)SendMessageW(GetDlgItem(g_window,ID_DATA_TABS),CB_GETCURSEL,0,0);}
static void inspector_set_view(int view){SendMessageW(GetDlgItem(g_window,ID_DATA_TABS),CB_SETCURSEL,view,0);}
static int inspector_action_width(void) {
  HWND button=GetDlgItem(g_window,ID_SHOW_HULL);wchar_t text[64];GetWindowTextW(button,text,64);
  HDC dc=GetDC(button);HGDIOBJ old=SelectObject(dc,g_button_font);SIZE size={0};
  GetTextExtentPoint32W(dc,text,lstrlenW(text),&size);SelectObject(dc,old);ReleaseDC(button,dc);
  int width=MulDiv(size.cx,96,(int)g_dpi)+24;return width<80?80:width>100?100:width;
}
static void inspect_hull_action(void) {
  memset(&g_member_hulls,0,sizeof(g_member_hulls));g_links_object=-2;
  BOOL clump=g_object>=0&&!strcmp(g_index.objects[g_object].kind,"Clump");
  wchar_t label[64];lstrcpyW(label,L"Show This Hull");BOOL enabled=g_inspected_hash[0]!=0;
  if(clump) {
    hv_find_hulls(&g_index,g_object,HV_HULL_MEMBERS,&g_member_hulls);
    _snwprintf(label,64,L"Show Hulls (%u)",g_member_hulls.count);
    enabled=g_member_hulls.ok&&g_member_hulls.count&&!g_member_hulls.limited&&!g_member_hulls.missing;
  } else if(g_object>=0&&strcmp(g_index.objects[g_object].kind,"Hull"))lstrcpyW(label,L"Hull Links");
  SetWindowTextW(GetDlgItem(g_window,ID_SHOW_HULL),label);EnableWindow(GetDlgItem(g_window,ID_SHOW_HULL),enabled);
  /* The caption changes with object kind/count; keep its content padding. */
  ui_move(ID_SHOW_HULL,g_sidebar_width+214,136,inspector_action_width(),24);
  RedrawWindow(g_window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
  if(inspector_view()==4)inspect_hull_links();
}
static int inspect_data_root(wchar_t root[MAX_PATH]) {
  wchar_t path[MAX_PATH];DWORD length=MAX_PATH;
  for(unsigned choice=0;choice<2;choice++) {
    BOOL found=choice?(GetModuleFileNameW(NULL,path,MAX_PATH)>0):
      (g_connection.process&&QueryFullProcessImageNameW(g_connection.process,0,path,&length));
    if(!found)continue;
    path[MAX_PATH-1]=0;
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash||slash-path>MAX_PATH-16)continue;
    lstrcpyW(slash,L"\\Data\\Scripts");DWORD attributes=GetFileAttributesW(path);
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)) {
      slash=wcsrchr(path,L'\\');*slash=0;lstrcpyW(root,path);return 1;
    }
  }return 0;
}
enum { WM_INSPECT_NAVIGATE=WM_APP+2 };
static uint64_t g_index_epoch;
static struct {BOOL pending;char hash[33];uint32_t level;uint64_t epoch;} g_navigation;
static void inspect_queue_navigation(const char *hash) {
  if(!hv_hash_valid(hash))return;
  BOOL already=g_navigation.pending;
  lstrcpynA(g_navigation.hash,hash,33);g_navigation.level=g_index_level;
  g_navigation.epoch=g_index_epoch;g_navigation.pending=TRUE;
  if(!already&&!PostMessageW(g_window,WM_INSPECT_NAVIGATE,0,0)) {
    g_navigation.pending=FALSE;notice(L"Could not open the linked object. Please try again.");
  }
}
static void inspect_lookup_hash(void) {
  wchar_t text[128];char hash[33];
  GetWindowTextW(GetDlgItem(g_window,ID_LOOKUP_HASH),text,128);
  unsigned start=0,end=(unsigned)wcslen(text);
  while(start<end&&iswspace(text[start]))start++;
  while(end>start&&iswspace(text[end-1]))end--;
  if(end-start!=32){notice(L"Enter one 32-character hash using 0–9 and a–f.");return;}
  for(unsigned i=0;i<32;i++) {
    wchar_t c=text[start+i];if(c>=L'A'&&c<=L'F')c+=L'a'-L'A';
    if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f'))) {
      notice(L"Enter one 32-character hash using 0–9 and a–f.");return;
    }hash[i]=(char)c;
  }hash[32]=0;
  if(hv_index_find(&g_index,hash)<0){notice(L"Object not found in this level.");return;}
  inspect_queue_navigation(hash);notice(L"Opening object properties.");
}

static void inspect_text(HWND control,const char *text) {
  int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0);
  if(count<=0||count>1048576){SetWindowTextW(control,L"Text is invalid or too large.");return;}
  wchar_t *wide=malloc((size_t)count*sizeof(wchar_t));if(!wide)return;
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,wide,count);
  SetWindowTextW(control,wide);free(wide);
  if(GetDlgCtrlID(control)==ID_OBJECT)ui_name_scrollbars(control);
}
#include "inspector_properties.h"
static void inspect_flush_highlight(void) {
  if(!g_highlight_pending||!g_connection.shared)return;
  DWORD lock=WaitForSingleObject(g_connection.mutex,0);
  if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED)return;
  g_connection.shared->highlight[0]=0;
  g_connection.shared->highlight_level=g_highlight_level;
  lstrcpynA(g_connection.shared->highlight,g_highlight_wanted,48);
  g_connection.shared->inspection_enabled=g_pick_enabled||g_connection.shared->highlight[0];
  ReleaseMutex(g_connection.mutex);
  g_highlight_pending=FALSE;
}
static void inspect_highlight(const char *hash) {
  g_highlight_wanted[0]=0;g_highlight_level=g_index_level;
  if(hash&&hv_hash_valid(hash))lstrcpynA(g_highlight_wanted,hash,48);
  g_highlight_pending=TRUE;inspect_flush_highlight();
}
/* Called under the channel mutex only after a drawing request is accepted.
 * The requested drawing replaces the picked overlay, including pending writes. */
static void inspect_drawing_changed(void) {
  g_highlight_wanted[0]=0;g_highlight_pending=FALSE;
  g_connection.shared->highlight[0]=0;
  g_connection.shared->inspection_enabled=g_pick_enabled;
}
static void inspect_refresh_highlight(void) {
  g_highlight_pending=TRUE;inspect_flush_highlight();
}
static void inspect_history_buttons(void) {
  EnableWindow(GetDlgItem(g_window,ID_BACK),g_history_count>0);
  EnableWindow(GetDlgItem(g_window,ID_FORWARD),g_forward_count>0);
}
static void inspect_history_push(char stack[64][33],unsigned *count,const char *hash) {
  if(!hv_hash_valid(hash))return;
  if(*count==64){memmove(stack,stack+1,63*sizeof(stack[0]));--*count;}
  lstrcpynA(stack[(*count)++],hash,33);
}
static void inspect_select(const char *hash,BOOL remember) {
  if(!hv_hash_valid(hash))return;
  inspector_copy_clear();
  if(remember&&strcmp(g_inspected_hash,hash)) {
    inspect_history_push(g_history,&g_history_count,g_inspected_hash);
    g_forward_count=0;
  }
  lstrcpynA(g_inspected_hash,hash,33);
  inspect_history_buttons();
  int selected=hv_index_find(&g_index,hash);
  if(selected<0){
    char text[1024];snprintf(text,sizeof(text),"Hull: %s\r\n%s\r\nYou can still copy this hash.",hash,
      g_index_error[0]?g_index_error:"No matching object in the loaded game data.");
    inspect_text(GetDlgItem(g_window,ID_OBJECT),text);g_object=-1;
    inspect_properties(NULL);
    SendMessageW(GetDlgItem(g_window,ID_INCOMING),LB_RESETCONTENT,0,0);
    SendMessageW(GetDlgItem(g_window,ID_OUTGOING),LB_RESETCONTENT,0,0);
    inspect_hull_action();return;
  }
  g_object=selected;hv_object *o=g_index.objects+selected;
  size_t cap=strlen(o->name)+strlen(o->type)+256;
  char *text=malloc(cap);
  if(text) {
    snprintf(text,cap,"%s  ·  %s%s%s\r\n%s",o->kind,o->name,o->type[0]?"  ·  ":"",o->type,o->hash);
    inspect_text(GetDlgItem(g_window,ID_OBJECT),text);free(text);
  }
  inspect_properties(o);
  HWND incoming=GetDlgItem(g_window,ID_INCOMING),outgoing=GetDlgItem(g_window,ID_OUTGOING);
  SendMessageW(incoming,LB_RESETCONTENT,0,0);SendMessageW(outgoing,LB_RESETCONTENT,0,0);
  SendMessageW(incoming,LB_SETHORIZONTALEXTENT,0,0);SendMessageW(outgoing,LB_SETHORIZONTALEXTENT,0,0);
  for(unsigned i=0;i<g_index.ref_count;i++) {
    hv_reference *r=g_index.refs+i;
    if(r->source!=selected&&r->target!=selected)continue;
    BOOL is_out=r->source==selected;int other=is_out?r->target:r->source;
    const hv_object *target=other>=0?g_index.objects+other:NULL;
    char line[1024];snprintf(line,sizeof(line),"%s | %s: %s | %s",target?target->hash:r->target_hash,
      target?target->kind:"Missing",target?target->name:"",r->field);
    wchar_t wide[1024];int n=MultiByteToWideChar(CP_UTF8,0,line,-1,wide,1024);if(!n)continue;
    HWND list=is_out?outgoing:incoming;
    LRESULT item=SendMessageW(list,LB_ADDSTRING,0,(LPARAM)wide);
    if(item>=0)SendMessageW(list,LB_SETITEMDATA,item,other);
    HDC dc=GetDC(list);HGDIOBJ old=SelectObject(dc,g_font);SIZE extent;
    GetTextExtentPoint32W(dc,wide,(int)wcslen(wide),&extent);SelectObject(dc,old);ReleaseDC(list,dc);
    if(extent.cx+ui_px(8)>SendMessageW(list,LB_GETHORIZONTALEXTENT,0,0))
      SendMessageW(list,LB_SETHORIZONTALEXTENT,extent.cx+ui_px(8),0);
  }
  inspect_hull_action();
}
static void inspect_pick_hull(const char *hash) {
  if(!hv_hash_valid(hash))return;
  inspect_select(hash,TRUE);
  inspect_highlight(hash);
}
static void inspect_history_step(BOOL forward) {
  unsigned *count=forward?&g_forward_count:&g_history_count;
  char (*stack)[33]=forward?g_forward:g_history;
  if(!*count)return;
  char target[33];lstrcpynA(target,stack[--*count],33);
  inspect_history_push(forward?g_history:g_forward,
    forward?&g_history_count:&g_forward_count,g_inspected_hash);
  g_navigation.pending=FALSE;
  inspect_select(target,FALSE);
}
static void inspect_load_level(uint32_t level) {
  if(level==g_index_level)return;
  ++g_index_epoch;g_navigation.pending=FALSE;
  hv_index_free(&g_index);g_object=-1;g_inspected_hash[0]=0;
  g_links_object=-2;memset(&g_member_hulls,0,sizeof(g_member_hulls));
  TreeView_DeleteAllItems(GetDlgItem(g_window,ID_HULL_LINKS));
  EnableWindow(GetDlgItem(g_window,ID_SHOW_HULL),FALSE);
  g_history_count=g_forward_count=g_hits_count=0;g_index_level=level;
  inspect_history_buttons();
  g_index_error[0]=0;
  g_index_from_game=FALSE;
  inspect_properties(NULL);
  SendMessageW(GetDlgItem(g_window,ID_HITS),LB_RESETCONTENT,0,0);
  SendMessageW(GetDlgItem(g_window,ID_INCOMING),LB_RESETCONTENT,0,0);
  SendMessageW(GetDlgItem(g_window,ID_OUTGOING),LB_RESETCONTENT,0,0);
  inspect_highlight(NULL);
  if(level>=12){
    SetWindowTextW(GetDlgItem(g_window,ID_INSPECT_LEVEL),L"Inspector — no level");
    SetWindowTextW(GetDlgItem(g_window,ID_OBJECT),L"No level loaded.");
    return;
  }
  wchar_t title[128];_snwprintf(title,128,L"Inspector — %ls",g_level_titles[level]);title[127]=0;
  SetWindowTextW(GetDlgItem(g_window,ID_INSPECT_LEVEL),title);
  wchar_t data_root[MAX_PATH];
  if(inspect_data_root(data_root)) {
    unsigned files=0,missing=0;g_index_from_game=TRUE;
    BOOL loaded=hd_load_game(&g_index,data_root,g_level_ids[level],g_index_error,&files,&missing);
    wchar_t provenance[160];_snwprintf(provenance,160,L"Game files · %u files loaded%ls",files,missing?L" · Some files missing":L"");provenance[159]=0;
    SetWindowTextW(GetDlgItem(g_window,ID_PROVENANCE),provenance);
    if(loaded)SetWindowTextW(GetDlgItem(g_window,ID_OBJECT),L"Pick a hull in Journey or enter an object hash above.");
    else inspect_text(GetDlgItem(g_window,ID_OBJECT),g_index_error);
    return;
  }
  SetWindowTextW(GetDlgItem(g_window,ID_PROVENANCE),L"Offline lookup · game files not verified");
  wchar_t path[MAX_PATH];DWORD length=GetModuleFileNameW(NULL,path,MAX_PATH);
  if(!length||length>=MAX_PATH)return;
  wchar_t *slash=wcsrchr(path,L'\\');
  if(!slash||slash-path>MAX_PATH-80)return;
  wchar_t filename[80];_snwprintf(filename,80,L"\\inspection\\%hs.hvi",g_level_ids[level]);filename[79]=0;
  lstrcpyW(slash,filename);FILE *f=_wfopen(path,L"rb");
  if(!f)lstrcpyA(g_index_error,"Game data not found. Place the viewer beside Journey.exe with the Data folder, then click Reload Data.");
  BOOL ok=FALSE;
  if(f) {
    if(!fseek(f,0,SEEK_END)) {
      long size=ftell(f);
      if(size>0&&size<=32*1024*1024&&!fseek(f,0,SEEK_SET)) {
        char *bytes=malloc((size_t)size);
        if(bytes){if(fread(bytes,1,(size_t)size,f)==(size_t)size)ok=hv_index_parse(&g_index,bytes,(size_t)size,g_level_ids[level]);free(bytes);}
      }
    }
    fclose(f);
    if(!ok)lstrcpyA(g_index_error,"Offline lookup unavailable. Place the viewer beside Journey.exe with the Data folder, then click Reload Data.");
  }
  SetWindowTextW(GetDlgItem(g_window,ID_OBJECT),ok?
    L"Pick a hull in Journey or enter an object hash above.":
    L"Game data not found. Place the viewer beside Journey.exe with the Data folder, then click Reload Data.");
}
static void inspector_create(void) {
  control(L"STATIC",L"Inspector — no level",SS_CENTERIMAGE|SS_ENDELLIPSIS,644,18,500,24,ID_INSPECT_LEVEL);
  control(L"BUTTON",L"Inspect",BS_AUTOCHECKBOX|WS_TABSTOP,644,51,140,28,ID_PICK);
  control(L"STATIC",HV_INSPECT_SHORTCUT_LABEL,SS_CENTERIMAGE,794,55,350,24,ID_INSPECT_HINT);
  HWND lookup=control(L"EDIT",L"",ES_MULTILINE|ES_AUTOHSCROLL|WS_TABSTOP,0,0,0,0,ID_LOOKUP_HASH);
  ui_font(lookup,g_font);SendMessageW(lookup,EM_SETLIMITTEXT,100,0);
  SendMessageW(lookup,EM_SETCUEBANNER,FALSE,(LPARAM)L"Object hash (any type)");
  SendMessageW(lookup,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(ui_px(4),ui_px(4)));
  control(L"BUTTON",L"Open",WS_TABSTOP,0,0,0,0,ID_LOOKUP_OPEN);
  control(L"STATIC",L"Picked Hulls",0,644,122,500,24,ID_HITS_LABEL);
  control(L"LISTBOX",L"",WS_VSCROLL|WS_HSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
          644,148,500,105,ID_HITS);
  ui_font(GetDlgItem(g_window,ID_HITS),g_font);
  control(L"STATIC",L"Enable Inspect Mode, then click a hull in the game.",0,644,260,500,36,ID_PICK_NOTE);
  control(L"BUTTON",L"←",WS_TABSTOP,644,302,70,28,ID_BACK);
  control(L"BUTTON",L"→",WS_TABSTOP,0,0,76,28,ID_FORWARD);
  control(L"BUTTON",L"Copy Hash",WS_TABSTOP,724,302,100,28,ID_COPY_HASH);
  control(L"BUTTON",L"Show This Hull",WS_TABSTOP,834,302,140,28,ID_SHOW_HULL);
  control(L"BUTTON",L"Reload Data",WS_TABSTOP,984,302,160,28,ID_RELOAD_INDEX);
  HWND details=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL|WS_HSCROLL|WS_TABSTOP,
          644,340,500,182,ID_OBJECT);
  SendMessageW(details,EM_SETLIMITTEXT,100000,0);ui_font(details,g_summary_font);
  SendMessageW(details,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(ui_px(6),ui_px(6)));
  control(L"STATIC",L"Read-only authored source data",0,0,0,0,0,ID_PROVENANCE);
  control(L"BUTTON",L"Hide zero / false",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,0,0,ID_VALUE_FOCUS);
  HWND views=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0,0,0,0,ID_DATA_TABS);
  SetWindowSubclass(views,ui_combo_proc,1,0);
  const wchar_t *labels[]={L"Properties",L"Raw Data",L"Referenced By",L"References",L"Hull Links"};
  for(unsigned i=0;i<5;i++)SendMessageW(views,CB_ADDSTRING,0,(LPARAM)labels[i]);
  inspector_set_view(0);
  HWND links=control(WC_TREEVIEWW,L"",WS_TABSTOP|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS,
    0,0,0,0,ID_HULL_LINKS);
  TreeView_SetBkColor(links,RGB(255,255,255));TreeView_SetTextColor(links,RGB(35,49,70));ShowWindow(links,SW_HIDE);
  HWND tree=control(WC_TREEVIEWW,L"",WS_TABSTOP|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS,
    0,0,0,0,ID_PROPERTIES);
  TreeView_SetBkColor(tree,RGB(255,255,255));TreeView_SetTextColor(tree,RGB(35,49,70));
  HWND raw=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,
    0,0,0,0,ID_RAW);
  ui_font(raw,g_code_font);SendMessageW(raw,EM_SETLIMITTEXT,1048576,0);ShowWindow(raw,SW_HIDE);
  control(L"STATIC",L"Double-click to inspect.",0,644,535,500,22,ID_IN_LABEL);
  control(L"LISTBOX",L"",WS_VSCROLL|WS_HSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
          644,560,500,115,ID_INCOMING);
  control(L"STATIC",L"Double-click to inspect.",0,644,685,500,22,ID_OUT_LABEL);
  control(L"LISTBOX",L"",WS_VSCROLL|WS_HSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
          644,710,500,115,ID_OUTGOING);
  g_text_selection=control(L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL,0,0,0,0,ID_TEXT_SELECTION);
  ShowWindow(g_text_selection,SW_HIDE);SetWindowSubclass(g_text_selection,inspector_copy_proc,1,2);
  const int copy_rows[]={ID_PROPERTIES,ID_HULL_LINKS,ID_INCOMING,ID_OUTGOING,ID_HITS};
  for(unsigned i=0;i<sizeof(copy_rows)/sizeof(*copy_rows);i++) {
    HWND row=GetDlgItem(g_window,copy_rows[i]);
    SetWindowLongPtrW(row,GWL_STYLE,GetWindowLongPtrW(row,GWL_STYLE)|WS_CLIPCHILDREN);
    SetWindowSubclass(row,inspector_copy_proc,1,0);
  }
  const int copy_edits[]={ID_OBJECT,ID_RAW};
  for(unsigned i=0;i<sizeof(copy_edits)/sizeof(*copy_edits);i++)
    SetWindowSubclass(GetDlgItem(g_window,copy_edits[i]),inspector_copy_proc,1,1);
  ui_font(GetDlgItem(g_window,ID_INSPECT_LEVEL),g_section_font);
  ui_font(GetDlgItem(g_window,ID_PICK),g_section_font);
  ui_font(GetDlgItem(g_window,ID_PROVENANCE),g_small_font);
  ui_font(GetDlgItem(g_window,ID_INSPECT_HINT),g_small_font);
  ShowWindow(GetDlgItem(g_window,ID_INCOMING),SW_HIDE);ShowWindow(GetDlgItem(g_window,ID_OUTGOING),SW_HIDE);
  ShowWindow(GetDlgItem(g_window,ID_IN_LABEL),SW_HIDE);ShowWindow(GetDlgItem(g_window,ID_OUT_LABEL),SW_HIDE);
  inspect_history_buttons();
}
static void inspector_layout(int width,int height) {
  inspector_copy_clear();
  int x=g_sidebar_width+64,w=width-x-32;
  ui_move(ID_INSPECT_LEVEL,x,74,w-138,28);
  ui_move(ID_PICK,x+w-134,74,82,28);ui_move(ID_INSPECT_HINT,x+w-48,74,48,28);
  ui_move(ID_LOOKUP_HASH,x,104,w-58,24);ui_move(ID_LOOKUP_OPEN,x+w-52,104,52,24);
  ui_center_edit(GetDlgItem(g_window,ID_LOOKUP_HASH));
  int picks=g_names_view?330:278;
  ui_move(ID_HITS_LABEL,32,picks,g_sidebar_width-16,22);
  ui_move(ID_HITS,32,picks+24,g_sidebar_width-16,height-picks-g_footer_reserve-32);
  ShowWindow(GetDlgItem(g_window,ID_PICK_NOTE),SW_HIDE);
  ui_move(ID_BACK,x,136,26,24);ui_move(ID_FORWARD,x+32,136,26,24);
  ui_move(ID_COPY_HASH,x+64,136,80,24);ui_move(ID_SHOW_HULL,x+150,136,inspector_action_width(),24);
  ui_move(ID_RELOAD_INDEX,x+w-78,136,78,24);
  ui_move(ID_OBJECT,x,168,w,56);ui_move(ID_PROVENANCE,x,230,w,18);
  ui_name_scrollbars(GetDlgItem(g_window,ID_OBJECT));
  /* Dropdown stays one row at every pane width; its height includes the popup. */
  ui_move(ID_DATA_TABS,x,252,156,160);
  ui_move(ID_VALUE_FOCUS,x+168,252,w-168,24);
  const int panes[]={ID_PROPERTIES,ID_RAW,ID_HULL_LINKS,ID_INCOMING,ID_OUTGOING};
  for(unsigned i=0;i<sizeof(panes)/sizeof(*panes);i++)ui_move(panes[i],x,286,w,height-g_footer_reserve-294);
  ui_raw_scrollbar(GetDlgItem(g_window,ID_RAW));
}
static HTREEITEM inspect_link_item(HTREEITEM parent,const char *text,int object) {
  wchar_t wide[1200];if(!MultiByteToWideChar(CP_UTF8,0,text,-1,wide,1200))lstrcpyW(wide,L"Long path label");
  TVINSERTSTRUCTW insert={.hParent=parent,.hInsertAfter=TVI_LAST,
    .item={.mask=TVIF_TEXT|TVIF_PARAM,.pszText=wide,.lParam=object>=0?(LPARAM)object+1:0}};
  return TreeView_InsertItem(GetDlgItem(g_window,ID_HULL_LINKS),&insert);
}
static void inspect_link_group(const char *title,int mode) {
  hv_hull_paths result;hv_find_hulls(&g_index,g_object,mode,&result);
  char label[1024];snprintf(label,sizeof(label),"%s — %u hulls",title,result.count);
  HTREEITEM group=inspect_link_item(TVI_ROOT,label,-1);
  if(!result.ok)inspect_link_item(group,"Could not search hull references.",-1);
  else if(!result.count)inspect_link_item(group,"No linked hulls found.",-1);
  if(result.limited)inspect_link_item(group,"Partial results: search limit reached (12 links / 64 hulls).",-1);
  if(result.missing)inspect_link_item(group,"Some linked objects were not found in the loaded data.",-1);
  for(unsigned i=0;i<result.count;i++) {
    const hv_hull_path *p=result.paths+i;int last=p->object[p->length];
    const hv_object *h=g_index.objects+last;
    snprintf(label,sizeof(label),"%s | %s | %u link%s",h->hash,h->name,p->length,p->length==1?"":"s");
    HTREEITEM row=inspect_link_item(group,label,last);
    for(unsigned j=0;j<=p->length;j++) {
      const hv_object *o=g_index.objects+p->object[j];
      if(j) {
        const hv_reference *r=g_index.refs+p->edge[j-1];
        snprintf(label,sizeof(label),"%s %s | %s: %.200s | %s",p->reverse[j-1]?"←":"→",r->field,o->kind,o->name,o->hash);
      } else snprintf(label,sizeof(label),"From %s: %.200s | %s",o->kind,o->name,o->hash);
      inspect_link_item(row,label,p->object[j]);
    }
  }
  TreeView_Expand(GetDlgItem(g_window,ID_HULL_LINKS),group,TVE_EXPAND);
}
static void inspect_hull_links(void) {
  if(g_links_object==g_object)return;
  g_links_object=g_object;HWND tree=GetDlgItem(g_window,ID_HULL_LINKS);
  SendMessageW(tree,WM_SETREDRAW,FALSE,0);TreeView_DeleteAllItems(tree);
  inspect_link_item(TVI_ROOT,"Double-click to inspect.",-1);
  if(g_object>=0) {
    BOOL clump=!strcmp(g_index.objects[g_object].kind,"Clump");
    inspect_link_group(clump?"Contained Hulls":"Referenced Hulls",clump?HV_HULL_MEMBERS:HV_HULL_REFERENCES);
    if(hv_link_control(g_index.objects+g_object))inspect_link_group("Linked Regions",HV_HULL_SOURCES);
  }
  SendMessageW(tree,WM_SETREDRAW,TRUE,0);
  ShowWindow(tree,inspector_view()==4?SW_SHOW:SW_HIDE);
  InvalidateRect(tree,NULL,TRUE);
}
static void inspector_data_view(void) {
  int view=inspector_view();
  inspector_copy_clear();
  const int ids[]={ID_PROPERTIES,ID_RAW,ID_INCOMING,ID_OUTGOING,ID_HULL_LINKS};
  if(view==4)inspect_hull_links();
  for(int i=0;i<5;i++)ShowWindow(GetDlgItem(g_window,ids[i]),view==i?SW_SHOW:SW_HIDE);
  ShowWindow(GetDlgItem(g_window,ID_IN_LABEL),SW_HIDE);
  ShowWindow(GetDlgItem(g_window,ID_OUT_LABEL),SW_HIDE);
  ShowWindow(GetDlgItem(g_window,ID_VALUE_FOCUS),view==0?SW_SHOW:SW_HIDE);
  /* Clear old flat borders and hidden control pixels as one complete update. */
  RedrawWindow(g_window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
}
static BOOL inspector_notify(LPARAM parameter) {
  NMHDR *header=(NMHDR *)parameter;
  if((header->idFrom==ID_PROPERTIES||header->idFrom==ID_HULL_LINKS)&&header->code==TVN_SELCHANGEDW)
    if(g_text_source==header->hwndFrom)inspector_copy_clear();
  if((header->idFrom==ID_PROPERTIES||header->idFrom==ID_HULL_LINKS)&&header->code==NM_DBLCLK) {
    HTREEITEM selected=TreeView_GetSelection(header->hwndFrom);TVITEMW item={.mask=TVIF_PARAM,.hItem=selected};
    if(selected&&TreeView_GetItem(header->hwndFrom,&item)&&item.lParam>0&&(uint64_t)item.lParam<=g_index.count) {
      /* Do not destroy this control's items inside its double-click callback.
       * Consume its default action; navigate after the control has unwound. */
      inspect_queue_navigation(g_index.objects[item.lParam-1].hash);return TRUE;
    }
    return FALSE;
  }
  return FALSE;
}
static BOOL inspector_command(WPARAM w) {
  unsigned id=LOWORD(w),notification=HIWORD(w);
  switch(id) {
    case ID_DATA_TABS:if(notification==CBN_SELCHANGE)inspector_data_view();return TRUE;
    case ID_LOOKUP_OPEN:inspect_lookup_hash();return TRUE;
    case ID_VALUE_FOCUS:
      g_hide_zero_false=SendMessageW(GetDlgItem(g_window,id),BM_GETCHECK,0,0)==BST_CHECKED;
      inspect_properties(g_object>=0?g_index.objects+g_object:NULL);return TRUE;
    case ID_PICK:
      g_pick_enabled=SendMessageW(GetDlgItem(g_window,id),BM_GETCHECK,0,0)==BST_CHECKED;
      if(g_pick_enabled)SetTimer(g_window,2,16,NULL);else KillTimer(g_window,2);
      if(g_connection.shared) {
        DWORD lock=WaitForSingleObject(g_connection.mutex,50);
        if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){g_connection.shared->inspect_mode=g_pick_enabled;ReleaseMutex(g_connection.mutex);}
      }
      inspect_refresh_highlight();
      return TRUE;
    case ID_RELOAD_INDEX: {
      uint32_t level=g_index_level;char hash[33],highlight[48];lstrcpynA(hash,g_inspected_hash,33);
      lstrcpynA(highlight,g_highlight_wanted,48);
      g_index_level=UINT32_MAX;inspect_load_level(level);
      if(hash[0])inspect_select(hash,FALSE);
      inspect_highlight(highlight);
      return TRUE;
    }
    case ID_HITS:
      if(notification==LBN_SELCHANGE) {
        LRESULT n=SendMessageW(GetDlgItem(g_window,id),LB_GETCURSEL,0,0);
        if(n>=0&&(unsigned)n<g_hits_count)inspect_pick_hull(g_hits[n].name);
      }return TRUE;
    case ID_INCOMING:case ID_OUTGOING:
      if(notification==LBN_SELCHANGE&&g_text_source==GetDlgItem(g_window,id))inspector_copy_clear();
      if(notification==LBN_DBLCLK) {
        HWND list=GetDlgItem(g_window,id);LRESULT n=SendMessageW(list,LB_GETCURSEL,0,0);
        if(n>=0){LRESULT target=SendMessageW(list,LB_GETITEMDATA,n,0);
          if(target>=0&&(uint64_t)target<g_index.count) {
            inspect_queue_navigation(g_index.objects[target].hash);
          }}
      }return TRUE;
    case ID_BACK:inspect_history_step(FALSE);return TRUE;
    case ID_FORWARD:inspect_history_step(TRUE);return TRUE;
    case ID_COPY_HASH:
      if(g_inspected_hash[0]){wchar_t hash[33];MultiByteToWideChar(CP_UTF8,0,g_inspected_hash,-1,hash,33);clipboard_write(g_window,hash);}
      return TRUE;
    case ID_SHOW_HULL:
      if(g_inspected_hash[0]&&(g_object<0||!strcmp(g_index.objects[g_object].kind,"Hull"))) {
        inspect_text(g_names,g_inspected_hash);send_config(HV_SELECTED);
        SendMessageW(g_window,WM_COMMAND,ID_NAMES_TAB,0);
      } else if(g_object>=0&&!strcmp(g_index.objects[g_object].kind,"Clump")) {
        if(!g_member_hulls.ok||g_member_hulls.limited||g_member_hulls.missing||!g_member_hulls.count) {
          notice(L"Clump hull list is empty or incomplete. Inspect Hull Links for details.");return TRUE;
        }
        char names[HV_NAMES*34+1];size_t used=0;
        for(unsigned i=0;i<g_member_hulls.count;i++) {
          hv_hull_path *p=g_member_hulls.paths+i;
          used+=(size_t)snprintf(names+used,sizeof(names)-used,"%s\r\n",g_index.objects[p->object[p->length]].hash);
        }
        inspect_text(g_names,names);send_config(HV_SELECTED);
        SendMessageW(g_window,WM_COMMAND,ID_NAMES_TAB,0);
      } else if(g_object>=0) {
        inspector_set_view(4);inspector_data_view();
      }return TRUE;
  }
  return FALSE;
}
static void inspector_navigate_pending(void) {
  if(!g_navigation.pending)return;
  char hash[33];lstrcpynA(hash,g_navigation.hash,33);
  BOOL current=g_navigation.level==g_index_level&&g_navigation.epoch==g_index_epoch;
  g_navigation.pending=FALSE;
  if(!current)return;
  inspector_set_view(0);inspector_data_view();
  inspect_select(hash,TRUE);
}
static void inspector_tick(void) {
  inspect_flush_highlight();
  if(!g_connection.shared){inspect_load_level(UINT32_MAX);return;}
  DWORD lock=WaitForSingleObject(g_connection.mutex,0);
  if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED)return;
  uint32_t level=g_connection.shared->live_level;
  BOOL mode=g_connection.shared->inspect_mode==1,available=g_connection.shared->inspect_available!=0;
  char input_message[128];memcpy(input_message,g_connection.shared->inspect_message,sizeof(input_message));input_message[127]=0;
  BOOL expired=g_inspect_request&&GetTickCount64()-g_connection.shared->pick_time>1500;
  if(expired)g_inspect_request=0;
  BOOL done=g_inspect_request&&g_connection.shared->pick_completed==g_inspect_request&&g_connection.shared->pick_level==level;
  if(g_connection.shared->pick_level!=level)g_inspect_request=0;
  char message[128]={0};
  if(done) {
    uint32_t n=g_connection.shared->pick_count;g_hits_count=n<HV_PICK_HITS?n:HV_PICK_HITS;
    memcpy(g_hits,g_connection.shared->pick_hits,sizeof(g_hits));
    for(unsigned i=0;i<g_hits_count;i++)g_hits[i].name[47]=0;
    memcpy(message,g_connection.shared->pick_message,sizeof(message));message[127]=0;
    g_inspect_request=0;
  }
  ReleaseMutex(g_connection.mutex);
  EnableWindow(GetDlgItem(g_window,ID_PICK),available);
  SendMessageW(GetDlgItem(g_window,ID_PICK),BM_SETCHECK,mode?BST_CHECKED:BST_UNCHECKED,0);
  BOOL mode_changed=g_pick_enabled!=mode;
  g_pick_enabled=mode;
  if(mode_changed)inspect_refresh_highlight();
  if(mode)SetTimer(g_window,2,16,NULL);else KillTimer(g_window,2);
  inspect_load_level(level);
  if(input_message[0])inspect_text(GetDlgItem(g_window,ID_PICK_NOTE),input_message);
  if(expired) {
    SetWindowTextW(GetDlgItem(g_window,ID_PICK_NOTE),L"No result. Draw hulls and keep Journey rendering, then pick again.");
    notice(L"No pick result. Draw hulls and keep Journey rendering, then pick again.");
  }
  if(done) {
    HWND list=GetDlgItem(g_window,ID_HITS);SendMessageW(list,LB_RESETCONTENT,0,0);
    for(unsigned i=0;i<g_hits_count;i++) {
      wchar_t row[48];_snwprintf(row,48,L"%hs",g_hits[i].name);row[47]=0;
      SendMessageW(list,LB_ADDSTRING,0,(LPARAM)row);
    }
    inspect_text(GetDlgItem(g_window,ID_PICK_NOTE),message);
    if(!g_hits_count&&message[0]) {
      wchar_t note[192];MultiByteToWideChar(CP_UTF8,0,message,-1,note,192);note[191]=0;
      if(!strcmp(message,"Choose a hull below. Overlapping or obscured hulls may also be listed."))
        lstrcpyW(note,L"Choose a hull in Picked Hulls. Results may include overlapping or hidden hulls.");
      else if(!strcmp(message,"No drawn hull at that point. Try another face or change the type filter."))
        lstrcpyW(note,L"No hull found here. Try another face or change the type filter.");
      notice(note);
    }
    if(g_hits_count){SendMessageW(list,LB_SETCURSEL,0,0);inspect_pick_hull(g_hits[0].name);}
  }
}
static void inspector_mouse(void) {
  BOOL down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0,pressed=down&&!g_mouse_down;
  g_mouse_down=down;
  if(!g_pick_enabled||!pressed||!g_connection.shared)return;
  HWND game=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(game,&pid);
  if(pid!=g_connection.pid)return;
  POINT cursor;RECT area;
  if(!GetCursorPos(&cursor)||!ScreenToClient(game,&cursor)||!GetClientRect(game,&area)||
     area.right<32||area.bottom<32||cursor.x<0||cursor.y<0||cursor.x>=area.right||cursor.y>=area.bottom)return;
  DWORD lock=WaitForSingleObject(g_connection.mutex,0);
  if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED)return;
  hv_shared *s=g_connection.shared;
  if(s->inspect_applied&&(s->pick_request==s->pick_completed||GetTickCount64()-s->pick_time>1500)&&s->live_level<12) {
    s->pick_x=(float)cursor.x/area.right;s->pick_y=(float)cursor.y/area.bottom;
    s->pick_width=area.right;s->pick_height=area.bottom;s->pick_level=s->live_level;
    s->pick_time=GetTickCount64();g_inspect_request=++s->pick_request;
  }
  ReleaseMutex(g_connection.mutex);
}
