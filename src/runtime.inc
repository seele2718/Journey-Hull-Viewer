/* Included by renderer.c so game-thread filtering has no public mutable API. */
#include "camera.h"
static HANDLE g_mapping, g_mutex;
static hv_shared *g_shared;
static volatile LONG g_started, g_ready;
static uint32_t g_revision;
static uint32_t g_sample_revision;
static uint64_t g_heartbeat;

static void process_camera(void *game) {
  static uint32_t completed, saved_bits;
  static BOOL saved_ok;
  static char saved_message[128];
  if (!g_camera_available) return;
  DWORD lock = WaitForSingleObject(g_mutex, 0);
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return;
  uint32_t request = g_shared->camera_request;
  if (request == completed) {
    if (g_shared->camera_completed != completed) {
      g_shared->camera_completed = completed;
      g_shared->camera_ok = saved_ok;
      g_shared->camera_bits = saved_bits;
      memcpy(g_shared->camera_message, saved_message, sizeof(saved_message));
    }
    ReleaseMutex(g_mutex);
    return;
  }
  uint32_t action = g_shared->camera_action;
  BOOL pending = request != g_shared->camera_completed;
  BOOL live = GetTickCount64() - g_shared->heartbeat <= HV_LEASE_MS;
  ReleaseMutex(g_mutex);
  if (!pending) return;
  hv_lua *state = NULL;
  uint32_t bits = 0;
  char message[128] = {0};
  if (game && readable_range((BYTE *)game + 0x20, sizeof(state)))
    memcpy(&state, (BYTE *)game + 0x20, sizeof(state));
  if (!readable_range(state, 0x40)) state = NULL;
  BOOL ok = FALSE;
  if (!live) lstrcpyA(message, "Camera request expired.");
  else ok = execute_camera(state, action, &bits, message);
  /* Never execute again just because the panel owns the mutex at completion. */
  completed = request; saved_bits = bits; saved_ok = ok;
  memcpy(saved_message, message, sizeof(message));
  lock = WaitForSingleObject(g_mutex, 0);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    g_shared->camera_completed = completed;
    g_shared->camera_ok = saved_ok;
    g_shared->camera_bits = saved_bits;
    memcpy(g_shared->camera_message, saved_message, sizeof(saved_message));
    ReleaseMutex(g_mutex);
  }
}

static void read_panel(void) {
  DWORD lock = WaitForSingleObject(g_mutex, 0);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    if (g_shared->magic == HV_MAGIC && g_shared->protocol == HV_PROTOCOL &&
        g_shared->size == sizeof(*g_shared)) {
      g_heartbeat = g_shared->heartbeat;
      if (g_revision != g_shared->revision && hv_config_valid(&g_shared->config)) {
        const hv_config *c = &g_shared->config;
        g_hull_filter_mode = c->mode == HV_ALL ? k_hull_filter_all :
            c->mode == HV_TYPES ? k_hull_filter_types :
            c->mode == HV_SELECTED ? k_hull_filter_selected : k_hull_filter_none;
        g_hull_filter_include_mask = c->include_mask;
        g_hull_filter_exclude_mask = c->exclude_mask;
        g_hull_selection_count = c->count;
        g_include_inactive = c->include_inactive && c->mode == HV_SELECTED;
        for (unsigned i = 0; i < c->count; ++i) {
          g_hull_selections[i].length = strlen(c->names[i]);
          memcpy(g_hull_selections[i].text, c->names[i], HV_NAME_SIZE);
        }
        g_revision = g_shared->revision;
        commit_hull_filter("panel");
      }
      g_shared->applied_revision = g_sample_revision;
      g_shared->active_matches = g_active_matches;
      g_shared->inactive_matches = g_inactive_matches;
      g_shared->unreadable_names = g_unreadable_names;
      LONG index = InterlockedCompareExchange(&g_hull_published_index, 0, 0);
      g_shared->drawn_hulls = InterlockedCompareExchange(&g_hull_snapshot_ready, 0, 0)
          ? g_hull_snapshots[index].hull_count : 0;
    }
    ReleaseMutex(g_mutex);
  }
  if (GetTickCount64() - g_heartbeat > HV_LEASE_MS) {
    g_hull_filter_mode = k_hull_filter_none;
    g_revision = 0; /* Reapply the panel's current config on reconnect. */
    InterlockedExchange(&g_hull_snapshot_ready, 0);
  }
}

/* The guarded wrapper's sole input is App*. Its tail passes App+0x4d8 to
 * GameUpdate. Capture after the original returns, on the same game thread. */
static uint64_t app_update_hook(void *app) {
  uint64_t result = g_app_update(app);
  if (InterlockedCompareExchange(&g_ready, 0, 0)) {
    read_panel();
    process_camera(app ? (BYTE *)app + 0x4d8 : NULL);
    publish_all_hulls(app ? (BYTE *)app + 0x4d8 : NULL);
    g_sample_revision = g_revision;
  }
  return result;
}

static void init_status(const char *message, BOOL ready) {
  log_text(message);
  DWORD lock = WaitForSingleObject(g_mutex, 1000);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    lstrcpynA(g_shared->status, message, sizeof(g_shared->status));
    g_shared->ready = ready;
    ReleaseMutex(g_mutex);
  }
}

__declspec(dllexport) DWORD WINAPI HullViewerStart(void *unused) {
  (void)unused;
  if (InterlockedCompareExchange(&g_started, 1, 0))
    return InterlockedCompareExchange(&g_ready, 0, 0) ? 0 : 1;
  wchar_t name[96];
  hv_object_name(name, 96, L"Lock", GetCurrentProcessId());
  g_mutex = CreateMutexW(NULL, FALSE, name);
  hv_object_name(name, 96, L"Data", GetCurrentProcessId());
  g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                 0, sizeof(hv_shared), name);
  if (!g_mutex || !g_mapping) return 2;
  g_shared = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hv_shared));
  if (!g_shared) return 2;
  DWORD lock = WaitForSingleObject(g_mutex, 1000);
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return 2;
  memset(g_shared, 0, sizeof(*g_shared));
  g_shared->magic = HV_MAGIC;
  g_shared->protocol = HV_PROTOCOL;
  g_shared->size = sizeof(*g_shared);
  lstrcpynA(g_shared->version, HV_VERSION, sizeof(g_shared->version));
  ReleaseMutex(g_mutex);

  BYTE *image = (BYTE *)GetModuleHandleW(NULL);
  if (!verify_image_guards(image)) {
    init_status("Unsupported game code or conflicting hooks. No hooks installed.", FALSE);
    return 3;
  }
  init_camera_api(image);
  lock = WaitForSingleObject(g_mutex, 1000);
  if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) {
    g_shared->camera_available = g_camera_available;
    ReleaseMutex(g_mutex);
  }
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK) {
    init_status("Hook library initialization failed. Restart Journey to retry.", FALSE);
    return 4;
  }
  status = MH_CreateHook(image + 0xf4880, app_update_hook, (void **)&g_app_update);
  if (status == MH_OK)
    status = MH_CreateHook(image + 0x2790c0, frame_pass_execute_hook,
                          (void **)&g_frame_pass_execute);
  if (status != MH_OK) {
    MH_Uninitialize();
    init_status("Could not prepare hooks. No hooks installed.", FALSE);
    return 4;
  }
  /* Both original pointers are ready before enabling either callback. MinHook
   * suspends other threads and relocates instruction pointers during patching. */
  if (MH_QueueEnableHook(image + 0xf4880) != MH_OK ||
      MH_QueueEnableHook(image + 0x2790c0) != MH_OK ||
      MH_ApplyQueued() != MH_OK) {
    /* Keep any partial hooks/trampolines resident and inert. Do not free code
     * another thread may already be executing. The game can continue normally. */
    init_status("Hook installation failed. Restart Journey before retrying.", FALSE);
    return 4;
  }
  InterlockedExchange(&g_ready, 1);
  init_status("Attached. Hulls hidden until you choose a view.", TRUE);
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
  return TRUE;
}
