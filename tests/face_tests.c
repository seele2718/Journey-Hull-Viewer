#include <assert.h>
#include <stdio.h>
#include "../src/face_math.h"
static const float corners[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                               {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
static const unsigned indices[36]={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
                                  3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
int main(void) {
  float mesh[36][3];
  for(unsigned reflected=0;reflected<2;reflected++)for(unsigned reversed=0;reversed<2;reversed++)
  for(unsigned moved=0;moved<2;moved++) {
    float origin[3]={moved?1200:0,moved?-700:0,moved?5000:0};
    for(unsigned i=0;i<36;i++) {
      unsigned at=i/3*3+(reversed?2-i%3:i%3);
      for(unsigned k=0;k<3;k++)mesh[i][k]=corners[indices[at]][k]*(reflected&&k==0?-1:1)+origin[k];
    }
    int orientation=hv_surface_orientation(mesh,36);assert(orientation==((reflected!=reversed)?-1:1));
    for(unsigned i=0;i<36;i+=3)assert(hv_shell_triangle(mesh[i],mesh[i+1],mesh[i+2]));
    for(unsigned axis=0;axis<3;axis++)for(int side=-1;side<=1;side++) {
      float eye[3]={origin[0],origin[1],origin[2]};eye[axis]+=3*side;unsigned visible=0;
      for(unsigned i=0;i<36;i+=3)visible+=hv_exterior_face(mesh[i],mesh[i+1],mesh[i+2],eye,orientation);
      assert(visible==(side?2:0)); /* No tinted exit wall from inside. */
    }
    float eye[3]={origin[0]+3,origin[1]+3,origin[2]+3};unsigned visible=0;
    for(unsigned i=0;i<36;i+=3)visible+=hv_exterior_face(mesh[i],mesh[i+1],mesh[i+2],eye,orientation);
    assert(visible==6);
    eye[0]=NAN;assert(!hv_exterior_face(mesh[0],mesh[1],mesh[2],eye,orientation));
  }
  float flat[12][3]={{0}};assert(!hv_surface_orientation(flat,12));
  assert(!hv_shell_triangle(flat[0],flat[1],flat[2]));
  mesh[0][0]=NAN;assert(!hv_shell_triangle(mesh[0],mesh[1],mesh[2]));
  assert(!hv_surface_orientation(NULL,36)&&!hv_surface_orientation(mesh,35));
  assert(!hv_exterior_face(mesh[0],mesh[1],mesh[2],NULL,1));
  puts("PASS: two-sided shell triangle validation for both windings, mirrored/translated hulls, malformed/degenerate rejection; separate orientation diagnostics");
  return 0;
}
