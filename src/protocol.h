#ifndef HULL_VIEWER_PROTOCOL_H
#define HULL_VIEWER_PROTOCOL_H
#include <windows.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>

#define HV_MAGIC 0x48564c31u
#define HV_PROTOCOL 10u
#define HV_PICK_HITS 32
typedef struct hv_pick_hit {
  char name[48];
  uint32_t mask, active;
  float bounds[6], pixels, depth;
} hv_pick_hit;
#define HV_NAMES 64
#define HV_NAME_SIZE 48
#define HV_LEASE_MS 2000
enum { HV_HIDDEN, HV_ALL, HV_TYPES, HV_SELECTED };
enum { HV_TINT_OFF,HV_TINT_FACES };
enum { HV_CAM_READ, HV_CAM_START, HV_CAM_STOP, HV_CAM_IGNORE, HV_CAM_COLLIDE,
       HV_CAM_LOCK, HV_CAM_UNLOCK, HV_CAM_MOVE, HV_CAM_COUNT };
typedef struct hv_config {
  uint32_t mode, include_mask, exclude_mask, count, include_inactive, fill_faces, opacity;
  char names[HV_NAMES][HV_NAME_SIZE];
} hv_config;
/* All shared fields are protected by the named mutex. Neither game callback
 * waits for that mutex. This is local IPC, not a network protocol. */
typedef struct hv_shared {
  uint32_t magic, protocol, size, revision;
  uint64_t heartbeat;
  hv_config config;
  uint32_t ready, drawn_hulls, applied_revision;
  uint32_t active_matches, inactive_matches, unreadable_names;
  char status[192];
  char version[32];
  uint32_t camera_available, camera_request, camera_completed, camera_action;
  uint32_t camera_ok, camera_bits;
  float camera_target[3];
  char camera_message[128];
  uint32_t live_level, pick_request, pick_completed, pick_level, pick_count;
  uint32_t pick_width, pick_height;
  float pick_x, pick_y;
  uint64_t pick_time;
  hv_pick_hit pick_hits[HV_PICK_HITS];
  char pick_message[128], highlight[48];
  uint32_t highlight_level;
  uint32_t inspection_enabled;
  uint32_t inspect_mode, inspect_available, inspect_applied;
  char inspect_message[128];
  uint32_t fill_available;
  char face_status[128];
  float view_position[3];
  uint64_t view_position_time;
} hv_shared;

static inline void hv_object_name(wchar_t *out, size_t count,
                                  const wchar_t *kind, DWORD pid) {
  _snwprintf(out, count, L"Local\\JourneyHullViewer.%ls.%lu", kind,
             (unsigned long)pid);
  out[count - 1] = 0;
}
static inline int hv_config_valid(const hv_config *c) {
  if (c->mode > HV_SELECTED || c->count > HV_NAMES || c->include_inactive > 1 || c->fill_faces > HV_TINT_FACES || c->opacity>100 ||
      (c->include_mask | c->exclude_mask) & ~63u) return 0;
  for (unsigned i = 0; i < c->count; ++i) {
    size_t n = 0;
    while (n < HV_NAME_SIZE && c->names[i][n]) {
      unsigned char ch = (unsigned char)c->names[i][n++];
      if (ch < 33 || ch > 126) return 0;
    }
    if (!n || n == HV_NAME_SIZE) return 0;
  }
  return 1;
}
static inline int hv_separator(unsigned char c) {
  return c <= 32 || c == ',' || c == ';' || c == '\'' || c == '"';
}
/* Names are ASCII identifiers. No dependence on the system ANSI code page. */
static inline int hv_ascii_names(const wchar_t *wide, char *out, size_t capacity) {
  size_t i = 0;
  for (; wide[i]; ++i) {
    if (i + 1 >= capacity || (unsigned)wide[i] > 127) return 0;
    out[i] = (char)wide[i];
  }
  if (i >= capacity) return 0;
  out[i] = 0;
  return 1;
}
/* Accept quoted or bare names; do not silently truncate. */
static inline int hv_parse_names(const char *text, hv_config *c) {
  c->count = 0;
  while (*text) {
    while (*text && hv_separator((unsigned char)*text)) ++text;
    if (!*text) break;
    if (c->count == HV_NAMES) return 0;
    unsigned n = 0;
    while (*text && !hv_separator((unsigned char)*text)) {
      if (n + 1 >= HV_NAME_SIZE || (unsigned char)*text > 126) return 0;
      c->names[c->count][n++] = *text++;
    }
    c->names[c->count++][n] = 0;
  }
  return hv_config_valid(c);
}
#endif
