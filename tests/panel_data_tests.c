/* Hidden synthetic panel; caller supplies its own read-only Data fixture beside
 * this EXE. No attachment, injected code, clipboard or desktop capture. */
#define wWinMain unused_panel_main
#include "../src/panel.c"
#undef wWinMain
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static HTREEITEM data_link(HWND tree,HTREEITEM item,int target) {
  for(;item;item=TreeView_GetNextSibling(tree,item)) {
    TVITEMW value={.mask=TVIF_PARAM,.hItem=item};CHECK(TreeView_GetItem(tree,&value));
    if(value.lParam==(LPARAM)target+1)return item;
    HTREEITEM child=data_link(tree,TreeView_GetChild(tree,item),target);if(child)return child;
  }return NULL;
}
int wmain(void) {
  WNDCLASSW wc={.lpfnWndProc=window_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"GameDataPanelFixture"};
  CHECK(RegisterClassW(&wc));HWND window=CreateWindowW(wc.lpszClassName,L"Fixture",WS_OVERLAPPEDWINDOW,0,0,1020,740,NULL,NULL,wc.hInstance,NULL);CHECK(window);
  KillTimer(window,1);g_connection.process=GetCurrentProcess();
  wchar_t root[MAX_PATH];CHECK(inspect_data_root(root));
  inspect_load_level(4);CHECK(g_index_from_game&&g_index.count==5925&&!g_index_error[0]);
  inspect_select("8dc1645e2ed9ee33aa75297e01b94d0c",FALSE);CHECK(g_object>=0);
  HWND incoming=GetDlgItem(window,ID_INCOMING);CHECK(SendMessageW(incoming,LB_GETCOUNT,0,0)>0);
  wchar_t text[1024];SendMessageW(incoming,LB_GETTEXT,0,(LPARAM)text);CHECK(wcslen(text)>32&&text[32]==' ');
  char hash[33];for(unsigned i=0;i<32;i++)hash[i]=(char)text[i];hash[32]=0;CHECK(hv_hash_valid(hash));
  int object=hv_index_find(&g_index,hash);CHECK(object>=0&&strstr(g_index.objects[object].detail,"__TriggerInstances.lua.gz"));
  SendMessageW(incoming,LB_SETCURSEL,0,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_INCOMING,LBN_DBLCLK),(LPARAM)incoming);
  inspector_navigate_pending();CHECK(g_object==object);
  inspector_command(ID_RELOAD_INDEX);CHECK(g_index_from_game&&g_object>=0&&!strcmp(g_inspected_hash,hash));
  GetWindowTextW(GetDlgItem(window,ID_PROVENANCE),text,1024);CHECK(wcsstr(text,L"Game files"));
  const char *timeline="d00b9fce57bce1fe2e6f1f53983009bc";
  const char *events[]={"dadd98ccd1789116423245fc0182a265","056f95242a8b9c9961c2397f5b7bdcef"};
  for(unsigned i=0;i<2;i++) {
    inspect_select(timeline,TRUE);CHECK(g_object>=0);
    int target=hv_index_find(&g_index,events[i]);CHECK(target>=0);
    HWND tree=GetDlgItem(window,ID_PROPERTIES);
    HTREEITEM item=data_link(tree,TreeView_GetRoot(tree),target);CHECK(item);
    TVITEMW value={.mask=TVIF_TEXT,.hItem=item,.pszText=text,.cchTextMax=1024};CHECK(TreeView_GetItem(tree,&value));
    CHECK(wcsstr(text,L"TriggerName"));
    TreeView_SelectItem(tree,item);
    NMHDR notify={.hwndFrom=tree,.idFrom=ID_PROPERTIES,.code=NM_DBLCLK};
    CHECK(SendMessageW(window,WM_NOTIFY,ID_PROPERTIES,(LPARAM)&notify));
    inspector_navigate_pending();CHECK(g_object==target&&!strcmp(g_index.objects[g_object].kind,"Trigger"));
    inspector_command(ID_BACK);CHECK(!strcmp(g_inspected_hash,timeline));
    inspector_command(ID_FORWARD);CHECK(!strcmp(g_inspected_hash,events[i]));
  }
  /* A real nested clump uses the existing named selection for all its hulls,
   * including faces. No renderer or live game is required by this fixture. */
  int clump=-1;hv_hull_paths members;
  for(unsigned i=0;i<g_index.count;i++)if(!strcmp(g_index.objects[i].kind,"Clump")) {
    hv_find_hulls(&g_index,(int)i,HV_HULL_MEMBERS,&members);
    if(members.ok&&members.count>1&&!members.limited&&!members.missing){clump=(int)i;break;}
  }
  CHECK(clump>=0);inspect_select(g_index.objects[clump].hash,TRUE);
  CHECK(g_member_hulls.count==members.count&&IsWindowEnabled(GetDlgItem(window,ID_SHOW_HULL)));
  hv_shared shared={0};g_connection.shared=&shared;g_connection.mutex=CreateMutexW(NULL,FALSE,NULL);
  CHECK(g_connection.mutex);inspect_highlight("8dc1645e2ed9ee33aa75297e01b94d0c");
  inspector_command(ID_SHOW_HULL);
  CHECK(!shared.highlight[0]&&!g_highlight_wanted[0]&&!g_highlight_pending);
  CHECK(shared.config.mode==HV_SELECTED&&shared.config.count==members.count&&shared.config.fill_faces==HV_TINT_FACES);
  for(unsigned i=0;i<members.count;i++) {
    int member=members.paths[i].object[members.paths[i].length];
    CHECK(!strcmp(shared.config.names[i],g_index.objects[member].hash));
  }
  g_connection.shared=NULL;CloseHandle(g_connection.mutex);g_connection.mutex=NULL;
  /* Trace the UG lobby-flag leaf back through its clump/branch to a region. */
  const char *leaf="f070e30d2ee3c6d8f5b526b6285eaf29",*region="05abb487918787af2bde690f614b7068";
  int leaf_id=hv_index_find(&g_index,leaf),region_id=hv_index_find(&g_index,region);
  CHECK(leaf_id>=0&&region_id>=0);hv_hull_paths paths;
  hv_find_hulls(&g_index,leaf_id,HV_HULL_SOURCES,&paths);CHECK(paths.ok&&paths.count);
  BOOL through_region=FALSE;
  for(unsigned i=0;i<paths.count;i++)for(unsigned j=0;j<=paths.paths[i].length;j++)
    if(paths.paths[i].object[j]==region_id)through_region=TRUE;
  CHECK(through_region);inspect_select(leaf,TRUE);inspector_command(ID_SHOW_HULL);
  CHECK(inspector_view()==4);
  HWND tree=GetDlgItem(window,ID_HULL_LINKS);
  HTREEITEM item=data_link(tree,TreeView_GetRoot(tree),region_id);CHECK(item);
  TreeView_SelectItem(tree,item);NMHDR notify={.hwndFrom=tree,.idFrom=ID_HULL_LINKS,.code=NM_DBLCLK};
  CHECK(SendMessageW(window,WM_NOTIFY,ID_HULL_LINKS,(LPARAM)&notify));
  inspector_navigate_pending();CHECK(g_object==region_id);
  inspector_command(ID_BACK);CHECK(g_object==leaf_id);
  inspector_command(ID_FORWARD);CHECK(g_object==region_id);
  unsigned full_rows=TreeView_GetCount(GetDlgItem(window,ID_PROPERTIES));
  int full_raw=GetWindowTextLengthW(GetDlgItem(window,ID_RAW));
  SendMessageW(GetDlgItem(window,ID_VALUE_FOCUS),BM_SETCHECK,BST_CHECKED,0);
  inspector_command(ID_VALUE_FOCUS);
  CHECK(g_hide_zero_false&&TreeView_GetCount(GetDlgItem(window,ID_PROPERTIES))<full_rows);
  CHECK(GetWindowTextLengthW(GetDlgItem(window,ID_RAW))==full_raw&&g_object==region_id);
  SendMessageW(GetDlgItem(window,ID_VALUE_FOCUS),BM_SETCHECK,BST_UNCHECKED,0);
  inspector_command(ID_VALUE_FOCUS);CHECK(!g_hide_zero_false&&TreeView_GetCount(GetDlgItem(window,ID_PROPERTIES))==full_rows);
  g_connection.process=NULL;SendMessageW(window,WM_CLOSE,0,0);
  puts("PASS: Data discovery/gzip-first, timeline links, multi-hull clump selection/faces, UG leaf-to-region paths, deferred Hull Links navigation and Back/Forward");return 0;
}
