#pragma once

// Fast questions about a drawn mesh: which triangle a ray hits first (through a box tree, so a click
// or a hover over a sculpt of millions of triangles takes microseconds, not a pass over all of
// them), and which triangles share an edge with a triangle (for walking over a face). Built once per
// mesh and kept with it; Qt-free.

#include <cstdint>
#include <vector>

struct CpuMesh;

class PickIndex {
 public:
  explicit PickIndex(const CpuMesh& mesh);

  // The first triangle along origin + t * dir with 0 <= t < maxT (dir need not be unit length:
  // t is in its steps). False if none.
  bool cast(const float origin[3], const float dir[3], float maxT, float* t, int* triangle) const;
  // The triangles sharing an edge with `triangle` (up to three; a non-manifold edge may give more).
  void neighbours(int triangle, std::vector<int>* out) const;
  // The triangles whose box overlaps [lo, hi].
  void inBox(const float lo[3], const float hi[3], std::vector<int>* out) const;
  size_t triangles() const { return triCount_; }

 private:
  struct Node {
    float lo[3], hi[3];
    uint32_t first;  // leaf: first entry in order_; inner: index of the left child (right is +1)
    uint32_t count;  // leaf: how many; 0 for an inner node
  };
  void build(uint32_t node, uint32_t first, uint32_t last, std::vector<float>& centres);
  const std::vector<float>& vertices_;
  const std::vector<uint32_t>& indices_;
  size_t triCount_ = 0;
  std::vector<uint32_t> order_;
  std::vector<Node> nodes_;
  // Vertex -> triangles, as offsets into a flat list.
  std::vector<uint32_t> cornerStart_;
  std::vector<uint32_t> cornerTris_;
};
