#ifndef HV_CAMERA_H
#define HV_CAMERA_H
#include <stdio.h>
#include <math.h>
/* Fixed camera actions only. Executed on the game update thread using the
 * game's protected Lua API; no console, arbitrary source or global overrides. */
typedef struct hv_lua hv_lua;
typedef int (*hv_continuation)(hv_lua *, int, intptr_t);
typedef struct hv_lua_api {
  int (*gettop)(hv_lua *);
  void (*settop)(hv_lua *, int);
  int (*checkstack)(hv_lua *, int);
  int (*type)(hv_lua *, int);
  const char *(*tolstring)(hv_lua *, int, size_t *);
  int (*loadbuffer)(hv_lua *, const char *, size_t, const char *, const char *);
  int (*pcall)(hv_lua *, int, int, int, intptr_t, hv_continuation);
} hv_lua_api;
static hv_lua_api g_camera_lua;
static BOOL g_camera_available;

static const char *camera_action_source(uint32_t action) {
  switch (action) {
    case HV_CAM_READ: return "";
    case HV_CAM_START: return "cs:StartDebugCam(); ";
    case HV_CAM_STOP: return "cs:StopDebugCam(); ";
    case HV_CAM_IGNORE: return "Vars.DebugCamera.collideTerrain(false); Vars.CameraSystem.defCollideTerrain(false); ";
    case HV_CAM_COLLIDE: return "Vars.DebugCamera.collideTerrain(true); Vars.CameraSystem.defCollideTerrain(true); ";
    case HV_CAM_LOCK: return "assert(cs:GetDebugCam(),'Debug camera unavailable'):SetAngleLock(true); ";
    case HV_CAM_UNLOCK: return "assert(cs:GetDebugCam(),'Debug camera unavailable'):SetAngleLock(false); ";
    case HV_CAM_MOVE: return "assert(cs:UsingDebugCam(),'Start Debug Cam first'); local k=cs:GetBlendState(); local d=assert(cs:GetDebugCam(),'Debug camera unavailable'); local locked=d:GetAngleLock(); d:SetXForm(k:rot(),{x,y,z},k:fov()); d:SetAngleLock(locked); ";
    default: return NULL;
  }
}
static BOOL execute_camera_command(hv_lua *state, uint32_t action, const float target[3], uint32_t *bits,
                            char message[128]) {
  const char *source = camera_action_source(action);
  *bits = 0;
  message[0] = 0;
  if (!source) { lstrcpyA(message, "Invalid camera action."); return FALSE; }
  if (!state) { lstrcpyA(message, "Game Lua is not ready."); return FALSE; }
  int top = g_camera_lua.gettop(state);
  BOOL ok = FALSE;
  if (!g_camera_lua.checkstack(state, 8)) {
    lstrcpyA(message, "Lua stack space unavailable.");
  } else {
    char command[1024];
    const char *prefix = "local cs=assert(game,'Game unavailable'):cameraSystem(); ";
    const char *suffix =
      "local d=cs:GetDebugCam(); local function b(v) return v and '1' or '0' end; "
      "return b(cs:UsingDebugCam())..b(Vars.DebugCamera.collideTerrain()).."
      "b(Vars.CameraSystem.defCollideTerrain())..b(d and d:GetAngleLock())";
    command[0]=0;
    if(action==HV_CAM_MOVE) {
      if(!target||!isfinite(target[0])||!isfinite(target[1])||!isfinite(target[2])||
          fabsf(target[0])>1000000||fabsf(target[1])>1000000||fabsf(target[2])>1000000) {
        lstrcpyA(message,"Invalid camera coordinates.");g_camera_lua.settop(state,top);return FALSE;
      }
      snprintf(command,sizeof(command),"local x,y,z=%.9g,%.9g,%.9g; ",(double)target[0],(double)target[1],(double)target[2]);
    }
    lstrcatA(command, prefix); lstrcatA(command, source); lstrcatA(command, suffix);
    int error = g_camera_lua.loadbuffer(state, command, strlen(command), "=hull viewer camera", "t");
    if (!error) error = g_camera_lua.pcall(state, 0, 1, 0, 0, NULL);
    if (g_camera_lua.type(state, -1) == 4) {
      size_t length = 0;
      const char *text = g_camera_lua.tolstring(state, -1, &length);
      if (error) {
        size_t n = length < 127 ? length : 127;
        if (text) memcpy(message, text, n);
        message[text ? n : 0] = 0;
      } else if (text && length == 4) {
        ok = TRUE;
        for (unsigned i = 0; i < 4; ++i) {
          if (text[i] != '0' && text[i] != '1') { ok = FALSE; break; }
          if (text[i] == '1') *bits |= 1u << i;
        }
      }
    }
    if (!ok && !message[0]) lstrcpyA(message, error ? "Camera command failed." : "Camera state unavailable.");
  }
  g_camera_lua.settop(state, top);
  return ok;
}
static BOOL execute_camera(hv_lua *state,uint32_t action,uint32_t *bits,char message[128]) {
  return execute_camera_command(state,action,NULL,bits,message);
}
static void init_camera_api(BYTE *image) {
  g_camera_available = verify_camera_guards(image);
  if (!g_camera_available) return;
  g_camera_lua.gettop = (void *)(image + 0x31aec0);
  g_camera_lua.settop = (void *)(image + 0x31aee0);
  g_camera_lua.checkstack = (void *)(image + 0x31b090);
  g_camera_lua.type = (void *)(image + 0x31b280);
  g_camera_lua.tolstring = (void *)(image + 0x31b3b0);
  g_camera_lua.loadbuffer = (void *)(image + 0x31e210);
  g_camera_lua.pcall = (void *)(image + 0x31c3d0);
}
#endif
