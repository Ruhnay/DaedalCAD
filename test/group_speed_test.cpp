// Big groups stay quick: a group that only puts parts together is drawn from its parts without the
// slow merge, groups are built side by side, a twist only refines where it has to, and the real
// merge (export) still comes out right. Many overlapping twisted plates, like the ribs of a lamp.
#include <QGuiApplication>
#include <QStringList>
#include <chrono>
#include <cmath>
#include <math.h>
#include <cstdio>
#include "document.h"
#include "evaluator.h"

static int failures = 0;

static void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++failures;
}

static double seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

static const DrawItem* itemFor(const std::vector<DrawItem>& items, const QString& id) {
  for (const DrawItem& item : items)
    if (item.id == id) return &item;
  return nullptr;
}

// A twisted plate made of a group (a box), standing on its edge, turned around the z axis.
static QString rib(Document& doc, double turn) {
  const QString box = doc.addShape("box", 0, 0);
  Node* b = doc.find(box);
  b->scale[0] = 60.0 / 30;  // 60 wide, 1.2 thick, 120 tall
  b->scale[1] = 1.2 / 30;
  b->scale[2] = 120.0 / 30;
  b->pos[0] = 30;
  b->pos[2] = 60;
  QString error;
  doc.groupSelected({box}, &error);
  Node* g = &doc.nodes.back();
  g->twist = 60;
  g->twistAxis = 2;
  const QString half = g->id;
  g->rot[2] = turn;
  return half;
}

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QString error;

  // 1. A thin strip keeps its thickness when twisted, with far fewer triangles than a fine mesh.
  {
    Document doc;
    const QString id = doc.addShape("box", 0, 0);
    Node* s = doc.find(id);
    s->scale[0] = 200.0 / 30;
    s->scale[1] = 20.0 / 30;
    s->scale[2] = 1.0 / 30;
    s->pos[2] = 0.5;
    s->twist = 360;
    s->twistAxis = 0;
    const CpuMesh m = evaluateSolid(doc, {}, &error);
    std::printf("  strip 200 x 20 x 1, 360 deg: %d triangles, volume %.1f\n", m.triangles, m.volume);
    expect(std::abs(m.volume - 4000) / 4000 < 0.025, "twisted strip keeps its volume (within 2.5%: about 0.02 mm thinner)");
    expect(m.triangles < 20000, "twisted strip is refined along its length, not everywhere");
  }

  // 2. Twelve overlapping twisted ribs, grouped: drawn at once from the ribs, merged for real on export.
  Document doc;
  QStringList ribs;
  for (int i = 0; i < 12; ++i) ribs << rib(doc, i * 15.0);
  {
    const auto start = std::chrono::steady_clock::now();
    const auto items = buildEditView(doc, {}, &error);
    std::printf("  12 ribs drawn one by one: %.2f s\n", seconds(start));
    expect(items.size() == 12, "every rib drawn");
  }
  expect(doc.groupSelected(ribs, &error), "group the ribs");
  const QString group = doc.nodes.back().id;
  expect(!groupMeshReady(doc, doc.nodes.back()), "the new group is not merged yet");
  double drawnTriangles = 0;
  {
    const auto start = std::chrono::steady_clock::now();
    const auto items = buildEditView(doc, {}, &error);
    const double took = seconds(start);
    std::printf("  the group drawn: %.2f s\n", took);
    const DrawItem* item = itemFor(items, group);
    expect(item && meshOf(item->mesh).triangles > 0, "the group is drawn as one item");
    expect(took < 2.0, "drawing the new group takes no merge (under 2 s)");
    if (item) drawnTriangles = meshOf(item->mesh).triangles;
    expect(groupMeshReady(doc, doc.nodes.back()), "and now it is ready for the inspector");
  }
  {
    const auto start = std::chrono::steady_clock::now();
    const CpuMesh merged = evaluateSolid(doc, {group}, &error);
    std::printf("  export merge: %.2f s, %d triangles (drawn %.0f), volume %.0f\n", seconds(start), merged.triangles,
                drawnTriangles, merged.volume);
    // Each rib is 60 x 1.2 x 120 = 8640 mm3; they overlap near the axis, so the union is less.
    expect(merged.volume > 12 * 8640 * 0.85 && merged.volume < 12 * 8640 * 0.999, "the export is a real union (overlaps counted once)");
    double lo[3], hi[3];
    expect(localBounds(doc, doc.nodes.back(), lo, hi), "group bounds");
    expect(merged.hasBounds && std::abs(merged.hi[2] - merged.lo[2] - (hi[2] - lo[2])) < 0.05,
           "drawn group and exported union have the same height");
    // The second time, the ribs come from the cache of group solids.
    const auto again = std::chrono::steady_clock::now();
    const CpuMesh second = evaluateSolid(doc, {group}, &error);
    expect(std::abs(second.volume - merged.volume) < 1e-6 * merged.volume, "built side by side, the same result every time");
    std::printf("  second export: %.2f s\n", seconds(again));
  }

  // 3. A group with a hole in it is still merged for drawing: the hole has to cut.
  {
    Document cut;
    const QString a = cut.addShape("box", 0, 0);
    const QString b = cut.addShape("box", 20, 0);
    const QString h = cut.addShape("cylinder", 10, 0);
    cut.find(h)->hole = true;
    cut.find(h)->scale[2] = 2;
    expect(cut.groupSelected({a, b, h}, &error), "group with a hole");
    const QString g = cut.nodes.back().id;
    const auto items = buildEditView(cut, {}, &error);
    const DrawItem* item = itemFor(items, g);
    const CpuMesh solid = evaluateSolid(cut, {}, &error);
    expect(item && std::abs(meshOf(item->mesh).volume - solid.volume) < 1.0, "a group with a hole is drawn cut, as it exports");
  }

  // 4. Hulls: two spheres wrap into a capsule; a chain follows the picked order; holes cut the hull.
  {
    Document h;
    const QString a = h.addShape("sphere", 0, 0);
    const QString b = h.addShape("sphere", 60, 0);
    for (const QString& id : {a, b}) h.find(id)->radius = 10;
    const double sphere = 4.0 / 3.0 * M_PI * 1000;
    const double capsule = sphere + M_PI * 100 * 60;  // two half spheres and a 60 mm cylinder between
    expect(h.hullSelected({b, a}, false, &error), "hull of two spheres");
    const Node& hull = h.nodes.back();
    expect(hull.hull && hull.hullMode == 0 && hull.name == "Hull", "a hull group");
    const auto kids = h.children(hull.id);
    expect(kids.size() == 2 && kids[0]->id == b && kids[1]->id == a, "its parts are in the order they were picked");
    const CpuMesh m = evaluateSolid(h, {}, &error);
    std::printf("  hull of two spheres: volume %.0f (capsule %.0f)\n", m.volume, capsule);
    expect(std::abs(m.volume - capsule) / capsule < 0.02, "two spheres wrap into a capsule");
    const auto items = buildEditView(h, {}, &error);
    const DrawItem* item = itemFor(items, hull.id);
    expect(item && std::abs(meshOf(item->mesh).volume - m.volume) < 1, "the view draws the hull, not the parts");

    // A chain through three spheres in an L: two capsules, not the triangle a plain hull makes.
    Document c;
    const QString p0 = c.addShape("sphere", 0, 0);
    const QString p1 = c.addShape("sphere", 60, 0);
    const QString p2 = c.addShape("sphere", 60, 60);
    for (const QString& id : {p0, p1, p2}) c.find(id)->radius = 10;
    expect(c.hullSelected({p0, p1, p2}, true, &error), "chain hull of three spheres");
    const double chain = evaluateSolid(c, {}, &error).volume;
    c.nodes.back().hullMode = 0;
    const double all = evaluateSolid(c, {}, &error).volume;
    std::printf("  three spheres in an L: chain %.0f, all together %.0f\n", chain, all);
    expect(std::abs(chain - (sphere + 2 * M_PI * 100 * 60)) / chain < 0.03, "the chain is two capsules joined at the corner");
    expect(all > chain * 1.5, "wrapped all together it fills the triangle between them");

    // A hole inside a hull cuts the hull.
    Document cut;
    const QString s0 = cut.addShape("sphere", 0, 0);
    const QString s1 = cut.addShape("sphere", 60, 0);
    for (const QString& id : {s0, s1}) cut.find(id)->radius = 10;
    const QString bore = cut.addShape("cylinder", 30, 0);
    Node* hole = cut.find(bore);
    hole->hole = true;
    hole->radius = 3;
    hole->scale[2] = 3;
    expect(cut.hullSelected({s0, s1, bore}, false, &error), "hull with a hole inside");
    const double bored = evaluateSolid(cut, {}, &error).volume;
    expect(bored < capsule - 100 && bored > capsule - 1000, "the hole cuts through the hull");
    // Back to a plain group: just the two spheres again.
    cut.nodes.back().hull = false;
    expect(std::abs(evaluateSolid(cut, {}, &error).volume - 2 * sphere) / sphere < 0.03, "switched off, it is a plain group");
  }

  std::printf(failures ? "%d failure(s)\n" : "all group speed checks passed\n", failures);
  return failures ? 1 : 0;
}
