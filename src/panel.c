#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include "protocol.h"
#include "version.h"
#include "clipboard.h"

enum { ID_ATTACH = 100, ID_HIDE, ID_ALL, ID_EXCEPT, ID_SELECTED, ID_TYPES,
       ID_NAMES, ID_STATUS, ID_PASTE, ID_COPY, ID_INACTIVE,
       ID_CAM_START, ID_CAM_STOP, ID_CAM_IGNORE, ID_CAM_LOCK,
       ID_CAM_HINT, ID_HULL_HINT, ID_DRAW_LABEL, ID_TYPES_LABEL,
       ID_CHECK = 200, WM_ATTACHED = WM_APP + 1 };
static HWND g_window, g_status, g_names, g_checks[6];
static HFONT g_font;
static BOOL g_busy;
static WNDPROC g_edit_proc;
static wchar_t g_notice[192];
static void notice(const wchar_t *text) {
  lstrcpynW(g_notice, text, 192);
  if (g_status) SetWindowTextW(g_status, text);
}
static void paste_names(void) {
  wchar_t text[4096];
  if (!clipboard_read(g_window, text, 4096)) {
    notice(L"Cannot read current clipboard text (or it exceeds 4095 characters). No previous paste was used.");
    return;
  }
  DWORD first, last;
  SendMessageW(g_names, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
  if ((size_t)GetWindowTextLengthW(g_names) - (last - first) + wcslen(text) > 4000) {
    notice(L"Pasting would exceed the 4000-character limit. Select text to replace first."); return;
  }
  SendMessageW(g_names, EM_REPLACESEL, TRUE, (LPARAM)text);
  SetFocus(g_names);
  notice(L"Pasted current Windows clipboard text. Click Show only these hulls to apply.");
}
static void copy_names(BOOL cut) {
  wchar_t text[4096]; DWORD first, last;
  GetWindowTextW(g_names, text, 4096);
  SendMessageW(g_names, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
  if (first == last || last > wcslen(text)) return;
  text[last] = 0;
  if (!clipboard_write(g_window, text + first)) { notice(L"Cannot write to the clipboard. Please try again."); return; }
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

static void camera_controls_enabled(BOOL enabled) {
  for (int id = ID_CAM_START; id <= ID_CAM_LOCK; ++id)
    EnableWindow(GetDlgItem(g_window, id), enabled);
}
static void camera_unknown(void) {
  SendMessageW(GetDlgItem(g_window, ID_CAM_IGNORE), BM_SETCHECK, BST_INDETERMINATE, 0);
  SendMessageW(GetDlgItem(g_window, ID_CAM_LOCK), BM_SETCHECK, BST_INDETERMINATE, 0);
}
static void send_camera(uint32_t action) {
  if (!g_connection.shared || g_busy || action >= HV_CAM_COUNT) return;
  DWORD lock = WaitForSingleObject(g_connection.mutex, 0);
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) { notice(L"Busy; try again."); return; }
  hv_shared *s = g_connection.shared;
  BOOL sent = s->camera_available && s->camera_request == s->camera_completed;
  if (sent) {
    s->camera_action = action;
    g_camera_pending = ++s->camera_request;
    s->heartbeat = GetTickCount64();
  }
  ReleaseMutex(g_connection.mutex);
  if (sent) { camera_controls_enabled(FALSE); notice(L"Camera: waiting for game update."); }
  else notice(L"Camera controls unavailable or busy.");
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

static void send_config(uint32_t mode) {
  if (!g_connection.shared || g_busy) return;
  hv_config config = {0};
  config.mode = mode;
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
      notice(L"Use at most 64 ASCII hull names, separated by spaces, commas or new lines."); return;
    }
    config.include_inactive = SendMessageW(GetDlgItem(g_window, ID_INACTIVE), BM_GETCHECK, 0, 0) == BST_CHECKED;
  }
  DWORD lock = WaitForSingleObject(g_connection.mutex, 50);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    g_connection.shared->config = config;
    ++g_connection.shared->revision;
    g_connection.shared->heartbeat = GetTickCount64();
    ReleaseMutex(g_connection.mutex);
    g_notice[0] = 0;
  } else notice(L"Game is busy; please try again.");
}
static HWND control(const wchar_t *type, const wchar_t *text, DWORD style,
                    int x, int y, int w, int h, int id) {
  HWND window = CreateWindowExW(type[0] == L'E' ? WS_EX_CLIENTEDGE : 0,
      type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_window,
      (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  SendMessageW(window, WM_SETFONT, (WPARAM)g_font, TRUE);
  return window;
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  switch (message) {
    case WM_CREATE: {
      g_window = window;
      g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
      control(L"STATIC", L"Start Journey normally, then attach the viewer.", 0, 18, 18, 430, 24, 0);
      control(L"BUTTON", L"Attach to Journey", WS_TABSTOP, 448, 12, 166, 32, ID_ATTACH);
      control(L"STATIC", L"Debug Cam", 0, 18, 55, 88, 26, 0);
      control(L"BUTTON", L"Start", WS_TABSTOP, 110, 51, 64, 28, ID_CAM_START);
      control(L"BUTTON", L"Stop", WS_TABSTOP, 182, 51, 64, 28, ID_CAM_STOP);
      control(L"BUTTON", L"Ignore terrain", BS_3STATE | WS_TABSTOP, 264, 51, 164, 28, ID_CAM_IGNORE);
      control(L"BUTTON", L"Set angle lock", BS_3STATE | WS_TABSTOP, 448, 51, 166, 28, ID_CAM_LOCK);
      camera_unknown();
      camera_controls_enabled(FALSE);
      control(L"STATIC", L"When in Debug Cam mode, press Fly to teleport the wayfarer to the camera.", 0,
              18, 84, 596, 22, ID_CAM_HINT);
      control(L"STATIC", L"Draw Hulls", 0, 18, 137, 88, 24, ID_DRAW_LABEL);
      control(L"BUTTON", L"Hide", WS_TABSTOP, 110, 130, 100, 32, ID_HIDE);
      control(L"BUTTON", L"Show all", WS_TABSTOP, 220, 130, 120, 32, ID_ALL);
      control(L"BUTTON", L"Except physical", WS_TABSTOP, 350, 130, 160, 32, ID_EXCEPT);
      control(L"STATIC", L"Hull Types (a hull may belong to several):", 0, 18, 180, 596, 22, ID_TYPES_LABEL);
      const wchar_t *types[] = {L"Boundary", L"Physical", L"Wind", L"Camera", L"Region", L"Internal"};
      for (int i = 0; i < 6; ++i) {
        g_checks[i] = control(L"BUTTON", types[i], BS_AUTOCHECKBOX | WS_TABSTOP,
                             18 + (i % 3) * 145, 208 + (i / 3) * 30, 140, 25, ID_CHECK + i);
        SendMessageW(g_checks[i], BM_SETCHECK, i == 1 ? BST_UNCHECKED : BST_CHECKED, 0);
      }
      control(L"BUTTON", L"Show checked types", WS_TABSTOP, 448, 219, 166, 32, ID_TYPES);
      control(L"STATIC", L"Hull Names (32-char; one per line or comma-separated)", 0, 18, 281, 425, 24, ID_HULL_HINT);
      control(L"BUTTON", L"Paste", WS_TABSTOP, 448, 275, 78, 30, ID_PASTE);
      control(L"BUTTON", L"Copy", WS_TABSTOP, 536, 275, 78, 30, ID_COPY);
      g_names = control(L"EDIT", L"", ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL |
                        ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                        18, 310, 596, 150, ID_NAMES);
      SendMessageW(g_names, EM_SETLIMITTEXT, 4000, 0);
      g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_names, GWLP_WNDPROC, (LONG_PTR)names_proc);
      control(L"BUTTON", L"Show only these hulls", WS_TABSTOP, 18, 473, 200, 32, ID_SELECTED);
      HWND inactive = control(L"BUTTON", L"Include inactive hulls (selected names)", BS_AUTOCHECKBOX | WS_TABSTOP, 234, 473, 380, 32, ID_INACTIVE);
      SendMessageW(inactive, BM_SETCHECK, BST_CHECKED, 0);
      g_status = control(L"STATIC", L"Not attached. Outlines are local to your game.", 0, 18, 532, 596, 60, ID_STATUS);
      SetTimer(window, 1, 250, NULL);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case ID_CAM_START: send_camera(HV_CAM_START); return 0;
        case ID_CAM_STOP: send_camera(HV_CAM_STOP); return 0;
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
            SetWindowTextW(g_status, L"Attaching...");
            HANDLE worker = CreateThread(NULL, 0, attach_worker, NULL, 0, NULL);
            if (worker) CloseHandle(worker);
            else { g_busy = FALSE; EnableWindow(GetDlgItem(window, ID_ATTACH), TRUE); }
          }
          return 0;
        case ID_HIDE: send_config(HV_HIDDEN); return 0;
        case ID_ALL: send_config(HV_ALL); return 0;
        case ID_SELECTED: send_config(HV_SELECTED); return 0;
        case ID_TYPES: send_config(HV_TYPES); return 0;
        case ID_EXCEPT:
          if (!g_busy && g_connection.shared) {
            DWORD lock = WaitForSingleObject(g_connection.mutex, 50);
            if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
              hv_config config = {.mode = HV_TYPES, .exclude_mask = 2};
              g_connection.shared->config = config;
              ++g_connection.shared->revision;
              ReleaseMutex(g_connection.mutex);
              g_notice[0] = 0;
            }
          }
          return 0;
      }
      break;
    case WM_ATTACHED:
      g_busy = FALSE;
      EnableWindow(GetDlgItem(window, ID_ATTACH), !w);
      SetWindowTextW(g_status, g_connection.message);
      g_camera_pending = 0;
      camera_unknown();
      if (w) send_camera(HV_CAM_READ);
      return 0;
    case WM_TIMER:
      if (!g_busy && g_connection.shared) {
        if (WaitForSingleObject(g_connection.process, 0) == WAIT_OBJECT_0) {
          connection_close(&g_connection);
          g_camera_pending = 0;
          camera_controls_enabled(FALSE);
          camera_unknown();
          EnableWindow(GetDlgItem(window, ID_ATTACH), TRUE);
          SetWindowTextW(g_status, L"Journey closed. Start it again, then attach.");
        } else {
          DWORD lock = WaitForSingleObject(g_connection.mutex, 0);
          if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
            g_connection.shared->heartbeat = GetTickCount64();
            wchar_t line[250];
            hv_shared *s = g_connection.shared;
            if (g_camera_pending && s->camera_completed == g_camera_pending) {
              g_camera_pending = 0;
              if (s->camera_ok) {
                uint32_t collide = s->camera_bits & 6;
                SendMessageW(GetDlgItem(window, ID_CAM_IGNORE), BM_SETCHECK,
                    collide == 0 ? BST_CHECKED : collide == 6 ? BST_UNCHECKED : BST_INDETERMINATE, 0);
                SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_SETCHECK,
                    s->camera_bits & 8 ? BST_CHECKED : BST_UNCHECKED, 0);
                g_notice[0] = 0;
              } else {
                camera_unknown();
                MultiByteToWideChar(CP_UTF8, 0, s->camera_message, -1, g_notice, 192);
                g_notice[191] = 0;
              }
            }
            camera_controls_enabled(s->camera_available && !g_camera_pending);
            if (s->config.mode == HV_SELECTED && s->revision == s->applied_revision) {
              _snwprintf(line, 250, L"%lu requested | %lu active, %lu inactive matches | %lu drawn\n%ls",
                (unsigned long)s->config.count, (unsigned long)s->active_matches,
                (unsigned long)s->inactive_matches, (unsigned long)s->drawn_hulls,
                g_notice[0] ? g_notice : s->active_matches + s->inactive_matches ?
                L"Inactive outlines do not activate any trigger." : s->unreadable_names ?
                L"No match. Some hull names could not be read." : L"No matching hull name found in this level.");
            } else {
              _snwprintf(line, 250, L"Attached to Journey (PID %lu). %ls | %lu hulls\n%ls",
                (unsigned long)g_connection.pid, s->config.mode == HV_HIDDEN ? L"Hidden" :
                s->revision != s->applied_revision ? L"Waiting for game update" : L"Showing",
                (unsigned long)s->drawn_hulls, g_notice[0] ? g_notice : L"Closing this panel hides the outlines.");
            }
            line[249] = 0;
            ReleaseMutex(g_connection.mutex);
            SetWindowTextW(g_status, line);
          }
        }
      }
      return 0;
    case WM_CLOSE:
      if (g_busy) { SetWindowTextW(g_status, L"Please wait for attachment to finish before closing."); return 0; }
      send_config(HV_HIDDEN);
      connection_close(&g_connection);
      DestroyWindow(window); return 0;
    case WM_DESTROY:
      KillTimer(window, 1);
      DeleteObject(g_font);
      PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
static void dispatch_panel_message(HWND window, MSG *msg) {
  if (!IsDialogMessageW(window, msg)) { TranslateMessage(msg); DispatchMessageW(msg); }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
  (void)previous; (void)command;
  WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = instance,
      .hCursor = LoadCursorW(NULL, IDC_ARROW), .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
      .lpszClassName = L"JourneyHullViewerPanel"};
  if (!RegisterClassW(&wc)) return 1;
  RECT rect = {0, 0, 632, 605};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&rect, style, FALSE);
  HWND window = CreateWindowW(wc.lpszClassName, L"Journey Hull Viewer", style,
      CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
      NULL, NULL, instance, NULL);
  if (!window) return 1;
  ShowWindow(window, show);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    dispatch_panel_message(window, &msg);
  }
  return (int)msg.wParam;
}
