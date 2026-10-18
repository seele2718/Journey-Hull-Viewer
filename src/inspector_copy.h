/* Native trees/lists cannot select substrings. A temporary borderless read-only
 * edit sits directly on the clicked text row, never in a separate panel. */
static HWND g_text_selection,g_text_source;
static BOOL g_text_link;
static ULONGLONG g_text_click_time;
static POINT g_text_click_point;
static HWND g_pick_press;
static POINT g_pick_origin;
static BOOL g_pick_text_drag;
static DWORD g_pick_text_anchor;
static BOOL inspector_copy_row(HWND source,wchar_t text[2200]) {
  text[0]=0;int id=GetDlgCtrlID(source);
  if(id==ID_PROPERTIES||id==ID_HULL_LINKS) {
    HTREEITEM selected=TreeView_GetSelection(source);
    TVITEMW item={.mask=TVIF_TEXT,.hItem=selected,.pszText=text,.cchTextMax=2200};
    return selected&&TreeView_GetItem(source,&item);
  }
  if(id==ID_INCOMING||id==ID_OUTGOING||id==ID_HITS) {
    LRESULT row=SendMessageW(source,LB_GETCURSEL,0,0);
    if(row<0)return FALSE;
    LRESULT size=SendMessageW(source,LB_GETTEXTLEN,row,0);
    if(size<0||size>=2200)return FALSE;
    return SendMessageW(source,LB_GETTEXT,row,(LPARAM)text)!=LB_ERR;
  }
  return FALSE;
}
static void inspector_copy_value(const wchar_t *row,wchar_t out[2200]) {
  /* A single hash is copied alone, never with a label/arrow/reference path. */
  size_t size=wcslen(row),found=SIZE_MAX;unsigned count=0;
  for(size_t i=0;i<size;) {
    if(!iswxdigit(row[i])){i++;continue;}
    size_t start=i;while(i<size&&iswxdigit(row[i]))i++;
    if(i-start==32&&(start==0||!iswalnum(row[start-1]))&&(i==size||!iswalnum(row[i]))){found=start;count++;}
  }
  if(count==1){wmemcpy(out,row+found,32);out[32]=0;return;}
  const wchar_t *value=wcschr(row,L':');value=value?value+1:row;
  while(iswspace(*value))value++;
  const wchar_t *p=value;BOOL bracket=*p==L'[';if(bracket)p++;
  double xyz[3];BOOL vector=TRUE;
  for(unsigned i=0;i<3;i++) {
    while(iswspace(*p))p++;
    wchar_t *end;xyz[i]=wcstod(p,&end);
    if(end==p||!isfinite(xyz[i])){vector=FALSE;break;}
    p=end;while(iswspace(*p))p++;
    if(i<2){if(*p!=L','){vector=FALSE;break;}p++;}
  }
  if(vector) {
    if(bracket){if(*p!=L']')vector=FALSE;else p++;}
    while(iswspace(*p))p++;
    if(*p)vector=FALSE;
  }
  if(vector){
    const wchar_t *start=value+(bracket?1:0),*end=value+wcslen(value);
    while(end>start&&iswspace(end[-1]))end--;
    if(bracket)end--;
    while(start<end&&iswspace(*start))start++;
    while(end>start&&iswspace(end[-1]))end--;
    size_t length=(size_t)(end-start);wmemcpy(out,start,length);out[length]=0;return;
  }
  /* Keep multi-hash rows complete so individual hashes can be selected. */
  lstrcpynW(out,count>1?row:value,2200);
}
static void inspector_copy_clear(void) {
  if(g_pick_press||g_pick_text_drag)ReleaseCapture();
  g_pick_press=NULL;g_pick_text_drag=FALSE;
  HWND source=g_text_source;g_text_source=NULL;g_text_link=FALSE;g_text_click_time=0;
  if(!g_text_selection)return;
  if(GetFocus()==g_text_selection&&source)SetFocus(source);
  ShowWindow(g_text_selection,SW_HIDE);
}
static BOOL inspector_text_begin(HWND source,POINT point) {
  int id=GetDlgCtrlID(source);RECT row,client;GetClientRect(source,&client);
  BOOL linked=FALSE;
  inspector_copy_clear();
  if(id==ID_PROPERTIES||id==ID_HULL_LINKS) {
    TVHITTESTINFO hit={.pt=point};TreeView_HitTest(source,&hit);
    if(!hit.hItem||!(hit.flags&TVHT_ONITEMLABEL))return FALSE;
    TreeView_SelectItem(source,hit.hItem);
    if(!TreeView_GetItemRect(source,hit.hItem,&row,TRUE))return FALSE;
    TVITEMW item={.mask=TVIF_PARAM,.hItem=hit.hItem};TreeView_GetItem(source,&item);linked=item.lParam>0;
  } else {
    LRESULT hit=SendMessageW(source,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
    if(HIWORD(hit)||SendMessageW(source,LB_GETITEMRECT,LOWORD(hit),(LPARAM)&row)==LB_ERR)return FALSE;
    if(!PtInRect(&row,point))return FALSE;
    SendMessageW(source,LB_SETCURSEL,LOWORD(hit),0);
    SendMessageW(g_window,WM_COMMAND,MAKEWPARAM(id,LBN_SELCHANGE),(LPARAM)source);
    linked=id==ID_INCOMING||id==ID_OUTGOING;
    row.left=client.left+2;
  }
  wchar_t text[2200];if(!inspector_copy_row(source,text))return FALSE;
  if(row.left<client.left)row.left=client.left;
  row.right=client.right;
  if(row.top<client.top||row.bottom>client.bottom||row.right-row.left<8)return FALSE;
  SetParent(g_text_selection,source);
  ui_font(g_text_selection,(HFONT)SendMessageW(source,WM_GETFONT,0,0));
  SetWindowTextW(g_text_selection,text);
  SetWindowPos(g_text_selection,HWND_TOP,row.left,row.top,row.right-row.left,row.bottom-row.top,SWP_NOACTIVATE);
  SendMessageW(g_text_selection,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
  SendMessageW(g_text_selection,EM_SETSEL,0,0);
  g_text_source=source;g_text_link=linked;
  ShowWindow(g_text_selection,SW_SHOWNA);SetFocus(g_text_selection);
  return TRUE;
}
static LRESULT CALLBACK inspector_copy_proc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
  (void)id;
  if(data==0) {
    if(GetDlgCtrlID(window)==ID_HITS) {
      if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK) {
        POINT point={(short)LOWORD(l),(short)HIWORD(l)};RECT row;
        LRESULT hit=SendMessageW(window,LB_ITEMFROMPOINT,0,l);
        if(!HIWORD(hit)&&SendMessageW(window,LB_GETITEMRECT,LOWORD(hit),(LPARAM)&row)!=LB_ERR&&PtInRect(&row,point)) {
          inspector_copy_clear();SetFocus(window);
          SendMessageW(window,LB_SETCURSEL,LOWORD(hit),0);
          SendMessageW(g_window,WM_COMMAND,MAKEWPARAM(ID_HITS,LBN_SELCHANGE),(LPARAM)window);
          g_pick_press=window;g_pick_origin=point;SetCapture(window);
          InvalidateRect(window,NULL,FALSE);return 0;
        }
      }
      if(message==WM_MOUSEMOVE&&g_pick_press==window&&(w&MK_LBUTTON)) {
        POINT point={(short)LOWORD(l),(short)HIWORD(l)},origin=g_pick_origin;
        if(abs(point.x-origin.x)>GetSystemMetrics(SM_CXDRAG)||abs(point.y-origin.y)>GetSystemMetrics(SM_CYDRAG)) {
          g_pick_press=NULL;ReleaseCapture();
          if(inspector_text_begin(window,origin)) {
            MapWindowPoints(window,g_text_selection,&origin,1);MapWindowPoints(window,g_text_selection,&point,1);
            g_pick_text_anchor=LOWORD(SendMessageW(g_text_selection,EM_CHARFROMPOS,0,MAKELPARAM(origin.x,origin.y)));
            g_pick_text_drag=TRUE;SetCapture(g_text_selection);
            SendMessageW(g_text_selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x,point.y));
          }
        }
        return 0;
      }
      if(message==WM_LBUTTONUP&&g_pick_press==window){g_pick_press=NULL;ReleaseCapture();return 0;}
      if(message==WM_CAPTURECHANGED||message==WM_KILLFOCUS)g_pick_press=NULL;
    }
    if(message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC)
      if((HWND)l==g_text_selection)return SendMessageW(g_window,message,w,l);
    if(message==WM_SIZE||message==WM_VSCROLL||message==WM_HSCROLL||message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL)
      if(g_text_source==window)inspector_copy_clear();
    if(message==WM_LBUTTONDOWN&&GetDlgCtrlID(window)!=ID_HITS) {
      POINT point={(short)LOWORD(l),(short)HIWORD(l)};
      if(inspector_text_begin(window,point)) {
        MapWindowPoints(window,g_text_selection,&point,1);
        return SendMessageW(g_text_selection,message,w,MAKELPARAM(point.x,point.y));
      }
    }
  } else if(data==2) {
    if(g_pick_text_drag&&(message==WM_MOUSEMOVE||message==WM_LBUTTONUP)) {
      DWORD end=LOWORD(SendMessageW(window,EM_CHARFROMPOS,0,l));
      SendMessageW(window,EM_SETSEL,g_pick_text_anchor,end);
      if(message==WM_LBUTTONUP){g_pick_text_drag=FALSE;ReleaseCapture();}
      return 0;
    }
    if(message==WM_CAPTURECHANGED)g_pick_text_drag=FALSE;
    if(message==WM_KILLFOCUS){
      if(g_pick_text_drag){g_pick_text_drag=FALSE;ReleaseCapture();}
      g_text_source=NULL;g_text_link=FALSE;ShowWindow(window,SW_HIDE);return DefSubclassProc(window,message,w,l);
    }
    /* The first click targeted the tree/list; the next targets this edit.
     * Preserve double-click semantics across that native-window boundary. */
    if(message==WM_LBUTTONDOWN) {
      POINT point={(short)LOWORD(l),(short)HIWORD(l)};ULONGLONG now=GetTickCount64();
      if(g_text_click_time&&now-g_text_click_time<=GetDoubleClickTime()&&
         abs(point.x-g_text_click_point.x)<=GetSystemMetrics(SM_CXDOUBLECLK)&&
         abs(point.y-g_text_click_point.y)<=GetSystemMetrics(SM_CYDOUBLECLK)) {
        message=WM_LBUTTONDBLCLK;g_text_click_time=0;
      } else {g_text_click_time=now;g_text_click_point=point;}
    }
    if(message==WM_MOUSEMOVE&&(w&MK_LBUTTON)&&
       (abs((short)LOWORD(l)-g_text_click_point.x)>GetSystemMetrics(SM_CXDRAG)||
        abs((short)HIWORD(l)-g_text_click_point.y)>GetSystemMetrics(SM_CYDRAG)))g_text_click_time=0;
    if(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL) {
      HWND source=g_text_source;inspector_copy_clear();
      return source?SendMessageW(source,message,w,l):0;
    }
    if(message==WM_LBUTTONDBLCLK&&g_text_link&&g_text_source) {
      HWND source=g_text_source;int source_id=GetDlgCtrlID(source);inspector_copy_clear();
      if(source_id==ID_PROPERTIES||source_id==ID_HULL_LINKS) {
        NMHDR notification={.hwndFrom=source,.idFrom=(UINT_PTR)source_id,.code=NM_DBLCLK};
        return SendMessageW(g_window,WM_NOTIFY,source_id,(LPARAM)&notification);
      }
      return SendMessageW(g_window,WM_COMMAND,MAKEWPARAM(source_id,LBN_DBLCLK),(LPARAM)source);
    }
  }
  BOOL modifier=(GetKeyState(VK_CONTROL)|GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000;
  if(message==WM_COPY||(message==WM_KEYDOWN&&modifier&&w=='C')) {
    wchar_t text[2200];
    if(data) {
      /* Native read-only edit remains selectable; copy exactly that range. */
      DWORD first=0,last=0;SendMessageW(window,EM_GETSEL,(WPARAM)&first,(LPARAM)&last);
      int length=GetWindowTextLengthW(window);
      if(first==last||last>(DWORD)length)return 0;
      wchar_t *all=malloc(((size_t)length+1)*sizeof(wchar_t));if(!all)return 0;
      GetWindowTextW(window,all,length+1);all[last]=0;
      if(!clipboard_write(g_window,all+first))notice(L"Could not copy to the clipboard. Please try again.");
      free(all);
    } else if(inspector_copy_row(window,text)) {
      wchar_t plain[2200];inspector_copy_value(text,plain);
      if(!clipboard_write(g_window,plain))notice(L"Could not copy to the clipboard. Please try again.");
    }
    return 0;
  }
  if(message==WM_KEYDOWN&&modifier&&w=='A'&&data){SendMessageW(window,EM_SETSEL,0,-1);return 0;}
  if(message==WM_CHAR&&modifier&&(w==3||w==1||w=='c'||w=='a'))return 0;
  return DefSubclassProc(window,message,w,l);
}
