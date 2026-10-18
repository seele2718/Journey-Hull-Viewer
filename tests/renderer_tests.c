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
  CHECK(!verify_fill_guards(image));
  for(unsigned i=0;i<sizeof(k_fill_guards)/sizeof(*k_fill_guards);i++)
    memcpy(image+k_fill_guards[i].rva,k_fill_guards[i].bytes,k_fill_guards[i].size);
  CHECK(verify_fill_guards(image));
  for(unsigned i=0;i<sizeof(k_fill_guards)/sizeof(*k_fill_guards);i++) {
    image[k_fill_guards[i].rva]^=1;CHECK(!verify_fill_guards(image));
    CHECK(verify_image_guards(image));image[k_fill_guards[i].rva]^=1;
  }
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
  for (uint32_t action = 0; action < HV_CAM_MOVE; ++action) {
    CHECK(execute_camera((hv_lua *)state, action, &bits, message));
    CHECK(bits == 9 && lua_top == 5);
    CHECK(strstr(lua_source, camera_action_source(action)));
  }
  CHECK(!execute_camera((hv_lua *)state, HV_CAM_COUNT, &bits, message));
  float target[3]={123.5f,-4,800};
  CHECK(execute_camera_command((hv_lua *)state,HV_CAM_MOVE,target,&bits,message));
  CHECK(strstr(lua_source,"local x,y,z=123.5,-4,800;")&&strstr(lua_source,"UsingDebugCam()")&&strstr(lua_source,"SetXForm(k:rot(),{x,y,z},k:fov())"));
  target[0]=NAN;CHECK(!execute_camera_command((hv_lua *)state,HV_CAM_MOVE,target,&bits,message));
  target[0]=1000001;CHECK(!execute_camera_command((hv_lua *)state,HV_CAM_MOVE,target,&bits,message));
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
  hv_shared shared = {.magic=HV_MAGIC,.protocol=HV_PROTOCOL,.size=sizeof(hv_shared)};
  g_shared = &shared; g_mutex = CreateMutexW(NULL, FALSE, NULL); CHECK(g_mutex);
  shared.heartbeat=GetTickCount64();g_input_supported=TRUE;g_input_toggle=1;
  read_panel();CHECK(shared.inspect_mode==1&&g_input_requested&&g_input_listening);
  g_input_toggle=1;read_panel();CHECK(!shared.inspect_mode&&!g_input_requested);g_input_supported=FALSE;
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
  c.mode=HV_SELECTED;c.fill_faces=3;CHECK(!hv_config_valid(&c));c.fill_faces=2;CHECK(!hv_config_valid(&c));c.fill_faces=1;CHECK(hv_config_valid(&c));
  c.opacity=101;CHECK(!hv_config_valid(&c));c.opacity=100;CHECK(hv_config_valid(&c));c.opacity=0;CHECK(hv_config_valid(&c));

  BYTE game[0x200] = {0}, barn[0x10] = {0}, hull[0xd0] = {0};
  BYTE edges[] = {0, 1},triangles[]={0,1,2};float points[3][4] = {{1,2,3,1},{4,5,6,1},{2,4,6,1}};
  *(void **)(game + 0x110) = barn; *(void **)(barn + 8) = hull;
  *(void **)(hull + 0x60) = edges; *(void **)(hull + 0x68) = points;
  *(uint32_t *)(hull + 0x78) = 1; *(uint32_t *)(hull + 0x7c) = 3;
  *(void **)(hull+0x50)=triangles;*(uint32_t *)(hull+0x70)=1;
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
  /* A multiply classified hull matches ANY selected category. */
  shared.config.exclude_mask=0;
  for(unsigned types=1;types<64;types++)for(unsigned checked=1;checked<64;checked++) {
    *(uint32_t *)(hull+0x84)=types;shared.config.include_mask=checked;++shared.revision;
    read_panel();publish_all_hulls(game);CHECK(hull_count()==((types&checked)?1u:0u));
  }
  *(uint32_t *)(hull+0x84)=16;
  shared.config.mode = HV_SELECTED; shared.config.exclude_mask = 0;
  CHECK(hv_parse_names("example", &shared.config)); ++shared.revision;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 1);
  CHECK(g_active_matches == 1 && g_inactive_matches == 0);
  shared.config.fill_faces=1;shared.config.opacity=70;++shared.revision;read_panel();publish_all_hulls(game);
  unsigned locked;hull_render_snapshot *filled=lock_hull_snapshot(&locked);CHECK(filled);
  CHECK(filled->fill_selected&&filled->fill_enabled&&filled->item_count==1&&filled->triangle_vertices==3);
  CHECK(fabsf(filled->opacity-.7f)<.0001f);
  InterlockedDecrement(&g_hull_snapshot_readers[locked]);
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
  shared.inspect_mode=1;g_input_mode=1;
  read_panel(); publish_all_hulls(game); CHECK(hull_count() == 0);
  CHECK(!shared.inspect_mode&&!g_input_requested);update_inspect_input(FALSE);CHECK(!g_input_mode);
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
  hv_shared view_shared={.heartbeat=GetTickCount64()};g_shared=&view_shared;g_mutex=CreateMutexW(NULL,FALSE,NULL);CHECK(g_mutex);
  *(float *)(camera+0x170+12*4)=10;*(float *)(camera+0x170+13*4)=20;*(float *)(camera+0x170+14*4)=30;
  g_hull_snapshot_ready=0;g_view_requested=1;publish_view_position(camera);
  CHECK(view_shared.view_position_time&&view_shared.view_position[0]==10&&view_shared.view_position[1]==20&&view_shared.view_position[2]==30);
  CloseHandle(g_mutex);g_mutex=NULL;g_shared=NULL;g_view_requested=0;
  *(float *)(camera + 0xc8) = 0; CHECK(!capture_pass_camera(camera, &snapshot));
}
static BYTE *fill_state;
static void *fill_override;
static unsigned fill_calls,fill_vertices,fill_fail;
static uint64_t fake_face_draw(void *device,uint32_t primitive,const float *vertices,
    const float *color,const uint32_t *indices,uint32_t count,const float *matrix) {
  CHECK(device==fill_state&&(primitive==1||primitive==2)&&count&&count<=4095&&!indices&&matrix);
  CHECK(primitive==2&&count%3==0&&color[3]==.25f);
  CHECK(color[0]==.12f&&color[1]==.14f&&color[2]==.17f);CHECK(!fill_override);
  const D3D11_RASTERIZER_DESC *r=(const D3D11_RASTERIZER_DESC *)(fill_state+4);
  const D3D11_DEPTH_STENCIL_DESC *d=(const D3D11_DEPTH_STENCIL_DESC *)(fill_state+0x2c);
  const D3D11_BLEND_DESC *b=(const D3D11_BLEND_DESC *)(fill_state+0x60);
  CHECK(r->FillMode==D3D11_FILL_SOLID&&r->CullMode==D3D11_CULL_NONE&&r->DepthClipEnable);
  CHECK(d->DepthEnable&&d->DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO&&d->DepthFunc==D3D11_COMPARISON_LESS_EQUAL);
  CHECK(!d->StencilEnable);
  CHECK(!b->AlphaToCoverageEnable&&!b->IndependentBlendEnable&&b->RenderTarget[0].BlendEnable);
  CHECK(b->RenderTarget[0].SrcBlend==D3D11_BLEND_SRC_ALPHA&&b->RenderTarget[0].DestBlend==D3D11_BLEND_INV_SRC_ALPHA);
  CHECK(b->RenderTarget[0].RenderTargetWriteMask==7); /* Preserve scene alpha. */
  for(unsigned i=0;i<count;i++)CHECK(vertices[i*4+3]==1);
  fill_calls++;fill_vertices+=count;return fill_fail;
}
static void face_drawing(void) {
  BYTE state[0x190]={0},before[0x190];memset(state,0xa5,0x168);memcpy(before,state,sizeof(state));
  fill_state=state;float matrix[16]={0};hull_render_snapshot *s=&g_hull_snapshots[0];
  memset(s,0,sizeof(*s));s->fill_enabled=TRUE;s->fill_selected=TRUE;s->opacity=.25f;s->triangle_vertices=3;s->item_count=64;
  for(unsigned i=0;i<k_hull_vertex_capacity;i++){s->triangles[i*3+1][0]=1;s->triangles[i*3+2][1]=1;}
  for(unsigned i=0;i<64;i++){s->items[i].color=2;s->items[i].triangle_count=3;snprintf(s->items[i].info.name,48,"%032u",i+1);}
  g_fill_supported=TRUE;fill_override=state;fill_calls=fill_vertices=0;
  const hull_shell_style style={.raster={.DepthClipEnable=TRUE},
    .depth={.DepthEnable=TRUE,.DepthFunc=D3D11_COMPARISON_LESS_EQUAL}};
  CHECK(draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==192);
  CHECK(fill_calls==1&&fill_vertices==192&&!memcmp(state,before,sizeof(state)));
  s->opacity=0;fill_calls=0;
  CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)&&!fill_calls);
  s->opacity=.25f;
  /* Reverse winding: the same triangle must draw from its other side too. */
  s->triangles[1][0]=-1;fill_calls=0;
  CHECK(draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==192&&fill_calls==1);
  s->triangles[1][0]=1;
  fill_calls=fill_vertices=0;
  CHECK(draw_hull_faces(s,s->items[0].info.name,state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==192);
  CHECK(fill_calls==2&&fill_vertices==192); /* Highlight never fills twice. */
  s->fill_selected=FALSE;fill_calls=0;CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)&&!fill_calls);
  CHECK(draw_hull_faces(s,s->items[0].info.name,state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==3&&fill_calls==1);
  s->fill_selected=TRUE;s->fill_enabled=FALSE;fill_calls=0;
  CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)&&!fill_calls);s->fill_enabled=TRUE;
  *(void **)(state+0x180)=state;CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions));*(void **)(state+0x180)=NULL;
  fill_fail=1;CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions));CHECK(!memcmp(state,before,sizeof(state))&&fill_override==state);fill_fail=0;
  s->item_count=1;s->items[0].triangle_count=4098;s->triangle_vertices=4098;fill_calls=fill_vertices=0;
  CHECK(draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==4098&&fill_calls==2);
  s->items[0].triangle_count=s->triangle_vertices=k_hull_vertex_capacity*3;fill_calls=fill_vertices=0;
  CHECK(draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)==65520&&fill_calls==16);
  CHECK(!memcmp(state,before,sizeof(state)));g_fill_supported=FALSE;
  fill_calls=0;CHECK(!draw_hull_faces(s,"",state,state,matrix,fake_face_draw,&fill_override,&style,g_hull_clip_positions)&&!fill_calls);
  fill_state=NULL;fill_override=NULL;
}
static void late_shell_handoff(void) {
  BYTE camera[0x1b0]={0},other[0x1b0]={0};memset(&g_late_shell,0,sizeof(g_late_shell));
  memset((void *)g_hull_snapshot_readers,0,sizeof(g_hull_snapshot_readers));
  CHECK(!late_shell_view_matches(camera,0));
  const float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  char highlight[48]="12345678901234567890123456789012";
  hull_shell_style style={0};
  for(unsigned round=0;round<10000;round++) {
    unsigned index=round%2,other_index=1-index,locked;
    InterlockedExchange(&g_hull_snapshot_ready,1);
    InterlockedExchange(&g_hull_published_index,(LONG)index);
    CHECK(lock_hull_snapshot(&locked)==&g_hull_snapshots[index]&&locked==index);
    retain_late_shell(index,camera,matrix,highlight,&style);
    CHECK(g_hull_snapshot_readers[index]==1&&late_shell_view_matches(camera,index));
    /* Publish a newer slot between passes. Do not substitute the new slot for
     * the retained outline snapshot. The pinned slot must
     * remain immutable, available to the late pass, with no producer waits. */
    InterlockedExchange(&g_hull_published_index,(LONG)other_index);
    CHECK(lock_hull_snapshot(&locked)==&g_hull_snapshots[other_index]);
    CHECK(locked!=g_late_shell.snapshot_index);
    InterlockedDecrement(&g_hull_snapshot_readers[locked]);
    CHECK(InterlockedCompareExchange(&g_hull_snapshot_readers[index],1,0)==1);
    CHECK(late_shell_view_matches(camera,g_late_shell.snapshot_index));
    CHECK(!late_shell_view_matches(other,index)&&!late_shell_view_matches(camera,other_index));
    camera[0x170]=1;CHECK(!late_shell_view_matches(camera,index));camera[0x170]=0;
    camera[0x40]=1;CHECK(!late_shell_view_matches(camera,index));camera[0x40]=0;
    discard_late_shell();discard_late_shell(); /* Consume/cancel exactly once. */
    CHECK(!late_shell_view_matches(camera,index)&&!g_hull_snapshot_readers[index]&&
      !g_hull_snapshot_readers[other_index]);
  }
  InterlockedExchange(&g_hull_snapshot_ready,0);
  puts("PASS: 10000 inter-pass publication changes retain the exact face snapshot; cancellation/mismatch release once, no waits");
  hv_shared shared={0};g_shared=&shared;g_mutex=CreateMutexW(NULL,FALSE,NULL);CHECK(g_mutex);
  publish_shell_status("Faces: post-fog.");CHECK(!strcmp(shared.face_status,"Faces: post-fog."));
  publish_shell_status("Faces: view changed; outlines only.");
  CHECK(!strcmp(shared.face_status,"Faces: view changed; outlines only."));
  CloseHandle(g_mutex);g_mutex=NULL;g_shared=NULL;
  memset(&g_late_shell,0,sizeof(g_late_shell));
}
typedef int (*integer_fn)(void);
static struct {void (*fn)(const native_callback_args *);void *user;} queued[8];
static unsigned queue_count;
static BOOL queue_error;
static uint64_t fake_enqueue(void *context,void (*fn)(const native_callback_args *),void *user) {
  CHECK(context==queued&&fn==face_job_callback&&user);
  if(queue_error)return 13;
  CHECK(queue_count<8);queued[queue_count].fn=fn;queued[queue_count++].user=user;return 0;
}
static void queued_handoff(void) {
  BYTE camera[0x1b0]={0};char name[48]="abcdefabcdefabcdefabcdefabcdefab";
  float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,12,34,56,1};hull_shell_style style={0};
  memset(g_face_jobs,0,sizeof(g_face_jobs));memset(&g_late_shell,0,sizeof(g_late_shell));
  memset((void *)g_hull_snapshot_readers,0,sizeof(g_hull_snapshot_readers));
  g_hull_snapshot_ready=1;g_hull_published_index=0;queue_count=0;queue_error=FALSE;
  for(unsigned i=0;i<4;i++) {
    unsigned locked;CHECK(lock_hull_snapshot(&locked)==g_hull_snapshots&&locked==0);
    retain_late_shell(locked,camera,matrix,name,&style);
    queue_late_hull_faces(queued,camera,fake_enqueue);
    CHECK(!g_late_shell.pending&&queue_count==i+1&&g_hull_snapshot_readers[0]==(LONG)i+1);
    CHECK(g_face_jobs[i].state==2&&!memcmp(g_face_jobs[i].matrix,matrix,64));
    /* The recording thread has NO realized scene target. No callback may be
     * executed here or infer visibility from the old device target cache. */
  }
  unsigned locked;CHECK(lock_hull_snapshot(&locked)==g_hull_snapshots);
  CHECK(g_hull_snapshot_readers[0]==5); /* Pending faces never exclude outlines. */
  retain_late_shell(locked,camera,matrix,name,&style);
  queue_late_hull_faces(queued,camera,fake_enqueue); /* Full pool: release, don't wait. */
  CHECK(queue_count==4&&!g_late_shell.pending&&g_hull_snapshot_readers[0]==4);
  CHECK(InterlockedCompareExchange(&g_hull_snapshot_readers[0],-1,0)==4);
  memset(camera,0xff,sizeof(camera));memset(&g_late_shell,0,sizeof(g_late_shell));
  for(unsigned i=0;i<4;i++) {
    face_job *job=queued[i].user;CHECK(job->state==2&&!memcmp(job->matrix,matrix,64));
    native_callback_args args={NULL,job};queued[i].fn(&args); /* Invalid device must cancel safely. */
    CHECK(!job->state&&g_hull_snapshot_readers[0]==3-(LONG)i);
    queued[i].fn(&args);CHECK(g_hull_snapshot_readers[0]==3-(LONG)i); /* No double release. */
  }
  memset(camera,0,sizeof(camera));queue_error=TRUE;
  CHECK(lock_hull_snapshot(&locked)==g_hull_snapshots);
  retain_late_shell(locked,camera,matrix,name,&style);queue_late_hull_faces(queued,camera,fake_enqueue);
  CHECK(!g_late_shell.pending&&!g_hull_snapshot_readers[0]&&!g_face_jobs[0].state&&queue_count==4);
  CHECK(InterlockedCompareExchange(&g_hull_snapshot_readers[0],-1,0)==0);
  CHECK(!lock_hull_snapshot(&locked));InterlockedExchange(&g_hull_snapshot_readers[0],0);
  g_hull_snapshot_ready=0;
  puts("PASS: deferred native callback owns immutable snapshot/matrix; four-job budget, outline co-readers, exclusive writer, delayed/stale/failing/duplicate completion safe; no immediate-target predicate at recording time");
}
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
static void picking(void) {
  hv_shared shared={0};
  g_shared=&shared;g_mutex=CreateMutexW(NULL,FALSE,NULL);CHECK(g_mutex);
  InterlockedExchange(&g_inspection_enabled,1);
  hull_render_snapshot *s=&g_hull_snapshots[0];
  s->level=4;s->item_count=2;
  for(unsigned i=0;i<2;i++) {
    memset(&s->items[i],0,sizeof(s->items[i]));
    lstrcpyA(s->items[i].info.name,i?"second":"first");
    s->items[i].info.mask=16;s->items[i].info.active=1;
    s->items[i].color=0;s->items[i].first=i*2;s->items[i].count=2;
    float a[3]={-.5f,0,i?.8f:.3f},b[3]={.5f,0,i?.8f:.3f};
    memcpy(s->positions[0][i*2],a,sizeof(a));memcpy(s->positions[0][i*2+1],b,sizeof(b));
  }
  const float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};char highlight[48]={0};
  shared.pick_request=10;shared.pick_level=4;shared.pick_width=1000;shared.pick_height=1000;
  shared.pick_x=.5f;shared.pick_y=.5f;shared.pick_time=shared.heartbeat=GetTickCount64();
  shared.highlight_level=4;lstrcpyA(shared.highlight,"first");
  pick_snapshot(s,m,highlight);
  CHECK(shared.pick_completed==10&&shared.pick_count==2);
  CHECK(!strcmp(shared.pick_hits[0].name,"first")&&!strcmp(highlight,"first"));
  CHECK(shared.pick_hits[0].depth<shared.pick_hits[1].depth);
  shared.pick_completed=0; /* Lost publication acknowledgement: do not recompute. */
  s->item_count=0;pick_snapshot(s,m,highlight);CHECK(shared.pick_completed==10&&shared.pick_count==2);
  shared.pick_request++;shared.pick_level=5;pick_snapshot(s,m,highlight);
  CHECK(shared.pick_completed==11&&shared.pick_count==0&&strstr(shared.pick_message,"Level changed"));
  shared.pick_request++;shared.pick_level=4;shared.pick_x=NAN;pick_snapshot(s,m,highlight);
  CHECK(shared.pick_count==0&&strstr(shared.pick_message,"Invalid"));
  shared.pick_request++;shared.pick_x=.5f;shared.pick_time=GetTickCount64()-2001;pick_snapshot(s,m,highlight);
  CHECK(shared.pick_count==0&&strstr(shared.pick_message,"expired"));
  /* A face-interior click well away from every outline must select the hull. */
  s->item_count=1;s->items[0].triangle_first=0;s->items[0].triangle_count=3;
  float face[3][3]={{-.8f,-.8f,.4f},{.8f,-.8f,.4f},{0,.8f,.4f}};memcpy(s->triangles,face,sizeof(face));
  shared.pick_request++;shared.pick_time=shared.heartbeat=GetTickCount64();
  pick_snapshot(s,m,highlight);CHECK(shared.pick_count==1&&shared.pick_hits[0].pixels==0);
  CloseHandle(g_mutex);g_mutex=NULL;g_shared=NULL;
  InterlockedExchange(&g_inspection_enabled,0);
}
int main(void) {
  guards(); protocol_and_hulls(); camera_commands(); late_hook();picking();face_drawing();late_shell_handoff();queued_handoff();
  puts("PASS: guards, filtering, snapshots, bounded transparent triangles/state restoration, camera actions, lease and20 concurrent late-hook rounds");
  return 0;
}
