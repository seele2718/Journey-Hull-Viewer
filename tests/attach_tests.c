#include <windows.h>
#include <string.h>
#include <wchar.h>
/* Mock only the clipboard boundary: tests must not read or replace the user's
 * clipboard. */
static HGLOBAL fixture_clipboard;
static HWND fixture_clipboard_owner;
static BOOL fixture_clipboard_busy;
static char fixture_debug[1024];
static SHORT fixture_keys[256];
static SHORT WINAPI fake_key_state(int key){return key>=0&&key<256?fixture_keys[key]:0;}
static void WINAPI fake_debug(LPCSTR text){lstrcpynA(fixture_debug,text,sizeof(fixture_debug));}
static BOOL fake_open(HWND owner) {
  fixture_clipboard_owner = owner;
  return !fixture_clipboard_busy;
}
static BOOL fake_close(void) { return TRUE; }
static HANDLE fake_get(UINT format) { return format == CF_UNICODETEXT ? fixture_clipboard : NULL; }
static BOOL fake_empty(void) {
  if (fixture_clipboard) GlobalFree(fixture_clipboard);
  fixture_clipboard = NULL; return TRUE;
}
static HANDLE fake_set(UINT format, HANDLE data) {
  if (format != CF_UNICODETEXT) return NULL;
  fixture_clipboard = data; return data;
}
#define OpenClipboard fake_open
#define CloseClipboard fake_close
#define GetClipboardData fake_get
#define EmptyClipboard fake_empty
#define SetClipboardData fake_set
#define OutputDebugStringA fake_debug
#define GetKeyState fake_key_state
#define wWinMain unused_panel_main
#include "../src/panel.c"
#undef wWinMain
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned fixture_enable_messages,fixture_check_messages,fixture_text_messages;
static BOOL fixture_with_index;
static LRESULT CALLBACK refresh_trace(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
  (void)id;(void)data;
  if(message==WM_ENABLE)fixture_enable_messages++;
  if(message==BM_SETCHECK)fixture_check_messages++;
  if(message==WM_SETTEXT)fixture_text_messages++;
  return DefSubclassProc(window,message,w,l);
}
static void drain_navigation(HWND window) {
  MSG msg;while(PeekMessageW(&msg,window,WM_INSPECT_NAVIGATE,WM_INSPECT_NAVIGATE,PM_REMOVE))DispatchMessageW(&msg);
}
static void button_corner_tests(HWND window) {
  HDC dc=CreateCompatibleDC(NULL);CHECK(dc);
  BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=160,.biHeight=-48,
    .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
  void *pixels=NULL;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);CHECK(bitmap&&pixels);
  HGDIOBJ old=SelectObject(dc,bitmap);
  const int ids[]={ID_ATTACH,ID_CAM_START,ID_CAM_STOP,ID_CAM_MOVE,ID_HIDE,ID_ALL,ID_TYPES,ID_NAMES_TAB,ID_COPY_HASH,ID_SHOW_HULL};
  const UINT states[]={0,ODS_DISABLED,ODS_SELECTED,ODS_FOCUS};
  unsigned saved_dpi=g_dpi;
  for(unsigned dpi=96;dpi<=192;dpi+=48) {
    g_dpi=dpi;
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++)for(unsigned j=0;j<sizeof(states)/sizeof(*states);j++) {
      DRAWITEMSTRUCT draw={.CtlType=ODT_BUTTON,.CtlID=(UINT)ids[i],.itemState=states[j],
        .hwndItem=GetDlgItem(window,ids[i]),.hDC=dc,.rcItem={0,0,160,48}};
      CHECK(draw.hwndItem);
      FillRect(dc,&draw.rcItem,(HBRUSH)GetStockObject(BLACK_BRUSH));
      CHECK(ui_draw_button(&draw));
      COLORREF expected=i<4?RGB(240,243,248):RGB(255,255,255);
      CHECK(GetPixel(dc,0,0)==expected&&GetPixel(dc,159,0)==expected);
      CHECK(GetPixel(dc,0,47)==expected&&GetPixel(dc,159,47)==expected);
    }
  }
  g_dpi=saved_dpi;SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
  puts("PASS: header/panel button corners match their surrounding background in normal/disabled/pressed/focused states at 96/144/192 DPI");
}
static HTREEITEM find_link(HWND tree,HTREEITEM item,int target) {
  for(;item;item=TreeView_GetNextSibling(tree,item)) {
    TVITEMW data={.mask=TVIF_PARAM,.hItem=item};CHECK(TreeView_GetItem(tree,&data));
    if(data.lParam==(LPARAM)target+1)return item;
    HTREEITEM child=find_link(tree,TreeView_GetChild(tree,item),target);if(child)return child;
  }
  return NULL;
}
static void queue_tree_link(HWND window,const char *target) {
  HWND tree=GetDlgItem(window,ID_PROPERTIES);int id=hv_index_find(&g_index,target);CHECK(id>=0);
  HTREEITEM item=find_link(tree,TreeView_GetRoot(tree),id);CHECK(item);
  TreeView_SelectItem(tree,item);unsigned count=TreeView_GetCount(tree);int previous=g_object;
  TreeView_EnsureVisible(tree,item);RECT row;CHECK(TreeView_GetItemRect(tree,item,&row,TRUE));
  CHECK(inspector_text_begin(tree,(POINT){row.left+4,(row.top+row.bottom)/2}));
  CHECK(g_text_link&&GetParent(g_text_selection)==tree);
  PostMessageW(g_text_selection,WM_LBUTTONUP,0,MAKELPARAM(4,4));
  SendMessageW(g_text_selection,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,4));
  MSG release;while(PeekMessageW(&release,g_text_selection,WM_MOUSEFIRST,WM_MOUSELAST,PM_REMOVE))DispatchMessageW(&release);
  CHECK(SendMessageW(g_text_selection,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(4,4))==TRUE);
  CHECK(g_navigation.pending&&g_object==previous&&TreeView_GetCount(tree)==count);
  drain_navigation(window);CHECK(g_object==id&&!g_navigation.pending);
}
static void check_single_line(HWND window, int id) {
  HWND label = GetDlgItem(window, id); CHECK(label);
  wchar_t text[256]; GetWindowTextW(label, text, 256);
  HDC dc = GetDC(label); CHECK(dc);
  HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(label, WM_GETFONT, 0, 0));
  SIZE extent; CHECK(GetTextExtentPoint32W(dc, text, (int)wcslen(text), &extent));
  RECT client; GetClientRect(label, &client);
  CHECK(extent.cx <= client.right && extent.cy <= client.bottom);
  SelectObject(dc, old); ReleaseDC(label, dc);
}
static void persistent_highlight_tests(HWND window) {
  const char *picked="8dc1645e2ed9ee33aa75297e01b94d0c",*other="80706360ff97d5461c413630495f6235";
  hv_shared *shared=g_connection.shared;
  inspect_pick_hull(picked);CHECK(!strcmp(shared->highlight,picked));
  for(unsigned i=0;i<90;i++) {
    inspect_queue_navigation(i%3==0?other:i%3==1?"05abb487918787af2bde690f614b7068":"f070e30d2ee3c6d8f5b526b6285eaf29");
    drain_navigation(window);CHECK(!strcmp(shared->highlight,picked));
  }
  for(unsigned i=0;i<70;i++){inspector_command(ID_BACK);CHECK(!strcmp(shared->highlight,picked));}
  for(unsigned i=0;i<70;i++){inspector_command(ID_FORWARD);CHECK(!strcmp(shared->highlight,picked));}
  inspect_select("00000000000000000000000000000000",TRUE);CHECK(!strcmp(shared->highlight,picked));
  inspector_command(ID_RELOAD_INDEX);CHECK(!strcmp(shared->highlight,picked));
  for(unsigned i=0;i<2;i++) {
    SendMessageW(GetDlgItem(window,ID_PICK),BM_SETCHECK,i?BST_CHECKED:BST_UNCHECKED,0);
    inspector_command(ID_PICK);CHECK(!strcmp(shared->highlight,picked)&&shared->inspection_enabled);
    shared->inspect_mode=!i;inspector_tick();CHECK(!strcmp(shared->highlight,picked));
  }
  SetWindowTextW(GetDlgItem(window,ID_LOOKUP_HASH),L"80706360ff97d5461c413630495f6235");
  inspector_command(ID_LOOKUP_OPEN);drain_navigation(window);CHECK(!strcmp(shared->highlight,picked));
  SendMessageW(window,WM_COMMAND,ID_FILL,0);CHECK(!strcmp(shared->highlight,picked));
  SetWindowTextW(g_names,L"\u2603");SendMessageW(window,WM_COMMAND,ID_SELECTED,0);
  CHECK(!strcmp(shared->highlight,picked)); /* Rejected drawing is not a selection. */
  g_hits_count=1;lstrcpynA(g_hits[0].name,other,48);
  HWND hits=GetDlgItem(window,ID_HITS);SendMessageW(hits,LB_RESETCONTENT,0,0);
  SendMessageW(hits,LB_ADDSTRING,0,(LPARAM)L"80706360ff97d5461c413630495f6235");
  SendMessageW(hits,LB_SETCURSEL,0,0);inspector_command(MAKEWPARAM(ID_HITS,LBN_SELCHANGE));
  CHECK(!strcmp(shared->highlight,other));
  inspect_select(picked,TRUE);inspector_command(ID_SHOW_HULL);
  CHECK(!shared->highlight[0]&&!g_highlight_wanted[0]&&!g_highlight_pending);
  CHECK(shared->config.mode==HV_SELECTED&&shared->config.count==1&&!strcmp(shared->config.names[0],picked));
  inspect_select(other,TRUE);CHECK(!shared->highlight[0]);
  const unsigned draw_ids[]={ID_HIDE,ID_ALL,ID_SELECTED,ID_CHECK};
  for(unsigned i=0;i<4;i++) {
    inspect_pick_hull(picked);lstrcpynA(g_highlight_wanted,other,48);g_highlight_pending=TRUE;
    SendMessageW(window,WM_COMMAND,draw_ids[i],0);
    CHECK(!shared->highlight[0]&&!g_highlight_wanted[0]&&!g_highlight_pending);
    inspect_flush_highlight();CHECK(!shared->highlight[0]);
  }
  inspect_pick_hull(picked);inspect_load_level(UINT32_MAX);
  CHECK(!shared->highlight[0]&&!g_highlight_wanted[0]);
  inspect_load_level(4);
  puts("PASS: persistent picked highlight across 90 visits/history/reload/toggles/lookup, explicit draw replacement, no stale pending restore, level clear");
}
static void check_summary_border(HWND window) {
  RECT area,summary;GetClientRect(window,&area);CHECK(GetWindowRect(GetDlgItem(window,ID_OBJECT),&summary));
  MapWindowPoints(NULL,window,(POINT *)&summary,2);
  HDC dc=CreateCompatibleDC(NULL);CHECK(dc);void *pixels=NULL;
  BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=area.right,.biHeight=-area.bottom,
    .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
  HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);CHECK(bitmap&&pixels);
  HGDIOBJ old=SelectObject(dc,bitmap);ui_draw(window,dc);
  CHECK(GetPixel(dc,summary.left-1,(summary.top+summary.bottom)/2)==RGB(220,226,235));
  SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
static void property_value_tests(HWND window) {
  hv_json doc={0};CHECK(hv_json_parse(&doc,"[true,false,0,-0.0,0e12,1,-2,0.25,1e-999,null,\"true\",\"0\"]"));
  const int expected[]={1,-1,-1,-1,-1,1,1,1,1,0,0,0};unsigned i=0;
  for(int child=doc.nodes[0].first;child>=0;child=doc.nodes[child].next)CHECK(property_emphasis(&doc,child)==expected[i++]);
  CHECK(i==12);hv_json_free(&doc);
  hv_object object={.hash="00000000000000000000000000000001",.kind="Trigger",.name="fixture",.type="fixture",
    .detail="{\"authored\":{\"Vars\":{\"yes\":true,\"no\":false,\"zero\":0,\"amount\":-2,\"text\":\"0\"}}}"};
  inspect_properties(&object);HWND tree=GetDlgItem(window,ID_PROPERTIES);
  unsigned full=TreeView_GetCount(tree);CHECK(full==8);
  HTREEITEM authored=TreeView_GetNextSibling(tree,TreeView_GetRoot(tree));
  HTREEITEM vars=TreeView_GetChild(tree,authored),item=TreeView_GetChild(tree,vars);
  const unsigned states[]={TVIS_BOLD,TVIS_CUT,TVIS_CUT,TVIS_BOLD,0};
  for(i=0;item;item=TreeView_GetNextSibling(tree,item)) {
    TVITEMW value={.mask=TVIF_STATE,.hItem=item,.stateMask=TVIS_BOLD|TVIS_CUT};
    CHECK(TreeView_GetItem(tree,&value));CHECK(value.state==states[i++]);
    NMTVCUSTOMDRAW paint={.nmcd={.hdr={.hwndFrom=tree,.idFrom=ID_PROPERTIES,.code=NM_CUSTOMDRAW},
      .dwDrawStage=CDDS_ITEMPREPAINT,.dwItemSpec=(DWORD_PTR)item}};
    SendMessageW(window,WM_NOTIFY,ID_PROPERTIES,(LPARAM)&paint);
    CHECK(paint.clrText==((value.state&TVIS_BOLD)?RGB(37,76,154):(value.state&TVIS_CUT)?RGB(112,120,132):RGB(35,49,70)));
  }
  CHECK(i==5);g_hide_zero_false=TRUE;inspect_properties(&object);CHECK(TreeView_GetCount(tree)==full-2);
  wchar_t raw[1024];GetWindowTextW(GetDlgItem(window,ID_RAW),raw,1024);CHECK(wcsstr(raw,L"false")&&wcsstr(raw,L"zero"));
  g_hide_zero_false=FALSE;inspect_properties(&object);CHECK(TreeView_GetCount(tree)==full);
  inspect_properties(NULL);
  puts("PASS: true/non-zero emphasis, signed/exponent zero, underflow, strings/null unchanged, optional hide and complete Raw Data");
}
static void copy_value_tests(HWND window) {
  wchar_t text[2200],copied[2200];
  const wchar_t *hash=L"8dc1645e2ed9ee33aa75297e01b94d0c";
  inspector_copy_value(L"position: [316.0, -71.70, 128.9]",text);
  CHECK(!wcscmp(text,L"316.0, -71.70, 128.9"));
  inspector_copy_value(L"hulls → 8dc1645e2ed9ee33aa75297e01b94d0c",text);CHECK(!wcscmp(text,hash));
  inspector_copy_value(L"position: [1, 2]",text);CHECK(!wcscmp(text,L"[1, 2]"));
  const wchar_t *multiple=L"links: 8dc1645e2ed9ee33aa75297e01b94d0c 00000000000000000000000000000001";
  inspector_copy_value(multiple,text);CHECK(!wcscmp(text,multiple));
  HWND tree=GetDlgItem(window,ID_PROPERTIES),value=g_text_selection;
  HTREEITEM item=property_item(TVI_ROOT,"position: [316.0, -71.70, 128.9]",0);
  TreeView_SelectItem(tree,item);
  CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  RECT row;CHECK(TreeView_GetItemRect(tree,item,&row,TRUE));
  CHECK(inspector_text_begin(tree,(POINT){row.left+4,(row.top+row.bottom)/2}));
  CHECK(GetParent(value)==tree&&(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  GetWindowTextW(value,text,2200);CHECK(!wcscmp(text,L"position: [316.0, -71.70, 128.9]"));
  /* Ordinary mouse dragging selects the clicked text in place, not a footer. */
  PostMessageW(value,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(90,5));
  PostMessageW(value,WM_LBUTTONUP,0,MAKELPARAM(90,5));
  SendMessageW(value,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,5));
  MSG drag;while(PeekMessageW(&drag,value,WM_MOUSEFIRST,WM_MOUSELAST,PM_REMOVE))DispatchMessageW(&drag);
  DWORD first=0,last=0;SendMessageW(value,EM_GETSEL,(WPARAM)&first,(LPARAM)&last);CHECK(last>first);
  SendMessageW(value,WM_COPY,0,0);CHECK(clipboard_read(window,copied,2200));
  CHECK(wcslen(copied)==last-first&&!wcsncmp(copied,text+first,last-first));
  int previous=g_object;fixture_keys[VK_CONTROL]=(SHORT)0x8000;
  SendMessageW(tree,WM_KEYDOWN,'C',0);fixture_keys[VK_CONTROL]=0;
  CHECK(clipboard_read(window,copied,2200)&&!wcscmp(copied,L"316.0, -71.70, 128.9"));
  CHECK(g_object==previous&&!g_navigation.pending);
  int number=(int)(wcsstr(text,L"-71.70")-text);
  SendMessageW(value,EM_SETSEL,number,number+6);SendMessageW(value,WM_COPY,0,0);
  CHECK(clipboard_read(window,copied,2200)&&!wcscmp(copied,L"-71.70"));
  SendMessageW(value,WM_CHAR,'x',0);GetWindowTextW(value,copied,2200);CHECK(!wcscmp(copied,text));
  HWND summary=GetDlgItem(window,ID_OBJECT);SetWindowTextW(summary,hash);
  SendMessageW(summary,EM_SETSEL,0,32);SendMessageW(summary,WM_COPY,0,0);
  CHECK(clipboard_read(window,copied,2200)&&!wcscmp(copied,hash));
  HWND list=GetDlgItem(window,ID_OUTGOING);
  inspector_set_view(3);inspector_data_view();
  CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  SendMessageW(list,LB_ADDSTRING,0,(LPARAM)L"8dc1645e2ed9ee33aa75297e01b94d0c | Hull");
  SendMessageW(list,LB_SETCURSEL,0,0);
  CHECK(SendMessageW(list,LB_GETITEMRECT,0,(LPARAM)&row)!=LB_ERR);
  CHECK(inspector_text_begin(list,(POINT){row.left+4,(row.top+row.bottom)/2}));
  GetWindowTextW(value,text,2200);CHECK(wcsstr(text,hash)==text);SendMessageW(list,WM_COPY,0,0);
  CHECK(clipboard_read(window,copied,2200)&&!wcscmp(copied,hash));
  CHECK(g_object==previous&&!g_navigation.pending);
  SendMessageW(list,WM_MOUSEWHEEL,0,0);CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  SendMessageW(list,LB_RESETCONTENT,0,0);inspect_properties(NULL);
  inspector_set_view(0);inspector_data_view();
  CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  HWND picks=GetDlgItem(window,ID_HITS);
  SendMessageW(picks,LB_ADDSTRING,0,(LPARAM)hash);
  CHECK(SendMessageW(picks,LB_GETITEMRECT,0,(LPARAM)&row)!=LB_ERR);
  LPARAM click=MAKELPARAM(row.left+4,(row.top+row.bottom)/2);
  SendMessageW(picks,WM_LBUTTONDOWN,MK_LBUTTON,click);
  SendMessageW(picks,WM_LBUTTONUP,0,click);
  CHECK(GetFocus()==picks&&SendMessageW(picks,LB_GETCURSEL,0,0)==0);
  CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  SendMessageW(picks,WM_COPY,0,0);CHECK(clipboard_read(window,copied,2200)&&!wcscmp(copied,hash));
  SendMessageW(picks,WM_LBUTTONDOWN,MK_LBUTTON,click);
  SendMessageW(picks,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(90,(row.top+row.bottom)/2));
  CHECK(GetFocus()==value&&g_text_source==picks&&g_pick_text_drag);
  SendMessageW(value,WM_LBUTTONUP,0,MAKELPARAM(90,5));
  SendMessageW(value,EM_GETSEL,(WPARAM)&first,(LPARAM)&last);CHECK(last>first);
  SendMessageW(value,WM_COPY,0,0);CHECK(clipboard_read(window,copied,2200));
  CHECK(wcslen(copied)==last-first&&!wcsncmp(copied,hash+first,last-first));
  SetFocus(tree);CHECK(!(GetWindowLongPtrW(value,GWL_STYLE)&WS_VISIBLE));
  CHECK(SendMessageW(picks,LB_GETCURSEL,0,0)==0);
  SendMessageW(picks,LB_RESETCONTENT,0,0);
  puts("PASS: picked-row click keeps native focused selection; drag selects substrings; focus-away preserves row; both copy paths work");
  puts("PASS: in-place hash/coordinate selection, precise tokens, partial read-only copy, scroll/view cleanup, list/tree copy without navigation");
}
static void hull_graph_tests(void) {
  hv_object objects[80];hv_reference refs[80];char hashes[80][33];
  for(int i=0;i<80;i++){snprintf(hashes[i],33,"%032x",i);objects[i]=(hv_object){hashes[i],"Hull","fixture","","{}"};}
  objects[0].kind="Trigger";objects[1].kind=objects[3].kind=objects[6].kind="Clump";
  objects[5].kind="Trigger";objects[5].type="EventFromRegion";objects[7].kind="Creature";
  hv_reference rows[]={
    {0,1,hashes[1],"Vars.hulls"},{1,2,hashes[2],"Objects[1]"},{1,3,hashes[3],"Objects[2]"},
    {3,4,hashes[4],"Objects[1]"},{3,1,hashes[1],"Objects[2]"},{3,2,hashes[2],"Objects[3]"},
    {5,1,hashes[1],"Vars.hulls"},{5,6,hashes[6],"Vars.effects"},{6,0,hashes[0],"Objects[1]"},
    {7,0,hashes[0],"trigger"},{7,8,hashes[8],"hull"},{1,8,hashes[8],"NotMembers"}};
  memcpy(refs,rows,sizeof(rows));hv_index index={.objects=objects,.refs=refs,.count=9,.ref_count=12};hv_hull_paths result;
  hv_find_hulls(&index,1,HV_HULL_MEMBERS,&result);CHECK(result.ok&&result.count==2&&!result.limited&&!result.missing);
  hv_find_hulls(&index,0,HV_HULL_SOURCES,&result);CHECK(result.ok&&result.count==2&&!result.limited);
  for(unsigned i=0;i<result.count;i++) {
    hv_hull_path *p=result.paths+i;CHECK(p->length>=4&&p->object[0]==0&&p->reverse[0]&&p->reverse[1]);
    CHECK(p->object[p->length]==2||p->object[p->length]==4);
    for(unsigned j=0;j<p->length;j++) {
      hv_reference *r=refs+p->edge[j];CHECK(p->object[j]==(p->reverse[j]?r->target:r->source));
      CHECK(p->object[j+1]==(p->reverse[j]?r->source:r->target));
    }
  }
  hv_find_hulls(&index,0,HV_HULL_REFERENCES,&result);CHECK(result.count==3);
  refs[12]=(hv_reference){3,-1,hashes[9],"Objects[4]"};index.ref_count++;
  hv_find_hulls(&index,1,HV_HULL_MEMBERS,&result);CHECK(result.count==2&&result.missing==1);
  objects[0].kind="Clump";index.count=80;index.ref_count=79;
  for(int i=1;i<80;i++){objects[i].kind="Hull";refs[i-1]=(hv_reference){0,i,hashes[i],"Objects[1]"};}
  hv_find_hulls(&index,0,HV_HULL_MEMBERS,&result);CHECK(result.ok&&result.count==64&&result.limited);
  index.count=15;index.ref_count=14;
  for(int i=0;i<14;i++){objects[i].kind="Clump";refs[i]=(hv_reference){i,i+1,hashes[i+1],"Objects[1]"};}
  hv_find_hulls(&index,0,HV_HULL_MEMBERS,&result);CHECK(result.ok&&!result.count&&result.limited);
  hv_find_hulls(&index,-1,HV_HULL_MEMBERS,&result);CHECK(!result.ok);
  puts("PASS: nested clumps, cycles/duplicates, missing members, shortest directional region paths, creature isolation and search limits");
}
static void run_case(BOOL conflict) {
  wchar_t path[MAX_PATH], cmd[MAX_PATH + 40], name[96];
  GetModuleFileNameW(NULL, path, MAX_PATH);
  wchar_t *slash = wcsrchr(path, L'\\'); CHECK(slash);
  lstrcpyW(slash + 1, L"fixture_target.exe");
  _snwprintf(cmd, MAX_PATH + 40, L"\"%ls\" %ls", path, conflict ? L"--conflict" : L"");
  STARTUPINFOW si = {.cb = sizeof(si)}; PROCESS_INFORMATION pi = {0};
  CHECK(CreateProcessW(path, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
  CloseHandle(pi.hThread);
  hv_object_name(name, 96, L"FixtureReady", pi.dwProcessId);
  HANDLE ready = NULL;
  for (int i = 0; i < 100 && !ready; ++i) { ready = OpenEventW(SYNCHRONIZE, FALSE, name); if (!ready) Sleep(30); }
  CHECK(ready); CHECK(WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0); CloseHandle(ready);
  connection c = {0}; BOOL ok = attach(&c, pi.dwProcessId);
  if (conflict) { CHECK(!ok); CHECK(wcsstr(c.message, L"conflicting hooks")); }
  else {
    if (!ok) fwprintf(stderr, L"attach: %ls\n", c.message);
    CHECK(ok && c.shared && c.shared->ready);
    CHECK(c.shared->config.mode == HV_HIDDEN);
    connection second = {0}; CHECK(!attach(&second, pi.dwProcessId));
    CHECK(wcsstr(second.message, L"Another viewer")); connection_close(&second);
    connection_close(&c);
    CHECK(attach(&c, pi.dwProcessId)); /* Panel reopen reuses the resident DLL. */
    BYTE byte; SIZE_T got;
    uintptr_t image = remote_module(pi.dwProcessId, L"fixture_target.exe", NULL); CHECK(image);
    CHECK(ReadProcessMemory(c.process, (void *)(image + 0xf4880), &byte, 1, &got) && byte == 0xe9);
    CHECK(ReadProcessMemory(c.process, (void *)(image + 0x2790c0), &byte, 1, &got) && byte == 0xe9);
    /* Exercise the actual controls without showing or capturing a window. */
    WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                   .lpszClassName = L"HullPanelFixture"};
    CHECK(RegisterClassW(&wc));
    HWND window = CreateWindowW(wc.lpszClassName, L"Fixture", WS_OVERLAPPEDWINDOW,
                                0, 0, 1200, 920, NULL, NULL, wc.hInstance, NULL);
    CHECK(window);
    button_corner_tests(window);
    for(unsigned i=0;i<6;i++)CHECK(SendMessageW(g_checks[i],BM_GETCHECK,0,0)==BST_UNCHECKED);
    CHECK(c.shared->config.mode==HV_HIDDEN);
    CHECK(SendMessageW(window,WM_GETICON,ICON_BIG,0));
    CHECK(SendMessageW(window,WM_GETICON,ICON_SMALL,0));
    const int icon_sizes[]={16,24,32,48,64,128,256};
    for(unsigned i=0;i<sizeof(icon_sizes)/sizeof(*icon_sizes);i++) {
      HICON icon=(HICON)LoadImageW(wc.hInstance,MAKEINTRESOURCEW(IDI_HULL_VIEWER),
          IMAGE_ICON,icon_sizes[i],icon_sizes[i],0);
      CHECK(icon);ICONINFO info={0};CHECK(GetIconInfo(icon,&info));
      BITMAP bitmap={0};CHECK(GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap));
      CHECK(bitmap.bmWidth==icon_sizes[i]&&bitmap.bmHeight==icon_sizes[i]);
      int edge=icon_sizes[i];BYTE *pixels=calloc((size_t)edge*edge,4);CHECK(pixels);
      BITMAPINFO dib={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=edge,.biHeight=-edge,
          .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
      HDC icon_dc=GetDC(window);
      CHECK(GetDIBits(icon_dc,info.hbmColor,0,edge,pixels,&dib,DIB_RGB_COLORS)==edge);
      ReleaseDC(window,icon_dc);
      CHECK(pixels[3]==0&&pixels[((edge/2)*edge+edge/2)*4+3]==255);
      /* Lanczos can leave <=4/255 ringing at the outermost tiny-size pixels. */
      for(int x=0;x<edge;x++)CHECK(pixels[x*4+3]<=4&&pixels[((edge-1)*edge+x)*4+3]<=4);
      free(pixels);
      DeleteObject(info.hbmColor);DeleteObject(info.hbmMask);DestroyIcon(icon);
    }
    puts("PASS: embedded application icon loads at all seven sizes with transparent margins and opaque center; large/small window icons assigned");
    property_value_tests(window);
    copy_value_tests(window);
    wchar_t angle_label[64];
    GetWindowTextW(GetDlgItem(window, ID_CAM_LOCK), angle_label, 64);
    CHECK(!wcscmp(angle_label, L"Set angle lock"));
    check_single_line(window, ID_HULL_HINT);
    check_single_line(window, ID_DRAW_LABEL);
    RECT label_rect, edit_rect, hint_rect, draw_rect;
    GetWindowRect(GetDlgItem(window, ID_HULL_HINT), &label_rect);
    GetWindowRect(g_names, &edit_rect);
    GetWindowRect(GetDlgItem(window, ID_CAM_HINT), &hint_rect);
    GetWindowRect(GetDlgItem(window, ID_HIDE), &draw_rect);
    CHECK(label_rect.bottom < edit_rect.top && hint_rect.top > draw_rect.bottom);
    RECT types_rect, status_rect;
    GetWindowRect(GetDlgItem(window, ID_TYPES_LABEL), &types_rect);
    GetWindowRect(g_status, &status_rect);
    CHECK(types_rect.left == edit_rect.left && types_rect.right == edit_rect.right);
    CHECK(types_rect.right < hint_rect.right);
    CHECK(GetWindowLongPtrW(GetDlgItem(window,ID_ATTACH),GWL_STYLE)&BS_OWNERDRAW);
    CHECK(GetDlgItem(window,ID_PROPERTIES)&&GetDlgItem(window,ID_RAW));
    check_single_line(window,ID_INSPECT_HINT);
    RECT row[5];const int row_ids[]={ID_BACK,ID_FORWARD,ID_COPY_HASH,ID_SHOW_HULL,ID_RELOAD_INDEX};
    for(unsigned i=0;i<5;i++) {
      CHECK(GetWindowRect(GetDlgItem(window,row_ids[i]),&row[i]));
      CHECK(row[i].top==row[0].top&&row[i].bottom==row[0].bottom);
      if(i)CHECK(row[i-1].right<row[i].left);
    }
    CHECK(row[2].right-row[2].left==ui_px(80));
    CHECK(row[3].right-row[3].left>=ui_px(80)&&row[3].right-row[3].left<=ui_px(100));
    CHECK(!IsWindowEnabled(GetDlgItem(window,ID_BACK))&&!IsWindowEnabled(GetDlgItem(window,ID_FORWARD)));
    check_summary_border(window);
    CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_OBJECT),GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL)));
    HWND summary_control=GetDlgItem(window,ID_OBJECT);
    SetWindowTextW(summary_control,L"Trigger: A deliberately long object description that extends beyond the narrow inspector summary width and must remain accessible without wrapping or losing its hash\r\n0123456789abcdef0123456789abcdef");
    ui_name_scrollbars(summary_control);
    CHECK(GetWindowLongPtrW(summary_control,GWL_STYLE)&WS_HSCROLL);
    CHECK(!(GetWindowLongPtrW(summary_control,GWL_STYLE)&WS_VSCROLL));
    SetWindowTextW(summary_control,L"Select a hull.");ui_name_scrollbars(summary_control);
    CHECK(!(GetWindowLongPtrW(summary_control,GWL_STYLE)&WS_HSCROLL));
    RECT edit_format;SendMessageW(GetDlgItem(window,ID_LOOKUP_HASH),EM_GETRECT,0,(LPARAM)&edit_format);
    CHECK(edit_format.top>0);
    CHECK(SendMessageW(GetDlgItem(window,ID_DATA_TABS),CB_GETCOUNT,0,0)==5);
    HWND view_selector=GetDlgItem(window,ID_DATA_TABS);
    SendMessageW(view_selector,WM_KEYDOWN,VK_DOWN,0);CHECK(inspector_view()==1);
    SendMessageW(view_selector,WM_KEYDOWN,VK_UP,0);CHECK(inspector_view()==0);
    RECT combo_rect;GetWindowRect(view_selector,&combo_rect);
    int combo_w=combo_rect.right-combo_rect.left,combo_h=combo_rect.bottom-combo_rect.top;
    HDC combo_screen=GetDC(view_selector),combo_dc=CreateCompatibleDC(combo_screen);
    HBITMAP combo_bitmap=CreateCompatibleBitmap(combo_screen,combo_w,combo_h);
    HGDIOBJ combo_old=SelectObject(combo_dc,combo_bitmap);
    SendMessageW(view_selector,WM_PRINT,(WPARAM)combo_dc,PRF_CLIENT|PRF_NONCLIENT);
    CHECK(GetPixel(combo_dc,0,0)==RGB(220,226,235));
    CHECK(GetPixel(combo_dc,combo_w-1,combo_h-1)==RGB(220,226,235));
    SelectObject(combo_dc,combo_old);DeleteObject(combo_bitmap);DeleteDC(combo_dc);ReleaseDC(view_selector,combo_screen);
    CHECK(!GetDlgItem(window,ID_PASTE)&&!GetDlgItem(window,ID_COPY));
    CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_APP_SUBTITLE),GWL_STYLE)&WS_VISIBLE));
    CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_APP_TITLE),GWL_STYLE)&WS_VISIBLE));
    int saved_side=g_sidebar_width;g_sidebar_width=240;panel_layout(window);
    HDC hash_dc=GetDC(g_names);HFONT fitted=(HFONT)SendMessageW(g_names,WM_GETFONT,0,0);
    CHECK(fitted==g_font&&fitted==(HFONT)SendMessageW(GetDlgItem(window,ID_PROPERTIES),WM_GETFONT,0,0));
    CHECK(fitted==(HFONT)SendMessageW(GetDlgItem(window,ID_HITS),WM_GETFONT,0,0));
    HGDIOBJ old_hash_font=SelectObject(hash_dc,fitted);SIZE hash_extent;
    CHECK(GetTextExtentPoint32W(hash_dc,L"ffffffffffffffffffffffffffffffff",32,&hash_extent));
    RECT name_area;GetClientRect(g_names,&name_area);
    CHECK(hash_extent.cx+ui_px(8)<=name_area.right);
    for(const wchar_t *digit=L"0123456789abcdef";*digit;digit++) {
      wchar_t full[33];for(unsigned i=0;i<32;i++)full[i]=*digit;full[32]=0;
      CHECK(GetTextExtentPoint32W(hash_dc,full,32,&hash_extent));
      CHECK(hash_extent.cx+ui_px(8)<=name_area.right);
    }
    RECT hit_area;GetClientRect(GetDlgItem(window,ID_HITS),&hit_area);CHECK(hash_extent.cx<=hit_area.right);
    SelectObject(hash_dc,old_hash_font);ReleaseDC(g_names,hash_dc);
    g_sidebar_width=saved_side;panel_layout(window);
    LOGFONTW normal,summary_font;
    CHECK(GetObjectW(g_font,sizeof(normal),&normal)&&GetObjectW(g_summary_font,sizeof(summary_font),&summary_font));
    CHECK(summary_font.lfHeight==normal.lfHeight);
    LOGFONTW hash_font,raw_font,button_font,camera_font;
    CHECK(GetObjectW(fitted,sizeof(hash_font),&hash_font));
    CHECK(GetObjectW(g_code_font,sizeof(raw_font),&raw_font));
    CHECK(GetObjectW(g_button_font,sizeof(button_font),&button_font));
    CHECK(GetObjectW(g_heading_font,sizeof(camera_font),&camera_font));
    CHECK(abs(hash_font.lfHeight)>abs(raw_font.lfHeight));
    CHECK(hash_font.lfHeight==normal.lfHeight&&hash_font.lfWidth==0&&!wcscmp(hash_font.lfFaceName,normal.lfFaceName));
    CHECK((HFONT)SendMessageW(GetDlgItem(window,ID_LOOKUP_HASH),WM_GETFONT,0,0)==g_font);
    CHECK(abs(button_font.lfHeight)<abs(normal.lfHeight)&&camera_font.lfWeight>=FW_SEMIBOLD);
    RECT camera_row,debug_row;GetWindowRect(GetDlgItem(window,ID_CAM_POS),&camera_row);
    GetWindowRect(GetDlgItem(window,ID_CAM_START),&debug_row);CHECK(camera_row.bottom<debug_row.top);
    /* Repeated view changes move the picked-list label and list; siblings
     * must clip each other and must not corrupt list contents. This fixture's
     * parent is hidden, so Windows need not retain a visible update region. */
    SendMessageW(GetDlgItem(window,ID_HITS),LB_ADDSTRING,0,(LPARAM)L"0123456789abcdef0123456789abcdef");
    for(unsigned pass=0;pass<100;pass++) {
      SendMessageW(window,WM_COMMAND,pass%2?ID_NAMES_TAB:ID_TYPES,0);
      HWND picks=GetDlgItem(window,ID_HITS);RECT label,pick;
      CHECK(GetWindowLongPtrW(picks,GWL_STYLE)&WS_CLIPSIBLINGS);
      GetWindowRect(GetDlgItem(window,ID_HITS_LABEL),&label);GetWindowRect(picks,&pick);
      CHECK(label.bottom<pick.top&&pick.bottom>pick.top);
      wchar_t row_text[64];CHECK(SendMessageW(picks,LB_GETCOUNT,0,0)==1);
      SendMessageW(picks,LB_GETTEXT,0,(LPARAM)row_text);CHECK(!wcscmp(row_text,L"0123456789abcdef0123456789abcdef"));
    }
    SendMessageW(GetDlgItem(window,ID_HITS),LB_RESETCONTENT,0,0);
    SendMessageW(window,WM_COMMAND,ID_TYPES,0);
    /* Layout adapts to client size; property and link panes stay inside it. */
    for(unsigned size=0;size<2;size++) {
      CHECK(SetWindowPos(window,NULL,0,0,size?1200:1020,size?840:740,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
      RECT client,child;GetClientRect(window,&client);
      const int ids[]={ID_PROPERTIES,ID_RAW,ID_INCOMING,ID_OUTGOING,ID_NAMES,ID_STATUS};
      for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
        GetWindowRect(GetDlgItem(window,ids[i]),&child);MapWindowPoints(NULL,window,(POINT *)&child,2);
        CHECK(child.left>=0&&child.top>=0&&child.right<=client.right&&child.bottom<=client.bottom);
        CHECK(child.right>child.left&&child.bottom>child.top);
      }
    }
    /* The toolbar's full control chain stays centered and evenly separated
     * at narrow/default/wide sizes, including the wrapping hint. */
    for(int width=720;width<=1440;width+=180) {
      RECT outer={0,0,ui_px(width),ui_px(620)};AdjustWindowRect(&outer,WS_OVERLAPPEDWINDOW,FALSE);
      CHECK(SetWindowPos(window,NULL,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
      const int cam_ids[]={ID_CAM_LABEL,ID_CAM_START,ID_CAM_STOP,ID_CAM_IGNORE,ID_CAM_LOCK};
      RECT previous={0},first={0};
      for(unsigned i=0;i<5;i++) {
        RECT current;GetWindowRect(GetDlgItem(window,cam_ids[i]),&current);
        if(!i)first=current;
        CHECK(abs(current.top+current.bottom-first.top-first.bottom)<=ui_px(2));
        if(i)CHECK(current.left-previous.right>=ui_px(i==6?6:i==2?8:12));
        previous=current;
      }
      RECT hint,pick,shortcut,tabs,focus,props;
      GetWindowRect(GetDlgItem(window,ID_CAM_HINT),&hint);CHECK(hint.top>previous.bottom);
      RECT footer_status;GetWindowRect(g_status,&footer_status);
      CHECK((footer_status.top==hint.top&&footer_status.right<hint.left)||footer_status.bottom<=hint.top);
      CHECK((GetWindowLongPtrW(GetDlgItem(window,ID_CAM_HINT),GWL_STYLE)&SS_TYPEMASK)==SS_LEFT);
      CHECK(ui_text_size(ID_CAM_HINT,MulDiv(hint.right-hint.left,96,(int)g_dpi)).cy<=32);
      const int position_ids[]={ID_CAM_POS,ID_CAM_TARGET_LABEL,ID_CAM_TARGET,ID_CAM_MOVE};
      RECT pos_previous={0};
      for(unsigned i=0;i<4;i++) {
        RECT current;GetWindowRect(GetDlgItem(window,position_ids[i]),&current);
        CHECK(current.top<first.top&&current.bottom<first.top);
        if(i){CHECK(current.left>pos_previous.right);CHECK(current.top==pos_previous.top&&current.bottom==pos_previous.bottom);}
        pos_previous=current;
      }
      RECT attach,target;GetWindowRect(GetDlgItem(window,ID_ATTACH),&attach);
      GetWindowRect(GetDlgItem(window,ID_CAM_TARGET),&target);
      CHECK(target.right-target.left==ui_px(128));
      CHECK(attach.left>pos_previous.right&&attach.left>previous.right);
      CHECK(abs(attach.right-first.left-ui_px(width-64))<=1);
      CHECK(SendMessageW(GetDlgItem(window,ID_ATTACH),WM_GETFONT,0,0)==(LRESULT)g_section_font);
      check_single_line(window,ID_ATTACH);
      CHECK(g_footer_reserve<=96);
      if(footer_status.top==hint.top)CHECK(g_footer_reserve==60);
      CHECK(attach.bottom-attach.top==ui_px(34));
      CHECK(abs(attach.top+attach.bottom-pos_previous.top-first.bottom)<=ui_px(1));
      CHECK(SendMessageW(GetDlgItem(window,ID_CAM_TARGET),WM_GETFONT,0,0)==SendMessageW(GetDlgItem(window,ID_CAM_IGNORE),WM_GETFONT,0,0));
      CHECK(SendMessageW(GetDlgItem(window,ID_CAM_TARGET_LABEL),WM_GETFONT,0,0)==SendMessageW(GetDlgItem(window,ID_CAM_IGNORE),WM_GETFONT,0,0));
      GetWindowRect(GetDlgItem(window,ID_PICK),&pick);GetWindowRect(GetDlgItem(window,ID_INSPECT_HINT),&shortcut);
      CHECK(pick.top==shortcut.top&&pick.bottom==shortcut.bottom&&pick.right<shortcut.left);
      GetWindowRect(GetDlgItem(window,ID_DATA_TABS),&tabs);GetWindowRect(GetDlgItem(window,ID_VALUE_FOCUS),&focus);
      GetWindowRect(GetDlgItem(window,ID_PROPERTIES),&props);
      RECT summary;GetWindowRect(GetDlgItem(window,ID_OBJECT),&summary);
      CHECK(summary.bottom-summary.top==ui_px(56));
      CHECK(abs(tabs.top-focus.top)<=ui_px(1)&&tabs.right<focus.left&&focus.bottom<props.top);
      CHECK(props.bottom-props.top>=ui_px(190));
      CHECK(!GetDlgItem(window,ID_VALUE_NOTE));
      CHECK(GetWindowLongPtrW(GetDlgItem(window,ID_CAM_LABEL),GWL_STYLE)&SS_CENTERIMAGE);
      wchar_t saved_status[512];GetWindowTextW(g_status,saved_status,512);
      panel_status_text(L"64 requested · 64 active · 64 inactive · 64 drawn\nCould not move the camera. The game is busy processing the previous request. Please wait for the current operation to finish and try again.\nFace tint unavailable. Outlines are still shown.\nClosing the viewer hides all hull drawings.");
      footer_layout measured=panel_footer(g_content_width);
      GetWindowRect(g_status,&footer_status);GetWindowRect(GetDlgItem(window,ID_CAM_HINT),&hint);
      CHECK(footer_status.bottom-footer_status.top>=ui_px(ui_text_size(ID_STATUS,measured.status_width).cy));
      CHECK((footer_status.top==hint.top&&footer_status.right<hint.left)||footer_status.bottom+ui_px(4)<=hint.top);
      CHECK(measured.reserve==g_footer_reserve);
      panel_status_text(saved_status);
    }
    CHECK(SetWindowPos(window,NULL,0,0,700,500,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    CHECK(g_content_height>=540&&g_content_width>=720);
    panel_scroll(window,SB_VERT,SB_BOTTOM);panel_scroll(window,SB_HORZ,SB_BOTTOM);
    /* Windows enforces the compact client minimum when the desktop permits
     * it; on a smaller work area the same controls remain scroll-accessible. */
    RECT compact;GetClientRect(window,&compact);
    CHECK(g_scroll_x>0||compact.right>=ui_px(720));
    CHECK(g_scroll_y>0||compact.bottom>=ui_px(540));
    RECT small_client,small_status;GetClientRect(window,&small_client);GetWindowRect(g_status,&small_status);
    MapWindowPoints(NULL,window,(POINT *)&small_status,2);
    CHECK(small_status.bottom<=small_client.bottom&&small_status.top>=0);
    CHECK(SetWindowPos(window,NULL,0,0,1020,740,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    CHECK(!g_scroll_x&&!g_scroll_y);
    int old_sidebar=g_sidebar_width;
    SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(ui_px(g_divider_x),ui_px(300)));
    CHECK(g_resizing_panes);
    SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(ui_px(g_divider_x+30),ui_px(300)));
    SendMessageW(window,WM_LBUTTONUP,0,0);
    CHECK(!g_resizing_panes&&g_sidebar_width==old_sidebar+30);
    g_sidebar_width=old_sidebar;panel_layout(window);
    SendMessageW(window,WM_COMMAND,ID_NAMES_TAB,0);
    CHECK(g_names_view&&(GetWindowLongPtrW(g_names,GWL_STYLE)&WS_VISIBLE));
    CHECK(!(GetWindowLongPtrW(g_checks[0],GWL_STYLE)&WS_VISIBLE));
    /* Exercise dialog routing: directly setting multiline text did not catch
     * Enter being consumed as a default-button action. No physical input. */
    CHECK(GetWindowLongPtrW(g_names, GWL_STYLE) & ES_WANTRETURN);
    CHECK(GetWindowLongPtrW(g_names, GWL_STYLE) & ES_AUTOHSCROLL);
    HWND raw_box=GetDlgItem(window,ID_RAW);
    SetWindowTextW(raw_box,L"short text");ui_raw_scrollbar(raw_box);
    CHECK(!(GetWindowLongPtrW(raw_box,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL|ES_AUTOHSCROLL)));
    wchar_t long_raw[2048];unsigned used=0;
    for(unsigned i=0;i<100;i++)used+=(unsigned)swprintf(long_raw+used,2048-used,L"line %u\r\n",i);
    SetWindowTextW(raw_box,long_raw);ui_raw_scrollbar(raw_box);
    CHECK(GetWindowLongPtrW(raw_box,GWL_STYLE)&WS_VSCROLL);
    SetWindowTextW(raw_box,L"");ui_raw_scrollbar(raw_box);
    CHECK(!(GetWindowLongPtrW(raw_box,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL)));
    SetWindowTextW(g_names,L"8dc1645e2ed9ee33aa75297e01b94d0c\r\nbd9d89c977ed505282b23220a5fb190d");
    ui_name_scrollbars(g_names);
    CHECK(!(GetWindowLongPtrW(g_names,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL)));
    SetWindowTextW(g_names,L"1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");
    ui_name_scrollbars(g_names);CHECK(GetWindowLongPtrW(g_names,GWL_STYLE)&WS_VSCROLL);
    SetWindowTextW(g_names,L"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    ui_name_scrollbars(g_names);CHECK(GetWindowLongPtrW(g_names,GWL_STYLE)&WS_HSCROLL);
    SetWindowTextW(g_names, L"first");
    SendMessageW(g_names, EM_SETSEL, 5, 5);
    MSG enter = {.hwnd = g_names, .message = WM_KEYDOWN, .wParam = VK_RETURN};
    CHECK(SendMessageW(g_names, WM_GETDLGCODE, VK_RETURN, (LPARAM)&enter) & DLGC_WANTMESSAGE);
    dispatch_panel_message(window, &enter);
    /* IsDialogMessage may itself translate/dispatch and return TRUE. Test the
     * resulting character and text, not its implementation-specific return. */
    MSG character;
    CHECK(PeekMessageW(&character, g_names, WM_CHAR, WM_CHAR, PM_REMOVE));
    CHECK(character.wParam == VK_RETURN);
    dispatch_panel_message(window, &character);
    wchar_t entered[64]; GetWindowTextW(g_names, entered, 64);
    CHECK(!wcscmp(entered, L"first\r\n"));
    SendMessageW(g_names, EM_REPLACESEL, TRUE, (LPARAM)L"second");
    GetWindowTextW(g_names, entered, 64);
    CHECK(!wcscmp(entered, L"first\r\nsecond"));
    g_connection = c;
    /* Fresh panel: the first check shows only that type; unchecking hides it. */
    panel_check_types(0);
    SendMessageW(g_checks[3],BM_CLICK,0,0);
    CHECK(g_connection.shared->config.mode==HV_TYPES&&g_connection.shared->config.include_mask==(1u<<3));
    for(unsigned i=0;i<6;i++)CHECK(SendMessageW(g_checks[i],BM_GETCHECK,0,0)==(i==3?BST_CHECKED:BST_UNCHECKED));
    SendMessageW(g_checks[3],BM_CLICK,0,0);
    CHECK(g_connection.shared->config.mode==HV_HIDDEN&&!g_connection.shared->config.include_mask);
    puts("PASS: initially unchecked types match hidden drawing; first checkbox click shows only its type and unchecking hides it");
    memset(&c, 0, sizeof(c));
    g_connection.shared->inspect_available=1;EnableWindow(GetDlgItem(window,ID_PICK),TRUE);
    fixture_keys[VK_CONTROL]=(SHORT)0x8000;
    MSG shortcut={.hwnd=window,.message=WM_KEYDOWN,.wParam='I'};
    dispatch_panel_message(window,&shortcut);CHECK(g_connection.shared->inspect_mode==1&&g_pick_enabled);
    CHECK(SendMessageW(GetDlgItem(window,ID_PICK),BM_GETCHECK,0,0)==BST_CHECKED);
    shortcut.lParam|=1u<<30;dispatch_panel_message(window,&shortcut);CHECK(g_connection.shared->inspect_mode==1);
    shortcut.lParam=0;dispatch_panel_message(window,&shortcut);CHECK(!g_connection.shared->inspect_mode&&!g_pick_enabled);
    fixture_keys[VK_CONTROL]=0;
    g_connection.shared->inspect_mode=1;inspector_tick();CHECK(g_pick_enabled);
    CHECK(SendMessageW(GetDlgItem(window,ID_PICK),BM_GETCHECK,0,0)==BST_CHECKED);
    g_connection.shared->inspect_mode=0;inspector_tick();CHECK(!g_pick_enabled);
    SendMessageW(window, WM_COMMAND, ID_ALL, 0);
    CHECK(g_connection.shared->config.mode == HV_ALL);
    CHECK(!GetDlgItem(window,ID_EXCEPT));
    for(unsigned mask=0;mask<64;mask++) {
      panel_check_types(mask);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_CHECK,BN_CLICKED),(LPARAM)g_checks[0]);
      CHECK(g_connection.shared->config.mode==(mask?HV_TYPES:HV_HIDDEN));
      CHECK(g_connection.shared->config.include_mask==mask&&!g_connection.shared->config.exclude_mask);
    }
    panel_check_types(61);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_CHECK,BN_CLICKED),(LPARAM)g_checks[0]);
    /* Mock only camera acknowledgements; fixture target never runs game Lua. */
    CHECK(!g_connection.shared->camera_available);
    g_connection.shared->camera_available = 1;
    g_camera_level=g_connection.shared->live_level;g_camera_read_time=GetTickCount64();
    SendMessageW(window, WM_COMMAND, ID_CAM_START, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_START && g_camera_pending);
    uint32_t camera_revision = g_connection.shared->camera_request;
    SendMessageW(window, WM_COMMAND, ID_CAM_STOP, 0);
    CHECK(g_connection.shared->camera_request == camera_revision); /* No overwrite. */
    g_connection.shared->camera_completed = camera_revision;
    g_connection.shared->camera_ok = 1; g_connection.shared->camera_bits = 9;
    SendMessageW(window, WM_TIMER, 1, 0);
    CHECK(!g_camera_pending);
    CHECK(!IsWindowEnabled(GetDlgItem(window,ID_CAM_START))&&IsWindowEnabled(GetDlgItem(window,ID_CAM_STOP)));
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_IGNORE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_GETCHECK, 0, 0) == BST_CHECKED);
    const int refresh_ids[]={ID_CAM_START,ID_CAM_STOP,ID_CAM_IGNORE,ID_CAM_LOCK,ID_CAM_MOVE,ID_CAM_POS,ID_STATUS};
    for(unsigned i=0;i<sizeof(refresh_ids)/sizeof(*refresh_ids);i++)SetWindowSubclass(GetDlgItem(window,refresh_ids[i]),refresh_trace,77,0);
    fixture_enable_messages=fixture_check_messages=fixture_text_messages=0;
    for(unsigned i=0;i<20;i++)SendMessageW(window,WM_TIMER,1,0);
    CHECK(!fixture_enable_messages&&!fixture_check_messages&&!fixture_text_messages);
    send_camera(HV_CAM_READ);CHECK(g_camera_pending&&g_camera_poll);
    g_connection.shared->camera_completed=g_connection.shared->camera_request;
    SendMessageW(window,WM_TIMER,1,0);
    CHECK(!fixture_enable_messages&&!fixture_check_messages&&!fixture_text_messages);
    for(unsigned i=0;i<sizeof(refresh_ids)/sizeof(*refresh_ids);i++)RemoveWindowSubclass(GetDlgItem(window,refresh_ids[i]),refresh_trace,77);
    SendMessageW(window, WM_COMMAND, ID_CAM_IGNORE, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_COLLIDE);
    g_connection.shared->camera_completed = g_connection.shared->camera_request;
    g_connection.shared->camera_bits = 6;
    SendMessageW(window, WM_TIMER, 1, 0);
    CHECK(IsWindowEnabled(GetDlgItem(window,ID_CAM_START))&&!IsWindowEnabled(GetDlgItem(window,ID_CAM_STOP)));
    SendMessageW(window, WM_COMMAND, ID_CAM_LOCK, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_LOCK);
    g_connection.shared->camera_completed = g_connection.shared->camera_request;
    g_connection.shared->camera_ok = 0;
    lstrcpyA(g_connection.shared->camera_message, "Camera test error");
    SendMessageW(window, WM_TIMER, 1, 0);
    CHECK(wcsstr(g_notice, L"Camera test error"));
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_GETCHECK, 0, 0) == BST_INDETERMINATE);
    /* Level transition invalidates old state and asks for a fresh read. */
    g_camera_state_known=TRUE;g_camera_state_bits=1;
    g_connection.shared->live_level++;
    SendMessageW(window,WM_TIMER,1,0);
    CHECK(!g_camera_state_known&&g_camera_pending&&g_camera_poll);
    CHECK(!IsWindowEnabled(GetDlgItem(window,ID_CAM_MOVE)));
    CHECK(g_connection.shared->camera_action==HV_CAM_READ);
    g_connection.shared->camera_completed=g_connection.shared->camera_request;
    g_connection.shared->camera_ok=1;g_connection.shared->camera_bits=0;
    SendMessageW(window,WM_TIMER,1,0);
    CHECK(!g_camera_pending&&g_camera_state_known&&!g_camera_state_bits);
    CHECK(IsWindowEnabled(GetDlgItem(window,ID_CAM_START))&&!IsWindowEnabled(GetDlgItem(window,ID_CAM_STOP)));
    uint32_t poll_revision=g_connection.shared->camera_request;
    SendMessageW(window,WM_TIMER,1,0);
    CHECK(g_connection.shared->camera_request==poll_revision);
    g_camera_state_bits=1;camera_controls_enabled(TRUE);
    SetWindowTextW(GetDlgItem(window,ID_CAM_TARGET),L"123.5, -4, 800");
    SendMessageW(window,WM_COMMAND,ID_CAM_MOVE,0);
    CHECK(g_connection.shared->camera_action==HV_CAM_MOVE);
    CHECK(g_connection.shared->camera_target[0]==123.5f&&g_connection.shared->camera_target[1]==-4&&g_connection.shared->camera_target[2]==800);
    g_connection.shared->camera_completed=g_connection.shared->camera_request;
    g_connection.shared->camera_bits=1;SendMessageW(window,WM_TIMER,1,0);
    CHECK(!wcscmp(g_notice,L"Camera moved to 123.5, -4, 800."));
    SendMessageW(window,WM_TIMER,1,0);CHECK(wcsstr(g_notice,L"Camera moved to"));
    poll_revision=g_connection.shared->camera_request;
    SetWindowTextW(GetDlgItem(window,ID_CAM_TARGET),L"1 2 nan");
    SendMessageW(window,WM_COMMAND,ID_CAM_MOVE,0);
    CHECK(g_connection.shared->camera_request==poll_revision);
    CHECK(!wcscmp(g_notice,L"Enter valid X, Y, Z coordinates."));
    SetWindowTextW(GetDlgItem(window,ID_CAM_TARGET),L"1, 2, 3");
    SendMessageW(window,WM_COMMAND,ID_CAM_MOVE,0);
    g_connection.shared->camera_completed=g_connection.shared->camera_request;
    g_connection.shared->camera_ok=0;lstrcpyA(g_connection.shared->camera_message,"Test failure.");
    SendMessageW(window,WM_TIMER,1,0);
    CHECK(!wcscmp(g_notice,L"Could not move the camera. Test failure."));
    SetWindowTextW(GetDlgItem(window,ID_OPACITY),L"70");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_OPACITY,EN_CHANGE),0);
    CHECK(g_connection.shared->config.opacity==70);
    SetWindowTextW(GetDlgItem(window,ID_OPACITY),L"101");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_OPACITY,EN_CHANGE),0);
    CHECK(g_connection.shared->config.opacity==70);
    SetWindowTextW(GetDlgItem(window,ID_OPACITY),L"0");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_OPACITY,EN_CHANGE),0);
    CHECK(g_connection.shared->config.opacity==0);
    SetWindowTextW(GetDlgItem(window,ID_OPACITY),L"100");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_OPACITY,EN_CHANGE),0);
    CHECK(g_connection.shared->config.opacity==100);
    SetWindowTextW(GetDlgItem(window,ID_OPACITY),L"25");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_OPACITY,EN_CHANGE),0);
    SendMessageW(window, WM_COMMAND, ID_TYPES, 0);
    CHECK(g_connection.shared->config.include_mask == 61);
    SetWindowTextW(g_names, L"8dc1645e2ed9ee33aa75297e01b94d0c\r\nbd9d89c977ed505282b23220a5fb190d");
    SendMessageW(window, WM_COMMAND, ID_SELECTED, 0);
    CHECK(g_connection.shared->config.mode == HV_SELECTED && g_connection.shared->config.count == 2);
    CHECK(g_connection.shared->config.fill_faces==HV_TINT_FACES);
    SendMessageW(GetDlgItem(window,ID_FILL),BM_SETCHECK,BST_UNCHECKED,0);
    SendMessageW(window,WM_COMMAND,ID_FILL,0);CHECK(!g_connection.shared->config.fill_faces);
    CHECK(g_connection.shared->config.mode==HV_SELECTED&&g_connection.shared->config.count==2);
    SendMessageW(GetDlgItem(window,ID_FILL),BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(window,WM_COMMAND,ID_FILL,0);CHECK(g_connection.shared->config.fill_faces);
    CHECK(g_connection.shared->config.include_inactive);
    CHECK(!strcmp(g_connection.shared->config.names[0], "8dc1645e2ed9ee33aa75297e01b94d0c"));
    g_connection.shared->applied_revision = g_connection.shared->revision;
    g_connection.shared->inactive_matches = 1;
    g_connection.shared->config.fill_faces=HV_TINT_FACES;
    lstrcpyA(g_connection.shared->face_status,"Faces: post-fog.");g_notice[0]=0;
    SendMessageW(window, WM_TIMER, 1, 0);
    wchar_t face_status[256];GetWindowTextW(g_status,face_status,256);
    CHECK(!wcsstr(face_status,L"post-fog"));g_connection.shared->face_status[0]=0;
    wchar_t summary[512]; GetWindowTextW(g_status, summary, 512);
    CHECK(wcsstr(summary, L"0 active · 1 inactive"));
    uint32_t revision = g_connection.shared->revision;
    SetWindowTextW(g_names, L"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    SendMessageW(window, WM_COMMAND, ID_SELECTED, 0);
    CHECK(g_connection.shared->revision == revision);
    SendMessageW(window, WM_TIMER, 1, 0);
    wchar_t status[512]; GetWindowTextW(g_status, status, 512);
    CHECK(wcsstr(status, L"Enter up to 64")); /* Error survives status refresh. */
    CHECK(clipboard_write(window, L"old clipboard"));
    SetWindowTextW(g_names, L"");
    SendMessageW(window, WM_COMMAND, ID_PASTE, 0);
    GetWindowTextW(g_names, status, 512); CHECK(!wcscmp(status, L"old clipboard"));
    CHECK(fixture_clipboard_owner == window);
    CHECK(clipboard_write(window, L"8dc1645e2ed9ee33aa75297e01b94d0c"));
    SendMessageW(g_names, EM_SETSEL, 0, -1);
    SendMessageW(g_names, WM_PASTE, 0, 0);
    GetWindowTextW(g_names, status, 512); CHECK(!wcscmp(status, L"8dc1645e2ed9ee33aa75297e01b94d0c"));
    fixture_clipboard_busy = TRUE;
    SetWindowTextW(g_names, L"");
    SendMessageW(window, WM_COMMAND, ID_PASTE, 0);
    CHECK(GetWindowTextLengthW(g_names) == 0); /* Failed read never reuses old data. */
    fixture_clipboard_busy = FALSE;
    SetWindowTextW(g_names, L"copy this part");
    SendMessageW(g_names, EM_SETSEL, 5, 9);
    SendMessageW(g_names, WM_COPY, 0, 0);
    CHECK(clipboard_read(window, status, 512) && !wcscmp(status, L"this"));
    if(fixture_with_index) {
    /* Generated links are parsed outside the game, then navigated using the
     * actual Win32 list notifications. No physical input or live game state. */
    g_connection.shared->live_level=4;inspector_tick();
    CHECK(g_index_level==4&&g_index.count>5000);
    persistent_highlight_tests(window);
    inspect_select("80706360ff97d5461c413630495f6235",FALSE);CHECK(g_object>=0);
    uint32_t lookup_revision=g_connection.shared->revision;
    SetWindowTextW(GetDlgItem(window,ID_LOOKUP_HASH),L"  F070E30D2EE3C6D8F5B526B6285EAF29  ");
    MSG lookup_enter={.hwnd=GetDlgItem(window,ID_LOOKUP_HASH),.message=WM_KEYDOWN,.wParam=VK_RETURN};
    dispatch_panel_message(window,&lookup_enter);CHECK(g_navigation.pending);drain_navigation(window);
    CHECK(!strcmp(g_inspected_hash,"f070e30d2ee3c6d8f5b526b6285eaf29"));
    CHECK(!strcmp(g_index.objects[g_object].kind,"Trigger"));
    CHECK(g_connection.shared->revision==lookup_revision); /* Open isn't Draw/Start. */
    const wchar_t *bad_hashes[]={L"short",L"g070e30d2ee3c6d8f5b526b6285eaf29",L"ffffffffffffffffffffffffffffffff"};
    for(unsigned i=0;i<3;i++) {
      SetWindowTextW(GetDlgItem(window,ID_LOOKUP_HASH),bad_hashes[i]);inspector_command(ID_LOOKUP_OPEN);
      CHECK(!g_navigation.pending&&!strcmp(g_inspected_hash,"f070e30d2ee3c6d8f5b526b6285eaf29"));
    }
    SetWindowTextW(GetDlgItem(window,ID_LOOKUP_HASH),L"80706360ff97d5461c413630495f6235");
    inspector_command(ID_LOOKUP_OPEN);drain_navigation(window);
    CHECK(!strcmp(g_inspected_hash,"80706360ff97d5461c413630495f6235"));
    CHECK(TreeView_GetCount(GetDlgItem(window,ID_PROPERTIES))>=4);
    wchar_t provenance[256];GetWindowTextW(GetDlgItem(window,ID_PROVENANCE),provenance,256);
    CHECK(wcsstr(provenance,L"Offline lookup")&&wcsstr(provenance,L"not verified"));
    inspector_set_view(1);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_DATA_TABS,CBN_SELCHANGE),0);
    CHECK(GetWindowLongPtrW(GetDlgItem(window,ID_RAW),GWL_STYLE)&WS_VISIBLE);
    CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_PROPERTIES),GWL_STYLE)&WS_VISIBLE));
    wchar_t raw[4096];GetWindowTextW(GetDlgItem(window,ID_RAW),raw,4096);CHECK(wcsstr(raw,L"\r\n  \"source\": "));
    inspect_select("80706360ff97d5461c413630495f6235",FALSE);
    CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_PROPERTIES),GWL_STYLE)&WS_VISIBLE));
    for(int view=2;view<4;view++) {
      inspector_set_view(view);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_DATA_TABS,CBN_SELCHANGE),0);
      CHECK(GetWindowLongPtrW(GetDlgItem(window,view==2?ID_INCOMING:ID_OUTGOING),GWL_STYLE)&WS_VISIBLE);
      CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_PROPERTIES),GWL_STYLE)&WS_VISIBLE));
      CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_RAW),GWL_STYLE)&WS_VISIBLE));
      inspect_select("80706360ff97d5461c413630495f6235",FALSE);
      CHECK(!(GetWindowLongPtrW(GetDlgItem(window,ID_PROPERTIES),GWL_STYLE)&WS_VISIBLE));
    }
    inspector_set_view(0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_DATA_TABS,CBN_SELCHANGE),0);
    const int view_ids[]={ID_PROPERTIES,ID_RAW,ID_INCOMING,ID_OUTGOING,ID_HULL_LINKS};
    for(unsigned pass=0;pass<100;pass++) {
      unsigned selected=pass%5;inspector_set_view((int)selected);inspector_data_view();
      CHECK(!(GetWindowLongPtrW(g_text_selection,GWL_STYLE)&WS_VISIBLE));
      RECT expected;GetWindowRect(GetDlgItem(window,ID_PROPERTIES),&expected);
      for(unsigned i=0;i<5;i++) {
        HWND pane=GetDlgItem(window,view_ids[i]);RECT actual;GetWindowRect(pane,&actual);
        CHECK(EqualRect(&expected,&actual));
        CHECK(!!(GetWindowLongPtrW(pane,GWL_STYLE)&WS_VISIBLE)==(i==selected));
      }
    }
    inspector_set_view(0);inspector_data_view();
    hv_index_free(&g_index);lstrcpyA(g_index_error,"Missing inspection/Level_Cave.hvi beside this EXE.");
    inspect_select("80706360ff97d5461c413630495f6235",FALSE);
    wchar_t missing[1024];GetWindowTextW(GetDlgItem(window,ID_OBJECT),missing,1024);
    CHECK(wcsstr(missing,L"Missing inspection/")&&!wcsstr(missing,L"No authored record"));
    SendMessageW(window,WM_COMMAND,ID_RELOAD_INDEX,0);
    CHECK(g_object>=0&&g_index.count>5000&&!g_index_error[0]);
    inspect_pick_hull("8dc1645e2ed9ee33aa75297e01b94d0c");
    CHECK(g_object>=0&&!strcmp(g_index.objects[g_object].kind,"Hull"));
    CHECK(!strcmp(g_connection.shared->highlight,"8dc1645e2ed9ee33aa75297e01b94d0c"));
    HWND incoming=GetDlgItem(window,ID_INCOMING);
    CHECK(SendMessageW(incoming,LB_GETCOUNT,0,0)>=2);
    SendMessageW(incoming,LB_SETCURSEL,0,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_INCOMING,LBN_DBLCLK),(LPARAM)incoming);
    CHECK(g_navigation.pending&&!strcmp(g_index.objects[g_object].kind,"Hull"));
    drain_navigation(window);
    CHECK(g_history_count>=1&&strcmp(g_index.objects[g_object].kind,"Hull"));
    CHECK(!strcmp(g_connection.shared->highlight,"8dc1645e2ed9ee33aa75297e01b94d0c"));
    char linked_hash[33];lstrcpyA(linked_hash,g_inspected_hash);
    SendMessageW(window,WM_COMMAND,ID_BACK,0);
    CHECK(!strcmp(g_index.objects[g_object].hash,"8dc1645e2ed9ee33aa75297e01b94d0c"));
    CHECK(g_forward_count==1&&IsWindowEnabled(GetDlgItem(window,ID_FORWARD)));
    SendMessageW(window,WM_COMMAND,ID_FORWARD,0);CHECK(!strcmp(g_inspected_hash,linked_hash));
    CHECK(!g_forward_count&&!IsWindowEnabled(GetDlgItem(window,ID_FORWARD)));
    SendMessageW(window,WM_COMMAND,ID_BACK,0);
    unsigned history_size=g_history_count;
    inspect_select("8dc1645e2ed9ee33aa75297e01b94d0c",TRUE);
    CHECK(g_forward_count==1&&g_history_count==history_size); /* Reopening current object isn't a new visit. */
    inspect_select("80706360ff97d5461c413630495f6235",TRUE);
    CHECK(!g_forward_count&&!IsWindowEnabled(GetDlgItem(window,ID_FORWARD)));
    SendMessageW(window,WM_COMMAND,ID_BACK,0);
    CHECK(!strcmp(g_inspected_hash,"8dc1645e2ed9ee33aa75297e01b94d0c"));
    SendMessageW(window,WM_COMMAND,ID_COPY_HASH,0);
    CHECK(clipboard_read(window,status,512)&&!wcscmp(status,L"8dc1645e2ed9ee33aa75297e01b94d0c"));
    SendMessageW(window,WM_COMMAND,ID_SHOW_HULL,0);
    CHECK(g_connection.shared->config.mode==HV_SELECTED&&g_connection.shared->config.count==1);
    /* Opening timeline entries use bare TriggerName strings, not $ref objects. */
    inspect_select("d00b9fce57bce1fe2e6f1f53983009bc",TRUE);
    queue_tree_link(window,"dadd98ccd1789116423245fc0182a265");
    SendMessageW(window,WM_COMMAND,ID_BACK,0);
    CHECK(!strcmp(g_inspected_hash,"d00b9fce57bce1fe2e6f1f53983009bc"));
    queue_tree_link(window,"056f95242a8b9c9961c2397f5b7bdcef");
    SendMessageW(window,WM_COMMAND,ID_BACK,0);
    SendMessageW(window,WM_COMMAND,ID_FORWARD,0);
    CHECK(!strcmp(g_inspected_hash,"056f95242a8b9c9961c2397f5b7bdcef"));
    /* Multi-hop path: region -> Branch56 -> clump -> SetLobbyFlag18.
     * Empty Vars at the terminal trigger is a normal leaf, not an index end. */
    inspect_select("05abb487918787af2bde690f614b7068",FALSE);
    queue_tree_link(window,"3a95a4e0e3afbd0cac62d4fa33c3b3ee");
    queue_tree_link(window,"e52def624c49cdf66b663d666c2f457e");
    queue_tree_link(window,"f070e30d2ee3c6d8f5b526b6285eaf29");
    HWND tree=GetDlgItem(window,ID_PROPERTIES);TreeView_SelectItem(tree,TreeView_GetRoot(tree));
    NMHDR leaf={.hwndFrom=tree,.idFrom=ID_PROPERTIES,.code=NM_DBLCLK};int before=g_object;
    SendMessageW(window,WM_NOTIFY,ID_PROPERTIES,(LPARAM)&leaf);drain_navigation(window);CHECK(g_object==before);
    /* Reject full-width stale/invalid item data; truncating to unsigned32 is unsafe. */
    TVITEMW invalid={.mask=TVIF_PARAM,.hItem=TreeView_GetRoot(tree),.lParam=(LPARAM)0x100000001ull};
    CHECK(TreeView_SetItem(tree,&invalid));
    CHECK(!SendMessageW(window,WM_NOTIFY,ID_PROPERTIES,(LPARAM)&leaf));CHECK(!g_navigation.pending);
    /* A reload cancels queued links before releasing their old index. */
    inspect_queue_navigation("4af38de043f6b6f7f3bf70db1c9d9613");
    SendMessageW(window,WM_COMMAND,ID_RELOAD_INDEX,0);before=g_object;drain_navigation(window);CHECK(g_object==before);
    /* Missing/cyclic references cannot execute data or grow history unbounded. */
    inspect_queue_navigation("00000000000000000000000000000000");drain_navigation(window);CHECK(g_object==-1);
    SendMessageW(window,WM_COMMAND,ID_BACK,0);CHECK(g_object>=0);
    SendMessageW(window,WM_COMMAND,ID_FORWARD,0);CHECK(g_object==-1&&!strcmp(g_inspected_hash,"00000000000000000000000000000000"));
    for(unsigned i=0;i<80;i++){inspect_queue_navigation(i%2?"4af38de043f6b6f7f3bf70db1c9d9613":"9320e80c2706880d0282cc4cad13e2d2");drain_navigation(window);}
    CHECK(g_history_count<=64);
    char newest[33];lstrcpyA(newest,g_inspected_hash);
    for(unsigned i=0;i<80;i++)SendMessageW(window,WM_COMMAND,ID_BACK,0);
    CHECK(!g_history_count&&g_forward_count==64&&!IsWindowEnabled(GetDlgItem(window,ID_BACK)));
    for(unsigned i=0;i<80;i++)SendMessageW(window,WM_COMMAND,ID_FORWARD,0);
    CHECK(!g_forward_count&&g_history_count==64&&!strcmp(g_inspected_hash,newest));
    EXCEPTION_RECORD fault={.ExceptionCode=0xe0424841,.ExceptionAddress=(void *)queue_tree_link};
    EXCEPTION_POINTERS fault_info={.ExceptionRecord=&fault};CHECK(panel_exception_filter(&fault_info)==EXCEPTION_EXECUTE_HANDLER);
    CHECK(strstr(fixture_debug,HV_VERSION)&&strstr(fixture_debug,"e0424841")&&strstr(fixture_debug,g_inspected_hash));
    inspect_queue_navigation("4af38de043f6b6f7f3bf70db1c9d9613");
    g_connection.shared->live_level=5;inspector_tick();
    drain_navigation(window);
    CHECK(g_index_level==5&&g_object==-1&&!g_history_count&&!g_forward_count&&!g_inspected_hash[0]);
    CHECK(!IsWindowEnabled(GetDlgItem(window,ID_BACK))&&!IsWindowEnabled(GetDlgItem(window,ID_FORWARD)));
    CHECK(!g_connection.shared->highlight[0]);
    } else puts("SKIP: optional installed-data navigation corpus (use scripts/test.py --data-root)");
    fake_empty();
    SendMessageW(window, WM_COMMAND, ID_HIDE, 0);
    CHECK(g_connection.shared->config.mode == HV_HIDDEN);
    SendMessageW(window, WM_CLOSE, 0, 0);
    CHECK(!g_connection.shared);
    MSG msg; while (PeekMessageW(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
  }
  connection_close(&c);
  hv_object_name(name, 96, L"FixtureExit", pi.dwProcessId);
  HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, name); CHECK(stop); SetEvent(stop); CloseHandle(stop);
  CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0); CloseHandle(pi.hProcess);
}
int wmain(int argc,wchar_t **argv) {
  CHECK(argc==1||(argc==2&&!wcscmp(argv[1],L"--with-index")));
  fixture_with_index=argc==2;
  hull_graph_tests();
  run_case(FALSE); run_case(TRUE);
  puts("PASS: late-load, reopen, hooks, single-line labels/layout, panel controls, Enter dialog routing, Unicode clipboard mocks, persistent errors and conflict rejection");
  return 0;
}
