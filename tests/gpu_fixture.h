#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t (*draw_primitive_uniform_color_fn)(void *,uint32_t,const float *,const float *,const uint32_t *,uint32_t,const float *);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vb,*cb;
static BYTE state[0x190];
static void *override_value;
static BOOL scene;
static unsigned calls,fail_at;
static float captured_lines[32752][4];
static unsigned captured_line_count;
static uint64_t draw(void *native,uint32_t primitive,const float *vertices,const float *color,
    const uint32_t *indices,uint32_t count,const float *matrix) {
  CHECK(native==device&&(primitive==1||primitive==2)&&!indices&&count<=4095&&count%(primitive==2?3:2)==0);
  if(!scene)CHECK(!override_value);
  calls++;if(fail_at&&calls==fail_at)return 1;
  if(primitive==1) {
    CHECK(captured_line_count+count<=32752);
    memcpy(captured_lines+captured_line_count,vertices,count*16);captured_line_count+=count;
  }
  ID3D11RasterizerState *r=NULL;ID3D11DepthStencilState *d=NULL;ID3D11BlendState *b=NULL;
  CHECK(SUCCEEDED(ID3D11Device_CreateRasterizerState(device,(void *)(state+4),&r)));
  CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,(void *)(state+0x2c),&d)));
  CHECK(SUCCEEDED(ID3D11Device_CreateBlendState(device,(void *)(state+0x60),&b)));
  ID3D11DeviceContext_RSSetState(context,r);ID3D11DeviceContext_OMSetDepthStencilState(context,d,state[0x188]);
  ID3D11DeviceContext_OMSetBlendState(context,b,NULL,~0u);
  HV_COM_RELEASE(r);HV_COM_RELEASE(d);HV_COM_RELEASE(b);
  D3D11_MAPPED_SUBRESOURCE mapped;CHECK(SUCCEEDED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)vb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)));
  memcpy(mapped.pData,vertices,count*16);ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)vb,0);
  float constants[20];memcpy(constants,matrix,64);memcpy(constants+16,color,16);
  ID3D11DeviceContext_UpdateSubresource(context,(ID3D11Resource *)cb,0,NULL,constants,0,0);
  UINT stride=16,offset=0;ID3D11DeviceContext_IASetInputLayout(context,layout);
  ID3D11DeviceContext_IASetVertexBuffers(context,0,1,&vb,&stride,&offset);
  ID3D11DeviceContext_IASetPrimitiveTopology(context,primitive==2?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
  ID3D11DeviceContext_VSSetShader(context,vs,NULL,0);ID3D11DeviceContext_VSSetConstantBuffers(context,0,1,&cb);
  ID3D11DeviceContext_PSSetShader(context,ps,NULL,0);
  ID3D11DeviceContext_Draw(context,count,0);return 0;
}
static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
static const float perspective[16]={1,0,0,0,0,1,0,0,0,0,10.0f/9,1,0,0,-1.0f/9,0};
static void shader_setup(void) {
  HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");CHECK(compiler);
  pD3DCompile compile=(pD3DCompile)(void *)GetProcAddress(compiler,"D3DCompile");CHECK(compile);
  const char *hlsl="cbuffer Params:register(b0){float4x4 transform;float4 color;}"
    "struct V{float4 pos:SV_Position;float4 color:COLOR0;};"
    "V vertex(float4 p:POSITION){V o;o.pos=mul(transform,p);o.color=color;return o;}"
    "float4 pixel(V v):SV_Target{return v.color;}";
  ID3DBlob *v=NULL,*p=NULL,*errors=NULL;
  HRESULT result=compile(hlsl,strlen(hlsl),"test",NULL,NULL,"vertex","vs_4_0",0,0,&v,&errors);
  if(FAILED(result)&&errors)fprintf(stderr,"Shader: %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
  CHECK(SUCCEEDED(result));HV_COM_RELEASE(errors);
  CHECK(SUCCEEDED(compile(hlsl,strlen(hlsl),"test",NULL,NULL,"pixel","ps_4_0",0,0,&p,&errors)));HV_COM_RELEASE(errors);
  CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),NULL,&vs)));
  CHECK(SUCCEEDED(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(p),ID3D10Blob_GetBufferSize(p),NULL,&ps)));
  D3D11_INPUT_ELEMENT_DESC element={.SemanticName="POSITION",.Format=DXGI_FORMAT_R32G32B32A32_FLOAT,.InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA};
  CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(device,&element,1,ID3D10Blob_GetBufferPointer(v),ID3D10Blob_GetBufferSize(v),&layout)));
  HV_COM_RELEASE(v);HV_COM_RELEASE(p);FreeLibrary(compiler);
  D3D11_BUFFER_DESC desc={.ByteWidth=4095*16,.Usage=D3D11_USAGE_DYNAMIC,.BindFlags=D3D11_BIND_VERTEX_BUFFER,.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE};
  CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&vb)));
  desc=(D3D11_BUFFER_DESC){.ByteWidth=80,.Usage=D3D11_USAGE_DEFAULT,.BindFlags=D3D11_BIND_CONSTANT_BUFFER};
  CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&cb)));
}
typedef struct targets {ID3D11Texture2D *color,*depth;ID3D11RenderTargetView *rtv;ID3D11DepthStencilView *dsv;unsigned width;} targets;
typedef struct pipeline {ID3D11RasterizerState *raster;ID3D11DepthStencilState *depth;
  ID3D11BlendState *blend;UINT reference,mask;float factors[4];} pipeline;
static pipeline capture_pipeline(void) {
  pipeline p={0};ID3D11DeviceContext_RSGetState(context,&p.raster);
  ID3D11DeviceContext_OMGetDepthStencilState(context,&p.depth,&p.reference);
  ID3D11DeviceContext_OMGetBlendState(context,&p.blend,p.factors,&p.mask);return p;
}
static void check_pipeline(const pipeline *p) {
  pipeline after=capture_pipeline();
  CHECK(after.raster==p->raster&&after.depth==p->depth&&after.blend==p->blend&&after.reference==p->reference&&
    after.mask==p->mask&&!memcmp(after.factors,p->factors,sizeof(p->factors)));
  HV_COM_RELEASE(after.raster);HV_COM_RELEASE(after.depth);HV_COM_RELEASE(after.blend);
}
static targets make_targets(unsigned width,BOOL stencil) {
  targets t={.width=width};D3D11_TEXTURE2D_DESC d={.Width=width,.Height=64,.MipLevels=1,.ArraySize=1,
    .Format=DXGI_FORMAT_R8G8B8A8_UNORM,.SampleDesc={1,0},.Usage=D3D11_USAGE_DEFAULT,.BindFlags=D3D11_BIND_RENDER_TARGET};
  CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&d,NULL,&t.color)));
  CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)t.color,NULL,&t.rtv)));
  d.Format=stencil?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&d,NULL,&t.depth)));
  D3D11_DEPTH_STENCIL_VIEW_DESC v={.Format=stencil?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_D32_FLOAT,.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D};
  CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilView(device,(ID3D11Resource *)t.depth,&v,&t.dsv)));
  ID3D11DeviceContext_OMSetRenderTargets(context,1,&t.rtv,t.dsv);
  D3D11_VIEWPORT viewport={.Width=(float)width,.Height=64,.MaxDepth=1};ID3D11DeviceContext_RSSetViewports(context,1,&viewport);
  ID3D11DeviceContext_ClearDepthStencilView(context,t.dsv,D3D11_CLEAR_DEPTH|(stencil?D3D11_CLEAR_STENCIL:0),1,0x5a);
  memset(state,0,sizeof(state));
  *(D3D11_RASTERIZER_DESC *)(state+4)=(D3D11_RASTERIZER_DESC){
    .FillMode=D3D11_FILL_SOLID,.CullMode=D3D11_CULL_NONE,.DepthClipEnable=TRUE};
  *(D3D11_DEPTH_STENCIL_DESC *)(state+0x2c)=(D3D11_DEPTH_STENCIL_DESC){
    .StencilReadMask=255,.StencilWriteMask=255,
    .FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS},
    .BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS}};
  D3D11_DEPTH_STENCIL_DESC *ds=(void *)(state+0x2c);ds->DepthEnable=TRUE;ds->DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
  ds->DepthFunc=D3D11_COMPARISON_ALWAYS;ds->StencilEnable=FALSE;
  D3D11_BLEND_DESC *bs=(void *)(state+0x60);bs->RenderTarget[0].BlendEnable=FALSE;bs->RenderTarget[0].RenderTargetWriteMask=15;
  const float color[4]={.2f,.3f,.4f,.8f},depths[4]={.1f,.5f,.9f,1};scene=TRUE;
  for(unsigned i=0;i<4;i++) {
    float left=-1+.5f*i,right=left+.5f,z=depths[i];
    const float quad[24]={left,-1,z,1,left,1,z,1,right,1,z,1,left,-1,z,1,right,1,z,1,right,-1,z,1};
    CHECK(!draw(device,2,quad,color,NULL,6,identity));
  }
  scene=FALSE;
  return t;
}
static void drop_targets(targets *t) {
  ID3D11DeviceContext_OMSetRenderTargets(context,0,NULL,NULL);
  HV_COM_RELEASE(t->rtv);HV_COM_RELEASE(t->dsv);HV_COM_RELEASE(t->color);HV_COM_RELEASE(t->depth);
}
static void read_texture(ID3D11Texture2D *texture,BYTE *bytes) {
  D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture,&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging=NULL;CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging)));
  ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)texture);
  D3D11_MAPPED_SUBRESOURCE mapped;CHECK(SUCCEEDED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
  for(unsigned y=0;y<64;y++)memcpy(bytes+y*desc.Width*4,(BYTE *)mapped.pData+y*mapped.RowPitch,desc.Width*4);
  ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);HV_COM_RELEASE(staging);
}
static const unsigned indices[36]={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
static void box(float mesh[36][3],float near_depth,float far_depth,BOOL reverse) {
  float points[8][3]={{-2,-2,near_depth},{2,-2,near_depth},{2,2,near_depth},{-2,2,near_depth},
    {-2,-2,far_depth},{2,-2,far_depth},{2,2,far_depth},{-2,2,far_depth}};
  for(unsigned i=0;i<36;i++)memcpy(mesh[i],points[indices[i/3*3+(reverse?2-i%3:i%3)]],12);
}
