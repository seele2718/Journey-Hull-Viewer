#ifndef HV_PICK_MATH_H
#define HV_PICK_MATH_H
#include <math.h>
/* Column-major world-to-clip, matching the matrix passed to the native line
 * renderer. Clip in homogeneous space before perspective division. */
static void hv_project(const float p[3], const float m[16], float q[4]) {
  for (int k=0;k<4;k++) q[k]=m[k]*p[0]+m[4+k]*p[1]+m[8+k]*p[2]+m[12+k];
}
static float hv_plane(const float p[4], int plane) {
  switch(plane) {
    case 0:return p[3]+p[0];case 1:return p[3]-p[0];
    case 2:return p[3]+p[1];case 3:return p[3]-p[1];
    case 4:return p[2];default:return p[3]-p[2];
  }
}
static int hv_clip_edge(float a[4], float b[4]) {
  for(int i=0;i<4;i++) if(!isfinite(a[i])||!isfinite(b[i]))return 0;
  for(int plane=0;plane<6;plane++) {
    float x=hv_plane(a,plane),y=hv_plane(b,plane);
    if(x<0&&y<0)return 0;
    if(x<0||y<0) {
      float t=x/(x-y),p[4];for(int k=0;k<4;k++)p[k]=a[k]+t*(b[k]-a[k]);
      for(int k=0;k<4;k++)if(x<0)a[k]=p[k];else b[k]=p[k];
    }
  }
  return a[3]>1e-5f&&b[3]>1e-5f;
}
static float hv_edge_distance(const float start[3],const float end[3],const float m[16],
                              float x,float y,unsigned width,unsigned height,float *depth) {
  float a[4],b[4];hv_project(start,m,a);hv_project(end,m,b);
  if(!hv_clip_edge(a,b))return 1e20f;
  float ax=(a[0]/a[3]+1)*width*.5f,ay=(1-a[1]/a[3])*height*.5f;
  float bx=(b[0]/b[3]+1)*width*.5f,by=(1-b[1]/b[3])*height*.5f;
  float dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;
  float t=len>1e-10f?((x-ax)*dx+(y-ay)*dy)/len:0;
  if(t<0)t=0;
  if(t>1)t=1;
  dx=x-(ax+t*dx);dy=y-(ay+t*dy);
  *depth=a[2]/a[3]+t*(b[2]/b[3]-a[2]/a[3]);
  return sqrtf(dx*dx+dy*dy);
}
/* Exact projected triangle coverage, clipped before division. This selects
 * hull faces, not a guessed bounding box or only the thin wireframe edges. */
static int hv_triangle_depth(const float a[3],const float b[3],const float c[3],
                             const float m[16],float x,float y,float *depth) {
  float p[16][4],q[16][4];int count=3;
  hv_project(a,m,p[0]);hv_project(b,m,p[1]);hv_project(c,m,p[2]);
  for(int i=0;i<3;i++)for(int k=0;k<4;k++)if(!isfinite(p[i][k]))return 0;
  for(int plane=0;plane<6;plane++) {
    int n=0;
    for(int i=0;i<count;i++) {
      float *s=p[(i+count-1)%count],*e=p[i];float ds=hv_plane(s,plane),de=hv_plane(e,plane);
      if((ds<0)!=(de<0)) {
        if(n>=16)return 0;
        float t=ds/(ds-de);for(int k=0;k<4;k++)q[n][k]=s[k]+t*(e[k]-s[k]);n++;
      }
      if(de>=0){if(n>=16)return 0;for(int k=0;k<4;k++)q[n][k]=e[k];n++;}
    }
    if(n<3)return 0;
    count=n;for(int i=0;i<count;i++)for(int k=0;k<4;k++)p[i][k]=q[i][k];
  }
  int hit=0;float closest=1;
  for(int i=1;i+1<count;i++) {
    const float *v[3]={p[0],p[i],p[i+1]};float u[3][3];int valid=1;
    for(int j=0;j<3;j++) {
      if(v[j][3]<=1e-5f){valid=0;break;}
      for(int k=0;k<3;k++)u[j][k]=v[j][k]/v[j][3];
    }
    if(!valid)continue;
    float denominator=(u[1][1]-u[2][1])*(u[0][0]-u[2][0])+(u[2][0]-u[1][0])*(u[0][1]-u[2][1]);
    if(fabsf(denominator)<1e-12f)continue;
    float s=((u[1][1]-u[2][1])*(x-u[2][0])+(u[2][0]-u[1][0])*(y-u[2][1]))/denominator;
    float t=((u[2][1]-u[0][1])*(x-u[2][0])+(u[0][0]-u[2][0])*(y-u[2][1]))/denominator;
    if(s<-.00001f||t<-.00001f||s+t>1.00001f)continue;
    float z=s*u[0][2]+t*u[1][2]+(1-s-t)*u[2][2];
    if(!isfinite(z)||z<-.00001f||z>1.00001f)continue;
    if(!hit||z<closest){closest=z;hit=1;}
  }
  if(hit)*depth=closest;
  return hit;
}
#endif
