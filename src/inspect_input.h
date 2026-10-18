/* Optional inspect input. No global API hook or external overlay dependency.
 * The game's foreground IAT slot is chained, preserving the original caller
 * return address whenever inspect suppression does not apply. */
#include "inspect_shortcut.h"
static volatile LONG g_input_mode __attribute__((used));
static uintptr_t g_input_mouse_return __attribute__((used));
static void *g_input_foreground_next __attribute__((used));
static HWND g_input_window;
static HHOOK g_input_messages,g_input_cursor;
static BOOL g_input_supported,g_input_iat_installed;
static BOOL g_input_requested;
static volatile LONG g_input_cancel;
static volatile LONG g_input_toggle;
static volatile LONG g_input_listening;
static BOOL g_input_shortcut_down;
static char g_input_message[128];

__attribute__((naked,noinline)) static HWND WINAPI inspect_foreground(void) {
  __asm__ volatile(
    "movq (%rsp), %rax\n\t"
    "cmpq g_input_mouse_return(%rip), %rax\n\t"
    "jne 1f\n\t"
    "cmpl $0, g_input_mode(%rip)\n\t"
    "je 1f\n\t"
    "xorl %eax, %eax\n\tret\n\t"
    "1: jmp *g_input_foreground_next(%rip)\n\t");
}
static void inspect_cancel_input(void) {
  InterlockedExchange(&g_input_mode,0);
  InterlockedExchange(&g_input_cancel,1);
}
static BOOL inspect_raw_forward(HRAWINPUT handle,BOOL active) {
  if(!active&&!g_input_listening)return TRUE;
  RAWINPUT raw;UINT size=sizeof(raw);
  UINT got=GetRawInputData(handle,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER));
  if(got==(UINT)-1||got<sizeof(RAWINPUTHEADER))return !active;
  if(raw.header.dwType!=RIM_TYPEKEYBOARD)return !active;
  if(got<sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD))return !active;
  BOOL release=(raw.data.keyboard.Flags&RI_KEY_BREAK)!=0;
  if(raw.data.keyboard.VKey==HV_INSPECT_KEY&&
     (g_input_shortcut_down||(g_input_listening&&hv_inspect_modifiers())))return release;
  return TRUE; /* Keep native keyboard camera controls available while picking. */
}
static BOOL inspect_filter_message(MSG *msg) {
  HWND window=msg->hwnd;UINT message=msg->message;WPARAM w=msg->wParam;LPARAM l=msg->lParam;
  BOOL active=InterlockedCompareExchange(&g_input_mode,0,0)!=0;
  if(window==g_input_window) {
    if(message==WM_KILLFOCUS||(message==WM_ACTIVATEAPP&&!w))g_input_shortcut_down=FALSE;
    if((message==WM_KEYDOWN||message==WM_SYSKEYDOWN)&&w==HV_INSPECT_KEY&&g_input_listening&&
       (g_input_shortcut_down||hv_inspect_modifiers())) {
      if(!g_input_shortcut_down&&!((uintptr_t)l&(1u<<30)))InterlockedIncrement(&g_input_toggle);
      g_input_shortcut_down=TRUE;return TRUE;
    }
    if((message==WM_CHAR||message==WM_SYSCHAR)&&g_input_shortcut_down&&(w==9||w=='i'||w=='I'))return TRUE;
    if((message==WM_KEYUP||message==WM_SYSKEYUP)&&w==HV_INSPECT_KEY)g_input_shortcut_down=FALSE;
    if(message==WM_INPUT&&!inspect_raw_forward((HRAWINPUT)l,active)) {
      DefWindowProcW(window,message,w,l);return TRUE;
    }
    if(active) {
      if(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)return TRUE;
    }
  }
  return FALSE;
}
static LRESULT CALLBACK inspect_message_hook(int code,WPARAM w,LPARAM l) {
  if(code==HC_ACTION&&w==PM_REMOVE) {
    MSG *msg=(MSG *)l;
    if(inspect_filter_message(msg)){msg->message=WM_NULL;msg->wParam=0;msg->lParam=0;}
  }
  return CallNextHookEx(g_input_messages,code,w,l);
}
static LRESULT CALLBACK inspect_cursor_hook(int code,WPARAM w,LPARAM l) {
  if(code==HC_ACTION&&g_input_mode) {
    const CWPRETSTRUCT *msg=(const CWPRETSTRUCT *)l;
    if(msg->hwnd==g_input_window&&msg->message==WM_SETCURSOR&&LOWORD(msg->lParam)==HTCLIENT)
      SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32512)));
  }
  return CallNextHookEx(g_input_cursor,code,w,l);
}
static BOOL CALLBACK inspect_find_window(HWND window,LPARAM parameter) {
  DWORD pid=0;GetWindowThreadProcessId(window,&pid);RECT rect;
  if(pid==GetCurrentProcessId()&&IsWindowVisible(window)&&!GetWindow(window,GW_OWNER)&&
     GetClientRect(window,&rect)&&rect.right>=64&&rect.bottom>=64) {
    HWND *best=(HWND *)parameter;RECT previous={0};if(*best)GetClientRect(*best,&previous);
    if((int64_t)rect.right*rect.bottom>(int64_t)previous.right*previous.bottom)*best=window;
  }
  return TRUE;
}
static BOOL inspect_install_window(HWND window) {
  if(!window||!IsWindow(window))return FALSE;
  DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid!=GetCurrentProcessId())return FALSE;
  HMODULE module=NULL;
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)(uintptr_t)inspect_message_hook,&module))return FALSE;
  DWORD thread=GetWindowThreadProcessId(window,NULL);g_input_window=window;
  g_input_messages=SetWindowsHookExW(WH_GETMESSAGE,inspect_message_hook,module,thread);
  if(g_input_messages)g_input_cursor=SetWindowsHookExW(WH_CALLWNDPROCRET,inspect_cursor_hook,module,thread);
  if(!g_input_cursor){if(g_input_messages)UnhookWindowsHookEx(g_input_messages);g_input_messages=NULL;return FALSE;}
  return TRUE;
}
static BOOL inspect_install_iat(BYTE *image) {
  void **slot=(void **)(image+0x553648);
  if(!readable_range(slot,sizeof(void *))||!*slot)return FALSE;
  void *previous=*slot;MEMORY_BASIC_INFORMATION region;
  if(!VirtualQuery(previous,&region,sizeof(region))||!(region.Protect&
       (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return FALSE;
  DWORD protection=0;
  if(!VirtualProtect(slot,sizeof(void *),PAGE_READWRITE,&protection))return FALSE;
  g_input_foreground_next=previous;g_input_mouse_return=(uintptr_t)image+0x288f23;
  MemoryBarrier();
  BOOL installed=InterlockedCompareExchangePointer(slot,(void *)inspect_foreground,previous)==previous;
  DWORD ignored;VirtualProtect(slot,sizeof(void *),protection,&ignored);
  if(installed)g_input_iat_installed=TRUE;
  return installed;
}
static void update_inspect_input(BOOL requested) {
  if((!requested&&!g_input_listening)||!g_input_supported) {
    InterlockedExchange(&g_input_mode,0);
    if(g_input_messages){UnhookWindowsHookEx(g_input_messages);g_input_messages=NULL;}
    if(g_input_cursor){UnhookWindowsHookEx(g_input_cursor);g_input_cursor=NULL;}
    g_input_shortcut_down=FALSE;
    return;
  }
  BYTE *image=(BYTE *)GetModuleHandleW(NULL);
  if(requested&&!g_input_iat_installed&&!inspect_install_iat(image)) {
    lstrcpyA(g_input_message,"Inspect cursor unavailable: input routing changed.");inspect_cancel_input();return;
  }
  if(requested&&*(void **)(image+0x553648)!=(void *)inspect_foreground) {
    lstrcpyA(g_input_message,"Inspect cursor unavailable: another input route replaced it.");inspect_cancel_input();return;
  }
  if(!g_input_messages) {
    HWND window=NULL;EnumWindows(inspect_find_window,(LPARAM)&window);
    if(!inspect_install_window(window)) {
      lstrcpyA(g_input_message,"Journey window is not ready for Inspect mode.");inspect_cancel_input();return;
    }
  }
  if(!IsWindow(g_input_window)) {
    lstrcpyA(g_input_message,"Journey window changed. Enable Inspect mode again.");g_input_window=NULL;inspect_cancel_input();return;
  }
  InterlockedExchange(&g_input_mode,requested);
  if(requested&&GetForegroundWindow()==g_input_window)SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32512)));
  g_input_message[0]=0;
}
