#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static RAWINPUT fake_raw;
static BOOL fake_raw_error;
static SHORT fake_keys[256];
static SHORT WINAPI fake_key_state(int key){return key>=0&&key<256?fake_keys[key]:0;}
static unsigned cursor_calls;
static HCURSOR WINAPI fake_cursor(HCURSOR cursor){cursor_calls++;return cursor;}
static UINT WINAPI fake_raw_data(HRAWINPUT handle,UINT command,LPVOID data,PUINT size,UINT header) {
  (void)handle;(void)command;(void)header;
  if(fake_raw_error)return (UINT)-1;
  if(*size<sizeof(fake_raw))return (UINT)-1;
  memcpy(data,&fake_raw,sizeof(fake_raw));*size=sizeof(fake_raw);return sizeof(fake_raw);
}
#define SetCursor fake_cursor
#define GetRawInputData fake_raw_data
#define GetKeyState fake_key_state
#include "../src/renderer.c"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static HWND __attribute__((noinline)) WINAPI caller_probe(void) {
  return (HWND)__builtin_return_address(0);
}
static void caller_preservation(void) {
  BYTE *image=VirtualAlloc(NULL,0x554000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);CHECK(image);
  *(void **)(image+0x553648)=(void *)caller_probe;
  CHECK(inspect_install_iat(image));
  /* Proper x64 call frame, exact native mouse-return address and a separate
   * nonmouse caller using the same IAT slot. No real process API is patched. */
  for(unsigned offset=0;offset<=0x100;offset+=0x100) {
    BYTE *code=image+0x288f19+offset;
    const BYTE begin[]={0x48,0x83,0xec,0x28,0xff,0x15};memcpy(code,begin,sizeof(begin));
    int32_t displacement=(int32_t)((image+0x553648)-(code+10));memcpy(code+6,&displacement,4);
    const BYTE end[]={0x48,0x83,0xc4,0x28,0xc3};memcpy(code+10,end,sizeof(end));
  }
  FlushInstructionCache(GetCurrentProcess(),image,0x554000);
  typedef HWND(WINAPI *call_fn)(void);
  call_fn mouse=(call_fn)(image+0x288f19),other=(call_fn)(image+0x289019);
  g_input_mode=0;CHECK(mouse()==(HWND)(image+0x288f23));CHECK(other()==(HWND)(image+0x289023));
  g_input_mode=1;CHECK(mouse()==NULL);CHECK(other()==(HWND)(image+0x289023));
  g_input_mode=0;CHECK(mouse()==(HWND)(image+0x288f23));
  VirtualFree(image,0,MEM_RELEASE);g_input_iat_installed=FALSE;
}
static LRESULT CALLBACK fixture_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
  if(message==WM_SETCURSOR)return TRUE;
  return DefWindowProcW(window,message,w,l);
}
static void routing(void) {
  HWND window=(HWND)(uintptr_t)0x1234;g_input_window=window;g_input_mode=1;
  g_input_listening=TRUE;g_input_mode=0;g_input_toggle=0;
  fake_keys[VK_CONTROL]=(SHORT)0x8000;
  MSG shortcut={.hwnd=window,.message=WM_KEYDOWN,.wParam='I'};
  CHECK(inspect_filter_message(&shortcut)&&g_input_toggle==1);
  shortcut.lParam|=1u<<30;CHECK(inspect_filter_message(&shortcut)&&g_input_toggle==1);
  fake_raw.header.dwType=RIM_TYPEKEYBOARD;fake_raw.data.keyboard.VKey='I';fake_raw.data.keyboard.Flags=0;
  CHECK(!inspect_raw_forward(NULL,FALSE));fake_raw.data.keyboard.Flags=RI_KEY_BREAK;CHECK(inspect_raw_forward(NULL,FALSE));
  MSG character={.hwnd=window,.message=WM_CHAR,.wParam=9};CHECK(inspect_filter_message(&character));
  shortcut.message=WM_KEYUP;CHECK(!inspect_filter_message(&shortcut));
  shortcut.message=WM_KEYDOWN;shortcut.lParam=0;CHECK(inspect_filter_message(&shortcut)&&g_input_toggle==2);
  shortcut.message=WM_KEYUP;CHECK(!inspect_filter_message(&shortcut));
  fake_keys[VK_CONTROL]=0;shortcut.message=WM_KEYDOWN;CHECK(!inspect_filter_message(&shortcut));
  fake_keys[VK_CONTROL]=(SHORT)0x8000;
  shortcut.hwnd=(HWND)(uintptr_t)0x5678;shortcut.message=WM_KEYDOWN;
  CHECK(!inspect_filter_message(&shortcut)&&g_input_toggle==2);
  shortcut.hwnd=window;shortcut.wParam=VK_OEM_3;shortcut.message=WM_SYSKEYDOWN;shortcut.lParam=1u<<29;
  fake_keys[VK_CONTROL]=0;fake_keys[VK_MENU]=(SHORT)0x8000;
  CHECK(!inspect_filter_message(&shortcut)); /* Old Alt+backtick belongs to other tools. */
  fake_keys[VK_CONTROL]=(SHORT)0x8000;shortcut.wParam='I';
  CHECK(!inspect_filter_message(&shortcut)); /* AltGr combinations are not Ctrl+I. */
  memset(fake_keys,0,sizeof(fake_keys));
  g_input_listening=FALSE;g_input_mode=1;
  MSG mouse={.hwnd=window,.message=WM_LBUTTONDOWN};CHECK(inspect_filter_message(&mouse));
  MSG key={.hwnd=window,.message=WM_KEYDOWN,.wParam='W'};CHECK(!inspect_filter_message(&key));
  const UINT camera_keys[]={'W','A','S','D',VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'Q','E',VK_SHIFT,VK_CONTROL,
    VK_ESCAPE,'I','J','K','L',VK_NUMPAD0,VK_F6};
  for(unsigned i=0;i<sizeof(camera_keys)/sizeof(camera_keys[0]);i++) {
    key.message=WM_KEYDOWN;key.wParam=camera_keys[i];CHECK(!inspect_filter_message(&key));
    key.message=WM_KEYUP;CHECK(!inspect_filter_message(&key));
    fake_raw.header.dwType=RIM_TYPEKEYBOARD;fake_raw.data.keyboard.VKey=(USHORT)camera_keys[i];
    fake_raw.data.keyboard.Flags=0;CHECK(inspect_raw_forward(NULL,TRUE));
    fake_raw.data.keyboard.Flags=RI_KEY_BREAK;CHECK(inspect_raw_forward(NULL,TRUE));
  }
  key.message=WM_CHAR;key.wParam='w';CHECK(!inspect_filter_message(&key));
  key.message=WM_KEYUP;CHECK(!inspect_filter_message(&key));
  key.hwnd=(HWND)(uintptr_t)0x5678;key.message=WM_KEYDOWN;CHECK(!inspect_filter_message(&key));
  fake_raw.header.dwType=RIM_TYPEKEYBOARD;fake_raw.data.keyboard.VKey='W';fake_raw.data.keyboard.Flags=0;
  CHECK(inspect_raw_forward(NULL,TRUE));fake_raw.data.keyboard.Flags=RI_KEY_BREAK;CHECK(inspect_raw_forward(NULL,TRUE));
  fake_raw.header.dwType=RIM_TYPEMOUSE;CHECK(!inspect_raw_forward(NULL,TRUE));CHECK(inspect_raw_forward(NULL,FALSE));
  fake_raw_error=TRUE;CHECK(!inspect_raw_forward(NULL,TRUE));fake_raw_error=FALSE;
  key.hwnd=window;key.wParam=VK_ESCAPE;CHECK(!inspect_filter_message(&key));
  CHECK(g_input_mode&&!g_input_cancel);
  key.message=WM_KEYUP;CHECK(!inspect_filter_message(&key));
  key.message=WM_CHAR;key.wParam=27;CHECK(!inspect_filter_message(&key));
  g_input_mode=0;CHECK(!inspect_filter_message(&mouse));
  g_input_mode=1;g_input_cancel=0;
  fake_raw.header.dwType=RIM_TYPEKEYBOARD;fake_raw.data.keyboard.VKey=VK_ESCAPE;fake_raw.data.keyboard.Flags=0;
  CHECK(inspect_raw_forward(NULL,TRUE));CHECK(g_input_mode&&!g_input_cancel);
  fake_raw.data.keyboard.Flags=RI_KEY_BREAK;CHECK(inspect_raw_forward(NULL,FALSE));
  /* Install actual thread-local hooks on a hidden synthetic window. Verify
   * no WndProc replacement, queued selection clicks consumed, camera clicks
   * restored off-mode, and all hook handles removed. No physical input. */
  WNDCLASSW wc={.lpfnWndProc=fixture_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"InspectInputFixture"};
  CHECK(RegisterClassW(&wc));
  window=CreateWindowW(wc.lpszClassName,L"Fixture",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,wc.hInstance,NULL);CHECK(window);
  WNDPROC original=(WNDPROC)GetWindowLongPtrW(window,GWLP_WNDPROC);
  CHECK(inspect_install_window(window));CHECK((WNDPROC)GetWindowLongPtrW(window,GWLP_WNDPROC)==original);
  g_input_mode=1;CHECK(PostMessageW(window,WM_LBUTTONDOWN,0,0));
  unsigned before_cursor=cursor_calls;
  SendMessageW(window,WM_SETCURSOR,(WPARAM)window,MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
  CHECK(cursor_calls>before_cursor);
  MSG message;CHECK(PeekMessageW(&message,window,WM_LBUTTONDOWN,WM_LBUTTONDOWN,PM_REMOVE));CHECK(message.message==WM_NULL);
  CHECK(PostMessageW(window,WM_KEYDOWN,'W',0));
  CHECK(PeekMessageW(&message,window,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE));
  CHECK(message.message==WM_KEYDOWN&&message.wParam=='W');
  CHECK(PostMessageW(window,WM_KEYDOWN,VK_UP,0));
  CHECK(PeekMessageW(&message,window,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE));
  CHECK(message.message==WM_KEYDOWN&&message.wParam==VK_UP);
  g_input_mode=0;CHECK(PostMessageW(window,WM_LBUTTONDOWN,0,0));
  CHECK(PeekMessageW(&message,window,WM_LBUTTONDOWN,WM_LBUTTONDOWN,PM_REMOVE));CHECK(message.message==WM_LBUTTONDOWN);
  update_inspect_input(FALSE);CHECK(!g_input_messages&&!g_input_cursor);
  CHECK((WNDPROC)GetWindowLongPtrW(window,GWLP_WNDPROC)==original);
  DestroyWindow(window);g_input_window=NULL;
}
int main(void) {
  caller_preservation();routing();
  puts("PASS: exact-callsite foreground suppression, original caller preservation, rebound keys/Escape/backtick unchanged, Ctrl+I/no repeat/character suppression, mouse/raw routing, thread-local hooks and WndProc preservation");
  return 0;
}
