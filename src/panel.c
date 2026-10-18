#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <stdio.h>
#include <wctype.h>
#include <math.h>
#include "protocol.h"
#include "version.h"
#include "resource.h"
#include "clipboard.h"
#include "inspect_shortcut.h"

enum { ID_ATTACH = 100, ID_HIDE, ID_ALL, ID_EXCEPT, ID_SELECTED, ID_TYPES,
       ID_NAMES, ID_STATUS, ID_PASTE, ID_COPY, ID_INACTIVE,
       ID_CAM_START, ID_CAM_STOP, ID_CAM_IGNORE, ID_CAM_LOCK,
       ID_CAM_HINT, ID_HULL_HINT, ID_DRAW_LABEL, ID_TYPES_LABEL,ID_FILL,ID_CAM_POS,ID_NAMES_TAB,
       ID_OPACITY,ID_OPACITY_LABEL,ID_CAM_TARGET,ID_CAM_MOVE,ID_CAM_TARGET_LABEL,
       ID_CHECK = 200, WM_ATTACHED = WM_APP + 1 };
static HWND g_window, g_status, g_names, g_checks[6];
static HFONT g_font;
static BOOL g_busy;
static WNDPROC g_edit_proc;
static wchar_t g_notice[192];
static void panel_status_text(const wchar_t *text);
static void notice(const wchar_t *text) {
  lstrcpynW(g_notice, text, 192);
  if (g_status) panel_status_text(text);
}
static void paste_names(void) {
  wchar_t text[4096];
  if (!clipboard_read(g_window, text, 4096)) {
    notice(L"Could not read the clipboard. Try copying the text again.");
    return;
  }
  DWORD first, last;
  SendMessageW(g_names, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
  if ((size_t)GetWindowTextLengthW(g_names) - (last - first) + wcslen(text) > 4000) {
    notice(L"Text exceeds the 4000-character limit."); return;
  }
  SendMessageW(g_names, EM_REPLACESEL, TRUE, (LPARAM)text);
  SetFocus(g_names);
  notice(L"Pasted. Click Show These Hulls to apply.");
}
static void copy_names(BOOL cut) {
  wchar_t text[4096]; DWORD first, last;
  GetWindowTextW(g_names, text, 4096);
  SendMessageW(g_names, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
  if (first == last || last > wcslen(text)) return;
  text[last] = 0;
  if (!clipboard_write(g_window, text + first)) { notice(L"Could not copy to the clipboard. Please try again."); return; }
  if (cut) SendMessageW(g_names, EM_REPLACESEL, TRUE, (LPARAM)L"");
}
static LRESULT CALLBACK names_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_PASTE) { paste_names(); return 0; }
  if (msg == WM_COPY || msg == WM_CUT) { copy_names(msg == WM_CUT); return 0; }
  BOOL modifier = (GetKeyState(VK_CONTROL) | GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000;
  if (msg == WM_KEYDOWN && modifier) {
    if (w == 'A') { SendMessageW(window, EM_SETSEL, 0, -1); return 0; }
    if (w == 'V') { paste_names(); return 0; }
    if (w == 'C' || w == 'X') { copy_names(w == 'X'); return 0; }
  }
  if (msg == WM_CHAR && (w == 1 || w == 3 || w == 22 || w == 24 ||
      (modifier && (w == 'a' || w == 'c' || w == 'v' || w == 'x')))) return 0;
  return CallWindowProcW(g_edit_proc, window, msg, w, l);
}
typedef struct connection {
  DWORD pid;
  HANDLE process, mapping, mutex, owner;
  hv_shared *shared;
  wchar_t message[320];
} connection;
static connection g_connection;
static uint32_t g_camera_pending;
static BOOL g_camera_state_known;
static uint32_t g_camera_state_bits;
static uint64_t g_camera_read_time;
static uint32_t g_camera_level=UINT32_MAX;
static BOOL g_camera_poll;
static uint32_t g_camera_sent_action;
static float g_camera_sent_target[3];
static unsigned g_opacity=25;
static void inspector_summary_border(HWND window,HDC dc);
#include "panel_style.h"
static uint32_t panel_tint_mode(void) {
  if(SendMessageW(GetDlgItem(g_window,ID_FILL),BM_GETCHECK,0,0)!=BST_CHECKED)return HV_TINT_OFF;
  return HV_TINT_FACES;
}

static void camera_controls_enabled(BOOL enabled) {
  ui_enable(GetDlgItem(g_window,ID_CAM_START),enabled&&(!g_camera_state_known||!(g_camera_state_bits&1)));
  ui_enable(GetDlgItem(g_window,ID_CAM_STOP),enabled&&(!g_camera_state_known||(g_camera_state_bits&1)));
  ui_enable(GetDlgItem(g_window,ID_CAM_IGNORE),enabled);
  ui_enable(GetDlgItem(g_window,ID_CAM_LOCK),enabled);
  ui_enable(GetDlgItem(g_window,ID_CAM_MOVE),enabled&&g_camera_state_known&&(g_camera_state_bits&1));
}
static void camera_unknown(void) {
  g_camera_state_known=FALSE;
  ui_check(GetDlgItem(g_window, ID_CAM_IGNORE), BST_INDETERMINATE);
  ui_check(GetDlgItem(g_window, ID_CAM_LOCK), BST_INDETERMINATE);
}
static void send_camera(uint32_t action) {
  if (!g_connection.shared || g_busy || action >= HV_CAM_COUNT) return;
  float target[3]={0};
  if(action==HV_CAM_MOVE) {
    if(!g_camera_state_known||!(g_camera_state_bits&1)){notice(L"Start Debug Cam before moving it.");return;}
    wchar_t text[160];GetWindowTextW(GetDlgItem(g_window,ID_CAM_TARGET),text,160);wchar_t *p=text;
    for(unsigned i=0;i<3;i++) {
      while(iswspace(*p)||*p==L','||*p==L';')p++;
      wchar_t *end;double value=wcstod(p,&end);
      if(end==p||!isfinite(value)){notice(L"Enter valid X, Y, Z coordinates.");return;}
      if(fabs(value)>1000000){notice(L"Coordinates must be between −1,000,000 and 1,000,000.");return;}
      target[i]=(float)value;p=end;
      if(i<2&&!iswspace(*p)&&*p!=L','&&*p!=L';'){notice(L"Enter valid X, Y, Z coordinates.");return;}
    }
    while(iswspace(*p))p++;
    if(*p){notice(L"Enter valid X, Y, Z coordinates.");return;}
  }
  DWORD lock = WaitForSingleObject(g_connection.mutex, 0);
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) { if(action!=HV_CAM_READ)notice(L"Game busy. Please try again."); return; }
  hv_shared *s = g_connection.shared;
  BOOL sent = s->camera_available && s->camera_request == s->camera_completed;
  if (sent) {
    s->camera_action = action;
    memcpy(s->camera_target,target,sizeof(target));
    g_camera_sent_action=action;memcpy(g_camera_sent_target,target,sizeof(target));
    g_camera_poll=action==HV_CAM_READ;
    if(g_camera_poll)g_camera_read_time=GetTickCount64();
    g_camera_pending = ++s->camera_request;
    s->heartbeat = GetTickCount64();
  }
  ReleaseMutex(g_connection.mutex);
  if (sent&&!g_camera_poll) { camera_controls_enabled(FALSE); notice(action==HV_CAM_MOVE?L"Moving camera…":L"Camera: waiting for game update."); }
  else if(!sent&&action!=HV_CAM_READ)notice(action==HV_CAM_MOVE?L"Could not move the camera. Camera controls unavailable or busy.":L"Camera controls unavailable or busy.");
}

static void connection_close(connection *c) {
  if (c->shared) UnmapViewOfFile(c->shared);
  if (c->mapping) CloseHandle(c->mapping);
  if (c->mutex) CloseHandle(c->mutex);
  if (c->process) CloseHandle(c->process);
  if (c->owner) CloseHandle(c->owner);
  c->shared = NULL;
  c->mapping = c->mutex = c->process = c->owner = NULL;
  c->pid = 0;
}
static void error_message(connection *c, const wchar_t *action) {
  DWORD error = GetLastError();
  _snwprintf(c->message, 320, L"%ls (Windows error %lu).", action,
             (unsigned long)error);
  c->message[319] = 0;
}
static uintptr_t remote_module(DWORD pid, const wchar_t *name,
                                wchar_t *full_path) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W m = {.dwSize = sizeof(m)};
  uintptr_t result = 0;
  if (Module32FirstW(snap, &m)) do {
    if (!_wcsicmp(m.szModule, name)) {
      result = (uintptr_t)m.modBaseAddr;
      if (full_path) lstrcpynW(full_path, m.szExePath, MAX_PATH);
      break;
    }
  } while (Module32NextW(snap, &m));
  CloseHandle(snap);
  return result;
}
static DWORD find_journey(void) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W p = {.dwSize = sizeof(p)};
  DWORD pid = 0;
  if (Process32FirstW(snap, &p)) do {
    if (!_wcsicmp(p.szExeFile, L"Journey.exe")) {
      if (pid) { pid = MAXDWORD; break; }
      pid = p.th32ProcessID;
    }
  } while (Process32NextW(snap, &p));
  CloseHandle(snap);
  return pid;
}
static BOOL open_channel(connection *c) {
  wchar_t name[96];
  hv_object_name(name, 96, L"Data", c->pid);
  c->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
  hv_object_name(name, 96, L"Lock", c->pid);
  c->mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
  if (c->mapping)
    c->shared = MapViewOfFile(c->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hv_shared));
  if (!c->shared || !c->mutex) return FALSE;
  DWORD lock = WaitForSingleObject(c->mutex, 1000);
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return FALSE;
  BOOL ok = c->shared->magic == HV_MAGIC && c->shared->protocol == HV_PROTOCOL &&
      c->shared->size == sizeof(hv_shared) &&
      !strncmp(c->shared->version, HV_VERSION, sizeof(c->shared->version));
  if (ok) {
    MultiByteToWideChar(CP_UTF8, 0, c->shared->status, -1, c->message, 320);
    ok = c->shared->ready;
    if (ok) {
      c->shared->config.mode = HV_HIDDEN;
      c->shared->config.fill_faces=panel_tint_mode();
      c->shared->config.opacity=g_opacity;
      ++c->shared->revision;
      c->shared->heartbeat = GetTickCount64();
    }
  } else lstrcpyW(c->message, L"Viewer version mismatch. Restart Journey, then use the matching EXE and DLL.");
  ReleaseMutex(c->mutex);
  return ok;
}

/* Runs outside the UI thread. It never modifies a file or another module's
 * code. The DLL itself verifies game code before installing its own hooks. */
static BOOL attach(connection *c, DWORD requested_pid) {
  c->pid = requested_pid ? requested_pid : find_journey();
  if (!c->pid || c->pid == MAXDWORD) {
    lstrcpyW(c->message, c->pid ? L"More than one Journey process. Close the extra instance first."
                              : L"Journey is not running. Start the game, then click Attach.");
    return FALSE;
  }
  wchar_t object[96];
  hv_object_name(object, 96, L"Panel", c->pid);
  c->owner = CreateMutexW(NULL, FALSE, object);
  if (!c->owner || GetLastError() == ERROR_ALREADY_EXISTS) {
    lstrcpyW(c->message, L"Another viewer panel is already attached to this game.");
    return FALSE;
  }
  c->process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
      PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE,
      FALSE, c->pid);
  if (!c->process) { error_message(c, L"Cannot open Journey"); return FALSE; }
  BOOL wow = TRUE;
  if (!IsWow64Process(c->process, &wow) || wow) {
    lstrcpyW(c->message, L"A 64-bit Journey process is required."); return FALSE;
  }
  wchar_t path[MAX_PATH] = {0}, loaded_path[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (!length || length >= MAX_PATH) {
    lstrcpyW(c->message, L"Cannot resolve the viewer folder path."); return FALSE;
  }
  wchar_t *slash = wcsrchr(path, L'\\');
  if (!slash || (slash - path) + 32 >= MAX_PATH) {
    lstrcpyW(c->message, L"Viewer folder path is too long."); return FALSE;
  }
  lstrcpyW(slash + 1, L"journey_hull_viewer.dll");
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    lstrcpyW(c->message, L"Keep journey_hull_viewer.dll beside this EXE."); return FALSE;
  }
  uintptr_t base = remote_module(c->pid, L"journey_hull_viewer.dll", loaded_path);
  if (base) {
    if (_wcsicmp(path, loaded_path)) {
      lstrcpyW(c->message, L"A viewer DLL from another folder is loaded. Restart Journey to switch builds.");
      return FALSE;
    }
    return open_channel(c);
  }
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll"), owner = NULL;
  FARPROC load = GetProcAddress(kernel, "LoadLibraryW");
  if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)load, &owner)) {
    error_message(c, L"Cannot resolve the Windows loader"); return FALSE;
  }
  wchar_t owner_path[MAX_PATH] = {0};
  length = GetModuleFileNameW(owner, owner_path, MAX_PATH);
  if (!length || length >= MAX_PATH) {
    lstrcpyW(c->message, L"Cannot resolve the Windows loader path."); return FALSE;
  }
  const wchar_t *owner_name = wcsrchr(owner_path, L'\\');
  uintptr_t remote_owner = remote_module(c->pid, owner_name ? owner_name + 1 : owner_path, NULL);
  if (!remote_owner) { lstrcpyW(c->message, L"Cannot locate the Windows loader in Journey."); return FALSE; }
  SIZE_T bytes = (wcslen(path) + 1) * sizeof(wchar_t), written = 0;
  void *argument = VirtualAllocEx(c->process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!argument) { error_message(c, L"Cannot allocate the DLL path"); return FALSE; }
  if (!WriteProcessMemory(c->process, argument, path, bytes, &written) || written != bytes) {
    error_message(c, L"Cannot copy the DLL path");
    VirtualFreeEx(c->process, argument, 0, MEM_RELEASE); return FALSE;
  }
  HANDLE thread = CreateRemoteThread(c->process, NULL, 0,
      (LPTHREAD_START_ROUTINE)(remote_owner + (uintptr_t)load - (uintptr_t)owner), argument, 0, NULL);
  if (!thread) {
    error_message(c, L"Cannot load the viewer DLL");
    VirtualFreeEx(c->process, argument, 0, MEM_RELEASE); return FALSE;
  }
  DWORD wait = WaitForSingleObject(thread, 10000);
  CloseHandle(thread);
  if (wait != WAIT_OBJECT_0) {
    /* The loader may still read its argument. Keep that tiny allocation alive
     * until game exit rather than freeing it underneath the remote thread. */
    lstrcpyW(c->message, L"DLL load timed out. Restart Journey before retrying."); return FALSE;
  }
  VirtualFreeEx(c->process, argument, 0, MEM_RELEASE);
  base = remote_module(c->pid, L"journey_hull_viewer.dll", NULL);
  if (!base) { lstrcpyW(c->message, L"Journey could not load the viewer DLL."); return FALSE; }
  /* Map our DLL without running its entry point or resolving imports, solely
   * to obtain the RVA of its own non-forwarded initialization export. */
  HMODULE local = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
  FARPROC start = local ? GetProcAddress(local, "HullViewerStart") : NULL;
  if (!start) {
    if (local) FreeLibrary(local);
    lstrcpyW(c->message, L"Invalid viewer DLL: initialization export missing."); return FALSE;
  }
  uintptr_t offset = (uintptr_t)start - (uintptr_t)local;
  FreeLibrary(local);
  thread = CreateRemoteThread(c->process, NULL, 0, (LPTHREAD_START_ROUTINE)(base + offset), NULL, 0, NULL);
  if (!thread) { error_message(c, L"Cannot initialize the viewer"); return FALSE; }
  wait = WaitForSingleObject(thread, 10000);
  CloseHandle(thread);
  if (wait != WAIT_OBJECT_0) {
    lstrcpyW(c->message, L"Initialization timed out. Restart Journey before retrying."); return FALSE;
  }
  if (!open_channel(c)) {
    if (!c->message[0]) lstrcpyW(c->message, L"Viewer initialization failed. Restart Journey to retry.");
    return FALSE;
  }
  return TRUE;
}
static DWORD WINAPI attach_worker(void *unused) {
  (void)unused;
  BOOL ok = attach(&g_connection, 0);
  if (!ok) connection_close(&g_connection);
  PostMessageW(g_window, WM_ATTACHED, ok, 0);
  return 0;
}

static void panel_check_types(unsigned mask) {
  for(unsigned i=0;i<6;i++)SendMessageW(g_checks[i],BM_SETCHECK,(mask&(1u<<i))?BST_CHECKED:BST_UNCHECKED,0);
}
static void inspect_drawing_changed(void);
static void send_config(uint32_t mode) {
  if (!g_connection.shared || g_busy) return;
  hv_config config = {0};
  config.mode = mode;
  config.fill_faces=panel_tint_mode();
  config.opacity=g_opacity;
  if (mode == HV_TYPES) {
    for (unsigned i = 0; i < 6; ++i)
      if (SendMessageW(g_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
        config.include_mask |= 1u << i;
    if (!config.include_mask) config.mode = HV_HIDDEN;
  }
  if (mode == HV_SELECTED) {
    wchar_t wide[4096]; char names[4096];
    GetWindowTextW(g_names, wide, 4096);
    if (!hv_ascii_names(wide, names, sizeof(names)) || !hv_parse_names(names, &config)) {
      notice(L"Enter up to 64 hull hashes, separated by spaces, commas or new lines."); return;
    }
    config.include_inactive = SendMessageW(GetDlgItem(g_window, ID_INACTIVE), BM_GETCHECK, 0, 0) == BST_CHECKED;
  }
  DWORD lock = WaitForSingleObject(g_connection.mutex, 50);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    g_connection.shared->config = config;
    inspect_drawing_changed();
    g_connection.shared->face_status[0] = 0;
    ++g_connection.shared->revision;
    g_connection.shared->heartbeat = GetTickCount64();
    ReleaseMutex(g_connection.mutex);
    g_notice[0] = 0;
  } else notice(L"Game busy. Please try again.");
}
static HWND control(const wchar_t *type, const wchar_t *text, DWORD style,
                    int x, int y, int w, int h, int id) {
  if(!wcscmp(type,L"BUTTON")&&(style&BS_TYPEMASK)==BS_PUSHBUTTON)style|=BS_OWNERDRAW;
  HWND window = CreateWindowExW(0,
      type, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, ui_px(x), ui_px(y), ui_px(w), ui_px(h), g_window,
      (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  SendMessageW(window, WM_SETFONT, (WPARAM)g_font, TRUE);
  SetWindowTheme(window,L"Explorer",NULL);
  return window;
}
#include "inspector_panel.h"
/* Debugger-only diagnostic. No automatic files, game memory or screenshots. */
static LONG WINAPI panel_exception_filter(EXCEPTION_POINTERS *exception) {
  if(!exception||!exception->ExceptionRecord)return EXCEPTION_CONTINUE_SEARCH;
  void *address=exception->ExceptionRecord->ExceptionAddress;HMODULE module=NULL;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)address,&module);
  char module_path[MAX_PATH]={0};if(module)GetModuleFileNameA(module,module_path,MAX_PATH);
  module_path[MAX_PATH-1]=0;
  const char *name=strrchr(module_path,'\\');name=name?name+1:module_path;
  char text[768];int length=snprintf(text,sizeof(text),
    "Journey Hull Viewer %s\r\nException: %08lx\r\nModule: %.100s\r\nOffset: %llx\r\nLevel: %u\r\nObject: %.32s\r\nRequested: %.32s\r\n",
    HV_VERSION,(unsigned long)exception->ExceptionRecord->ExceptionCode,name,
    (unsigned long long)((uintptr_t)address-(uintptr_t)module),g_index_level,g_inspected_hash,g_navigation.hash);
  if(length<=0)return EXCEPTION_EXECUTE_HANDLER;
  OutputDebugStringA(text);
  return EXCEPTION_EXECUTE_HANDLER;
}
typedef struct footer_layout { int hint_width,hint_height,status_width,status_height,reserve;BOOL stacked; } footer_layout;
static footer_layout panel_footer(int width) {
  footer_layout f={0};f.hint_height=ui_text_size(ID_CAM_HINT,width-64).cy;f.hint_width=320;
  while(f.hint_width<width-64&&ui_text_size(ID_CAM_HINT,f.hint_width).cy>f.hint_height)f.hint_width+=4;
  f.stacked=width-64-f.hint_width-16<264;
  f.status_width=f.stacked?width-64:width-80-f.hint_width;
  f.status_height=ui_text_size(ID_STATUS,f.status_width).cy;
  if(f.status_height<44)f.status_height=44;
  f.reserve=16+(f.stacked?f.status_height+4+f.hint_height:(f.status_height>f.hint_height?f.status_height:f.hint_height));
  return f;
}
static void panel_layout(HWND window) {
  if(g_layout_busy)return;
  g_layout_busy=TRUE;
  RECT rect;int visible_w=0,visible_h=0;
  for(int pass=0;pass<3;pass++) {
    GetClientRect(window,&rect);visible_w=MulDiv(rect.right,96,(int)g_dpi);visible_h=MulDiv(rect.bottom,96,(int)g_dpi);
    g_content_width=visible_w<720?720:visible_w;
    g_footer_reserve=panel_footer(g_content_width).reserve;
    int minimum_height=444+g_footer_reserve;if(minimum_height<540)minimum_height=540;
    g_content_height=visible_h<minimum_height?minimum_height:visible_h;
    SCROLLINFO sx={.cbSize=sizeof(sx),.fMask=SIF_RANGE|SIF_PAGE|SIF_POS,.nMin=0,
      .nMax=g_content_width-1,.nPage=(UINT)visible_w,.nPos=g_scroll_x};
    SCROLLINFO sy={.cbSize=sizeof(sy),.fMask=SIF_RANGE|SIF_PAGE|SIF_POS,.nMin=0,
      .nMax=g_content_height-1,.nPage=(UINT)visible_h,.nPos=g_scroll_y};
    SetScrollInfo(window,SB_HORZ,&sx,TRUE);SetScrollInfo(window,SB_VERT,&sy,TRUE);
  }
  SCROLLINFO sx={.cbSize=sizeof(sx),.fMask=SIF_POS},sy=sx;
  GetScrollInfo(window,SB_HORZ,&sx);GetScrollInfo(window,SB_VERT,&sy);
  g_scroll_x=sx.nPos;g_scroll_y=sy.nPos;
  int width=g_content_width,height=g_content_height;
  if(g_sidebar_width>width-432)g_sidebar_width=width-432;
  if(g_sidebar_width>380)g_sidebar_width=380;
  if(g_sidebar_width<270)g_sidebar_width=270;
  g_divider_x=40+g_sidebar_width;
  int side=g_sidebar_width-16;
  ShowWindow(GetDlgItem(window,ID_APP_TITLE),SW_HIDE);ShowWindow(GetDlgItem(window,ID_APP_SUBTITLE),SW_HIDE);
  ShowWindow(GetDlgItem(window,ID_VIEW_TITLE),SW_HIDE);
  int label_width=ui_text_size(ID_CAM_TARGET_LABEL,0).cx;
  int attach_width=ui_text_size(ID_ATTACH,0).cx+24;
  if(attach_width<144)attach_width=144;
  int camera_width=width-32-attach_width-16-32-label_width-8-128-8-78-8;
  if(camera_width>200)camera_width=200;
  ui_move(ID_CAM_POS,32,6,camera_width,24);
  int move_x=32+camera_width+8;
  ui_move(ID_CAM_TARGET_LABEL,move_x,6,label_width,24);move_x+=label_width+8;
  ui_move(ID_CAM_TARGET,move_x,6,128,24);ui_move(ID_CAM_MOVE,move_x+136,6,78,24);
  ui_center_edit(GetDlgItem(window,ID_CAM_TARGET));
  /* One centerline; measured text widths keep group spacing stable at any width. */
  int cam_x=32,label_w=ui_text_size(ID_CAM_LABEL,0).cx;
  ui_move(ID_CAM_LABEL,cam_x,32,label_w,24);cam_x+=label_w+12;
  ui_move(ID_CAM_START,cam_x,32,44,24);cam_x+=52;
  ui_move(ID_CAM_STOP,cam_x,32,44,24);cam_x+=56;
  int ignore_w=ui_text_size(ID_CAM_IGNORE,0).cx+24;
  ui_move(ID_CAM_IGNORE,cam_x,32,ignore_w,24);cam_x+=ignore_w+12;
  int lock_w=ui_text_size(ID_CAM_LOCK,0).cx+24;
  ui_move(ID_CAM_LOCK,cam_x,32,lock_w,24);
  ui_move(ID_ATTACH,width-32-attach_width,14,attach_width,34);
  /* Keep the exact two sentences on two lines; stack below status when narrow. */
  footer_layout footer=panel_footer(width);
  g_footer_reserve=footer.reserve;
  int footer_y=height-g_footer_reserve+8;
  ui_move(ID_CAM_HINT,footer.stacked?32:width-32-footer.hint_width,footer_y+(footer.stacked?footer.status_height+4:0),footer.stacked?width-64:footer.hint_width,footer.hint_height);
  ui_move(ID_DRAW_LABEL,32,78,side-132,24);
  ui_move(ID_HIDE,32+side-126,78,60,24);ui_move(ID_ALL,32+side-60,78,60,24);
  ui_move(ID_FILL,32,110,100,24);ui_move(ID_OPACITY,138,110,38,24);ui_move(ID_OPACITY_LABEL,182,110,side-150,24);
  ui_center_edit(GetDlgItem(window,ID_OPACITY));
  ui_move(ID_TYPES,32,142,(side-8)/2,24);ui_move(ID_NAMES_TAB,40+(side-8)/2,142,(side-8)/2,24);
  ui_move(ID_TYPES_LABEL,32,250,side,20);
  for(int i=0;i<6;i++)ui_move(ID_CHECK+i,32+(i%2)*(side/2),176+(i/2)*24,side/2-4,22);
  ui_move(ID_HULL_HINT,32,176,side,18);ui_move(ID_NAMES,32,198,side,66);
  ui_move(ID_SELECTED,32,270,side,24);ui_move(ID_INACTIVE,32,300,side,22);
  const int named[]={ID_HULL_HINT,ID_NAMES,ID_SELECTED,ID_INACTIVE};
  for(unsigned i=0;i<sizeof(named)/sizeof(*named);i++)ShowWindow(GetDlgItem(window,named[i]),g_names_view?SW_SHOW:SW_HIDE);
  for(int i=0;i<6;i++)ShowWindow(g_checks[i],g_names_view?SW_HIDE:SW_SHOW);
  ShowWindow(GetDlgItem(window,ID_TYPES_LABEL),g_names_view?SW_HIDE:SW_SHOW);
  InvalidateRect(GetDlgItem(window,ID_TYPES),NULL,TRUE);InvalidateRect(GetDlgItem(window,ID_NAMES_TAB),NULL,TRUE);
  ui_move(ID_STATUS,32,footer_y,footer.status_width,footer.status_height);
  inspector_layout(width,height);ui_name_scrollbars(g_names);g_layout_busy=FALSE;
  RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
}
static void panel_status_text(const wchar_t *text) {
  ui_text_changed(g_status,text);
  /* Frequent status polling must not reflow the panel or dismiss text selection
   * unless the measured footer actually needs a different amount of space. */
  if(g_content_width>=720&&panel_footer(g_content_width).reserve!=g_footer_reserve)panel_layout(g_window);
}
static void panel_scroll(HWND window,int bar,int action) {
  SCROLLINFO info={.cbSize=sizeof(info),.fMask=SIF_ALL};GetScrollInfo(window,bar,&info);
  int pos=info.nPos;
  switch(action) {
    case SB_LINEUP:pos-=24;break;case SB_LINEDOWN:pos+=24;break;
    case SB_PAGEUP:pos-=(int)info.nPage;break;case SB_PAGEDOWN:pos+=(int)info.nPage;break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=info.nTrackPos;break;
    case SB_TOP:pos=info.nMin;break;case SB_BOTTOM:pos=info.nMax;break;
    default:return;
  }
  info.fMask=SIF_POS;info.nPos=pos;SetScrollInfo(window,bar,&info,TRUE);
  info.fMask=SIF_POS;GetScrollInfo(window,bar,&info);
  if(bar==SB_VERT)g_scroll_y=info.nPos;else g_scroll_x=info.nPos;
  panel_layout(window);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  switch (message) {
    case WM_CREATE: {
      g_window = window;
      HDC dc=GetDC(window);g_dpi=(unsigned)GetDeviceCaps(dc,LOGPIXELSY);ReleaseDC(window,dc);
      if(g_dpi<72||g_dpi>384)g_dpi=96;
      HINSTANCE instance=GetModuleHandleW(NULL);
      SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)LoadImageW(instance,
          MAKEINTRESOURCEW(IDI_HULL_VIEWER),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));
      SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)LoadImageW(instance,
          MAKEINTRESOURCEW(IDI_HULL_VIEWER),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
      ui_fonts();INITCOMMONCONTROLSEX common={sizeof(common),ICC_TREEVIEW_CLASSES|ICC_TAB_CLASSES};InitCommonControlsEx(&common);
      ui_font(control(L"STATIC",L"Journey Hull Viewer",0,0,0,0,0,ID_APP_TITLE),g_heading_font);
      ui_font(control(L"STATIC",L"Read-only spatial inspection",0,0,0,0,0,ID_APP_SUBTITLE),g_small_font);
      ui_font(control(L"STATIC",L"View Controls",0,0,0,0,0,ID_VIEW_TITLE),g_section_font);
      ui_font(control(L"BUTTON", L"Attach to Journey", WS_TABSTOP, 448, 12, 166, 32, ID_ATTACH),g_section_font);
      ui_font(control(L"STATIC", L"Debug Cam", SS_CENTERIMAGE, 18, 55, 88, 26, ID_CAM_LABEL),g_section_font);
      control(L"BUTTON", L"Start", WS_TABSTOP, 110, 51, 64, 28, ID_CAM_START);
      control(L"BUTTON", L"Stop", WS_TABSTOP, 182, 51, 64, 28, ID_CAM_STOP);
      control(L"BUTTON", L"Ignore terrain", BS_3STATE | WS_TABSTOP, 264, 51, 164, 28, ID_CAM_IGNORE);
      control(L"BUTTON", L"Set angle lock", BS_3STATE | WS_TABSTOP, 448, 51, 166, 28, ID_CAM_LOCK);
      HWND target=control(L"EDIT",L"",ES_MULTILINE|ES_AUTOHSCROLL|WS_TABSTOP,0,0,0,0,ID_CAM_TARGET);
      SendMessageW(target,EM_SETLIMITTEXT,150,0);
      ui_font(target,g_font);
      control(L"STATIC",L"Input x,y,z:",SS_CENTERIMAGE,0,0,0,0,ID_CAM_TARGET_LABEL);
      control(L"BUTTON",L"Move Cam",WS_TABSTOP,0,0,0,0,ID_CAM_MOVE);
      HWND opacity=control(L"EDIT",L"25",ES_MULTILINE|ES_NUMBER|WS_TABSTOP,0,0,0,0,ID_OPACITY);
      SendMessageW(opacity,EM_SETLIMITTEXT,3,0);
      control(L"STATIC",L"% opacity",SS_CENTERIMAGE,0,0,0,0,ID_OPACITY_LABEL);
      camera_unknown();
      camera_controls_enabled(FALSE);
      ui_font(control(L"STATIC", L"In Debug Cam mode, press Fly to teleport your wayfarer to the camera.\r\nYour companion can see this too, so please be considerate.", SS_LEFT,
              18, 84, 596, 22, ID_CAM_HINT),g_small_font);
      ui_font(control(L"STATIC",L"Camera: —",SS_CENTERIMAGE|SS_ENDELLIPSIS,0,0,0,0,ID_CAM_POS),g_section_font);
      ui_font(control(L"STATIC", L"Draw Hulls", SS_CENTERIMAGE, 18, 137, 88, 24, ID_DRAW_LABEL),g_section_font);
      HWND fill=control(L"BUTTON",L"Tint Faces",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,0,0,ID_FILL);
      SendMessageW(fill,BM_SETCHECK,BST_CHECKED,0);
      control(L"BUTTON", L"Hide All", WS_TABSTOP, 110, 130, 100, 32, ID_HIDE);
      control(L"BUTTON", L"Show All", WS_TABSTOP, 220, 130, 120, 32, ID_ALL);
      ui_font(control(L"STATIC", L"Shows hulls matching any selected type.", 0, 18, 180, 596, 22, ID_TYPES_LABEL),g_small_font);
      const wchar_t *types[] = {L"Boundary", L"Physical", L"Wind", L"Camera", L"Region", L"Internal"};
      for (int i = 0; i < 6; ++i) {
        g_checks[i] = control(L"BUTTON", types[i], BS_AUTOCHECKBOX | WS_TABSTOP,
                             18 + (i % 3) * 145, 208 + (i / 3) * 30, 140, 25, ID_CHECK + i);
        SendMessageW(g_checks[i], BM_SETCHECK, BST_UNCHECKED, 0);
      }
      control(L"BUTTON", L"Types", WS_TABSTOP, 448, 219, 166, 32, ID_TYPES);
      control(L"BUTTON", L"Names", WS_TABSTOP,0,0,0,0,ID_NAMES_TAB);
      control(L"STATIC", L"Hull Hashes (32 characters)", 0, 18, 281, 425, 24, ID_HULL_HINT);
      g_names = control(L"EDIT", L"", ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL |
                        ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_HSCROLL | WS_VSCROLL | WS_TABSTOP,
                        18, 310, 596, 150, ID_NAMES);
      SendMessageW(g_names, EM_SETLIMITTEXT, 4000, 0);
      ui_font(g_names,g_font);SendMessageW(g_names,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(ui_px(4),ui_px(4)));
      g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_names, GWLP_WNDPROC, (LONG_PTR)names_proc);
      control(L"BUTTON", L"Show These Hulls", WS_TABSTOP, 18, 473, 200, 32, ID_SELECTED);
      HWND inactive = control(L"BUTTON", L"Include inactive hulls", BS_AUTOCHECKBOX | WS_TABSTOP, 234, 473, 380, 32, ID_INACTIVE);
      SendMessageW(inactive, BM_SETCHECK, BST_CHECKED, 0);
      g_status = control(L"STATIC", L"Not attached. Hull drawings are visible only to you.", 0, 18, 532, 596, 60, ID_STATUS);
      ui_font(g_status,g_small_font);
      inspector_create();
      panel_layout(window);
      SetTimer(window, 1, 250, NULL);
      return 0;
    }
    case WM_SIZE:if(g_font&&w!=SIZE_MINIMIZED)panel_layout(window);return 0;
    case WM_LBUTTONDOWN: {
      int x=MulDiv((short)LOWORD(l),96,(int)g_dpi)+g_scroll_x;
      int y=MulDiv((short)HIWORD(l),96,(int)g_dpi)+g_scroll_y;
      if(abs(x-g_divider_x)<=7&&y>=68&&y<g_content_height-48){g_resizing_panes=TRUE;SetCapture(window);return 0;}break;
    }
    case WM_MOUSEMOVE:
      if(g_resizing_panes){g_sidebar_width=MulDiv((short)LOWORD(l),96,(int)g_dpi)+g_scroll_x-40;panel_layout(window);return 0;}break;
    case WM_LBUTTONUP:if(g_resizing_panes){g_resizing_panes=FALSE;ReleaseCapture();return 0;}break;
    case WM_CAPTURECHANGED:g_resizing_panes=FALSE;return 0;
    case WM_SETCURSOR:
      if(LOWORD(l)==HTCLIENT){POINT p;GetCursorPos(&p);ScreenToClient(window,&p);
        int x=MulDiv(p.x,96,(int)g_dpi)+g_scroll_x,y=MulDiv(p.y,96,(int)g_dpi)+g_scroll_y;
        if(abs(x-g_divider_x)<=7&&y>=68&&y<g_content_height-48){SetCursor(LoadCursorW(NULL,IDC_SIZEWE));return TRUE;}}
      break;
    case WM_VSCROLL:case WM_HSCROLL:
      if(!l){panel_scroll(window,message==WM_VSCROLL?SB_VERT:SB_HORZ,LOWORD(w));return 0;}break;
    case WM_MOUSEWHEEL:
      panel_scroll(window,SB_VERT,GET_WHEEL_DELTA_WPARAM(w)>0?SB_LINEUP:SB_LINEDOWN);return 0;
    case WM_GETMINMAXINFO: {
      MONITORINFO monitor={.cbSize=sizeof(monitor)};
      GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
      RECT minimum={0,0,ui_px(720),ui_px(540)};AdjustWindowRect(&minimum,WS_OVERLAPPEDWINDOW,FALSE);
      int width=minimum.right-minimum.left,height=minimum.bottom-minimum.top;
      int available_w=monitor.rcWork.right-monitor.rcWork.left-24,available_h=monitor.rcWork.bottom-monitor.rcWork.top-24;
      if(available_w>0&&width>available_w)width=available_w;
      if(available_h>0&&height>available_h)height=available_h;
      MINMAXINFO *info=(MINMAXINFO *)l;info->ptMinTrackSize.x=width;info->ptMinTrackSize.y=height;return 0;
    }
    case WM_PAINT:ui_paint(window);return 0;
    case WM_PRINTCLIENT:ui_draw(window,(HDC)w);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_DRAWITEM:if(ui_draw_button((const DRAWITEMSTRUCT *)l))return TRUE;break;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN: {
      HDC dc=(HDC)w;int id=GetDlgCtrlID((HWND)l);
      BOOL root=id==ID_APP_TITLE||id==ID_APP_SUBTITLE||id==ID_STATUS||
        id==ID_CAM_HINT||id==ID_CAM_POS||id==ID_CAM_LABEL||id==ID_CAM_IGNORE||id==ID_CAM_LOCK||id==ID_CAM_TARGET_LABEL;
      BOOL muted=root||id==ID_PROVENANCE||id==ID_INSPECT_HINT||id==ID_CAM_HINT||id==ID_NAMES_NOTE||id==ID_PICK_NOTE;
      SetTextColor(dc,muted?RGB(94,109,132):RGB(30,43,64));
      SetBkColor(dc,root?RGB(240,243,248):RGB(255,255,255));
      return (LRESULT)(root?g_background:g_surface);
    }
    case WM_NOTIFY:
      if(((NMHDR *)l)->idFrom==ID_PROPERTIES&&((NMHDR *)l)->code==NM_CUSTOMDRAW)return property_draw((NMTVCUSTOMDRAW *)l);
      if(inspector_notify(l))return TRUE;
      break;
    case WM_INSPECT_NAVIGATE:inspector_navigate_pending();return 0;
    case WM_COMMAND:
      if(LOWORD(w)==ID_NAMES&&(HIWORD(w)==EN_CHANGE||HIWORD(w)==EN_UPDATE)) {ui_name_scrollbars(g_names);return 0;}
      if(LOWORD(w)==ID_RAW&&(HIWORD(w)==EN_CHANGE||HIWORD(w)==EN_UPDATE)) {ui_raw_scrollbar(GetDlgItem(window,ID_RAW));return 0;}
      if(LOWORD(w)==ID_OBJECT&&(HIWORD(w)==EN_CHANGE||HIWORD(w)==EN_UPDATE)) {ui_name_scrollbars(GetDlgItem(window,ID_OBJECT));return 0;}
      if(LOWORD(w)==ID_OPACITY&&(HIWORD(w)==EN_CHANGE||HIWORD(w)==EN_KILLFOCUS)) {
        wchar_t text[8],*end;GetWindowTextW(GetDlgItem(window,ID_OPACITY),text,8);unsigned long value=wcstoul(text,&end,10);
        if(!text[0]||*end||value>100){if(HIWORD(w)==EN_KILLFOCUS)notice(L"Opacity must be between 0 and 100%.");return 0;}
        g_opacity=(unsigned)value;SendMessageW(window,WM_COMMAND,ID_FILL,0);return 0;
      }
      if(inspector_command(w))return 0;
      if(LOWORD(w)>=ID_CHECK&&LOWORD(w)<ID_CHECK+6&&HIWORD(w)==BN_CLICKED){send_config(HV_TYPES);return 0;}
      switch (LOWORD(w)) {
        case ID_FILL:
          if(g_connection.shared) {
            DWORD lock=WaitForSingleObject(g_connection.mutex,0);
            if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED) {
              g_connection.shared->config.fill_faces=panel_tint_mode();g_connection.shared->face_status[0]=0;
              g_connection.shared->config.opacity=g_opacity;
              ++g_connection.shared->revision;ReleaseMutex(g_connection.mutex);
            }
          }return 0;
        case ID_CAM_START: send_camera(HV_CAM_START); return 0;
        case ID_CAM_STOP: send_camera(HV_CAM_STOP); return 0;
        case ID_CAM_MOVE: send_camera(HV_CAM_MOVE); return 0;
        case ID_CAM_IGNORE:
          send_camera(SendMessageW(GetDlgItem(window, ID_CAM_IGNORE), BM_GETCHECK, 0, 0) == BST_CHECKED
              ? HV_CAM_COLLIDE : HV_CAM_IGNORE); return 0;
        case ID_CAM_LOCK:
          send_camera(SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_GETCHECK, 0, 0) == BST_CHECKED
              ? HV_CAM_UNLOCK : HV_CAM_LOCK); return 0;
        case ID_PASTE: paste_names(); return 0;
        case ID_COPY: copy_names(FALSE); return 0;
        case ID_ATTACH:
          if (!g_busy && !g_connection.shared) {
            g_busy = TRUE;
            g_connection.message[0] = 0;
            EnableWindow(GetDlgItem(window, ID_ATTACH), FALSE);
            panel_status_text(L"Attaching...");
            HANDLE worker = CreateThread(NULL, 0, attach_worker, NULL, 0, NULL);
            if (worker) CloseHandle(worker);
            else { g_busy = FALSE; EnableWindow(GetDlgItem(window, ID_ATTACH), TRUE); }
          }
          return 0;
        case ID_HIDE: panel_check_types(0);send_config(HV_HIDDEN); return 0;
        case ID_ALL: panel_check_types(63);send_config(HV_ALL); return 0;
        case ID_SELECTED: send_config(HV_SELECTED); return 0;
        case ID_NAMES_TAB:g_names_view=TRUE;panel_layout(window);return 0;
        case ID_TYPES:g_names_view=FALSE;panel_layout(window);return 0;
      }
      break;
    case WM_ATTACHED:
      g_busy = FALSE;
      EnableWindow(GetDlgItem(window, ID_ATTACH), !w);
      panel_status_text(g_connection.message);
      g_camera_pending = 0;
      camera_unknown();
      if (w) send_camera(HV_CAM_READ);
      return 0;
    case WM_TIMER:
      if(w==2){inspector_mouse();return 0;}
      if (!g_busy && g_connection.shared) {
        if (WaitForSingleObject(g_connection.process, 0) == WAIT_OBJECT_0) {
          connection_close(&g_connection);
          g_camera_pending = 0;
          camera_controls_enabled(FALSE);
          camera_unknown();
          SetWindowTextW(GetDlgItem(window,ID_CAM_POS),L"Camera: —");
          EnableWindow(GetDlgItem(window, ID_ATTACH), TRUE);
          panel_status_text(L"Journey closed. Start it again, then attach.");
        } else {
          DWORD lock = WaitForSingleObject(g_connection.mutex, 0);
          if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
            g_connection.shared->heartbeat = GetTickCount64();
            wchar_t line[250];
            hv_shared *s = g_connection.shared;
            BOOL camera_level_changed=g_camera_level!=s->live_level;
            if(camera_level_changed){g_camera_level=s->live_level;g_camera_read_time=0;}
            wchar_t position[128];
            if(s->view_position_time&&GetTickCount64()-s->view_position_time<3000)
              _snwprintf(position,128,L"Camera: %.1f, %.1f, %.1f",s->view_position[0],s->view_position[1],s->view_position[2]);
            else lstrcpyW(position,L"Camera: —");
            position[127]=0;ui_text_changed(GetDlgItem(window,ID_CAM_POS),position);
            if (g_camera_pending && s->camera_completed == g_camera_pending) {
              g_camera_pending = 0;
              if (s->camera_ok) {
                g_camera_state_known=TRUE;g_camera_state_bits=s->camera_bits;
                uint32_t collide = s->camera_bits & 6;
                ui_check(GetDlgItem(window, ID_CAM_IGNORE),
                    collide == 0 ? BST_CHECKED : collide == 6 ? BST_UNCHECKED : BST_INDETERMINATE);
                ui_check(GetDlgItem(window, ID_CAM_LOCK),s->camera_bits & 8 ? BST_CHECKED : BST_UNCHECKED);
                if(!g_camera_poll) {
                  if(g_camera_sent_action==HV_CAM_MOVE)
                    _snwprintf(g_notice,192,L"Camera moved to %.6g, %.6g, %.6g.",g_camera_sent_target[0],g_camera_sent_target[1],g_camera_sent_target[2]);
                  else g_notice[0]=0;
                }
              } else {
                camera_unknown();
                if(!g_camera_poll){
                  wchar_t reason[128]={0};MultiByteToWideChar(CP_UTF8,0,s->camera_message,-1,reason,128);reason[127]=0;
                  _snwprintf(g_notice,192,g_camera_sent_action==HV_CAM_MOVE?L"Could not move the camera. %ls":L"%ls",reason);g_notice[191]=0;
                }
              }
            }
            if(camera_level_changed)camera_unknown();
            camera_controls_enabled(s->camera_available && (!g_camera_pending||g_camera_poll));
            ui_enable(GetDlgItem(window,ID_FILL),s->fill_available!=0);
            wchar_t face_note[128]={0};const wchar_t *detail=g_notice;
            if(!g_notice[0]&&s->config.fill_faces!=HV_TINT_OFF&&s->config.mode!=HV_HIDDEN&&s->face_status[0]) {
              if(strcmp(s->face_status,"Faces: post-fog."))
                lstrcpyW(face_note,!strcmp(s->face_status,"Faces: no surface triangles submitted.")?L"No hull faces available to draw.":L"Face tint unavailable. Outlines are still shown.");
              detail=face_note;
            }
            if (s->config.mode == HV_SELECTED && s->revision == s->applied_revision) {
              _snwprintf(line, 250, L"%lu requested · %lu active · %lu inactive · %lu drawn\n%ls",
                (unsigned long)s->config.count, (unsigned long)s->active_matches,
                (unsigned long)s->inactive_matches, (unsigned long)s->drawn_hulls,
                detail[0] ? detail : s->active_matches + s->inactive_matches ?
                L"Drawing inactive hulls does not activate their triggers." : s->unreadable_names ?
                L"No match. Some hull names could not be read." : L"No matching hull name found in this level.");
            } else {
              wchar_t drawing[80];
              if(s->config.mode==HV_HIDDEN)lstrcpyW(drawing,L"Hulls hidden");
              else if(s->revision!=s->applied_revision)lstrcpyW(drawing,L"Waiting for game update");
              else _snwprintf(drawing,80,L"%lu hulls drawn",(unsigned long)s->drawn_hulls);
              _snwprintf(line,250,L"Attached to Journey · %ls\n%ls",drawing,detail[0]?detail:L"Closing the viewer hides all hull drawings.");
            }
            line[249] = 0;
            ReleaseMutex(g_connection.mutex);
            panel_status_text(line);
          }
        }
      }
      inspector_tick();
      if(g_connection.shared&&!g_camera_pending&&GetTickCount64()-g_camera_read_time>=1000)send_camera(HV_CAM_READ);
      return 0;
    case WM_CLOSE:
      if (g_busy) { panel_status_text(L"Please wait for attachment to finish before closing."); return 0; }
      send_config(HV_HIDDEN);
      g_pick_enabled=FALSE;
      if(g_connection.shared) {
        DWORD lock=WaitForSingleObject(g_connection.mutex,0);
        if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){g_connection.shared->inspect_mode=0;g_connection.shared->heartbeat=0;ReleaseMutex(g_connection.mutex);}
      }
      inspect_highlight(NULL);
      connection_close(&g_connection);
      DestroyWindow(window); return 0;
    case WM_DESTROY:
      KillTimer(window, 1);
      KillTimer(window, 2);
      hv_index_free(&g_index);
      ui_release();
      PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
static void dispatch_panel_message(HWND window, MSG *msg) {
  if(msg->message==WM_KEYDOWN&&msg->wParam==VK_RETURN) {
    if(msg->hwnd==GetDlgItem(window,ID_CAM_TARGET)){send_camera(HV_CAM_MOVE);return;}
    if(msg->hwnd==GetDlgItem(window,ID_OPACITY)){SetFocus(GetDlgItem(window,ID_FILL));return;}
  }
  if(msg->hwnd==GetDlgItem(window,ID_LOOKUP_HASH)&&msg->message==WM_KEYDOWN&&msg->wParam==VK_RETURN) {
    inspect_lookup_hash();return;
  }
  if((msg->message==WM_SYSKEYDOWN||msg->message==WM_KEYDOWN)&&msg->wParam==HV_INSPECT_KEY&&
     hv_inspect_modifiers()) {
    if(!((uintptr_t)msg->lParam&(1u<<30))&&g_connection.shared&&IsWindowEnabled(GetDlgItem(window,ID_PICK))) {
      HWND pick=GetDlgItem(window,ID_PICK);
      SendMessageW(pick,BM_SETCHECK,SendMessageW(pick,BM_GETCHECK,0,0)==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);
      inspector_command(ID_PICK);
    }
    return;
  }
  if (!IsDialogMessageW(window, msg)) { TranslateMessage(msg); DispatchMessageW(msg); }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
  (void)previous; (void)command;
  SetUnhandledExceptionFilter(panel_exception_filter);
  WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = instance,
      .hIcon = LoadIconW(instance,MAKEINTRESOURCEW(IDI_HULL_VIEWER)),
      .hCursor = LoadCursorW(NULL, IDC_ARROW), .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
      .lpszClassName = L"JourneyHullViewerPanel"};
  if (!RegisterClassW(&wc)) return 1;
  HDC screen_dc=GetDC(NULL);g_dpi=(unsigned)GetDeviceCaps(screen_dc,LOGPIXELSY);ReleaseDC(NULL,screen_dc);
  if(g_dpi<72||g_dpi>384)g_dpi=96;
  RECT rect = {0, 0, ui_px(1000), ui_px(620)};
  DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
  AdjustWindowRect(&rect, style, FALSE);
  MONITORINFO monitor={.cbSize=sizeof(monitor)};
  GetMonitorInfoW(MonitorFromPoint((POINT){0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
  int width=rect.right-rect.left,height=rect.bottom-rect.top;
  int available_w=monitor.rcWork.right-monitor.rcWork.left-24,available_h=monitor.rcWork.bottom-monitor.rcWork.top-24;
  if(available_w>0&&width>available_w)width=available_w;
  if(available_h>0&&height>available_h)height=available_h;
  HWND window = CreateWindowW(wc.lpszClassName, L"Journey Hull Viewer", style,
      monitor.rcWork.left+(available_w+24-width)/2,monitor.rcWork.top+(available_h+24-height)/2,width,height,
      NULL, NULL, instance, NULL);
  if (!window) return 1;
  ShowWindow(window, show);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    dispatch_panel_message(window, &msg);
  }
  return (int)msg.wParam;
}
