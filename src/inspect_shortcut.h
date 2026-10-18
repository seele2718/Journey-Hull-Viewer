#ifndef HV_INSPECT_SHORTCUT_H
#define HV_INSPECT_SHORTCUT_H
#define HV_INSPECT_KEY 'I'
#define HV_INSPECT_SHORTCUT_LABEL L"Ctrl+I"
/* Queue-relative modifier state, shared by the game message hook and panel. */
static inline BOOL hv_inspect_modifiers(void) {
  return (GetKeyState(VK_CONTROL)&0x8000)&&!(GetKeyState(VK_MENU)&0x8000);
}
#endif
