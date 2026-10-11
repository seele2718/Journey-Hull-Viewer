/* Synthetic large image. Its populated game-like bytes are NEVER executed. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/protocol.h"
static volatile char padding[0x4060000];
static BOOL readable_range(const void *p, SIZE_T n) {
  MEMORY_BASIC_INFORMATION info;
  return p && VirtualQuery(p, &info, sizeof(info)) == sizeof(info) &&
      info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
      (uintptr_t)p + n <= (uintptr_t)info.BaseAddress + info.RegionSize;
}
static BOOL bytes_equal(const BYTE *a, const BYTE *b, SIZE_T n) { return !memcmp(a,b,n); }
#include "image_guards.h"
int main(int argc, char **argv) {
  padding[sizeof(padding)-1] = 1;
  BYTE *image = (BYTE *)GetModuleHandleW(NULL);
  for (unsigned i = 0; i < sizeof(k_image_guards)/sizeof(*k_image_guards); ++i) {
    const image_guard *g = k_image_guards + i;
    DWORD old;
    if (!VirtualProtect(image + g->rva, g->size, PAGE_EXECUTE_READWRITE, &old)) return 2;
    memcpy(image + g->rva, g->bytes, g->size);
  }
  if (!verify_image_guards(image)) return 3;
  /* This fixture never executes Lua. Camera entry guards deliberately absent. */
  if (verify_camera_guards(image)) return 3;
  if (argc > 1 && !strcmp(argv[1], "--conflict")) image[0x2790c0] ^= 1;
  wchar_t name[96];
  hv_object_name(name, 96, L"FixtureExit", GetCurrentProcessId());
  HANDLE exit_event = CreateEventW(NULL, TRUE, FALSE, name);
  hv_object_name(name, 96, L"FixtureReady", GetCurrentProcessId());
  HANDLE ready = CreateEventW(NULL, TRUE, TRUE, name);
  WaitForSingleObject(exit_event, 30000);
  CloseHandle(ready); CloseHandle(exit_event);
  return 0;
}
