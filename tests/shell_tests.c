/* Own off-screen D3D resources, never a game attachment or screen capture.
 * Exercise the production shell submission with the same small GPU adapter
 * independent of the game. The adapter is not game code. */
#include "../src/renderer.c"
#include "gpu_fixture.h"

static BYTE job_device[0x1000];
static uint64_t queued_draw_adapter(void *native,uint32_t primitive,const float *vertices,
    const float *color,const uint32_t *at,uint32_t count,const float *matrix) {
  CHECK(native==job_device);memcpy(state,job_device+0x730,sizeof(state));
  /* Additional bindings changed by the native immediate helper, independent
   * of the fixture's own small drawing shader. */
  ID3D11Buffer *empty_buffers[2]={NULL,NULL};UINT zero[2]={0,0};
  ID3D11DeviceContext_IASetVertexBuffers(context,1,2,empty_buffers,zero,zero);
  ID3D11Buffer *empty_cb=NULL;ID3D11ShaderResourceView *empty_resource=NULL;
  ID3D11SamplerState *empty_sampler=NULL;
  ID3D11DeviceContext_PSSetConstantBuffers(context,0,1,&empty_cb);
  ID3D11DeviceContext_PSSetShaderResources(context,0,1,&empty_resource);
  ID3D11DeviceContext_PSSetSamplers(context,0,1,&empty_sampler);
  ID3D11DeviceContext_GSSetShader(context,NULL,NULL,0);
  ID3D11DeviceContext_CSSetShader(context,NULL,NULL,0);
  return draw(device,primitive,vertices,color,at,count,matrix);
}
static uint64_t queued_prepare_adapter(void *immediate,void *native,const void *source) {
  CHECK(native==job_device&&immediate==job_device+0x4e0&&source==job_device+0x730);
  memcpy(state,source,sizeof(state));return 0;
}
static void queued_gpu_shell(void) {
  targets t=make_targets(64,TRUE);hull_render_snapshot *s=&g_hull_snapshots[0];
  memset(s,0,sizeof(*s));s->fill_enabled=s->fill_selected=TRUE;s->opacity=.25f;s->item_count=1;
  s->triangle_vertices=s->items[0].triangle_count=36;box(s->triangles,.3f,.7f,FALSE);
  for(unsigned i=0;i<36;i++)for(unsigned k=0;k<2;k++)s->triangles[i][k]*=.375f;
  BYTE before[64*64*4],after[sizeof(before)],db[sizeof(before)],da[sizeof(before)],saved[sizeof(state)];
  read_texture(t.color,before);read_texture(t.depth,db);memcpy(saved,state,sizeof(state));
  ID3D11Buffer *sentinel_buffers[3]={vb,vb,vb};UINT strides[3]={16,32,48},offsets[3]={0,16,32};
  ID3D11DeviceContext_IASetVertexBuffers(context,0,3,sentinel_buffers,strides,offsets);
  ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
  ID3D11DeviceContext_PSSetConstantBuffers(context,0,1,&cb);
  ID3D11DeviceContext_VSSetShader(context,NULL,NULL,0);
  ID3D11DeviceContext_PSSetShader(context,NULL,NULL,0);
  ID3D11SamplerState *sentinel_sampler=NULL;
  D3D11_SAMPLER_DESC sampler_desc={.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT,
    .AddressU=D3D11_TEXTURE_ADDRESS_CLAMP,.AddressV=D3D11_TEXTURE_ADDRESS_CLAMP,
    .AddressW=D3D11_TEXTURE_ADDRESS_CLAMP,.MaxLOD=D3D11_FLOAT32_MAX};
  CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(device,&sampler_desc,&sentinel_sampler)));
  ID3D11DeviceContext_PSSetSamplers(context,0,1,&sentinel_sampler);
  face_pipeline saved_bindings;capture_face_pipeline(context,&saved_bindings);
  pipeline original=capture_pipeline();memset(job_device,0,sizeof(job_device));
  *(void **)(job_device+0xa8)=context;*(void **)(job_device+0x3e0)=t.rtv;*(void **)(job_device+0x420)=t.dsv;
  memcpy(job_device+0x730,state,sizeof(state));face_job *job=&g_face_jobs[0];memset(job,0,sizeof(*job));
  memcpy(job->matrix,identity,64);job->style=(hull_shell_style){
    .raster={.DepthClipEnable=TRUE},.depth={.DepthEnable=TRUE,.DepthFunc=D3D11_COMPARISON_LESS_EQUAL}};
  BYTE outline_scratch[64];memset(g_hull_clip_positions,0x56,sizeof(outline_scratch));
  memcpy(outline_scratch,g_hull_clip_positions,sizeof(outline_scratch));
  g_hull_snapshot_ready=1;g_hull_snapshot_readers[0]=1;job->state=2;override_value=s;calls=0;
  execute_face_job(job,job_device,queued_draw_adapter,queued_prepare_adapter,&override_value);
  CHECK(!job->state&&!g_hull_snapshot_readers[0]&&calls==1&&override_value==s);
  CHECK(!memcmp(job_device+0x730,saved,sizeof(state))&&!memcmp(outline_scratch,g_hull_clip_positions,sizeof(outline_scratch)));
  face_pipeline after_bindings;capture_face_pipeline(context,&after_bindings);
  CHECK(!memcmp(&saved_bindings,&after_bindings,sizeof(saved_bindings)));release_face_pipeline(&after_bindings);
  check_pipeline(&original);read_texture(t.color,after);read_texture(t.depth,da);CHECK(!memcmp(db,da,sizeof(db)));
  CHECK(!memcmp(before+(32*64+12)*4,after+(32*64+12)*4,4)); /* Foreground stays unchanged. */
  CHECK(memcmp(before+(32*64+50)*4,after+(32*64+50)*4,3)); /* Empty-depth background sees walls. */
  execute_face_job(job,job_device,queued_draw_adapter,queued_prepare_adapter,&override_value);CHECK(calls==1);
  job->state=2;g_hull_snapshot_readers[0]=1;fail_at=calls+1;
  execute_face_job(job,job_device,queued_draw_adapter,queued_prepare_adapter,&override_value);fail_at=0;
  CHECK(!job->state&&!g_hull_snapshot_readers[0]&&calls==2);
  capture_face_pipeline(context,&after_bindings);
  CHECK(!memcmp(&saved_bindings,&after_bindings,sizeof(saved_bindings)));release_face_pipeline(&after_bindings);
  for(unsigned mode=0;mode<3;mode++) {
    job->state=2;g_hull_snapshot_readers[0]=1;g_hull_snapshot_ready=mode!=0;
    if(mode==1)AcquireSRWLockExclusive(&g_native_draw_lock);
    if(mode==2)*(void **)(job_device+0x420)=NULL;
    execute_face_job(job,job_device,queued_draw_adapter,queued_prepare_adapter,&override_value);
    if(mode==1)ReleaseSRWLockExclusive(&g_native_draw_lock);
    CHECK(!job->state&&!g_hull_snapshot_readers[0]&&calls==2);
  }
  g_hull_snapshot_ready=0;HV_COM_RELEASE(original.raster);HV_COM_RELEASE(original.depth);HV_COM_RELEASE(original.blend);
  release_face_pipeline(&saved_bindings);HV_COM_RELEASE(sentinel_sampler);
  drop_targets(&t);puts("PASS: production queued job draws real GPU shell after deferred target binding; sky/foreground pixels, depth/alpha/pipeline/source/outline scratch preserved; hide/busy/missing-target/duplicate completion releases once");
}
static unsigned shell_layers(unsigned x,unsigned y,BOOL projective,
                             float near_depth,float far_depth,float extent,unsigned depth_mode) {
  double px=2*(x+.5)/64-1,py=1-2*(y+.5)/64;
  double origin[3]={projective?0:px,projective?0:py,0};
  double direction[3]={projective?px:0,projective?py:0,1};
  double low[3]={-extent,-extent,near_depth},high[3]={extent,extent,far_depth};
  double entry=-1e30,exit=1e30;
  for(unsigned i=0;i<3;i++) {
    if(direction[i]==0) {
      if(origin[i]<low[i]||origin[i]>high[i])return 0;
    } else {
      double a=(low[i]-origin[i])/direction[i],b=(high[i]-origin[i])/direction[i];
      if(a>b){double temporary=a;a=b;b=temporary;}
      if(a>entry)entry=a;
      if(b<exit)exit=b;
    }
  }
  if(entry>=exit)return 0;
  if(depth_mode==4)return 0; /* Native read-only stencil comparison rejects. */
  const double depths[4]={.1,.5,.9,1};unsigned hits=0;
  double walls[2]={entry,exit};
  for(unsigned i=0;i<2;i++) {
    double z=walls[i];
    if(z<(projective?.1:0)||z>1)continue; /* Ordinary near/far clipping. */
    double depth=projective?(10.0/9-1.0/(9*z)):z;
    /* mode1 uses reversed depth, mode2 disables scene depth, like an overlay.
     * Both differ from the retained LESS_EQUAL source descriptor. */
    if(depth_mode==1)depth=1-depth;
    double scene_depth=depth_mode==1?1-depths[x/16]:depths[x/16];
    if(depth_mode==2||(depth_mode==1?depth>=scene_depth-1e-6:depth<=scene_depth+1e-6))hits++;
  }
  return hits;
}
/* Reproduce the verified Fog_fs RGB operation:
 * OUT.rgb = lerp(colorTap.rgb, fog.rgb, fog.a).
 * Piecewise fog amounts stand in for the depth-derived ComputeFog result;
 * this is not a claim to execute Journey's compiled fog shader. */
static void fog_composite(void) {
  BYTE saved[sizeof(state)];memcpy(saved,state,sizeof(state));hull_shell_style style={
    .raster={.DepthClipEnable=TRUE},.depth={.DepthFunc=D3D11_COMPARISON_ALWAYS}};
  hull_surface_state(state,&style);
  const float amounts[4]={.1f,.5f,1,1};scene=TRUE;
  for(unsigned i=0;i<4;i++) {
    float left=-1+.5f*i,right=left+.5f;
    float color[4]={.1f,.2f,.3f,amounts[i]};
    const float quad[24]={left,-1,0,1,left,1,0,1,right,1,0,1,left,-1,0,1,right,1,0,1,right,-1,0,1};
    CHECK(!draw(device,2,quad,color,NULL,6,identity));
  }
  scene=FALSE;memcpy(state,saved,sizeof(state));
}
static void post_fog_shell(void) {
  targets t=make_targets(64,TRUE);hull_render_snapshot *s=&g_hull_snapshots[0];
  memset(s,0,sizeof(*s));s->fill_enabled=s->fill_selected=TRUE;s->opacity=.25f;s->item_count=1;
  s->triangle_vertices=s->items[0].triangle_count=36;
  lstrcpyA(s->items[0].info.name,"11111111111111111111111111111111");
  box(s->triangles,.3f,.7f,FALSE);
  for(unsigned i=0;i<36;i++)for(unsigned j=0;j<2;j++)s->triangles[i][j]*=.375f;
  D3D11_DEPTH_STENCIL_DESC desc=*(D3D11_DEPTH_STENCIL_DESC *)(state+0x2c);
  desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;desc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
  ID3D11DepthStencilState *native=NULL;
  CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,&desc,&native)));
  ID3D11DeviceContext_OMSetDepthStencilState(context,native,0);HV_COM_RELEASE(native);
  hull_shell_style outline_style=read_hull_shell_style(context);
  BYTE early[64*64*4],fog[sizeof(early)],late[sizeof(early)],db[sizeof(early)],da[sizeof(early)];
  read_texture(t.depth,db);override_value=s;
  CHECK(draw_hull_shell(s,s->items[0].info.name,device,context,state,identity,draw,&override_value,&outline_style,g_hull_clip_positions)==36);
  fog_composite();read_texture(t.color,early);
  const float clear[4]={.2f,.3f,.4f,.8f};ID3D11DeviceContext_ClearRenderTargetView(context,t.rtv,clear);
  fog_composite();read_texture(t.color,fog);pipeline fog_pipeline=capture_pipeline();
  BYTE saved[sizeof(state)];memcpy(saved,state,sizeof(state));
  CHECK(draw_hull_shell(s,s->items[0].info.name,device,context,state,identity,draw,&override_value,&outline_style,g_hull_clip_positions)==36);
  check_pipeline(&fog_pipeline);CHECK(!memcmp(state,saved,sizeof(state))&&override_value==s);
  read_texture(t.color,late);read_texture(t.depth,da);CHECK(!memcmp(db,da,sizeof(db)));
  unsigned lost=0,visible=0;const float tint[3]={.12f,.14f,.17f};
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
    unsigned p=(y*64+x)*4,layers=shell_layers(x,y,FALSE,.3f,.7f,.75f,0);
    for(unsigned c=0;c<3;c++) {
      double expected=fog[p+c];
      for(unsigned n=0;n<layers;n++)expected=.75*expected+.25*tint[c]*255;
      CHECK(fabs(expected-late[p+c])<=2);
    }
    CHECK(late[p+3]==fog[p+3]);
    if(layers&&x>=32) {
      if(!memcmp(early+p,fog+p,4))lost++;
      if(memcmp(late+p,fog+p,4))visible++;
    }
  }
  CHECK(lost>100&&visible==lost);
  HV_COM_RELEASE(fog_pipeline.raster);HV_COM_RELEASE(fog_pipeline.depth);HV_COM_RELEASE(fog_pipeline.blend);
  drop_targets(&t);
  puts("PASS: post-fog shell remains visible over depth-derived full-fog backgrounds; pre-fog shell erased there by verified RGB lerp; foreground occlusion and pipeline/depth/alpha preserved");
}
int main(void) {
  HMODULE runtime=LoadLibraryW(L"d3d11.dll");CHECK(runtime);
  PFN_D3D11_CREATE_DEVICE create=(PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(runtime,"D3D11CreateDevice");CHECK(create);
  D3D_FEATURE_LEVEL actual;
  const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1};
  HRESULT result=create(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,levels,2,D3D11_SDK_VERSION,&device,&actual,&context);
  if(FAILED(result))result=create(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,levels,2,D3D11_SDK_VERSION,&device,&actual,&context);
  CHECK(SUCCEEDED(result));printf("Shell D3D feature level %x\n",actual);shader_setup();
  hull_render_snapshot *s=&g_hull_snapshots[0];
  g_fill_supported=TRUE;
  for(unsigned depth_mode=0;depth_mode<6;depth_mode++)for(unsigned mode=0;mode<10;mode++) {
    BOOL projective=mode>=4,inside=(mode%4)>=2,reverse=(mode%2)!=0;
    float near_depth=inside?-.2f:.3f,far_depth=mode>=8?1.3f:.7f;
    float extent=projective?.35f:.75f;
    targets t=make_targets(64,TRUE);
    float matrix[16];memcpy(matrix,projective?perspective:identity,sizeof(matrix));
    if(depth_mode==1) {
      /* Complement clip Z, not W; reverse both scene and hull projection. */
      for(unsigned i=0;i<4;i++)matrix[i*4+2]=matrix[i*4+3]-matrix[i*4+2];
      const float color[4]={.2f,.3f,.4f,.8f},depths[4]={.1f,.5f,.9f,1};scene=TRUE;
      for(unsigned i=0;i<4;i++) {
        float left=-1+.5f*i,right=left+.5f,z=1-depths[i];
        const float quad[24]={left,-1,z,1,left,1,z,1,right,1,z,1,left,-1,z,1,right,1,z,1,right,-1,z,1};
        CHECK(!draw(device,2,quad,color,NULL,6,identity));
      }
      scene=FALSE;
    }
    BYTE before[64*64*4],after[sizeof(before)],depth_before[sizeof(before)],depth_after[sizeof(before)];
    read_texture(t.color,before);read_texture(t.depth,depth_before);
    memset(s,0,sizeof(*s));s->fill_enabled=TRUE;s->fill_selected=TRUE;s->opacity=.25f;
    s->item_count=1;s->triangle_vertices=s->items[0].triangle_count=36;
    s->items[0].color=2;lstrcpyA(s->items[0].info.name,"11111111111111111111111111111111");
    box(s->triangles,near_depth,far_depth,reverse);
    for(unsigned i=0;i<36;i++)for(unsigned axis=0;axis<2;axis++)s->triangles[i][axis]*=extent/2;
    /* The shell must not depend on exterior orientation or camera membership. */
    BYTE saved[sizeof(state)];memcpy(saved,state,sizeof(state));
    /* Simulate the original outline's REALIZED state, including a pass
     * override. Production must copy this, not source+2c. */
    D3D11_DEPTH_STENCIL_DESC native_depth=*(D3D11_DEPTH_STENCIL_DESC *)(state+0x2c);
    native_depth.DepthEnable=depth_mode!=2;
    native_depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    native_depth.DepthFunc=depth_mode==1?D3D11_COMPARISON_GREATER_EQUAL:D3D11_COMPARISON_LESS_EQUAL;
    if(depth_mode==3||depth_mode==4) {
      native_depth.StencilEnable=TRUE;native_depth.StencilReadMask=native_depth.StencilWriteMask=255;
      native_depth.FrontFace=native_depth.BackFace=(D3D11_DEPTH_STENCILOP_DESC){
        D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_INCR,D3D11_STENCIL_OP_DECR,D3D11_COMPARISON_EQUAL};
    }
    ID3D11DepthStencilState *native=NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,&native_depth,&native)));
    ID3D11DeviceContext_OMSetDepthStencilState(context,depth_mode==5?NULL:native,depth_mode==3?0x5a:0x63);HV_COM_RELEASE(native);
    if(depth_mode==5)ID3D11DeviceContext_RSSetState(context,NULL);
    pipeline original=capture_pipeline();
    if(depth_mode==1&&mode==0) {
      /* Forcing LESS_EQUAL with reversed scene depth makes the wall disappear
       * over clear depth and appear over foreground. This negative case checks
       * that the effective native depth convention is preserved. */
      const hull_shell_style forced={.raster={.DepthClipEnable=TRUE},
        .depth={.DepthEnable=TRUE,.DepthFunc=D3D11_COMPARISON_LESS_EQUAL}};
      BYTE wrong[sizeof(before)];override_value=s;calls=0;
      CHECK(draw_hull_faces(s,s->items[0].info.name,device,state,matrix,draw,&override_value,&forced,g_hull_clip_positions)==36);
      read_texture(t.color,wrong);unsigned wrong_foreground=0,wrong_sky=0;
      for(unsigned y=10;y<54;y++)for(unsigned x=0;x<64;x++) {
        unsigned p=(y*64+x)*4;
        if(memcmp(wrong+p,before+p,4)){if(x<16)wrong_foreground++;if(x>=48)wrong_sky++;}
      }
      CHECK(wrong_foreground>100&&!wrong_sky);
      const float clear[4]={.2f,.3f,.4f,.8f};ID3D11DeviceContext_ClearRenderTargetView(context,t.rtv,clear);
      ID3D11DeviceContext_RSSetState(context,original.raster);
      ID3D11DeviceContext_OMSetDepthStencilState(context,original.depth,original.reference);
      ID3D11DeviceContext_OMSetBlendState(context,original.blend,original.factors,original.mask);
      puts("PASS negative witness: forced LESS_EQUAL draws on foreground but vanishes against reversed clear depth");
    }
    override_value=s;calls=0;
    CHECK(draw_hull_shell(s,s->items[0].info.name,device,context,state,matrix,draw,&override_value,NULL,g_hull_clip_positions)==36);
    CHECK(calls==1&&override_value==s&&!memcmp(saved,state,sizeof(state)));
    check_pipeline(&original);
    read_texture(t.color,after);read_texture(t.depth,depth_after);
    CHECK(!memcmp(depth_before,depth_after,sizeof(depth_before)));
    unsigned tinted=0,empty_space=0,untouched=0;
    const float tint[3]={.12f,.14f,.17f};
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
      unsigned p=(y*64+x)*4;
      unsigned layers=shell_layers(x,y,projective,near_depth,far_depth,extent,depth_mode);
      CHECK(after[p+3]==before[p+3]);
      if(!layers){CHECK(!memcmp(before+p,after+p,4));untouched++;continue;}
      for(unsigned c=0;c<3;c++) {
        double expected=before[p+c];
        for(unsigned i=0;i<layers;i++)expected=expected*.75+tint[c]*255*.25;
        if(fabs(expected-after[p+c])>2) {
          fprintf(stderr,"shell depth%u mode%u pixel%u,%u c%u layers%u expected%.2f got%u\n",depth_mode,mode,x,y,c,layers,expected,after[p+c]);exit(1);
        }
      }
      tinted++;if(x>=48)empty_space++;
    }
    if(depth_mode==4)CHECK(!tinted&&untouched==4096);
    else CHECK(tinted>100);
    /* With depth disabled a close perspective wall can fill the entire view;
     * the independent per-pixel intersection oracle above still checks shape. */
    if(depth_mode!=2||!projective)CHECK(untouched>100);
    if(mode<4&&depth_mode!=4)CHECK(empty_space>0); /* Clear-depth background through a wall. */
    /* Error path and repeated frames restore the exact original bindings,
     * blend factors, stencil ref, source bytes and override pointer. */
    calls=0;fail_at=1;
    CHECK(!draw_hull_shell(s,s->items[0].info.name,device,context,state,matrix,draw,&override_value,NULL,g_hull_clip_positions));
    CHECK(override_value==s&&!memcmp(saved,state,sizeof(state)));check_pipeline(&original);fail_at=0;
    for(unsigned i=0;i<8;i++) {
      CHECK(draw_hull_shell(s,s->items[0].info.name,device,context,state,matrix,draw,&override_value,NULL,g_hull_clip_positions)==36);
      check_pipeline(&original);
    }
    HV_COM_RELEASE(original.raster);HV_COM_RELEASE(original.depth);HV_COM_RELEASE(original.blend);
    printf("shell depth%u mode%u: tinted%u untouched%u sky%u\n",depth_mode,mode,tinted,untouched,empty_space);
    drop_targets(&t);
  }
  post_fog_shell();queued_gpu_shell();
  ID3D11DeviceContext_ClearState(context);HV_COM_RELEASE(vs);HV_COM_RELEASE(ps);
  HV_COM_RELEASE(layout);HV_COM_RELEASE(vb);HV_COM_RELEASE(cb);HV_COM_RELEASE(context);HV_COM_RELEASE(device);FreeLibrary(runtime);
  puts("PASS: production 25-percent charcoal shell matches independent per-pixel ray/box blends with realized LESS_EQUAL, reversed GREATER_EQUAL, disabled/default depth and stencil accept/reject; inside/outside/both windings/perspective, original depth/stencil/alpha/pipeline/source/override preserved on success/failure/repeated frames; no grid");
  return 0;
}
