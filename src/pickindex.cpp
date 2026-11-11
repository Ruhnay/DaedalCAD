#include "pickindex.h"

#include "evaluator.h"

#include <algorithm>
#include <cmath>

PickIndex::PickIndex(const CpuMesh& mesh) : vertices_(mesh.vertices), indices_(mesh.indices) {
  triCount_ = indices_.size() / 3;
  const size_t vertexCount = vertices_.size() / 3;
  if (triCount_ == 0) return;
  // Box tree over triangle centres, split at the median of the longest side.
  std::vector<float> centres(triCount_ * 3);
  for (size_t t = 0; t < triCount_; ++t)
    for (int a = 0; a < 3; ++a)
      centres[t * 3 + a] = (vertices_[indices_[t * 3] * 3 + a] + vertices_[indices_[t * 3 + 1] * 3 + a] +
                            vertices_[indices_[t * 3 + 2] * 3 + a]) / 3.f;
  order_.resize(triCount_);
  for (size_t t = 0; t < triCount_; ++t) order_[t] = static_cast<uint32_t>(t);
  nodes_.reserve(triCount_ / 2 + 8);
  nodes_.push_back({});
  build(0, 0, static_cast<uint32_t>(triCount_), centres);
  // Corners: which triangles use each vertex.
  cornerStart_.assign(vertexCount + 1, 0);
  for (uint32_t v : indices_)
    if (v < vertexCount) ++cornerStart_[v + 1];
  for (size_t v = 0; v < vertexCount; ++v) cornerStart_[v + 1] += cornerStart_[v];
  cornerTris_.resize(cornerStart_.back());
  std::vector<uint32_t> fill(cornerStart_.begin(), cornerStart_.end() - 1);
  for (size_t i = 0; i < indices_.size(); ++i)
    if (indices_[i] < vertexCount) cornerTris_[fill[indices_[i]]++] = static_cast<uint32_t>(i / 3);
}

void PickIndex::build(uint32_t node, uint32_t first, uint32_t last, std::vector<float>& centres) {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  float clo[3] = {1e30f, 1e30f, 1e30f}, chi[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t k = first; k < last; ++k) {
    const uint32_t t = order_[k];
    for (int c = 0; c < 3; ++c) {
      const float* p = &vertices_[indices_[t * 3 + c] * 3];
      for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], p[a]), hi[a] = std::max(hi[a], p[a]);
    }
    for (int a = 0; a < 3; ++a) clo[a] = std::min(clo[a], centres[t * 3 + a]), chi[a] = std::max(chi[a], centres[t * 3 + a]);
  }
  Node& n = nodes_[node];
  for (int a = 0; a < 3; ++a) n.lo[a] = lo[a], n.hi[a] = hi[a];
  if (last - first <= 4) {
    n.first = first;
    n.count = last - first;
    return;
  }
  const int axis = (chi[0] - clo[0] >= chi[1] - clo[1] && chi[0] - clo[0] >= chi[2] - clo[2]) ? 0
                   : (chi[1] - clo[1] >= chi[2] - clo[2])                                       ? 1
                                                                                                 : 2;
  const uint32_t mid = first + (last - first) / 2;
  std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + last,
                   [&](uint32_t a, uint32_t b) { return centres[a * 3 + axis] < centres[b * 3 + axis]; });
  const uint32_t left = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back({});
  nodes_.push_back({});
  nodes_[node].first = left;  // (n may have moved with the push_backs)
  nodes_[node].count = 0;
  build(left, first, mid, centres);
  build(left + 1, mid, last, centres);
}

bool PickIndex::cast(const float o[3], const float d[3], float maxT, float* tOut, int* triangle) const {
  if (nodes_.empty() || triCount_ == 0) return false;
  float inv[3];
  for (int a = 0; a < 3; ++a) inv[a] = 1.f / (std::abs(d[a]) > 1e-20f ? d[a] : (d[a] < 0 ? -1e-20f : 1e-20f));
  float best = maxT;
  int hit = -1;
  uint32_t stack[128];
  int top = 0;
  stack[top++] = 0;
  auto enter = [&](const Node& n) {
    float t0 = 0.f, t1 = best;
    for (int a = 0; a < 3; ++a) {
      float near = (n.lo[a] - o[a]) * inv[a], far = (n.hi[a] - o[a]) * inv[a];
      if (near > far) std::swap(near, far);
      t0 = std::max(t0, near);
      t1 = std::min(t1, far);
      if (t0 > t1) return -1.f;
    }
    return t0;
  };
  while (top > 0) {
    const Node& n = nodes_[stack[--top]];
    if (enter(n) < 0.f) continue;
    if (n.count) {
      for (uint32_t k = n.first; k < n.first + n.count; ++k) {
        const uint32_t t = order_[k];
        const float* a = &vertices_[indices_[t * 3] * 3];
        const float* b = &vertices_[indices_[t * 3 + 1] * 3];
        const float* c = &vertices_[indices_[t * 3 + 2] * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::abs(det) < 1e-12f) continue;
        const float id = 1.f / det;
        const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
        if (u < 0.f || u > 1.f) continue;
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
        if (v < 0.f || u + v > 1.f) continue;
        const float along = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * id;
        if (along >= 0.f && along < best) {
          best = along;
          hit = static_cast<int>(order_[k]);
        }
      }
    } else if (top + 2 <= 128) {
      // The nearer child last, so it is looked at first.
      const float l = enter(nodes_[n.first]), r = enter(nodes_[n.first + 1]);
      if (l >= 0.f && r >= 0.f) {
        stack[top++] = l < r ? n.first + 1 : n.first;
        stack[top++] = l < r ? n.first : n.first + 1;
      } else if (l >= 0.f) {
        stack[top++] = n.first;
      } else if (r >= 0.f) {
        stack[top++] = n.first + 1;
      }
    }
  }
  if (hit < 0) return false;
  *tOut = best;
  *triangle = hit;
  return true;
}

void PickIndex::neighbours(int triangle, std::vector<int>* out) const {
  out->clear();
  if (triangle < 0 || static_cast<size_t>(triangle) >= triCount_) return;
  const uint32_t* corner = &indices_[static_cast<size_t>(triangle) * 3];
  for (int c = 0; c < 3; ++c) {
    const uint32_t v = corner[c];
    if (v + 1 >= cornerStart_.size()) continue;
    for (uint32_t k = cornerStart_[v]; k < cornerStart_[v + 1]; ++k) {
      const uint32_t other = cornerTris_[k];
      if (static_cast<int>(other) == triangle) continue;
      int shared = 0;
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) shared += indices_[other * 3 + i] == corner[j];
      if (shared >= 2 && std::find(out->begin(), out->end(), static_cast<int>(other)) == out->end())
        out->push_back(static_cast<int>(other));
    }
  }
}

void PickIndex::inBox(const float lo[3], const float hi[3], std::vector<int>* out) const {
  out->clear();
  if (nodes_.empty()) return;
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const Node& n = nodes_[stack.back()];
    stack.pop_back();
    bool overlap = true;
    for (int a = 0; a < 3; ++a) overlap = overlap && n.lo[a] <= hi[a] && n.hi[a] >= lo[a];
    if (!overlap) continue;
    if (n.count) {
      for (uint32_t k = n.first; k < n.first + n.count; ++k) out->push_back(static_cast<int>(order_[k]));
    } else {
      stack.push_back(n.first);
      stack.push_back(n.first + 1);
    }
  }
}
