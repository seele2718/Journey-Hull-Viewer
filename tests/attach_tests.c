#include <windows.h>
#include <string.h>
#include <wchar.h>
/* Mock only the clipboard boundary: tests must not read or replace the user's
 * clipboard. */
static HGLOBAL fixture_clipboard;
static HWND fixture_clipboard_owner;
static BOOL fixture_clipboard_busy;
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
#define wWinMain unused_panel_main
#include "../src/panel.c"
#undef wWinMain
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
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
                                0, 0, 650, 570, NULL, NULL, wc.hInstance, NULL);
    CHECK(window);
    wchar_t angle_label[64];
    GetWindowTextW(GetDlgItem(window, ID_CAM_LOCK), angle_label, 64);
    CHECK(!wcscmp(angle_label, L"Set angle lock"));
    check_single_line(window, ID_HULL_HINT);
    check_single_line(window, ID_CAM_HINT);
    check_single_line(window, ID_DRAW_LABEL);
    RECT label_rect, edit_rect, hint_rect, draw_rect;
    GetWindowRect(GetDlgItem(window, ID_HULL_HINT), &label_rect);
    GetWindowRect(g_names, &edit_rect);
    GetWindowRect(GetDlgItem(window, ID_CAM_HINT), &hint_rect);
    GetWindowRect(GetDlgItem(window, ID_HIDE), &draw_rect);
    CHECK(label_rect.bottom < edit_rect.top && hint_rect.bottom < draw_rect.top);
    CHECK(draw_rect.top - hint_rect.bottom >= 24);
    RECT types_rect, status_rect;
    GetWindowRect(GetDlgItem(window, ID_TYPES_LABEL), &types_rect);
    GetWindowRect(g_status, &status_rect);
    CHECK(types_rect.left == edit_rect.left && types_rect.right == edit_rect.right);
    CHECK(types_rect.right == status_rect.right && types_rect.right == hint_rect.right);
    /* Exercise dialog routing: directly setting multiline text did not catch
     * Enter being consumed as a default-button action. No physical input. */
    CHECK(GetWindowLongPtrW(g_names, GWL_STYLE) & ES_WANTRETURN);
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
    memset(&c, 0, sizeof(c));
    SendMessageW(window, WM_COMMAND, ID_ALL, 0);
    CHECK(g_connection.shared->config.mode == HV_ALL);
    SendMessageW(window, WM_COMMAND, ID_EXCEPT, 0);
    CHECK(g_connection.shared->config.mode == HV_TYPES && g_connection.shared->config.exclude_mask == 2);
    /* Mock only camera acknowledgements; fixture target never runs game Lua. */
    CHECK(!g_connection.shared->camera_available);
    g_connection.shared->camera_available = 1;
    SendMessageW(window, WM_COMMAND, ID_CAM_START, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_START && g_camera_pending);
    uint32_t camera_revision = g_connection.shared->camera_request;
    SendMessageW(window, WM_COMMAND, ID_CAM_STOP, 0);
    CHECK(g_connection.shared->camera_request == camera_revision); /* No overwrite. */
    g_connection.shared->camera_completed = camera_revision;
    g_connection.shared->camera_ok = 1; g_connection.shared->camera_bits = 9;
    SendMessageW(window, WM_TIMER, 1, 0);
    CHECK(!g_camera_pending);
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_IGNORE), BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_GETCHECK, 0, 0) == BST_CHECKED);
    SendMessageW(window, WM_COMMAND, ID_CAM_IGNORE, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_COLLIDE);
    g_connection.shared->camera_completed = g_connection.shared->camera_request;
    g_connection.shared->camera_bits = 6;
    SendMessageW(window, WM_TIMER, 1, 0);
    SendMessageW(window, WM_COMMAND, ID_CAM_LOCK, 0);
    CHECK(g_connection.shared->camera_action == HV_CAM_LOCK);
    g_connection.shared->camera_completed = g_connection.shared->camera_request;
    g_connection.shared->camera_ok = 0;
    lstrcpyA(g_connection.shared->camera_message, "Camera test error");
    SendMessageW(window, WM_TIMER, 1, 0);
    CHECK(wcsstr(g_notice, L"Camera test error"));
    CHECK(SendMessageW(GetDlgItem(window, ID_CAM_LOCK), BM_GETCHECK, 0, 0) == BST_INDETERMINATE);
    SendMessageW(window, WM_COMMAND, ID_TYPES, 0);
    CHECK(g_connection.shared->config.include_mask == 61);
    SetWindowTextW(g_names, L"8dc1645e2ed9ee33aa75297e01b94d0c\r\nbd9d89c977ed505282b23220a5fb190d");
    SendMessageW(window, WM_COMMAND, ID_SELECTED, 0);
    CHECK(g_connection.shared->config.mode == HV_SELECTED && g_connection.shared->config.count == 2);
    CHECK(g_connection.shared->config.include_inactive);
    CHECK(!strcmp(g_connection.shared->config.names[0], "8dc1645e2ed9ee33aa75297e01b94d0c"));
    g_connection.shared->applied_revision = g_connection.shared->revision;
    g_connection.shared->inactive_matches = 1;
    SendMessageW(window, WM_TIMER, 1, 0);
    wchar_t summary[512]; GetWindowTextW(g_status, summary, 512);
    CHECK(wcsstr(summary, L"0 active, 1 inactive"));
    uint32_t revision = g_connection.shared->revision;
    SetWindowTextW(g_names, L"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    SendMessageW(window, WM_COMMAND, ID_SELECTED, 0);
    CHECK(g_connection.shared->revision == revision);
    SendMessageW(window, WM_TIMER, 1, 0);
    wchar_t status[512]; GetWindowTextW(g_status, status, 512);
    CHECK(wcsstr(status, L"Use at most 64")); /* Error survives status refresh. */
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
int wmain(void) {
  run_case(FALSE); run_case(TRUE);
  puts("PASS: late-load, reopen, hooks, single-line labels/layout, panel controls, Enter dialog routing, Unicode clipboard mocks, persistent errors and conflict rejection");
  return 0;
}
