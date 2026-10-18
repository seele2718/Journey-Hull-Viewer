#ifndef HV_FACE_MATH_H
#define HV_FACE_MATH_H
#include <math.h>
/* Closed hulls may use either winding. Signed enclosed volume determines
 * which side is exterior, including non-convex shapes. Translate first to
 * avoid cancellation from large world coordinates. Degenerate data stays lines. */
static inline int hv_surface_orientation(const float (*vertices)[3],unsigned count) {
  if(!vertices||count<12||count%3)return 0;
  double volume=0,magnitude=0;
  for(unsigned i=0;i<count;i+=3) {
    double p[3][3];
    for(unsigned j=0;j<3;j++)for(unsigned k=0;k<3;k++) {
      p[j][k]=(double)vertices[i+j][k]-vertices[0][k];
      if(!isfinite(p[j][k]))return 0;
    }
    double term=p[0][0]*(p[1][1]*p[2][2]-p[1][2]*p[2][1])
               +p[0][1]*(p[1][2]*p[2][0]-p[1][0]*p[2][2])
               +p[0][2]*(p[1][0]*p[2][1]-p[1][1]*p[2][0]);
    volume+=term;magnitude+=fabs(term);
  }
  if(!isfinite(volume)||fabs(volume)<=fmax(1e-18,magnitude*1e-10))return 0;
  return volume>0?1:-1;
}
/* Shells are two-sided. Reject malformed/zero-area triangles, not back faces. */
static int hv_shell_triangle(const float a[3],const float b[3],const float c[3]) {
  double ab[3],ac[3];
  for(unsigned k=0;k<3;k++) {
    if(!isfinite(a[k])||!isfinite(b[k])||!isfinite(c[k]))return 0;
    ab[k]=(double)b[k]-a[k];ac[k]=(double)c[k]-a[k];
  }
  double n[3]={ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
  return fabs(n[0])+fabs(n[1])+fabs(n[2])>0;
}
/* Retained for orientation/corpus diagnostics, not shell-face visibility. */
static inline int hv_exterior_face(const float a[3],const float b[3],const float c[3],
                            const float eye[3],int orientation) {
  if(!orientation||!eye)return 0;
  double ab[3],ac[3],view[3];
  for(unsigned k=0;k<3;k++) {
    ab[k]=(double)b[k]-a[k];ac[k]=(double)c[k]-a[k];view[k]=(double)eye[k]-a[k];
    if(!isfinite(ab[k])||!isfinite(ac[k])||!isfinite(view[k]))return 0;
  }
  double n[3]={ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
  double side=orientation*(n[0]*view[0]+n[1]*view[1]+n[2]*view[2]);
  /* A tiny camera-on-plane band prevents a face flashing across the view. */
  return side>(fabs(n[0])+fabs(n[1])+fabs(n[2]))*1e-4;
}
#endif
