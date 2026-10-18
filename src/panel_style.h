/* Lightweight native panel styling. No renderer/UI work is added to Journey. */
static HFONT g_heading_font,g_section_font,g_code_font,g_small_font,g_summary_font,g_button_font;
static HBRUSH g_background,g_surface;
static unsigned g_dpi=96;
static int g_scroll_x,g_scroll_y,g_content_width=1000,g_content_height=700;
static BOOL g_layout_busy;
static int g_footer_reserve=84;
static int g_sidebar_width=270,g_divider_x;
static BOOL g_resizing_panes,g_names_view;
enum { ID_APP_TITLE=500,ID_APP_SUBTITLE,ID_VIEW_TITLE,ID_CAM_LABEL,ID_NAMES_NOTE,
       ID_CONNECTION_NOTE };
static int ui_px(int value){return MulDiv(value,(int)g_dpi,96);}
static void ui_move(int id,int x,int y,int width,int height) {
  HWND child=GetDlgItem(g_window,id);
  /* Do not copy old control/parent pixels while siblings change places.
   * One full child repaint follows the completed layout. */
  if(child)SetWindowPos(child,NULL,ui_px(x-g_scroll_x),ui_px(y-g_scroll_y),ui_px(width),ui_px(height),
    SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
}
static void ui_font(HWND child,HFONT font){SendMessageW(child,WM_SETFONT,(WPARAM)font,TRUE);}
static void ui_enable(HWND child,BOOL enabled) {
  if(!!IsWindowEnabled(child)!=!!enabled)EnableWindow(child,enabled);
}
static void ui_check(HWND child,LRESULT state) {
  if(SendMessageW(child,BM_GETCHECK,0,0)!=state)SendMessageW(child,BM_SETCHECK,state,0);
}
static void ui_text_changed(HWND child,const wchar_t *text) {
  wchar_t previous[512];int length=GetWindowTextLengthW(child);
  if(length<512){GetWindowTextW(child,previous,512);if(!wcscmp(previous,text))return;}
  SetWindowTextW(child,text);
}
static void ui_center_edit(HWND child) {
  RECT rect;GetClientRect(child,&rect);HDC dc=GetDC(child);
  HGDIOBJ old=SelectObject(dc,(HFONT)SendMessageW(child,WM_GETFONT,0,0));TEXTMETRICW metrics;
  GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(child,dc);
  rect.left+=ui_px(4);rect.right-=ui_px(4);rect.top=(rect.bottom-metrics.tmHeight)/2;
  SendMessageW(child,EM_SETRECTNP,0,(LPARAM)&rect);
}
static void ui_fonts(void) {
  g_font=CreateFontW(-ui_px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_heading_font=CreateFontW(-ui_px(16),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_section_font=CreateFontW(-ui_px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_small_font=CreateFontW(-ui_px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_code_font=CreateFontW(-ui_px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
  g_summary_font=CreateFontW(-ui_px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_button_font=CreateFontW(-ui_px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  g_background=CreateSolidBrush(RGB(240,243,248));g_surface=CreateSolidBrush(RGB(255,255,255));
}
static void ui_release(void) {
  DeleteObject(g_font);DeleteObject(g_heading_font);DeleteObject(g_section_font);
  DeleteObject(g_small_font);DeleteObject(g_code_font);DeleteObject(g_summary_font);DeleteObject(g_button_font);DeleteObject(g_background);DeleteObject(g_surface);
}
/* Keep the native dropdown/keyboard interaction; only paint its closed face.
 * WM_PRINT uses this same painter so own-control previews are faithful. */
static void ui_combo_face(HWND window,HDC dc) {
  RECT rect;GetWindowRect(window,&rect);OffsetRect(&rect,-rect.left,-rect.top);
  int saved=SaveDC(dc);FillRect(dc,&rect,g_surface);
  HPEN pen=CreatePen(PS_SOLID,1,RGB(220,226,235));SelectObject(dc,pen);
  SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,0,0,rect.right,rect.bottom);
  wchar_t text[64]={0};LRESULT selected=SendMessageW(window,CB_GETCURSEL,0,0);
  if(selected>=0&&SendMessageW(window,CB_GETLBTEXTLEN,selected,0)<64)
    SendMessageW(window,CB_GETLBTEXT,selected,(LPARAM)text);
  SelectObject(dc,(HFONT)SendMessageW(window,WM_GETFONT,0,0));
  SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(35,49,70));
  RECT label=rect;label.left+=ui_px(6);label.right-=ui_px(24);
  DrawTextW(dc,text,-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
  HPEN arrow=CreatePen(PS_SOLID,1,RGB(90,110,139));SelectObject(dc,arrow);
  int x=rect.right-ui_px(13),y=rect.bottom/2;
  MoveToEx(dc,x-ui_px(3),y-ui_px(1),NULL);LineTo(dc,x,y+ui_px(2));LineTo(dc,x+ui_px(3),y-ui_px(1));
  if(GetFocus()==window){InflateRect(&label,-ui_px(2),-ui_px(3));DrawFocusRect(dc,&label);}
  RestoreDC(dc,saved);DeleteObject(pen);DeleteObject(arrow);
}
static LRESULT CALLBACK ui_combo_proc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
  (void)id;(void)data;
  if(message==WM_PAINT) {
    PAINTSTRUCT paint;BeginPaint(window,&paint);HDC dc=GetWindowDC(window);
    ui_combo_face(window,dc);ReleaseDC(window,dc);EndPaint(window,&paint);return 0;
  }
  if(message==WM_PRINT||message==WM_PRINTCLIENT){ui_combo_face(window,(HDC)w);return 0;}
  if(message==WM_NCPAINT||message==WM_ERASEBKGND)return 1;
  LRESULT result=DefSubclassProc(window,message,w,l);
  if(message==CB_SETCURSEL||message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_ENABLE)
    InvalidateRect(window,NULL,FALSE);
  return result;
}
/* Measure the actual selected fonts, including platform font substitutions. */
static SIZE ui_text_size(int id,int wrap_width) {
  HWND child=GetDlgItem(g_window,id);wchar_t text[512];GetWindowTextW(child,text,512);
  HDC dc=GetDC(child);HGDIOBJ old=SelectObject(dc,(HFONT)SendMessageW(child,WM_GETFONT,0,0));
  RECT box={0,0,ui_px(wrap_width),0};
  DrawTextW(dc,text,-1,&box,DT_CALCRECT|DT_NOPREFIX|(wrap_width?DT_WORDBREAK:DT_SINGLELINE));
  SelectObject(dc,old);ReleaseDC(child,dc);
  SIZE size={(box.right*96+(int)g_dpi-1)/(int)g_dpi,(box.bottom*96+(int)g_dpi-1)/(int)g_dpi};return size;
}
static void ui_raw_scrollbar(HWND raw) {
  static BOOL busy;if(busy||!raw)return;busy=TRUE;
  HDC dc=GetDC(raw);HGDIOBJ old=SelectObject(dc,(HFONT)SendMessageW(raw,WM_GETFONT,0,0));
  TEXTMETRICW metrics;GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(raw,dc);
  ShowScrollBar(raw,SB_VERT,FALSE);
  for(int pass=0;pass<2;pass++) {
    RECT area;GetClientRect(raw,&area);
    ShowScrollBar(raw,SB_VERT,SendMessageW(raw,EM_GETLINECOUNT,0,0)*metrics.tmHeight+ui_px(4)>area.bottom);
  }
  busy=FALSE;
}
static void ui_name_scrollbars(HWND names) {
  static BOOL busy;if(busy||!names)return;busy=TRUE;
  wchar_t text[4001];int length=GetWindowTextW(names,text,4001);
  HDC dc=GetDC(names);HGDIOBJ old=SelectObject(dc,(HFONT)SendMessageW(names,WM_GETFONT,0,0));
  TEXTMETRICW metrics;GetTextMetricsW(dc,&metrics);int widest=0,lines=1,start=0;
  for(int i=0;i<=length;i++)if(i==length||text[i]==L'\n') {
    int end=i;if(end>start&&text[end-1]==L'\r')end--;SIZE extent={0};
    GetTextExtentPoint32W(dc,text+start,end-start,&extent);if(extent.cx>widest)widest=extent.cx;
    if(i<length)lines++;
    start=i+1;
  }
  SelectObject(dc,old);ReleaseDC(names,dc);
  RECT rect;GetClientRect(names,&rect);LONG_PTR style=GetWindowLongPtrW(names,GWL_STYLE);
  int full_w=rect.right+((style&WS_VSCROLL)?GetSystemMetrics(SM_CXVSCROLL):0);
  int full_h=rect.bottom+((style&WS_HSCROLL)?GetSystemMetrics(SM_CYHSCROLL):0);
  BOOL horizontal=FALSE,vertical=FALSE;
  for(int i=0;i<3;i++) {
    vertical=lines*metrics.tmHeight+ui_px(4)>full_h-(horizontal?GetSystemMetrics(SM_CYHSCROLL):0);
    horizontal=widest+ui_px(8)>full_w-(vertical?GetSystemMetrics(SM_CXVSCROLL):0);
  }
  ShowScrollBar(names,SB_HORZ,horizontal);ShowScrollBar(names,SB_VERT,vertical);busy=FALSE;
}
static void ui_draw(HWND window,HDC dc) {
  RECT r;GetClientRect(window,&r);
  FillRect(dc,&r,g_background);
  HPEN pen=CreatePen(PS_SOLID,1,RGB(220,226,235));HGDIOBJ oldpen=SelectObject(dc,pen);
  HGDIOBJ oldbrush=SelectObject(dc,g_surface);
  RoundRect(dc,ui_px(16-g_scroll_x),ui_px(66-g_scroll_y),ui_px(32+g_sidebar_width-g_scroll_x),ui_px(g_content_height-g_footer_reserve-g_scroll_y),ui_px(6),ui_px(6));
  RoundRect(dc,ui_px(48+g_sidebar_width-g_scroll_x),ui_px(66-g_scroll_y),ui_px(g_content_width-16-g_scroll_x),ui_px(g_content_height-g_footer_reserve-g_scroll_y),ui_px(6),ui_px(6));
  MoveToEx(dc,ui_px(g_divider_x-g_scroll_x),ui_px(74-g_scroll_y),NULL);
  LineTo(dc,ui_px(g_divider_x-g_scroll_x),ui_px(g_content_height-g_footer_reserve-8-g_scroll_y));
  inspector_summary_border(window,dc);
  SelectObject(dc,oldpen);SelectObject(dc,oldbrush);DeleteObject(pen);
}
static void ui_paint(HWND window) {
  PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);ui_draw(window,dc);EndPaint(window,&paint);
}
static BOOL ui_draw_button(const DRAWITEMSTRUCT *item) {
  if(item->CtlType!=ODT_BUTTON)return FALSE;
  /* Owner-drawn rounded corners must cover the native button's erase color. */
  BOOL header=item->CtlID==ID_ATTACH||item->CtlID==ID_CAM_START||
    item->CtlID==ID_CAM_STOP||item->CtlID==ID_CAM_MOVE;
  FillRect(item->hDC,&item->rcItem,header?g_background:g_surface);
  BOOL primary=item->CtlID==ID_ATTACH||item->CtlID==ID_SELECTED||
    (item->CtlID==ID_TYPES&&!g_names_view)||(item->CtlID==ID_NAMES_TAB&&g_names_view);
  BOOL disabled=(item->itemState&ODS_DISABLED)!=0,pressed=(item->itemState&ODS_SELECTED)!=0;
  COLORREF fill=primary?RGB(43,96,191):RGB(248,250,253);
  if(pressed)fill=primary?RGB(30,71,148):RGB(229,235,245);
  if(disabled)fill=RGB(236,239,244);
  HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,primary&&!disabled?fill:RGB(209,218,231));
  HGDIOBJ oldpen=SelectObject(item->hDC,pen),oldbrush=SelectObject(item->hDC,brush);
  RECT rect=item->rcItem;RoundRect(item->hDC,rect.left,rect.top,rect.right,rect.bottom,ui_px(8),ui_px(8));
  SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?RGB(143,153,169):primary?RGB(255,255,255):RGB(44,58,80));
  HGDIOBJ oldfont=SelectObject(item->hDC,item->CtlID==ID_ATTACH?g_section_font:g_button_font);wchar_t text[128];GetWindowTextW(item->hwndItem,text,128);
  DrawTextW(item->hDC,text,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  if(item->itemState&ODS_FOCUS){InflateRect(&rect,-ui_px(4),-ui_px(4));DrawFocusRect(item->hDC,&rect);}
  SelectObject(item->hDC,oldfont);SelectObject(item->hDC,oldpen);SelectObject(item->hDC,oldbrush);
  DeleteObject(pen);DeleteObject(brush);return TRUE;
}
