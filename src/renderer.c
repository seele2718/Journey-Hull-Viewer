#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <d3d11.h>
#include <string.h>
#include "protocol.h"
#include "version.h"
#include "MinHook.h"
#include "pick_math.h"
#include "face_math.h"

enum { k_absolute_jump_size = 14 };
typedef uint64_t (*draw_primitive_uniform_color_fn)(
    void *render_device, uint32_t primitive_type, const float *positions,
    const float *color, const uint32_t *indices, uint32_t vertex_count,
    const float *transform);
#define HV_COM_RELEASE(p) do {if(p){((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p));(p)=NULL;}}while(0)
static volatile LONG g_view_requested;
static uint64_t g_view_sampled;
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
  BOOL fill_enabled,fill_selected;
  float opacity;
  uint32_t level, item_count;
  struct {
    hv_pick_hit info;
    uint32_t color, first, count;
    uint32_t triangle_first,triangle_count;
  } items[10000];
  uint32_t triangle_vertices;
  float triangles[k_hull_vertex_capacity*3][3];
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
static BOOL g_fill_faces,g_fill_supported;
static float g_fill_opacity=.25f;
static uint32_t g_live_level = UINT32_MAX;
static volatile LONG g_inspection_enabled;
static HANDLE g_mapping, g_mutex;
static hv_shared *g_shared;
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
  g_live_level = UINT32_MAX;
  if (game_root && readable_range((BYTE *)game_root+0x1e0,sizeof(void *))) {
    BYTE *player=*(BYTE **)((BYTE *)game_root+0x1e0);
    if (readable_range(player ? player+0x14 : NULL,4)) {
      uint32_t level=*(uint32_t *)(player+0x14);
      if(level<12)g_live_level=level;
    }
  }
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
  if (InterlockedCompareExchange(&g_hull_snapshot_readers[target_index], -1,
                                 0) != 0) {
    return;
  }

  hull_render_snapshot *snapshot = &g_hull_snapshots[target_index];
  snapshot->hull_count = 0;
  snapshot->fill_enabled=g_fill_faces;
  snapshot->opacity=g_fill_opacity;
  snapshot->fill_selected=g_hull_filter_mode==k_hull_filter_selected;
  snapshot->item_count = 0;
  snapshot->triangle_vertices = 0;
  snapshot->level = g_live_level;
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
      BOOL metadata=InterlockedCompareExchange(&g_inspection_enabled,0,0)||(g_fill_faces&&snapshot->fill_selected);
      SIZE_T length=0;const char *name=metadata?physical_hull_name(hull,&length):NULL;
      if(name && length<HV_NAME_SIZE && snapshot->item_count<10000) {
        unsigned item=snapshot->item_count++;
        memset(&snapshot->items[item],0,sizeof(snapshot->items[item]));
        memcpy(snapshot->items[item].info.name,name,length);
        snapshot->items[item].info.mask=type_mask;
        snapshot->items[item].info.active=is_active!=0;
        for(unsigned k=0;k<3;k++) {
          snapshot->items[item].info.bounds[k]=*(float *)(hull+0x10+4*k);
          snapshot->items[item].info.bounds[k+3]=*(float *)(hull+0x20+4*k);
        }
        snapshot->items[item].color=color;snapshot->items[item].first=before;
        snapshot->items[item].count=snapshot->counts[color]-before;
        BYTE *triangles=*(BYTE **)(hull+0x50);
        unsigned triangle_count=*(uint32_t *)(hull+0x70);
        unsigned first_triangle=snapshot->triangle_vertices;
        snapshot->items[item].triangle_first=first_triangle;
        if(triangle_count>0&&triangle_count<=100000&&
           first_triangle+triangle_count*3<=k_hull_vertex_capacity*3&&
           readable_range(triangles,(SIZE_T)triangle_count*3)) {
          for(unsigned t=0;t<triangle_count;t++) {
            BOOL valid=TRUE;
            for(unsigned j=0;j<3;j++) {
              unsigned v=triangles[t*3+j];if(v>=vertex_count){valid=FALSE;break;}
              for(unsigned k=0;k<3;k++)if(!float_is_finite(vertices[v*4+k]))valid=FALSE;
            }
            if(!valid)continue;
            for(unsigned j=0;j<3;j++) {
              unsigned v=triangles[t*3+j];
              for(unsigned k=0;k<3;k++)snapshot->triangles[snapshot->triangle_vertices][k]=vertices[v*4+k];
              snapshot->triangle_vertices++;
            }
          }
        }
        snapshot->items[item].triangle_count=snapshot->triangle_vertices-first_triangle;
      }
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
    LONG readers=InterlockedCompareExchange(&g_hull_snapshot_readers[index],0,0);
    if(readers<0||readers>=32||
        InterlockedCompareExchange(&g_hull_snapshot_readers[index],readers+1,readers)!=readers)
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

/* Only requested clicks do hit testing. Never use game pointers from the panel.
 * The current render camera and one completed immutable geometry snapshot are
 * used together. Results can include obscured edges; no depth-buffer readback. */
static void pick_snapshot(hull_render_snapshot *s,const float matrix[16],char highlight[48]) {
  static uint32_t completed;
  static hv_pick_hit hits[HV_PICK_HITS];
  static uint32_t count;
  static char message[128];
  uint32_t request,level,width,height;float x,y;uint64_t time,heartbeat;
  if(!g_shared||!g_mutex||!InterlockedCompareExchange(&g_inspection_enabled,0,0))return;
  DWORD lock=WaitForSingleObject(g_mutex,0);
  if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED)return;
  if(g_shared->highlight_level==s->level)memcpy(highlight,g_shared->highlight,48);
  request=g_shared->pick_request;level=g_shared->pick_level;
  width=g_shared->pick_width;height=g_shared->pick_height;
  x=g_shared->pick_x;y=g_shared->pick_y;time=g_shared->pick_time;heartbeat=g_shared->heartbeat;
  if(request==completed) {
    if(g_shared->pick_completed!=completed) {
      g_shared->pick_count=count;memcpy(g_shared->pick_hits,hits,sizeof(hits));
      memcpy(g_shared->pick_message,message,sizeof(message));g_shared->pick_completed=completed;
    }
    ReleaseMutex(g_mutex);return;
  }
  ReleaseMutex(g_mutex);
  count=0;message[0]=0;
  uint64_t now=GetTickCount64();
  if(now-time>1000||now-heartbeat>HV_LEASE_MS)lstrcpyA(message,"Pick expired; try again.");
  else if(s->level>=12||level!=s->level)lstrcpyA(message,"Level changed; pick again.");
  else if(width<32||height<32||width>16384||height>16384||
          !isfinite(x)||!isfinite(y)||x<0||x>1||y<0||y>1)
    lstrcpyA(message,"Invalid pick coordinates.");
  else {
    for(unsigned i=0;i<s->item_count;i++) {
      hv_pick_hit hit=s->items[i].info;float best=9,depth=1;
      unsigned triangle=s->items[i].triangle_first,end=triangle+s->items[i].triangle_count;
      for(;triangle+2<end;triangle+=3) {
        float z=1;
        if(hv_triangle_depth(s->triangles[triangle],s->triangles[triangle+1],s->triangles[triangle+2],
                             matrix,x*2-1,1-y*2,&z)&& (best!=0||z<depth)) {best=0;depth=z;}
      }
      unsigned color=s->items[i].color,first=s->items[i].first,n=s->items[i].count;
      for(unsigned edge=first;edge+1<first+n;edge+=2) {
        float d=1,pixels=hv_edge_distance(s->positions[color][edge],s->positions[color][edge+1],
                                         matrix,x*width,y*height,width,height,&d);
        if(pixels<best){best=pixels;depth=d;}
      }
      if(best>8)continue;
      hit.pixels=best;hit.depth=depth;
      unsigned at=0;
      while(at<count&&(hits[at].pixels<best||
            (hits[at].pixels==best&&hits[at].depth<=depth)))at++;
      if(at<HV_PICK_HITS) {
        if(count<HV_PICK_HITS)count++;
        for(unsigned k=count-1;k>at;k--)hits[k]=hits[k-1];
        hits[at]=hit;
      }
    }
    lstrcpyA(message,count?"Choose a hull below. Overlapping or obscured hulls may also be listed.":"No drawn hull at that point. Try another face or change the type filter.");
  }
  completed=request;
  lock=WaitForSingleObject(g_mutex,0);
  if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED) {
    if(g_shared->pick_request==request) {
      g_shared->pick_count=count;memcpy(g_shared->pick_hits,hits,sizeof(hits));
      memcpy(g_shared->pick_message,message,sizeof(message));g_shared->pick_completed=completed;
    }
    ReleaseMutex(g_mutex);
  }
}

static const float g_hull_colors[k_hull_color_count][4] = {
    {1,0,.6f,1},{0,1,0,1},{.25f,.75f,1,1},
    {1,.75f,.25f,1},{1,.125f,0,1},{1,1,1,1}};
/* Selection accent is independent of hull type, shared by named groups and
 * individual picks. Neutral charcoal fill with a blue boundary accent. */
static const float g_selection_face[4]={.12f,.14f,.17f,.25f};
static const float g_selection_edge[4]={.05f,.35f,1,1};
/* The guarded native state realizer consumes D3D11 descriptors at these
 * offsets. Preserve all bytes and cached ownership; do not create COM states
 * or change scene depth. Native helper batches must not split triangles. */
typedef struct hull_shell_style {
  D3D11_RASTERIZER_DESC raster;
  D3D11_DEPTH_STENCIL_DESC depth;
  UINT reference;
} hull_shell_style;
/* One render-thread-only handoff from the original outline pass to the
 * post-fog blended-geometry pass. No retained COM objects or frame allocations. */
static struct {
  BOOL pending;
  unsigned snapshot_index;
  void *camera;
  float world[16],projection[16],matrix[16];
  char highlight[48];
  hull_shell_style style;
} g_late_shell;
typedef struct face_job {
  volatile LONG state; /* 0 free, 1 preparing, 2 queued, 3 executing. */
  unsigned snapshot_index;
  float matrix[16];
  char highlight[48];
  hull_shell_style style;
  float scratch[4095][4];
} face_job;
static face_job g_face_jobs[4];
static SRWLOCK g_native_draw_lock=SRWLOCK_INIT,g_face_status_lock=SRWLOCK_INIT;
typedef struct native_callback_args { void *device,*user; } native_callback_args;
typedef uint64_t (*queue_callback_fn)(void *,void (*)(const native_callback_args *),void *);
static void discard_late_shell(void) {
  if(!g_late_shell.pending)return;
  g_late_shell.pending=FALSE;
  InterlockedDecrement(&g_hull_snapshot_readers[g_late_shell.snapshot_index]);
}
/* Transfer the caller's already-held snapshot lock, rather than re-reading the
 * published slot later. The producer remains nonblocking if both slots are busy. */
static void retain_late_shell(unsigned index,void *camera,const float matrix[16],
    const char highlight[48],const hull_shell_style *style) {
  discard_late_shell();
  g_late_shell.style=*style;
  memcpy(g_late_shell.matrix,matrix,sizeof(g_late_shell.matrix));
  memcpy(g_late_shell.world,(BYTE *)camera+0x170,sizeof(g_late_shell.world));
  memcpy(g_late_shell.projection,(BYTE *)camera+0x40,sizeof(g_late_shell.projection));
  memcpy(g_late_shell.highlight,highlight,sizeof(g_late_shell.highlight));
  g_late_shell.camera=camera;g_late_shell.snapshot_index=index;g_late_shell.pending=TRUE;
}
static void publish_shell_status(const char *message) {
  static uint64_t last;static char previous[128];uint64_t now=GetTickCount64();
  if(!g_shared||!g_mutex||!TryAcquireSRWLockExclusive(&g_face_status_lock))return;
  if(!strcmp(previous,message)&&now-last<1000) {
    ReleaseSRWLockExclusive(&g_face_status_lock);return;
  }
  DWORD lock=WaitForSingleObject(g_mutex,0);
  if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED) {
    lstrcpynA(g_shared->face_status,message,128);lstrcpynA(previous,message,128);last=now;
    ReleaseMutex(g_mutex);
  }
  ReleaseSRWLockExclusive(&g_face_status_lock);
}
static hull_shell_style read_hull_shell_style(ID3D11DeviceContext *context) {
  ID3D11DepthStencilState *depth=NULL;ID3D11RasterizerState *raster=NULL;
  hull_shell_style style={
    .raster={.FillMode=D3D11_FILL_SOLID,.CullMode=D3D11_CULL_BACK,.DepthClipEnable=TRUE},
    .depth={.DepthEnable=TRUE,.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL,
      .DepthFunc=D3D11_COMPARISON_LESS,.StencilReadMask=255,.StencilWriteMask=255,
      .FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS},
      .BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS}}};
  ID3D11DeviceContext_OMGetDepthStencilState(context,&depth,&style.reference);
  ID3D11DeviceContext_RSGetState(context,&raster);
  if(depth)ID3D11DepthStencilState_GetDesc(depth,&style.depth);
  if(raster)ID3D11RasterizerState_GetDesc(raster,&style.raster);
  HV_COM_RELEASE(depth);HV_COM_RELEASE(raster);return style;
}
static void hull_surface_state(BYTE *state,const hull_shell_style *style) {
  D3D11_RASTERIZER_DESC *raster=(D3D11_RASTERIZER_DESC *)(state+4);
  D3D11_DEPTH_STENCIL_DESC *depth=(D3D11_DEPTH_STENCIL_DESC *)(state+0x2c);
  D3D11_BLEND_DESC *blend=(D3D11_BLEND_DESC *)(state+0x60);
  *raster=style->raster;
  raster->FillMode=D3D11_FILL_SOLID;raster->CullMode=D3D11_CULL_NONE;
  *depth=style->depth;
  depth->DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
  depth->StencilWriteMask=0;
  depth->FrontFace.StencilFailOp=depth->FrontFace.StencilDepthFailOp=depth->FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
  depth->BackFace.StencilFailOp=depth->BackFace.StencilDepthFailOp=depth->BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
  state[0x188]=(BYTE)style->reference;
  blend->AlphaToCoverageEnable=FALSE;blend->IndependentBlendEnable=FALSE;
  blend->RenderTarget[0]=(D3D11_RENDER_TARGET_BLEND_DESC){.BlendEnable=TRUE,
    .SrcBlend=D3D11_BLEND_SRC_ALPHA,.DestBlend=D3D11_BLEND_INV_SRC_ALPHA,.BlendOp=D3D11_BLEND_OP_ADD,
    .SrcBlendAlpha=D3D11_BLEND_ZERO,.DestBlendAlpha=D3D11_BLEND_ONE,.BlendOpAlpha=D3D11_BLEND_OP_ADD,
    .RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE};
}
static uint32_t draw_hull_faces(const hull_render_snapshot *snapshot,const char *highlight,
    void *device,BYTE *state,const float matrix[16],draw_primitive_uniform_color_fn draw,void **override_slot,
    const hull_shell_style *style,float (*scratch)[4]) {
  if(!g_fill_supported||!snapshot->fill_enabled||snapshot->opacity<=0)return 0;
  if(!override_slot||!style||!scratch)return 0;
  if(!snapshot->fill_selected&&(!highlight||!highlight[0]))return 0;
  if(*(void **)(state+0x168)||*(void **)(state+0x170)||*(void **)(state+0x178)||*(void **)(state+0x180))return 0;
  BYTE saved[0x190];memcpy(saved,state,sizeof(saved));hull_surface_state(state,style);
  /* The native binder otherwise replaces our blend/depth descriptor with the
   * active pass override. Bypass it only during these synchronous face draws;
   * restore it before outlines or any further native rendering. */
  void *saved_override=*override_slot;*override_slot=NULL;
  MemoryBarrier();uint32_t total=0,calls=0;
  for(unsigned group=snapshot->fill_selected?0:6;group<7&&calls<16;group++) {
    float tint[4];memcpy(tint,g_selection_face,sizeof(tint));
    tint[3]=snapshot->opacity;
    unsigned buffered=0;
    for(unsigned i=0;i<snapshot->item_count&&calls<16;i++) {
      BOOL selected=highlight&&highlight[0]&&!strncmp(snapshot->items[i].info.name,highlight,48);
      if(!snapshot->fill_selected&&!selected)continue;
      if((selected?6:snapshot->items[i].color%k_hull_color_count)!=group)continue;
      unsigned first=snapshot->items[i].triangle_first,count=snapshot->items[i].triangle_count;
      if(first>snapshot->triangle_vertices||count>snapshot->triangle_vertices-first||count%3)continue;
      for(unsigned offset=0;offset<count&&calls<16;offset+=3) {
        const float (*face)[3]=snapshot->triangles+first+offset;
        /* A normal translucent shell: both sides, including exit walls seen
         * from inside. Use the effective outline depth/stencil rules, never
         * write either buffer or classify/colour scene objects as a volume. */
        if(!hv_shell_triangle(face[0],face[1],face[2]))continue;
        for(unsigned j=0;j<3;j++) {
          for(unsigned axis=0;axis<3;axis++)scratch[buffered][axis]=face[j][axis];
          scratch[buffered++][3]=1;
        }
        if(buffered==4095) {
          calls++;if(draw(device,2,&scratch[0][0],tint,NULL,buffered,matrix))goto restore;
          total+=buffered;buffered=0;
        }
      }
    }
    if(buffered&&calls<16) {
      calls++;if(draw(device,2,&scratch[0][0],tint,NULL,buffered,matrix))goto restore;
      total+=buffered;
    }
  }
restore:
  *override_slot=saved_override;memcpy(state,saved,sizeof(saved));MemoryBarrier();return total;
}
static uint32_t draw_hull_shell(const hull_render_snapshot *snapshot,const char *highlight,
    void *device,ID3D11DeviceContext *context,BYTE *state,const float matrix[16],
    draw_primitive_uniform_color_fn draw,void **override_slot,const hull_shell_style *outline_style,
    float (*scratch)[4]) {
  if(!context||!g_fill_supported||!snapshot->fill_enabled||
      (!snapshot->fill_selected&&(!highlight||!highlight[0])))return 0;
  /* Call ONLY after a successful ordinary outline draw. Those draws retain
   * Journey's active pass override. Read the ACTUAL realized state, not the
   * retained source descriptor that the pass can override. No GPU readback. */
  ID3D11DepthStencilState *depth=NULL;
  ID3D11RasterizerState *raster=NULL;ID3D11BlendState *blend=NULL;
  UINT reference=0,mask=0;float factors[4];
  ID3D11DeviceContext_OMGetDepthStencilState(context,&depth,&reference);
  ID3D11DeviceContext_RSGetState(context,&raster);
  ID3D11DeviceContext_OMGetBlendState(context,&blend,factors,&mask);
  hull_shell_style style=outline_style?*outline_style:read_hull_shell_style(context);
  uint32_t total=draw_hull_faces(snapshot,highlight,device,state,matrix,draw,override_slot,&style,scratch);
  ID3D11DeviceContext_OMSetDepthStencilState(context,depth,reference);
  ID3D11DeviceContext_RSSetState(context,raster);
  ID3D11DeviceContext_OMSetBlendState(context,blend,factors,mask);
  HV_COM_RELEASE(depth);HV_COM_RELEASE(raster);HV_COM_RELEASE(blend);
  return total;
}
static void draw_scene_depth_hulls_locked(void *render_device_pointer,
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
  char highlight[48]={0};pick_snapshot(snapshot,world_to_clip,highlight);

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
                            &g_hull_clip_positions[0][0], snapshot->fill_selected?g_selection_edge:g_hull_colors[color],
                            NULL, count, world_to_clip);
    submitted_vertices += count;
  }

  /* Fog later recolours this image using SCENERY depth. Defer only shell
   * faces until the existing post-fog, full-resolution blended geometry pass.
   * Keep original outlines at their existing boundary. Pin this exact snapshot
   * through the late pass: the game thread may publish the other slot meanwhile. */
  if(submitted_vertices&&!combined_result&&g_fill_supported&&snapshot->fill_enabled&&
      (snapshot->fill_selected||highlight[0])) {
    hull_shell_style style=read_hull_shell_style(*(ID3D11DeviceContext **)(render_device+0xa8));
    retain_late_shell(locked_index,pass_context,world_to_clip,highlight,&style);
  }

  if(highlight[0])for(unsigned i=0;i<snapshot->item_count;i++) {
    if(strncmp(snapshot->items[i].info.name,highlight,48))continue;
    unsigned color=snapshot->items[i].color,first=snapshot->items[i].first,n=snapshot->items[i].count;
    for(unsigned k=0;k<n;k++) {
      for(unsigned axis=0;axis<3;axis++)g_hull_clip_positions[k][axis]=snapshot->positions[color][first+k][axis];
      g_hull_clip_positions[k][3]=1;
    }
    draw(render_device,1,&g_hull_clip_positions[0][0],g_selection_edge,NULL,n,world_to_clip);
    break;
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
  if(!g_late_shell.pending||g_late_shell.snapshot_index!=locked_index)
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

static BOOL late_shell_view_matches(void *camera,unsigned index) {
  return g_late_shell.pending&&camera&&g_late_shell.camera==camera&&
    index==g_late_shell.snapshot_index&&readable_range(camera,0x1b0)&&
    !memcmp(g_late_shell.world,(BYTE *)camera+0x170,sizeof(g_late_shell.world))&&
    !memcmp(g_late_shell.projection,(BYTE *)camera+0x40,sizeof(g_late_shell.projection));
}
static void draw_scene_depth_hulls(void *device,void *camera) {
  if(!TryAcquireSRWLockExclusive(&g_native_draw_lock))return;
  draw_scene_depth_hulls_locked(device,camera);
  ReleaseSRWLockExclusive(&g_native_draw_lock);
}
static void finish_face_job(face_job *job) {
  InterlockedDecrement(&g_hull_snapshot_readers[job->snapshot_index]);
  InterlockedExchange(&job->state,0);
}
/* The native uniform helper changes these bindings. A stream callback must
 * restore them too, so subsequent queued materials and native caches agree.
 * Fixed stack storage, no resource copies, shader compilation or GPU wait. */
typedef struct face_pipeline {
  ID3D11VertexShader *vs;ID3D11PixelShader *ps;
  ID3D11GeometryShader *gs;ID3D11ComputeShader *cs;
  ID3D11ClassInstance *classes[4][256];UINT class_count[4];
  ID3D11InputLayout *layout;ID3D11Buffer *vertices[3],*vs_cb,*ps_cb;
  UINT strides[3],offsets[3];D3D11_PRIMITIVE_TOPOLOGY topology;
  ID3D11ShaderResourceView *resource;ID3D11SamplerState *sampler;
} face_pipeline;
static void capture_face_pipeline(ID3D11DeviceContext *context,face_pipeline *p) {
  memset(p,0,sizeof(*p));
  /* Query the count first. Some compatibility runtimes don't write this
   * optional output, so a capacity of256 must not become a restore count. */
  ID3D11DeviceContext_VSGetShader(context,&p->vs,NULL,&p->class_count[0]);
  ID3D11DeviceContext_PSGetShader(context,&p->ps,NULL,&p->class_count[1]);
  ID3D11DeviceContext_GSGetShader(context,&p->gs,NULL,&p->class_count[2]);
  ID3D11DeviceContext_CSGetShader(context,&p->cs,NULL,&p->class_count[3]);
  for(unsigned i=0;i<4;i++)if(p->class_count[i]>256)p->class_count[i]=0;
  if(p->class_count[0])ID3D11DeviceContext_VSGetShader(context,NULL,p->classes[0],&p->class_count[0]);
  if(p->class_count[1])ID3D11DeviceContext_PSGetShader(context,NULL,p->classes[1],&p->class_count[1]);
  if(p->class_count[2])ID3D11DeviceContext_GSGetShader(context,NULL,p->classes[2],&p->class_count[2]);
  if(p->class_count[3])ID3D11DeviceContext_CSGetShader(context,NULL,p->classes[3],&p->class_count[3]);
  ID3D11DeviceContext_IAGetInputLayout(context,&p->layout);
  ID3D11DeviceContext_IAGetVertexBuffers(context,0,3,p->vertices,p->strides,p->offsets);
  ID3D11DeviceContext_IAGetPrimitiveTopology(context,&p->topology);
  ID3D11DeviceContext_VSGetConstantBuffers(context,0,1,&p->vs_cb);
  ID3D11DeviceContext_PSGetConstantBuffers(context,0,1,&p->ps_cb);
  ID3D11DeviceContext_PSGetShaderResources(context,0,1,&p->resource);
  ID3D11DeviceContext_PSGetSamplers(context,0,1,&p->sampler);
}
static void release_face_pipeline(face_pipeline *p) {
  HV_COM_RELEASE(p->vs);HV_COM_RELEASE(p->ps);HV_COM_RELEASE(p->gs);HV_COM_RELEASE(p->cs);
  HV_COM_RELEASE(p->layout);HV_COM_RELEASE(p->vs_cb);HV_COM_RELEASE(p->ps_cb);
  HV_COM_RELEASE(p->resource);HV_COM_RELEASE(p->sampler);
  for(unsigned i=0;i<3;i++)HV_COM_RELEASE(p->vertices[i]);
  for(unsigned i=0;i<4;i++)for(unsigned j=0;j<p->class_count[i]&&j<256;j++)HV_COM_RELEASE(p->classes[i][j]);
}
static void restore_face_pipeline(ID3D11DeviceContext *context,face_pipeline *p) {
  ID3D11DeviceContext_VSSetShader(context,p->vs,p->classes[0],p->class_count[0]);
  ID3D11DeviceContext_PSSetShader(context,p->ps,p->classes[1],p->class_count[1]);
  ID3D11DeviceContext_GSSetShader(context,p->gs,p->classes[2],p->class_count[2]);
  ID3D11DeviceContext_CSSetShader(context,p->cs,p->classes[3],p->class_count[3]);
  ID3D11DeviceContext_IASetInputLayout(context,p->layout);
  ID3D11DeviceContext_IASetVertexBuffers(context,0,3,p->vertices,p->strides,p->offsets);
  ID3D11DeviceContext_IASetPrimitiveTopology(context,p->topology);
  ID3D11DeviceContext_VSSetConstantBuffers(context,0,1,&p->vs_cb);
  ID3D11DeviceContext_PSSetConstantBuffers(context,0,1,&p->ps_cb);
  ID3D11DeviceContext_PSSetShaderResources(context,0,1,&p->resource);
  ID3D11DeviceContext_PSSetSamplers(context,0,1,&p->sampler);
  release_face_pipeline(p);
}
static void execute_face_job(face_job *job,BYTE *render_device,
    draw_primitive_uniform_color_fn draw,prepare_render_state_fn prepare,void **override_slot) {
  if(InterlockedCompareExchange(&job->state,3,2)!=2)return;
  if(!InterlockedCompareExchange(&g_hull_snapshot_ready,0,0))goto done;
  if(!render_device||!*(void **)(render_device+0xa8)||
      !*(void **)(render_device+0x3e0)||!*(void **)(render_device+0x420)) {
    publish_shell_status("Faces: scene targets unavailable; outlines only.");goto done;
  }
  if(!TryAcquireSRWLockExclusive(&g_native_draw_lock)) {
    publish_shell_status("Faces: renderer busy; outlines only.");goto done;
  }
  ID3D11DeviceContext *context=*(ID3D11DeviceContext **)(render_device+0xa8);
  face_pipeline bindings;capture_face_pipeline(context,&bindings);
  unsigned submitted=draw_hull_shell(&g_hull_snapshots[job->snapshot_index],job->highlight,render_device,
    context,render_device+0x730,
    job->matrix,draw,override_slot,&job->style,job->scratch);
  prepare(render_device+0x4e0,render_device,render_device+0x730);
  restore_face_pipeline(context,&bindings);
  ReleaseSRWLockExclusive(&g_native_draw_lock);
  publish_shell_status(submitted?"Faces: post-fog.":"Faces: no surface triangles submitted.");
done:
  finish_face_job(job);
}
static void face_job_callback(const native_callback_args *args) {
  if(!args)return;
  face_job *job=NULL;
  for(unsigned i=0;i<4;i++)if(args->user==&g_face_jobs[i])job=&g_face_jobs[i];
  if(!job)return;
  BYTE *image=(BYTE *)GetModuleHandleW(NULL);
  BYTE *device=args->device==image+0x4040810?args->device:NULL;
  execute_face_job(job,device,(draw_primitive_uniform_color_fn)(image+0x36e7c0),
    (prepare_render_state_fn)(image+0x36a5c0),(void **)(image+0x3c11750));
}
static void queue_late_hull_faces(void *render_context,void *camera,queue_callback_fn enqueue) {
  if(!g_late_shell.pending)return;
  unsigned index=g_late_shell.snapshot_index;
  BOOL matched=late_shell_view_matches(camera,index);
  if(matched) {
    if(render_context&&enqueue)for(unsigned i=0;i<4;i++) {
      face_job *job=&g_face_jobs[i];
      if(InterlockedCompareExchange(&job->state,1,0)!=0)continue;
      job->snapshot_index=index;job->style=g_late_shell.style;
      memcpy(job->matrix,g_late_shell.matrix,sizeof(job->matrix));
      memcpy(job->highlight,g_late_shell.highlight,sizeof(job->highlight));
      g_late_shell.pending=FALSE; /* Transfer reader ownership to this job. */
      InterlockedExchange(&job->state,2);
      if(enqueue(render_context,face_job_callback,job)&&InterlockedCompareExchange(&job->state,3,2)==2) {
        finish_face_job(job);publish_shell_status("Faces: queue rejected draw; outlines only.");
      }
      return;
    }
    publish_shell_status("Faces: render queue busy; outlines only.");
  } else {
    publish_shell_status("Faces: view changed; outlines only.");
  }
  discard_late_shell();
}

static void publish_view_position(void *pass_context) {
  uint64_t now=GetTickCount64();
  if(!g_shared||!g_mutex||!g_view_requested||now-g_view_sampled<100)return;
  g_view_sampled=now;
  camera_snapshot camera;if(!capture_pass_camera(pass_context,&camera))return;
  DWORD lock=WaitForSingleObject(g_mutex,0);
  if(lock!=WAIT_OBJECT_0&&lock!=WAIT_ABANDONED)return;
  if(now-g_shared->heartbeat<=HV_LEASE_MS) {
    memcpy(g_shared->view_position,camera.world+12,sizeof(g_shared->view_position));
    g_shared->view_position_time=now;
  }
  ReleaseMutex(g_mutex);
}
static void frame_pass_execute_hook(void *pass, void *render_context,
                                    void *pass_context) {
  g_frame_pass_execute(pass, render_context, pass_context);
  BOOL drawing=InterlockedCompareExchange(&g_hull_snapshot_ready,0,0)!=0;
  if(!drawing) {
    discard_late_shell();
    if(!g_view_requested||GetTickCount64()-g_view_sampled<100)return;
  }
  if(pass==NULL||!readable_range(pass,0x100))return;
  BOOL outlines=text_equal((const char *)pass+0x40,"passColorPostpass",FALSE);
  BOOL shell=text_equal((const char *)pass+0x40,"passBlendedGeom",FALSE);
  if(outlines) {
    if(g_late_shell.pending)publish_shell_status("Faces: post-fog pass unavailable; outlines only.");
    discard_late_shell();
  }
  if((!outlines&&!shell)||(shell&&!((BYTE *)pass)[0x90]))return;
  BYTE *image = (BYTE *)GetModuleHandleW(NULL);
  BYTE *render_device = image + 0x4040810;
  BYTE *targets = *(BYTE **)(image + 0x3ce4c90);
  if (!readable_range(targets, 0x88)) return;
  void *color = *(void **)(targets + 0x70);
  void *depth = *(void **)(targets + 0x80);
  if (color != NULL && depth != NULL &&
      *(void **)((BYTE *)pass + 0xb8) == color &&
      *(void **)((BYTE *)pass + 0xf8) == depth) {
    if(outlines) {
      publish_view_position(pass_context);
      if(drawing)draw_scene_depth_hulls(render_device,pass_context);
    } else if(drawing&&g_fill_supported)queue_late_hull_faces(render_context,pass_context,
      (queue_callback_fn)(image+0x350370));
  }
}

#include "image_guards.h"
#include "runtime.inc"
