#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"
#include "version.h"
#include "MinHook.h"

enum { k_absolute_jump_size = 14 };
typedef uint64_t (*draw_primitive_uniform_color_fn)(
    void *render_device, uint32_t primitive_type, const float *positions,
    const float *color, const uint32_t *indices, uint32_t vertex_count,
    const float *transform);
typedef uint64_t (*prepare_render_state_fn)(void *immediate_renderer,
                                            void *render_device,
                                            const void *state_source);
typedef float *(*matrix_inverse_fn)(float *result, const float *matrix);
typedef float *(*matrix_multiply_fn)(const float *left, float *result,
                                     const float *right);
typedef uint64_t (*app_update_fn)(void *app);
typedef void (*frame_pass_execute_fn)(void *pass, void *render_context,
                                      void *pass_context);
typedef struct __attribute__((aligned(16))) camera_snapshot {
  float world[16];
  float fov;
  float aspect;
  float near_plane;
  float far_plane;
  float screencrop;
} camera_snapshot;

enum {
  k_hull_color_count = 6,
  // Match the PS3 DebugLines global vertex budget.
  k_hull_vertex_capacity = 0x20000,
  k_hull_vertices_per_color = k_hull_vertex_capacity,
};

typedef struct hull_render_snapshot {
  uint32_t hull_count;
  uint32_t counts[k_hull_color_count];
  float positions[k_hull_color_count][k_hull_vertices_per_color][3];
} hull_render_snapshot;

enum {
  k_hull_filter_all = 0,
  k_hull_filter_types = 1,
  k_hull_filter_selected = 2,
  k_hull_filter_none = 3,
  k_hull_selection_capacity = 64,
  k_hull_name_capacity = 48,
};

typedef struct hull_name_selection {
  SIZE_T length;
  char text[k_hull_name_capacity];
} hull_name_selection;

static app_update_fn g_app_update;
static frame_pass_execute_fn g_frame_pass_execute;
static hull_render_snapshot g_hull_snapshots[2];
static volatile LONG g_hull_snapshot_readers[2];
static volatile LONG g_hull_published_index;
static volatile LONG g_hull_snapshot_ready;
static volatile LONG g_hull_snapshot_logged;
static volatile LONG g_hull_draw_logged;
static volatile LONG g_hull_filter_mode = k_hull_filter_none;
static volatile LONG g_hull_filter_revision;
static volatile LONG g_hull_filter_logged_revision = -1;
static uint32_t g_hull_filter_include_mask;
static uint32_t g_hull_filter_exclude_mask;
static hull_name_selection g_hull_selections[k_hull_selection_capacity];
static unsigned g_hull_selection_count;
static BOOL g_include_inactive;
/* Written only by the game-thread producer, never the render callback. */
static uint32_t g_active_matches, g_inactive_matches, g_unreadable_names;
static float g_hull_clip_positions[k_hull_vertices_per_color][4];
typedef struct log_buffer {
  char data[768];
  SIZE_T length;
} log_buffer;

static void buffer_text(log_buffer *buffer, const char *text) {
  while (*text != '\0' && buffer->length + 1 < sizeof(buffer->data)) {
    buffer->data[buffer->length++] = *text++;
  }
}

static void buffer_u64(log_buffer *buffer, uint64_t value) {
  char reversed[20];
  unsigned count = 0;
  do {
    reversed[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0 && count < sizeof(reversed));
  while (count > 0 && buffer->length + 1 < sizeof(buffer->data)) {
    buffer->data[buffer->length++] = reversed[--count];
  }
}

static void buffer_hex(log_buffer *buffer, uintptr_t value) {
  static const char digits[] = "0123456789abcdef";
  char reversed[sizeof(uintptr_t) * 2];
  unsigned count = 0;
  buffer_text(buffer, "0x");
  do {
    reversed[count++] = digits[value & 0xf];
    value >>= 4;
  } while (value != 0 && count < sizeof(reversed));
  while (count > 0 && buffer->length + 1 < sizeof(buffer->data)) {
    buffer->data[buffer->length++] = reversed[--count];
  }
}

static void write_log(log_buffer *buffer) {
  buffer->data[buffer->length] = '\0';
  OutputDebugStringA(buffer->data);
}
static void log_text(const char *text) {
  log_buffer buffer = {{0}, 0};
  buffer_text(&buffer, text);
  write_log(&buffer);
}

static BOOL bytes_equal(const BYTE *left, const BYTE *right, SIZE_T size) {
  for (SIZE_T i = 0; i < size; ++i) {
    if (left[i] != right[i]) {
      return FALSE;
    }
  }
  return TRUE;
}

static void copy_bytes(BYTE *destination, const BYTE *source, SIZE_T size) {
  for (SIZE_T i = 0; i < size; ++i) {
    destination[i] = source[i];
  }
}

static char ascii_lower(char value) {
  if (value >= 'A' && value <= 'Z') {
    return (char)(value + ('a' - 'A'));
  }
  return value;
}

static BOOL text_equal(const char *left, const char *right,
                       BOOL ignore_case) {
  while (*left != '\0' && *right != '\0') {
    char left_value = ignore_case ? ascii_lower(*left) : *left;
    char right_value = ignore_case ? ascii_lower(*right) : *right;
    if (left_value != right_value) {
      return FALSE;
    }
    ++left;
    ++right;
  }
  return *left == *right;
}

static BOOL float_is_finite(float value) {
  union {
    float value;
    uint32_t bits;
  } converted = {value};
  return (converted.bits & 0x7f800000U) != 0x7f800000U;
}

static BOOL readable_range(const void *address, SIZE_T size) {
  if (address == NULL || size == 0) {
    return FALSE;
  }
  uintptr_t start = (uintptr_t)address;
  uintptr_t end = start + size;
  if (end < start) {
    return FALSE;
  }
  uintptr_t cursor = start;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION information;
    if (VirtualQuery((const void *)cursor, &information,
                     sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return FALSE;
    }
    uintptr_t region_start = (uintptr_t)information.BaseAddress;
    uintptr_t region_end = region_start + information.RegionSize;
    if (region_end <= cursor) {
      return FALSE;
    }
    cursor = region_end < end ? region_end : end;
  }
  return TRUE;
}

static const char *hull_filter_mode_name(LONG mode) {
  switch (mode) {
    case k_hull_filter_all:
      return "all";
    case k_hull_filter_types:
      return "types";
    case k_hull_filter_selected:
      return "selected";
    case k_hull_filter_none:
      return "none";
    default:
      return "invalid";
  }
}

static void log_hull_filter_status(const char *reason) {
  log_buffer buffer = {{0}, 0};
  buffer_text(&buffer, "hull filter ");
  buffer_text(&buffer, reason);
  buffer_text(&buffer, ": mode=");
  buffer_text(&buffer, hull_filter_mode_name(g_hull_filter_mode));
  buffer_text(&buffer, " include=");
  buffer_hex(&buffer, g_hull_filter_include_mask);
  buffer_text(&buffer, " exclude=");
  buffer_hex(&buffer, g_hull_filter_exclude_mask);
  buffer_text(&buffer, " selectedNames=");
  buffer_u64(&buffer, g_hull_selection_count);
  write_log(&buffer);
}

static void commit_hull_filter(const char *reason) {
  MemoryBarrier();
  InterlockedIncrement(&g_hull_filter_revision);
  log_hull_filter_status(reason);
}

static BOOL selected_hull_name_matches(const char *name, SIZE_T length) {
  for (unsigned selection = 0; selection < g_hull_selection_count;
       ++selection) {
    const hull_name_selection *candidate = &g_hull_selections[selection];
    if (candidate->length == length &&
        bytes_equal((const BYTE *)candidate->text, (const BYTE *)name,
                    length)) {
      return TRUE;
    }
  }
  return FALSE;
}

static const char *physical_hull_name(BYTE *hull, SIZE_T *length) {
  // PC PhysicalHull::GetName is just `lea rax,[rcx+0xa0]; ret`: this is an
  // inline C string, not an MSVC std::string object.
  const char *text = (const char *)(hull + 0xa0);
  if (!readable_range(text, 0x30)) {
    return NULL;
  }
  for (SIZE_T index = 0; index < 0x30; ++index) {
    unsigned char character = (unsigned char)text[index];
    if (character == 0) {
      if (index == 0) {
        return NULL;
      }
      *length = index;
      return text;
    }
    if (character < 0x20 || character > 0x7e) {
      return NULL;
    }
  }
  return NULL;
}

static unsigned hull_color_index(uint32_t type_mask) {
  // Match the PS3 debug palette and its observed type priority.
  if ((type_mask & 0x08) != 0) return 0;  // Camera: magenta.
  if ((type_mask & 0x02) != 0) return 1;  // Physical: green.
  if ((type_mask & 0x10) != 0) return 2;  // Region: sky blue.
  if ((type_mask & 0x01) != 0) return 3;  // Boundary: orange.
  if ((type_mask & 0x04) != 0) return 3;  // Wind: orange.
  if ((type_mask & 0x20) != 0) return 4;  // Internal/unknown: red-orange.
  return 5;                               // Unclassified: white.
}

static void publish_all_hulls(void *game_root) {
  g_active_matches = g_inactive_matches = g_unreadable_names = 0;
  if (g_hull_filter_mode == k_hull_filter_none ||
      (g_hull_filter_mode == k_hull_filter_selected && g_hull_selection_count == 0) ||
      game_root == NULL ||
      !readable_range((BYTE *)game_root + 0x110, sizeof(void *))) {
    InterlockedExchange(&g_hull_snapshot_ready, 0);
    return;
  }
  BYTE *hull_barn = *(BYTE **)((BYTE *)game_root + 0x110);
  if (hull_barn == NULL || !readable_range(hull_barn + 8, sizeof(void *))) {
    InterlockedExchange(&g_hull_snapshot_ready, 0);
    return;
  }

  LONG published = InterlockedCompareExchange(&g_hull_published_index, 0, 0);
  unsigned target_index = published == 0 ? 1 : 0;
  if (InterlockedCompareExchange(&g_hull_snapshot_readers[target_index], 1,
                                 0) != 0) {
    return;
  }

  hull_render_snapshot *snapshot = &g_hull_snapshots[target_index];
  snapshot->hull_count = 0;
  for (unsigned color = 0; color < k_hull_color_count; ++color) {
    snapshot->counts[color] = 0;
  }

  BYTE *hull = *(BYTE **)(hull_barn + 8);
  unsigned walked = 0;
  unsigned active_seen = 0;
  unsigned filter_matches = 0;
  uint32_t total_vertices = 0;
  BOOL capacity_reached = FALSE;
  while (hull != NULL && walked < 10000 && !capacity_reached) {
    ++walked;
    if (!readable_range(hull, 0xd0)) {
      break;
    }
    BYTE *next = *(BYTE **)(hull + 0x98);
    uint32_t is_active = *(uint32_t *)(hull + 0x80);
    BOOL selected_match = FALSE;
    if (g_hull_filter_mode == k_hull_filter_selected) {
      SIZE_T name_length = 0;
      const char *name = physical_hull_name(hull, &name_length);
      if (!name) ++g_unreadable_names;
      selected_match = name && selected_hull_name_matches(name, name_length);
      if (selected_match) {
        if (is_active) ++g_active_matches;
        else ++g_inactive_matches;
      }
    }
    if (is_active == 0 && !(g_include_inactive && selected_match)) {
      hull = next;
      continue;
    }
    ++active_seen;

    uint32_t edge_count = *(uint32_t *)(hull + 0x78);
    uint32_t vertex_count = *(uint32_t *)(hull + 0x7c);
    uint32_t type_mask = *(uint32_t *)(hull + 0x84);
    LONG filter_mode = g_hull_filter_mode;
    BOOL filter_match = filter_mode == k_hull_filter_all;
    if (filter_mode == k_hull_filter_types) {
      uint32_t include_mask = g_hull_filter_include_mask;
      uint32_t exclude_mask = g_hull_filter_exclude_mask;
      filter_match =
          (include_mask == 0 || (type_mask & include_mask) != 0) &&
          (type_mask & exclude_mask) == 0;
    } else if (filter_mode == k_hull_filter_selected) {
      filter_match = selected_match;
    }
    if (!filter_match) {
      hull = next;
      continue;
    }
    ++filter_matches;

    BYTE *edges = *(BYTE **)(hull + 0x60);
    const float *vertices = *(const float **)(hull + 0x68);
    if (edge_count == 0 || edge_count > 100000 || vertex_count == 0 ||
        vertex_count > 256 ||
        !readable_range(edges, (SIZE_T)edge_count * 2) ||
        !readable_range(vertices, (SIZE_T)vertex_count * 16)) {
      hull = next;
      continue;
    }

    unsigned color = hull_color_index(type_mask);
    uint32_t before = snapshot->counts[color];
    for (uint32_t edge = 0; edge < edge_count; ++edge) {
      if (total_vertices + 2 > k_hull_vertex_capacity) {
        capacity_reached = TRUE;
        break;
      }
      unsigned first = edges[edge * 2];
      unsigned second = edges[edge * 2 + 1];
      if (first >= vertex_count || second >= vertex_count) {
        continue;
      }
      const float *endpoints[2] = {
          vertices + first * 4,
          vertices + second * 4,
      };
      BOOL endpoints_valid = TRUE;
      for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
        for (unsigned axis = 0; axis < 3; ++axis) {
          if (!float_is_finite(endpoints[endpoint][axis])) {
            endpoints_valid = FALSE;
          }
        }
      }
      if (!endpoints_valid) {
        continue;
      }
      for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
        float *destination =
            snapshot->positions[color][snapshot->counts[color]++];
        destination[0] = endpoints[endpoint][0];
        destination[1] = endpoints[endpoint][1];
        destination[2] = endpoints[endpoint][2];
      }
      total_vertices += 2;
    }
    if (snapshot->counts[color] != before) {
      ++snapshot->hull_count;
    }
    hull = next;
  }

  MemoryBarrier();
  InterlockedExchange(&g_hull_snapshot_readers[target_index], 0);
  InterlockedExchange(&g_hull_published_index, (LONG)target_index);
  InterlockedExchange(&g_hull_snapshot_ready, 1);

  LONG filter_revision =
      InterlockedCompareExchange(&g_hull_filter_revision, 0, 0);
  if (g_hull_filter_logged_revision != filter_revision) {
    g_hull_filter_logged_revision = filter_revision;
    log_buffer buffer = {{0}, 0};
    buffer_text(&buffer, "filtered hull snapshot: mode=");
    buffer_text(&buffer, hull_filter_mode_name(g_hull_filter_mode));
    buffer_text(&buffer, " activeSeen=");
    buffer_u64(&buffer, active_seen);
    buffer_text(&buffer, " filterMatches=");
    buffer_u64(&buffer, filter_matches);
    buffer_text(&buffer, " drawnHulls=");
    buffer_u64(&buffer, snapshot->hull_count);
    buffer_text(&buffer, " vertices=");
    buffer_u64(&buffer, total_vertices);
    write_log(&buffer);
  }

  if (snapshot->hull_count != 0 &&
      InterlockedCompareExchange(&g_hull_snapshot_logged, 1, 0) == 0) {
    log_buffer buffer = {{0}, 0};
    buffer_text(&buffer, "PS3-style hull snapshot published: hulls=");
    buffer_u64(&buffer, snapshot->hull_count);
    buffer_text(&buffer, " walked=");
    buffer_u64(&buffer, walked);
    buffer_text(&buffer, " activeSeen=");
    buffer_u64(&buffer, active_seen);
    buffer_text(&buffer, " vertices=");
    buffer_u64(&buffer, total_vertices);
    buffer_text(&buffer, " capacityReached=");
    buffer_u64(&buffer, capacity_reached);
    buffer_text(&buffer, " verticesByColor=");
    for (unsigned color = 0; color < k_hull_color_count; ++color) {
      if (color != 0) buffer_text(&buffer, ",");
      buffer_u64(&buffer, snapshot->counts[color]);
    }
    write_log(&buffer);
  }
}

static hull_render_snapshot *lock_hull_snapshot(unsigned *locked_index) {
  if (InterlockedCompareExchange(&g_hull_snapshot_ready, 0, 0) == 0) {
    return NULL;
  }
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    LONG index = InterlockedCompareExchange(&g_hull_published_index, 0, 0);
    if (index < 0 || index > 1) {
      return NULL;
    }
    if (InterlockedCompareExchange(&g_hull_snapshot_readers[index], 1, 0) != 0)
      continue;
    MemoryBarrier();
    if (InterlockedCompareExchange(&g_hull_snapshot_ready, 0, 0) != 0 &&
        InterlockedCompareExchange(&g_hull_published_index, 0, 0) == index) {
      *locked_index = (unsigned)index;
      return &g_hull_snapshots[index];
    }
    InterlockedDecrement(&g_hull_snapshot_readers[index]);
  }
  return NULL;
}

static BOOL capture_pass_camera(void *pass_context,
                                camera_snapshot *camera) {
  // FUN_1402793B0 receives CameraNode::native (+0x28) as its fifth argument
  // and forwards it unchanged to every frame pass. The ordinary camera
  // accessors read these exact fields. Reading them here keeps debug lines
  // on the same camera frame as the depth buffer; the GameUpdate-tail mirror
  // is necessarily one rendered frame old.
  if (pass_context == NULL ||
      !readable_range((BYTE *)pass_context + 0x40, 16 * sizeof(float)) ||
      !readable_range((BYTE *)pass_context + 0xc8, 12) ||
      !readable_range((BYTE *)pass_context + 0x154, sizeof(float)) ||
      !readable_range((BYTE *)pass_context + 0x170, 16 * sizeof(float))) {
    return FALSE;
  }

  copy_bytes((BYTE *)camera->world,
             (const BYTE *)pass_context + 0x170,
             sizeof(camera->world));
  camera->near_plane = *(const float *)((const BYTE *)pass_context + 0xc8);
  camera->far_plane = *(const float *)((const BYTE *)pass_context + 0xcc);
  camera->aspect = *(const float *)((const BYTE *)pass_context + 0xd0);
  camera->fov = *(const float *)((const BYTE *)pass_context + 0x154);
  camera->screencrop = 1.0f;

  for (unsigned i = 0; i < 16; ++i) {
    if (!float_is_finite(camera->world[i]) ||
        !float_is_finite(((const float *)((const BYTE *)pass_context +
                                         0x40))[i])) {
      return FALSE;
    }
  }
  if (!float_is_finite(camera->near_plane) ||
      camera->near_plane <= 0.0f ||
      !float_is_finite(camera->far_plane) ||
      camera->far_plane <= camera->near_plane ||
      !float_is_finite(camera->fov) || camera->fov <= 1.0f ||
      camera->fov >= 179.0f ||
      !float_is_finite(camera->aspect) || camera->aspect <= 0.2f ||
      camera->aspect >= 5.0f) {
    return FALSE;
  }
  return TRUE;
}

static void draw_scene_depth_hulls(void *render_device_pointer,
                                   void *pass_context) {
  unsigned locked_index;
  hull_render_snapshot *snapshot = lock_hull_snapshot(&locked_index);
  if (snapshot == NULL || snapshot->hull_count == 0) {
    if (snapshot != NULL) {
      InterlockedDecrement(&g_hull_snapshot_readers[locked_index]);
    }
    return;
  }

  BYTE *image = (BYTE *)GetModuleHandleW(NULL);
  BYTE *render_device = (BYTE *)render_device_pointer;
  if (render_device != image + 0x4040810 ||
      *(void **)(render_device + 0xa8) == NULL ||
      *(void **)(render_device + 0x3e0) == NULL ||
      *(void **)(render_device + 0x420) == NULL ||
      *(void **)(render_device + 0x4e0) == NULL ||
      *(void **)(render_device + 0x568) == NULL ||
      *(void **)(render_device + 0x570) == NULL ||
      *(void **)(render_device + 0x578) == NULL ||
      *(void **)(render_device + 0x580) == NULL) {
    InterlockedDecrement(&g_hull_snapshot_readers[locked_index]);
    return;
  }

  camera_snapshot camera;
  if (!capture_pass_camera(pass_context, &camera)) {
    InterlockedDecrement(&g_hull_snapshot_readers[locked_index]);
    return;
  }

  // Journey's own camera-frustum path in FUN_14007F440 computes this exact
  // transform. PBulletDebug supplies the same kind of matrix to the same
  // immediate float4 line renderer used below. Keeping vertices in world
  // space makes their Z exactly comparable with the scene depth buffer,
  // including asymmetric or jittered projection details.
  float view[16];
  float world_to_clip[16];
  matrix_inverse_fn inverse = (matrix_inverse_fn)(image + 0x804b0);
  matrix_multiply_fn multiply = (matrix_multiply_fn)(image + 0x7aa40);
  inverse(view, (const float *)((const BYTE *)pass_context + 0x170));
  multiply((const float *)((const BYTE *)pass_context + 0x40),
           world_to_clip, view);
  for (unsigned i = 0; i < 16; ++i) {
    if (!float_is_finite(world_to_clip[i])) {
      InterlockedDecrement(&g_hull_snapshot_readers[locked_index]);
      return;
    }
  }

  // The native uniform-color helper realizes its state from this retained
  // Phyre description immediately before Draw. Change that source, rather
  // than setting D3D state externally (which the helper would overwrite).
  BYTE *debug_state_source = render_device + 0x730;
  uint32_t *source_depth_enable =
      (uint32_t *)(debug_state_source + 0x2c);
  uint32_t *source_depth_write_mask =
      (uint32_t *)(debug_state_source + 0x30);
  uint32_t *source_depth_func =
      (uint32_t *)(debug_state_source + 0x34);
  uint32_t saved_depth_enable = *source_depth_enable;
  uint32_t saved_depth_write_mask = *source_depth_write_mask;
  uint32_t saved_depth_func = *source_depth_func;
  *source_depth_enable = 1;
  *source_depth_write_mask = 0;
  *source_depth_func = 4;  // D3D11_COMPARISON_LESS_EQUAL.
  MemoryBarrier();

  static const float colors[k_hull_color_count][4] = {
      {1.0f, 0.0f, 0.6f, 1.0f},
      {0.0f, 1.0f, 0.0f, 1.0f},
      {0.25f, 0.75f, 1.0f, 1.0f},
      {1.0f, 0.75f, 0.25f, 1.0f},
      {1.0f, 0.125f, 0.0f, 1.0f},
      {1.0f, 1.0f, 1.0f, 1.0f},
  };
  draw_primitive_uniform_color_fn draw =
      (draw_primitive_uniform_color_fn)(image + 0x36e7c0);
  uint64_t combined_result = 0;
  uint64_t submitted_vertices = 0;
  for (unsigned color = 0; color < k_hull_color_count; ++color) {
    uint32_t count = snapshot->counts[color];
    if (count > k_hull_vertices_per_color) {
      count = k_hull_vertices_per_color;
    }
    count &= ~1U;
    if (count == 0) continue;
    for (uint32_t vertex = 0; vertex < count; ++vertex) {
      g_hull_clip_positions[vertex][0] =
          snapshot->positions[color][vertex][0];
      g_hull_clip_positions[vertex][1] =
          snapshot->positions[color][vertex][1];
      g_hull_clip_positions[vertex][2] =
          snapshot->positions[color][vertex][2];
      g_hull_clip_positions[vertex][3] = 1.0f;
    }
    combined_result |= draw(render_device, 1,
                            &g_hull_clip_positions[0][0], colors[color],
                            NULL, count, world_to_clip);
    submitted_vertices += count;
  }

  *source_depth_enable = saved_depth_enable;
  *source_depth_write_mask = saved_depth_write_mask;
  *source_depth_func = saved_depth_func;
  MemoryBarrier();
  prepare_render_state_fn prepare =
      (prepare_render_state_fn)(image + 0x36a5c0);
  uint64_t restore_result =
      prepare(render_device + 0x4e0, render_device, debug_state_source);
  uint32_t drawn_hull_count = snapshot->hull_count;
  InterlockedDecrement(&g_hull_snapshot_readers[locked_index]);

  if (submitted_vertices != 0 &&
      InterlockedCompareExchange(&g_hull_draw_logged, 1, 0) == 0) {
    log_buffer buffer = {{0}, 0};
    buffer_text(&buffer, "native-view-projection world-depth hulls submitted: hulls=");
    buffer_u64(&buffer, drawn_hull_count);
    buffer_text(&buffer, " vertices=");
    buffer_u64(&buffer, submitted_vertices);
    buffer_text(&buffer, " combinedResult=");
    buffer_u64(&buffer, combined_result);
    buffer_text(&buffer, " restoreResult=");
    buffer_u64(&buffer, restore_result);
    write_log(&buffer);
  }
}

static void frame_pass_execute_hook(void *pass, void *render_context,
                                    void *pass_context) {
  g_frame_pass_execute(pass, render_context, pass_context);
  if (InterlockedCompareExchange(&g_hull_snapshot_ready, 0, 0) == 0 ||
      pass == NULL || !readable_range(pass, 0x100) ||
      !text_equal((const char *)pass + 0x40, "passColorPostpass", FALSE)) return;
  BYTE *image = (BYTE *)GetModuleHandleW(NULL);
  BYTE *render_device = image + 0x4040810;
  BYTE *targets = *(BYTE **)(image + 0x3ce4c90);
  if (!readable_range(targets, 0x88)) return;
  void *color = *(void **)(targets + 0x70);
  void *depth = *(void **)(targets + 0x80);
  if (color != NULL && depth != NULL &&
      *(void **)((BYTE *)pass + 0xb8) == color &&
      *(void **)((BYTE *)pass + 0xf8) == depth &&
      *(void **)(render_device + 0x3e0) != NULL &&
      *(void **)(render_device + 0x420) != NULL)
    draw_scene_depth_hulls(render_device, pass_context);
}

#include "image_guards.h"
#include "runtime.inc"
