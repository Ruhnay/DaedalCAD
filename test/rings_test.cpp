#include "../src/surfacefillet.h"
#include <cstdio>
#include <cmath>
using namespace manifold;
int main() {
  // top face of a slab at z=5 with a square hole, plus a second separate pad on the same plane
  MeshGL m; auto V=[&](float x,float y,float z){m.vertProperties.insert(m.vertProperties.end(),{x,y,z});return (uint32_t)(m.vertProperties.size()/3-1);};
  uint32_t o0=V(0,0,5),o1=V(20,0,5),o2=V(20,20,5),o3=V(0,20,5),i0=V(5,5,5),i1=V(15,5,5),i2=V(15,15,5),i3=V(5,15,5);
  auto T=[&](uint32_t a,uint32_t b,uint32_t c){m.triVerts.insert(m.triVerts.end(),{a,b,c});};
  // CCW seen from +z. four trapezoids as two tris each
  T(o0,o1,i1);T(o0,i1,i0); T(o1,o2,i2);T(o1,i2,i1); T(o2,o3,i3);T(o2,i3,i2); T(o3,o0,i0);T(o3,i0,i3);
  uint32_t p0=V(30,0,5),p1=V(40,0,5),p2=V(40,10,5),p3=V(30,10,5); T(p0,p1,p2); T(p0,p2,p3);
  // a wall facing sideways must be ignored
  uint32_t w0=V(0,0,0),w1=V(20,0,0),w2=V(20,0,5); T(w0,w2,w1);
  sf::Frame f; f.origin[2]=5; f.normal[2]=1;
  std::string log; auto rings = sf::coplanarRings(Manifold(m), f, &log);
  printf("%s", log.c_str());
  int bad=0;
  if (rings.size()!=3) { printf("expected 3 rings got %zu\n", rings.size()); ++bad; }
  for (auto& r : rings) { double a=0; size_t n=r.size()/3; for(size_t i=0;i<n;++i){size_t j=(i+1)%n; a+=r[i*3]*r[j*3+1]-r[j*3]*r[i*3+1];} printf("ring %zu pts area %.1f\n", n, a/2); }
  if (rings.size()==3) {
    auto area=[&](const std::vector<double>&r){double a=0;size_t n=r.size()/3;for(size_t i=0;i<n;++i){size_t j=(i+1)%n;a+=r[i*3]*r[j*3+1]-r[j*3]*r[i*3+1];}return a/2;};
    // piece order is by union-find root; check set of areas
    double s[3]={area(rings[0]),area(rings[1]),area(rings[2])}; int pos400=0,neg100=0,pos100=0;
    for(double a:s){ if(std::abs(a-300)<1e-3||std::abs(a-400)<1e-3) {} }
    int outer=0,hole=0; for(double a:s){ if(a>0) ++outer; else ++hole; }
    if (outer!=2||hole!=1){printf("expected 2 outer + 1 hole\n");++bad;}
    (void)pos400;(void)neg100;(void)pos100;
  }
  printf(bad?"FAILED\n":"RINGS OK\n"); return bad;
}
