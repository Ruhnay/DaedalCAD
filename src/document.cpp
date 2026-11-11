#include "document.h"

#include <algorithm>
#include <atomic>

#include "evaluator.h"

#include <QDateTime>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
int seq = 1;
}

QString newId() {
  return QStringLiteral("n%1%2").arg(QDateTime::currentMSecsSinceEpoch(), 0, 16).arg(++seq);
}

namespace {
std::atomic<double>& printerClearanceStore() {
  static std::atomic<double> value{0.2};
  return value;
}
}  // namespace

double printerClearance() { return printerClearanceStore().load(); }
void setPrinterClearance(double mm) { printerClearanceStore().store(std::clamp(mm, 0.0, 2.0)); }

double halfHeight(const Node& node) {
  if (node.shape == QLatin1String("sphere")) return node.radius;
  if (node.shape == QLatin1String("hemisphere")) return node.radius * 0.5;
  if (node.shape == QLatin1String("torus")) return node.radius2;
  if (node.shape == QLatin1String("capsule")) return node.size[2] * 0.5 + node.radius;
  return std::max(0.05, node.size[2] * 0.5);
}

double tidyLength(double value) {
  if (!std::isfinite(value)) return value;
  const double out = std::round(value * 1e4) / 1e4;
  return out == 0.0 ? 0.0 : out;  // no -0
}

double tidyAngle(double value) {
  if (!std::isfinite(value)) return value;
  const double out = std::round(value * 1e4) / 1e4;
  return out == 0.0 ? 0.0 : out;
}

double tidyScale(double value) {
  if (!std::isfinite(value)) return value;
  const double out = std::round(value * 1e6) / 1e6;
  if (std::abs(out) < 1e-6) return value;  // never round a tiny scale to zero
  // Only float noise comes off (the viewport works in floats: 0.99999994 becomes 1). Real digits
  // stay: a 30 mm box made 2.5 mm high needs 1/12, and 0.083333 left it 0.00001 mm short, so it
  // no longer lined up with the 2.5 mm parts beside it and slicers showed grooves at the joins.
  if (std::abs(out - value) > 2.5e-7 * std::abs(value)) return value;
  return out;
}

void tidyPlacement(Node& node) {
  for (int axis = 0; axis < 3; ++axis) {
    node.pos[axis] = tidyLength(node.pos[axis]);
    node.rot[axis] = tidyAngle(node.rot[axis]);
    node.scale[axis] = tidyScale(node.scale[axis]);
  }
}

Node* Document::find(const QString& id) {
  for (auto& node : nodes)
    if (node.id == id) return &node;
  return nullptr;
}

const Node* Document::find(const QString& id) const {
  for (const auto& node : nodes)
    if (node.id == id) return &node;
  return nullptr;
}

std::vector<Node*> Document::children(const QString& parentId) {
  std::vector<Node*> out;
  for (auto& node : nodes)
    if (node.parentId == parentId) out.push_back(&node);
  return out;
}

std::vector<const Node*> Document::children(const QString& parentId) const {
  std::vector<const Node*> out;
  for (const auto& node : nodes)
    if (node.parentId == parentId) out.push_back(&node);
  return out;
}

QString Document::addShape(const QString& shape, double x, double y, double edgeFillet) {
  Node node;
  node.id = newId();
  node.kind = Node::Kind::Shape;
  node.shape = shape;
  node.name = shape + QStringLiteral(" ") + QString::number(static_cast<int>(nodes.size() + 1));
  node.edgeFillet = edgeFillet;
  node.pos[0] = snapOn ? std::round(x / snap) * snap : x;
  node.pos[1] = snapOn ? std::round(y / snap) * snap : y;
  if (shape == QLatin1String("sphere") || shape == QLatin1String("hemisphere")) node.radius = 16;
  if (shape == QLatin1String("cylinder")) {
    node.radius = 12;
    node.size[2] = 36;
    node.sides = 64;
  }
  if (shape == QLatin1String("cone")) {
    node.radius = 14;
    node.radius2 = 0;
    node.size[2] = 32;
  }
  if (shape == QLatin1String("tube")) {
    node.radius = 14;
    node.radius2 = 9;
    node.size[2] = 28;
  }
  if (shape == QLatin1String("capsule")) {
    node.radius = 8;
    node.size[2] = 28;
  }
  if (shape == QLatin1String("torus")) {
    node.radius = 16;
    node.radius2 = 5;
  }
  if (shape == QLatin1String("diamond")) {
    node.radius = 16;
    node.size[2] = 24;
    node.sides = 4;
  }
  if (shape == QLatin1String("star") || shape == QLatin1String("starflat")) {
    node.radius = 16;
    node.radius2 = 7;
    node.sides = 5;
    node.size[2] = shape == QLatin1String("star") ? 10 : 6;
  }
  if (shape == QLatin1String("roof")) {
    node.size[0] = 30;
    node.size[1] = 40;
    node.size[2] = 18;
  }
  if (shape == QLatin1String("prism")) {
    node.radius = 16;
    node.size[2] = 24;
    node.sides = 6;
  }
  if (shape == QLatin1String("pyramid")) {
    node.size[0] = node.size[1] = 28;
    node.size[2] = 32;
  }
  if (shape == QLatin1String("heart")) {
    node.size[0] = 30;
    node.size[1] = 30;
    node.size[2] = 8;
  }
  if (shape == QLatin1String("wedge")) {
    node.size[0] = 36;
    node.size[1] = 24;
    node.size[2] = 18;
  }
  if (shape == QLatin1String("text")) node.size[2] = 5;  // depth of the letters
  node.pos[2] = workplaneZ + halfHeight(node);
  static const char* colors[] = {"#d7c4a3", "#8aa2a8", "#c4a574", "#a3b59a", "#c4b4a8", "#a8b0c4"};
  node.color = QColor(colors[nodes.size() % 6]);
  nodes.push_back(node);
  return node.id;
}

static void collect(const Document& doc, const QString& id, std::vector<QString>* ids) {
  ids->push_back(id);
  for (const auto* child : doc.children(id)) collect(doc, child->id, ids);
}

void Document::remove(const QString& id) {
  std::vector<QString> ids;
  collect(*this, id, &ids);
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                             [&](const Node& node) {
                               return std::find(ids.begin(), ids.end(), node.id) != ids.end();
                             }),
              nodes.end());
}

bool Document::hullSelected(const QStringList& ids, bool chain, QString* error) {
  if (ids.isEmpty()) {
    if (error) *error = QStringLiteral("Select the parts to wrap in a hull.");
    return false;
  }
  if (!makeGroup(ids, error)) return false;
  Node& group = nodes.back();
  group.hull = true;
  group.hullMode = chain ? 1 : 0;
  group.name = chain ? QStringLiteral("Chain hull") : QStringLiteral("Hull");
  {
    // A chain runs in the order the parts were picked: put them in that order in the list (also
    // for a plain hull, so switching it to a chain later follows the same order).
    std::vector<size_t> places;
    std::vector<Node> picked;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].parentId == group.id) places.push_back(i);
    for (const QString& id : ids)
      for (size_t place : places)
        if (nodes[place].id == id) picked.push_back(nodes[place]);
    for (size_t i = 0; i < places.size() && i < picked.size(); ++i) nodes[places[i]] = picked[i];
  }
  return true;
}

bool Document::groupSelected(const QStringList& ids, QString* error) {
  if (ids.size() < 2) {
    if (error) *error = QStringLiteral("Select two or more parts in the same group.");
    return false;
  }
  return makeGroup(ids, error);
}

bool Document::makeGroup(const QStringList& ids, QString* error) {
  const Node* first = find(ids.front());
  if (!first) return false;
  for (const auto& id : ids) {
    const Node* node = find(id);
    if (!node || node->parentId != first->parentId) {
      if (error) *error = QStringLiteral("Group parts that live in the same group.");
      return false;
    }
  }
  Node group;
  group.kind = Node::Kind::Group;
  group.id = newId();
  group.parentId = first->parentId;
  group.name = QStringLiteral("Group");
  double lo[3] = {1e300, 1e300, 1e300};
  double hi[3] = {-1e300, -1e300, -1e300};
  bool any = false;
  const double zero[3] = {0, 0, 0};
  for (const auto& id : ids) {
    const Node* node = find(id);
    if (!node) continue;
    double localLo[3];
    double localHi[3];
    if (!localBounds(*this, *node, localLo, localHi)) continue;
    const QMatrix4x4 placed = placementMatrix(node->pos, node->rot, zero, node->scale);
    for (int corner = 0; corner < 8; ++corner) {
      const QVector3D point = placed.map(QVector3D((corner & 1) ? localHi[0] : localLo[0],
                                                    (corner & 2) ? localHi[1] : localLo[1],
                                                    (corner & 4) ? localHi[2] : localLo[2]));
      for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = std::min(lo[axis], static_cast<double>(point[axis]));
        hi[axis] = std::max(hi[axis], static_cast<double>(point[axis]));
      }
      any = true;
    }
  }
  if (any) {
    // Rounded, so the children below are shifted by a clean amount.
    for (int axis = 0; axis < 3; ++axis) group.pos[axis] = tidyLength((lo[axis] + hi[axis]) * 0.5);
  }
  for (auto& node : nodes) {
    if (!ids.contains(node.id)) continue;
    node.parentId = group.id;
    for (int axis = 0; axis < 3; ++axis) node.pos[axis] -= group.pos[axis];
  }
  nodes.push_back(group);
  return true;
}

bool Document::ungroup(const QString& id, QString* error) {
  Node* group = find(id);
  if (!group || group->kind != Node::Kind::Group) {
    if (error) *error = QStringLiteral("Select a group.");
    return false;
  }
  const QString parent = group->parentId;
  const double gpos[3] = {group->pos[0], group->pos[1], group->pos[2]};
  const double grot[3] = {group->rot[0], group->rot[1], group->rot[2]};
  const double gscale[3] = {group->scale[0], group->scale[1], group->scale[2]};
  const double zero[3] = {0, 0, 0};
  const QMatrix4x4 parentM = placementMatrix(gpos, grot, zero, gscale);
  for (auto& node : nodes) {
    if (node.parentId != id) continue;
    const QMatrix4x4 world = parentM * placementMatrix(node.pos, node.rot, zero, node.scale);
    decomposePlacement(world, node.pos, node.rot, node.scale);
    tidyPlacement(node);
    node.parentId = parent;
  }
  remove(id);
  return true;
}

bool Document::merge(const QStringList& ids, QString* error) {
  if (ids.isEmpty()) {
    if (error) *error = QStringLiteral("Select a group, or the parts to merge into one.");
    return false;
  }
  if (ids.size() == 1) {
    Node* node = find(ids.front());
    if (!node) return false;
    if (node->kind != Node::Kind::Group) {
      if (error) *error = QStringLiteral("A single part is already one piece. Select a group, or several parts.");
      return false;
    }
    if (node->merged) {
      if (error) *error = QStringLiteral("“%1” is merged already.").arg(node->name);
      return false;
    }
    node->merged = true;
    return true;
  }
  if (!makeGroup(ids, error)) return false;
  Node& group = nodes.back();
  group.merged = true;
  group.name = QStringLiteral("Merged");
  return true;
}

bool Document::unmerge(const QString& id, QString* error) {
  Node* node = find(id);
  if (!node || node->kind != Node::Kind::Group || !node->merged) {
    if (error) *error = QStringLiteral("Select a merged part to unmerge.");
    return false;
  }
  node->merged = false;
  return true;
}

bool Document::addHolesToMerged(const QString& mergedId, const QStringList& holeIds, QString* error) {
  Node* group = find(mergedId);
  if (!group || group->kind != Node::Kind::Group || !group->merged) return false;
  const double zero[3] = {0, 0, 0};
  bool invertible = false;
  const QMatrix4x4 into = placementMatrix(group->pos, group->rot, zero, group->scale).inverted(&invertible);
  if (!invertible) {
    if (error) *error = QStringLiteral("That merged part is squashed flat; the holes cannot go into it.");
    return false;
  }
  for (const QString& id : holeIds) {
    Node* node = find(id);
    if (!node || node->parentId != group->parentId || !node->hole) {
      if (error) *error = QStringLiteral("Only holes next to the merged part can go into it.");
      return false;
    }
  }
  for (const QString& id : holeIds) {
    Node* node = find(id);
    // Where it stands now, seen from inside the merged part.
    const QMatrix4x4 local = into * placementMatrix(node->pos, node->rot, zero, node->scale);
    const double was[3] = {node->rot[0], node->rot[1], node->rot[2]};
    decomposePlacementNear(local, was, node->pos, node->rot, node->scale);
    tidyPlacement(*node);
    node->parentId = group->id;
  }
  return true;
}

bool Document::repeat(const QStringList& ids, int count, const double moveIn[3], const double drotIn[3],
                      const double resizeIn[3], QString* error) {
  if (ids.isEmpty()) {
    if (error) *error = QStringLiteral("Select something to repeat.");
    return false;
  }
  const Node* first = find(ids.front());
  if (!first) return false;
  for (const auto& id : ids) {
    const Node* node = find(id);
    if (!node || node->parentId != first->parentId) {
      if (error) *error = QStringLiteral("Repeat parts that live in the same group.");
      return false;
    }
  }
  Node pattern;
  pattern.kind = Node::Kind::Pattern;
  pattern.id = newId();
  pattern.parentId = first->parentId;
  pattern.name = QStringLiteral("Repeat ×%1").arg(count);
  pattern.count = std::max(1, count);
  for (int i = 0; i < 3; ++i) {
    pattern.move[i] = moveIn[i];
    pattern.drot[i] = drotIn[i];
    pattern.resize[i] = resizeIn[i];
  }
  if (ids.size() == 1) {
    find(ids.front())->parentId = pattern.id;
  } else {
    Node inner;
    inner.kind = Node::Kind::Group;
    inner.id = newId();
    inner.parentId = pattern.id;
    inner.name = QStringLiteral("Repeated");
    nodes.push_back(inner);
    for (const auto& id : ids) find(id)->parentId = inner.id;
  }
  nodes.push_back(pattern);
  return true;
}

void Document::toggleHole(const QStringList& ids) {
  for (const auto& id : ids) {
    if (Node* node = find(id); node && (node->kind == Node::Kind::Shape || node->kind == Node::Kind::Group))
      node->hole = !node->hole;
  }
}

void Document::duplicate(const QString& id) {
  if (!find(id)) return;
  std::vector<Node> copies;
  const QString parent = find(id)->parentId;
  std::function<void(const QString&, const QString&, bool)> walk = [&](const QString& oldId, const QString& newParent, bool root) {
    const Node* src = find(oldId);
    if (!src) return;
    Node copy = *src;
    copy.id = newId();
    copy.parentId = newParent;
    if (root) {
      copy.name += QStringLiteral(" copy");
      copy.pos[0] += 15;
    }
    const QString created = copy.id;
    copies.push_back(std::move(copy));
    QStringList kids;
    for (const Node* child : children(oldId)) kids << child->id;
    for (const QString& kid : kids) walk(kid, created, false);
  };
  walk(id, parent, true);
  nodes.insert(nodes.end(), copies.begin(), copies.end());
}

void Document::duplicateInPlace(const QString& id) {
  if (!find(id)) return;
  std::vector<Node> copies;
  const QString parent = find(id)->parentId;
  std::function<void(const QString&, const QString&)> walk = [&](const QString& oldId, const QString& newParent) {
    const Node* src = find(oldId);
    if (!src) return;
    Node copy = *src;
    copy.id = newId();
    copy.parentId = newParent;
    if (oldId == id) copy.name += QStringLiteral(" copy");
    const QString created = copy.id;
    copies.push_back(std::move(copy));
    QStringList kids;
    for (const Node* child : children(oldId)) kids << child->id;
    for (const QString& kid : kids) walk(kid, created);
  };
  walk(id, parent);
  nodes.insert(nodes.end(), copies.begin(), copies.end());
}
