#include <stdio.h>
#include <stdlib.h>
#include "../src/renderer.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned hull_count(void) {
  unsigned index;
  hull_render_snapshot *s = lock_hull_snapshot(&index);
  if (!s) return 0;
  unsigned n = s->hull_count;
  InterlockedDecrement(&g_hull_snapshot_readers[index]);
  return n;
}
static void guards(void) {
  CHECK(!verify_image_guards(NULL));
  CHECK(!verify_image_guards((BYTE *)GetModuleHandleW(NULL)));
  BYTE *image = VirtualAlloc(NULL, 0x4050000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  CHECK(image);
  IMAGE_DOS_HEADER *dos = (void *)image;
  dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x100;
  IMAGE_NT_HEADERS64 *nt = (void *)(image + 0x100);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage = 0x4050000;
  for (unsigned i = 0; i < sizeof(k_image_guards)/sizeof(*k_image_guards); ++i)
    memcpy(image + k_image_guards[i].rva, k_image_guards[i].bytes, k_image_guards[i].size);
  CHECK(verify_image_guards(image));
  CHECK(!verify_camera_guards(image));
  for (unsigned i = 0; i < sizeof(k_camera_guards)/sizeof(*k_camera_guards); ++i)
    memcpy(image + k_camera_guards[i].rva, k_camera_guards[i].bytes, k_camera_guards[i].size);
  CHECK(verify_camera_guards(image));
  for (unsigned i = 0; i < sizeof(k_camera_guards)/sizeof(*k_camera_guards); ++i) {
    image[k_camera_guards[i].rva] ^= 1;
    CHECK(!verify_camera_guards(image));
    CHECK(verify_image_guards(image)); /* Camera mismatch does not block drawing. */
    image[k_camera_guards[i].rva] ^= 1;
  }
  image[0x500] = 123; CHECK(verify_image_guards(image));
  for (unsigned i = 0; i < sizeof(k_image_guards)/sizeof(*k_image_guards); ++i) {
    image[k_image_guards[i].rva] ^= 1;
    CHECK(!verify_image_guards(image));
    image[k_image_guards[i].rva] ^= 1;
  }
  nt->OptionalHeader.SizeOfImage = 0x1000;
  CHECK(!verify_image_guards(image));
  nt->OptionalHeader.SizeOfImage = 0x4050000;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
  CHECK(!verify_image_guards(image));
  VirtualFree(image, 0, MEM_RELEASE);
}
static int lua_top = 5, lua_calls, lua_load_error, lua_call_error, lua_stack_ok = 1;
static const char *lua_result = "1001";
static char lua_source[1024];
static int mock_top(hv_lua *s) { (void)s; return lua_top; }
static void mock_settop(hv_lua *s, int n) { (void)s; lua_top = n; }
static int mock_stack(hv_lua *s, int n) { (void)s; CHECK(n == 8); return lua_stack_ok; }
static int mock_type(hv_lua *s, int n) { (void)s; CHECK(n == -1); return 4; }
static const char *mock_string(hv_lua *s, int n, size_t *size) {
  (void)s; CHECK(n == -1); *size = strlen(lua_result); return lua_result;
}
static int mock_load(hv_lua *s, const char *text, size_t size, const char *name, const char *mode) {
  (void)s; CHECK(size < sizeof(lua_source)); CHECK(name && !strcmp(mode, "t"));
  memcpy(lua_source, text, size + 1); ++lua_top; return lua_load_error;
}
static int mock_call(hv_lua *s, int nargs, int nresults, int err, intptr_t ctx, hv_continuation cont) {
  (void)s; CHECK(!nargs && nresults == 1 && !err && !ctx && !cont); ++lua_calls; return lua_call_error;
}
static void camera_commands(void) {
  g_camera_lua = (hv_lua_api){mock_top, mock_settop, mock_stack, mock_type, mock_string, mock_load, mock_call};
  BYTE state[0x40] = {0}, game[0x100] = {0};
  uint32_t bits; char message[128];
  for (uint32_t action = 0; action < HV_CAM_COUNT; ++action) {
    CHECK(execute_camera((hv_lua *)state, action, &bits, message));
    CHECK(bits == 9 && lua_top == 5);
    CHECK(strstr(lua_source, camera_action_source(action)));
  }
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_COUNT, &bits, message));
  CHECK(!execute_camera(NULL, HV_CAM_START, &bits, message));
  int calls = lua_calls;
  lua_load_error = 1; lua_result = "compile error";
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_START, &bits, message));
  CHECK(lua_calls == calls && lua_top == 5 && !strcmp(message, "compile error"));
  lua_load_error = 0; lua_call_error = 1; lua_result = "runtime error";
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_START, &bits, message));
  CHECK(lua_top == 5 && !strcmp(message, "runtime error"));
  lua_call_error = 0; lua_stack_ok = 0;
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_START, &bits, message));
  CHECK(lua_top == 5); lua_stack_ok = 1; lua_result = "malformed";
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_START, &bits, message));
  lua_result = "1001";
  hv_shared shared = {0};
  g_shared = &shared; g_mutex = CreateMutexW(NULL, FALSE, NULL); CHECK(g_mutex);
  g_camera_available = TRUE;
  *(void **)(game + 0x20) = state;
  shared.heartbeat = GetTickCount64(); shared.camera_request = 1; shared.camera_action = HV_CAM_IGNORE;
  calls = lua_calls;
  process_camera(game); process_camera(game);
  CHECK(lua_calls == calls + 1 && shared.camera_completed == 1 && shared.camera_ok);
  CHECK(strstr(lua_source, "Vars.DebugCamera.collideTerrain(false)") &&
        strstr(lua_source, "Vars.CameraSystem.defCollideTerrain(false)"));
  shared.camera_completed = 0; /* Simulate delayed result publication: no replay. */
  process_camera(game); CHECK(lua_calls == calls + 1 && shared.camera_completed == 1);
  shared.camera_request = 2; shared.heartbeat = GetTickCount64() - HV_LEASE_MS - 1;
  process_camera(game); CHECK(!shared.camera_ok && shared.camera_completed == 2);
  CHECK(lua_calls == calls + 1 && strstr(shared.camera_message, "expired"));
  CloseHandle(g_mutex); g_mutex = NULL; g_shared = NULL; g_camera_available = FALSE;
}
static void protocol_and_hulls(void) {
  hv_config c = {.mode = HV_SELECTED};
  CHECK(hv_parse_names("a,b; c\r\nd", &c) && c.count == 4);
  CHECK(hv_parse_names("\"8dc1645e2ed9ee33aa75297e01b94d0c\"", &c));
  CHECK(c.count == 1 && !strcmp(c.names[0], "8dc1645e2ed9ee33aa75297e01b94d0c"));
  char ascii[64];
  CHECK(hv_ascii_names(L"8dc1645e2ed9ee33aa75297e01b94d0c", ascii, sizeof(ascii)));
  CHECK(!strcmp(ascii, c.names[0]));
  CHECK(!hv_ascii_names(L"a\x1234", ascii, sizeof(ascii)));
  CHECK(!hv_ascii_names(L"long", ascii, 4));
  CHECK(!hv_parse_names("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", &c));
  char many[200] = {0};
  for (int i = 0; i < 65; ++i) strcat(many, "a ");
  CHECK(!hv_parse_names(many, &c));
  c.count = 0; c.include_mask = 64; CHECK(!hv_config_valid(&c));
  c.include_mask = 0; c.mode = 999; CHECK(!hv_config_valid(&c));

  BYTE game[0x200] = {0}, barn[0x10] = {0}, hull[0xd0] = {0};
  BYTE edges[] = {0, 1}; float points[2][4] = {{1,2,3,1},{4,5,6,1}};
  *(void **)(game + 0x110) = barn; *(void **)(barn + 8) = hull;
  *(void **)(hull + 0x60) = edges; *(void **)(hull + 0x68) = points;
  *(uint32_t *)(hull + 0x78) = 1; *(uint32_t *)(hull + 0x7c) = 2;
  *(uint32_t *)(hull + 0x80) = 1; *(uint32_t *)(hull + 0x84) = 0x10;
  strcpy((char *)hull + 0xa0, "example");
  hv_shared shared = {.magic = HV_MAGIC, .protocol = HV_PROTOCOL, .size = sizeof(hv_shared)};
  g_shared = &shared; g_mutex = CreateMutexW(NULL, FALSE, NULL); CHECK(g_mutex);
  shared.heartbeat = GetTickCount64(); shared.revision = 1;
  shared.config.mode = HV_ALL;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  shared.config.mode = HV_TYPES; shared.config.include_mask = 2; ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  shared.config.include_mask = 0; shared.config.exclude_mask = 2; ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  shared.config.exclude_mask = 16; ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  shared.config.mode = HV_SELECTED; shared.config.exclude_mask = 0;
  CHECK(hv_parse_names("example", &shared.config)); ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  CHECK(g_active_matches == 1 && g_inactive_matches == 0);
  *(uint32_t *)(hull + 0x80) = 0;
  publish_all_hulls(game); CHECK(hull_count() == 0);
  CHECK(g_active_matches == 0 && g_inactive_matches == 1);
  shared.config.include_inactive = 1; ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  CHECK(*(uint32_t *)(hull + 0x80) == 0); /* Never enable the gameplay hull. */
  CHECK(hv_parse_names("missing", &shared.config)); ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  CHECK(g_active_matches == 0 && g_inactive_matches == 0);
  CHECK(hv_parse_names("example", &shared.config)); ++shared.revision;
  read_panel();
  *(uint32_t *)(hull + 0x80) = 1; edges[0] = 255;
  publish_all_hulls(game); CHECK(hull_count() == 0);
  edges[0] = 0; publish_all_hulls(game); CHECK(hull_count() == 1);
  unsigned next = 1u - (unsigned)g_hull_published_index;
  g_hull_snapshot_readers[next] = 1;
  LONG previous = g_hull_published_index;
  publish_all_hulls(game); CHECK(g_hull_published_index == previous);
  g_hull_snapshot_readers[next] = 0;
  shared.heartbeat = GetTickCount64() - HV_LEASE_MS - 1;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  shared.heartbeat = GetTickCount64();
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  shared.config.mode = HV_HIDDEN; ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  publish_all_hulls(NULL); CHECK(hull_count() == 0);
  CloseHandle(g_mutex); g_shared = NULL; g_mutex = NULL;
  BYTE camera[0x1b0] = {0}; camera_snapshot snapshot;
  *(float *)(camera + 0xc8) = .1f; *(float *)(camera + 0xcc) = 1000;
  *(float *)(camera + 0xd0) = 1.7f; *(float *)(camera + 0x154) = 70;
  CHECK(capture_pass_camera(camera, &snapshot));
  *(float *)(camera + 0xc8) = 0; CHECK(!capture_pass_camera(camera, &snapshot));
}
typedef int (*integer_fn)(void);
static integer_fn original_integer;
static volatile LONG worker_stop, worker_error;
static int integer_hook(void) { return original_integer() + 1; }
static DWORD WINAPI call_worker(void *code) {
  integer_fn fn = (integer_fn)code;
  while (!InterlockedCompareExchange(&worker_stop, 0, 0)) {
    int v = fn();
    if (v != 7 && v != 8) InterlockedExchange(&worker_error, 1);
  }
  return 0;
}
static void late_hook(void) {
  /* Synthetic function only. Concurrent calls exercise relocation of an
   * in-flight prologue while MinHook enables the new entry point. */
  const BYTE body[] = {0x40,0x53,0x48,0x83,0xec,0x20,0xb8,7,0,0,0,
                       0x48,0x83,0xc4,0x20,0x5b,0xc3};
  BYTE *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  CHECK(code); memcpy(code, body, sizeof(body));
  FlushInstructionCache(GetCurrentProcess(), code, sizeof(body));
  CHECK(MH_Initialize() == MH_OK);
  for (unsigned round = 0; round < 20; ++round) {
    worker_stop = worker_error = 0;
    CHECK(MH_CreateHook(code, integer_hook, (void **)&original_integer) == MH_OK);
    HANDLE thread = CreateThread(NULL, 0, call_worker, code, 0, NULL); CHECK(thread);
    Sleep(2);
    CHECK(MH_QueueEnableHook(code) == MH_OK && MH_ApplyQueued() == MH_OK);
    CHECK(((integer_fn)code)() == 8);
    Sleep(2);
    InterlockedExchange(&worker_stop, 1);
    CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0); CloseHandle(thread);
    CHECK(!worker_error);
    CHECK(MH_DisableHook(code) == MH_OK && MH_RemoveHook(code) == MH_OK);
    CHECK(((integer_fn)code)() == 7);
  }
  CHECK(MH_Uninitialize() == MH_OK);
  VirtualFree(code, 0, MEM_RELEASE);
}
int main(void) {
  guards(); protocol_and_hulls(); camera_commands(); late_hook();
  puts("PASS: guards, parser bounds, filtering, snapshots, active flags, camera actions/errors/stack/once-only, panel lease and 20 concurrent late-hook rounds");
  return 0;
}
