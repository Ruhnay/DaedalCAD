// Generators: every generator builds with its defaults and with its options switched, pieces in a
// print layout sit on the bed, and parts meant to fit together (box and lid, jar and lid, bolt and
// nut, two gears, the hinge leaves) never overlap.

#include <QImage>
#include <QDir>
#include <QFile>
#include "generators.h"

#include <manifold/cross_section.h>

#include <QJsonObject>

#include <chrono>
#include <cmath>
#include <math.h>
#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// Edges shorter than a nanometre: corners that were meant to be one but came out a rounding error
// apart (usually where two pieces were made flush). CAD programs cannot make edges that short, so
// the STEP export of such a part fails.
int tinyEdges(const manifold::Manifold& solid) {
  const manifold::MeshGL64 mesh = solid.GetMeshGL64();
  const auto& p = mesh.vertProperties;
  const size_t k = mesh.numProp;
  int count = 0;
  for (size_t i = 0; i + 2 < mesh.triVerts.size(); i += 3)
    for (size_t e = 0; e < 3; ++e) {
      const size_t a = mesh.triVerts[i + e], b = mesh.triVerts[i + (e + 1) % 3];
      const double dx = p[k * a] - p[k * b], dy = p[k * a + 1] - p[k * b + 1], dz = p[k * a + 2] - p[k * b + 2];
      if (dx * dx + dy * dy + dz * dz < 1e-12) ++count;
    }
  return count / 2;
}

std::vector<gen::Piece> make(const char* id, QJsonObject values, const std::string& label, double* ms = nullptr) {
  const auto start = std::chrono::steady_clock::now();
  QString warning;
  auto pieces = gen::build(QString::fromLatin1(id), values, &warning);
  const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (ms) *ms = took;
  std::printf("%-34s %zu piece(s) %8.1f ms %s\n", label.c_str(), pieces.size(), took, warning.toStdString().c_str());
  check(!pieces.empty(), label + ": built");
  for (const auto& piece : pieces) {
    check(piece.solid.Status() == manifold::Manifold::Error::NoError, label + ": " + piece.name.toStdString() + " manifold");
    check(piece.solid.Volume() > 1, label + ": " + piece.name.toStdString() + " has volume");
    const int tiny = tinyEdges(piece.solid);
    check(tiny == 0, label + ": " + piece.name.toStdString() + " has " + std::to_string(tiny) + " edges under a nanometre");
  }
  return pieces;
}

double overlap(const gen::Piece& a, const gen::Piece& b) { return (a.solid ^ b.solid).Volume(); }

void onBed(const std::vector<gen::Piece>& pieces, const std::string& label) {
  double lowest = 1e18;
  for (const auto& piece : pieces) lowest = std::min(lowest, piece.solid.BoundingBox().min.z);
  for (const auto& piece : pieces)
    check(std::abs(piece.solid.BoundingBox().min.z - lowest) < 1e-6, label + ": " + piece.name.toStdString() + " on the bed");
}

void apart(const std::vector<gen::Piece>& pieces, const std::string& label, double minGap) {
  for (size_t i = 0; i < pieces.size(); ++i)
    for (size_t j = i + 1; j < pieces.size(); ++j) {
      const double v = overlap(pieces[i], pieces[j]);
      check(v < 1e-6, label + ": pieces overlap by " + std::to_string(v) + " mm3");
      if (minGap > 0) {
        const double gap = pieces[i].solid.MinGap(pieces[j].solid, minGap * 4);
        check(gap > minGap * 0.99, label + ": gap " + std::to_string(gap) + " below " + std::to_string(minGap));
      }
    }
}

}  // namespace

int main() {
  // Defaults for every generator.
  for (const gen::Spec& spec : gen::specs()) {
    double ms = 0;
    const auto pieces = make(spec.id.toLatin1().constData(), {}, "default " + spec.id.toStdString(), &ms);
    check(ms < 4000, spec.id.toStdString() + ": default builds in time");
    // Centered as a whole.
    double lo[3] = {1e18, 1e18, 1e18}, hi[3] = {-1e18, -1e18, -1e18};
    for (const auto& piece : pieces) {
      const auto box = piece.solid.BoundingBox();
      lo[0] = std::min(lo[0], box.min.x), hi[0] = std::max(hi[0], box.max.x);
      lo[1] = std::min(lo[1], box.min.y), hi[1] = std::max(hi[1], box.max.y);
      lo[2] = std::min(lo[2], box.min.z), hi[2] = std::max(hi[2], box.max.z);
    }
    for (int a = 0; a < 3; ++a) check(std::abs(lo[a] + hi[a]) < 1e-6, spec.id.toStdString() + ": centered");
    // Normalizing keeps every value, and unknown choices fall back to the default.
    QJsonObject odd = gen::defaults(spec.id);
    for (const gen::Param& p : spec.params)
      if (p.type == gen::Param::Choice) odd[p.key] = QStringLiteral("nonsense");
    const QJsonObject fixed = gen::normalized(spec.id, odd);
    for (const gen::Param& p : spec.params)
      if (p.type == gen::Param::Choice) check(fixed[p.key].toString() == p.defChoice, spec.id.toStdString() + ": choice fallback");
  }

  // Box: every lid style, print and assembled, with dividers.
  for (const char* lid : {"lip", "snap", "slide", "none"}) {
    for (const char* layout : {"print", "assembled"}) {
      QJsonObject v{{"lid", lid}, {"layout", layout}, {"dividersX", 2}, {"dividersY", 1}};
      const std::string label = std::string("box ") + lid + " " + layout;
      const auto pieces = make("box", v, label);
      check(pieces.size() == (std::string(lid) == "none" ? 1u : 2u), label + ": piece count");
      if (std::string(layout) == "print") onBed(pieces, label);
      if (pieces.size() == 2) apart(pieces, label, std::string(layout) == "assembled" ? 0.1 : 0);
    }
  }
  {
    // A lip lid with 0.3 mm clearance: the lid's rim stays 0.3 mm from the walls.
    const auto pieces = make("box", QJsonObject{{"lid", "lip"}, {"layout", "assembled"}, {"clearance", 0.3}}, "box lip 0.3");
    if (pieces.size() == 2) {
      const double gap = pieces[0].solid.MinGap(pieces[1].solid, 2);
      check(std::abs(gap - 0.3) < 0.02, "box lip 0.3: gap is the clearance (" + std::to_string(gap) + ")");
    }
    // The lid of a box has to actually cover it.
    const auto box = pieces[0].solid.BoundingBox(), lid = pieces[1].solid.BoundingBox();
    check(std::abs((box.max.x - box.min.x) - (lid.max.x - lid.min.x)) < 1e-6, "box lip: lid as long as the box");
  }
  // Sliding lids, in grooves and over rails: they slide all the way out along -X without touching
  // the box, cannot lift out, and with the snap lock meet a small bump on the way that they clear
  // when closed.
  for (const char* style : {"inset", "outset"}) {
    for (const char* layout : {"print", "assembled"}) {
      const std::string label = std::string("box slide ") + style + " " + layout;
      const auto pieces = make("box", QJsonObject{{"lid", "slide"}, {"slideStyle", style}, {"layout", layout}, {"dividersX", 1}}, label);
      check(pieces.size() == 2u, label + ": piece count");
      if (std::string(layout) == "print") onBed(pieces, label);
      if (pieces.size() == 2) apart(pieces, label, std::string(layout) == "assembled" ? 0.1 : 0);
    }
    const std::string label = std::string("box slide ") + style;
    const auto plain = make("box", QJsonObject{{"lid", "slide"}, {"slideStyle", style}, {"slideSnap", false}, {"layout", "assembled"}}, label);
    if (plain.size() == 2) {
      const manifold::Manifold& box = plain[0].solid;
      const manifold::Manifold& lid = plain[1].solid;
      check((box ^ lid).Volume() < 1e-6, label + ": closed, lid and box do not overlap");
      check((box ^ lid.Translate(manifold::vec3(0, 0, 1.0))).Volume() > 1, label + ": lid cannot lift out");
      bool free = true;
      for (double x = 0.5; x < 85; x += 4) free = free && (box ^ lid.Translate(manifold::vec3(-x, 0, 0))).Volume() < 1e-6;
      check(free, label + ": lid slides all the way out along -X");
      check((box ^ lid.Translate(manifold::vec3(1.0, 0, 0))).Volume() > 1e-3, label + ": lid stops when closed (cannot go on past +X)");
      if (std::string(style) == "outset") {
        // Flush with the box's sides: as wide as the box, and covering its whole top.
        const auto b = box.BoundingBox(), l = lid.BoundingBox();
        check(std::abs((b.max.y - b.min.y) - (l.max.y - l.min.y)) < 1e-6, label + ": lid as wide as the box");
        check(l.min.z < b.max.z - 1, label + ": lid's rim reaches down the outside of the walls");
      }
    }
    const auto snap = make("box", QJsonObject{{"lid", "slide"}, {"slideStyle", style}, {"slideSnap", true}, {"layout", "assembled"}}, label + " snap");
    if (snap.size() == 2) {
      const manifold::Manifold& box = snap[0].solid;
      const manifold::Manifold& lid = snap[1].solid;
      check((box ^ lid).Volume() < 1e-6, label + " snap: closed, the bump sits in its groove");
      const double click = (box ^ lid.Translate(manifold::vec3(-3.0, 0, 0))).Volume();
      check(click > 1e-3 && click < 20, label + " snap: a small bump to ride over when opening (" + std::to_string(click) + " mm3)");
      check((box ^ lid.Translate(manifold::vec3(-90.0, 0, 0))).Volume() < 1e-6, label + " snap: and out it comes");
    }
  }

  // Printer clearance: with "Use printer clearance" on, the lid gap is the printer's; off, the box's own.
  {
    const auto printer = gen::build("box", QJsonObject{{"layout", "assembled"}, {"usePrinter", true}, {"clearance", 0.4}}, nullptr, 0.07);
    const auto own = gen::build("box", QJsonObject{{"layout", "assembled"}, {"usePrinter", false}, {"clearance", 0.4}}, nullptr, 0.07);
    if (printer.size() == 2 && own.size() == 2) {
      const double a = printer[0].solid.MinGap(printer[1].solid, 2);
      const double b = own[0].solid.MinGap(own[1].solid, 2);
      check(std::abs(a - 0.07) < 0.01, "box follows the printer clearance (" + std::to_string(a) + ")");
      check(std::abs(b - 0.4) < 0.02, "box with its own clearance keeps it (" + std::to_string(b) + ")");
    }
    check(gen::defaults("box")["usePrinter"].toBool(), "new boxes use the printer clearance");
    check(!gen::defaults("jar")["usePrinter"].toBool() && !gen::defaults("bolt")["usePrinter"].toBool(),
          "threads start with their own clearance");
    const QJsonObject jar = gen::resolved("jar", QJsonObject{{"usePrinter", true}}, 0.1);
    check(std::abs(jar["clearance"].toDouble() - 0.1 * std::sqrt(2.0)) < 1e-9, "jar: flank gap = printer clearance");
    const QJsonObject nut = gen::resolved("nut", QJsonObject{{"usePrinter", true}}, 0.1);
    check(std::abs(nut["clearance"].toDouble() - 0.2) < 1e-9, "nut: flank gap = printer clearance");
    // Shown only when all conditions hold.
    const gen::Spec* box = gen::spec("box");
    const gen::Param* field = nullptr;
    for (const auto& p : box->params) if (p.key == "clearance") field = &p;
    check(field && !gen::visible(*field, gen::normalized("box", QJsonObject{{"usePrinter", true}})), "own clearance hidden while printer is on");
    check(field && gen::visible(*field, gen::normalized("box", QJsonObject{{"usePrinter", false}})), "own clearance shown when off");
    check(field && !gen::visible(*field, gen::normalized("box", QJsonObject{{"usePrinter", false}, {"lid", "none"}})), "no clearance without a lid");
  }

  // Electronics enclosure: every option builds, the lid clears the box and its posts, openings go through.
  for (const char* lid : {"screws", "lip"})
    for (const char* vents : {"none", "slots", "honeycomb"}) {
      const std::string label = std::string("enclosure ") + lid + " " + vents;
      const auto pieces = make("enclosure", QJsonObject{{"lid", lid}, {"vents", vents}, {"layout", "assembled"}, {"sideVents", true},
                                                         {"pcb", true}, {"hole", std::string(vents) == "slots" ? "screw" : "insert"}},
                               label);
      if (pieces.size() == 2) apart(pieces, label, 0.04);
      const auto print = make("enclosure", QJsonObject{{"lid", lid}, {"vents", vents}}, label + " print");
      if (print.size() == 2) onBed(print, label + " print");
    }
  {
    const auto with = make("enclosure", QJsonObject{{"cut1Wall", "front"}}, "enclosure one opening");
    const auto without = make("enclosure", QJsonObject{{"cut1Wall", "none"}}, "enclosure no opening");
    const auto three = make("enclosure", QJsonObject{{"cut1Wall", "front"}, {"cut2Wall", "left"}, {"cut2Type", "round"}, {"cut3Wall", "back"},
                                                     {"cut3Type", "rect"}, {"cut3Z", 10}},
                            "enclosure three openings");
    if (!with.empty() && !without.empty() && !three.empty()) {
      check(with[0].solid.Genus() == without[0].solid.Genus() + 1, "an opening goes through the wall");
      check(three[0].solid.Genus() == without[0].solid.Genus() + 3, "three openings, three holes");
    }
  }

  // Battery holder: each battery drops into its pocket without touching, and every pocket is there.
  for (const char* cell : {"AA", "AAA", "C", "D", "18650", "21700", "CR2032", "9V"}) {
    const std::string label = std::string("battery ") + cell;
    const auto holder = make("battery", QJsonObject{{"cell", cell}, {"columns", 3}, {"rows", 2}}, label);
    if (holder.empty()) continue;
    const auto box = holder[0].solid.BoundingBox();
    const QJsonObject v = gen::normalized("battery", QJsonObject{{"cell", cell}});
    const double c = v["clearance"].toDouble(), web = v["web"].toDouble(), rim = v["rim"].toDouble(), base = v["base"].toDouble();
    struct Size { double d, len, w, h; int kind; };
    const std::map<std::string, Size> sizes = {{"AA", {14.5, 50.5, 0, 0, 0}}, {"AAA", {10.5, 44.5, 0, 0, 0}}, {"C", {26.2, 50, 0, 0, 0}},
                                               {"D", {34.2, 61.5, 0, 0, 0}}, {"18650", {18.6, 65.2, 0, 0, 0}}, {"21700", {21.4, 70.2, 0, 0, 0}},
                                               {"CR2032", {20, 3.2, 0, 0, 1}}, {"9V", {0, 48.5, 26.5, 17.5, 2}}};
    const Size sz = sizes.at(cell);
    // The battery in the first pocket (column 0, row 0), standing on the pocket floor.
    manifold::Manifold battery;
    double px, py;
    if (sz.kind == 0) {
      battery = manifold::Manifold::Cylinder(sz.len, sz.d * 0.5, sz.d * 0.5, 96);
      px = py = sz.d + 2 * c;
    } else if (sz.kind == 1) {
      battery = manifold::Manifold::Cylinder(sz.len, sz.d * 0.5, sz.d * 0.5, 96).Rotate(0, 90, 0).Translate({-sz.len * 0.5, 0, sz.d * 0.5});
      px = sz.len + 2 * c;
      py = sz.d + 2 * c;
    } else {
      battery = manifold::Manifold::Cube({sz.w, sz.h, sz.len}).Translate({-sz.w * 0.5, -sz.h * 0.5, 0});
      px = sz.w + 2 * c;
      py = sz.h + 2 * c;
    }
    const double x0 = box.min.x + rim + px * 0.5, y0 = box.min.y + rim + py * 0.5;
    battery = battery.Translate({x0, y0, box.min.z + base + c * 0.5});
    check((holder[0].solid ^ battery).Volume() < 1e-6, label + ": the battery fits its pocket");
    const auto lower = battery.Translate({0, 0, -c});  // pushed onto the floor: it must touch
    check((holder[0].solid ^ lower.Translate({0, 0, -1})).Volume() > 0.01, label + ": the pocket has a floor");
    check(std::abs((box.max.x - box.min.x) - (3 * (px + web) - web + 2 * rim)) < 1e-3, label + ": width fits three pockets");
  }

  // Ball joint: the ball sits in the socket with clearance, is held in, and swings.
  for (const char* base : {"plate", "foot", "camera", "none"}) {
    const std::string label = std::string("ball joint ") + base;
    const auto pieces = make("balljoint", QJsonObject{{"layout", "assembled"}, {"ballBase", base}, {"socketBase", base}, {"usePrinter", false}}, label);
    if (pieces.size() != 2) continue;
    apart(pieces, label, 0.14);
    // Pulled straight out, the ball catches on the socket's lip.
    const auto pulled = pieces[0].solid.Translate({0, 0, 2.0});
    check((pulled ^ pieces[1].solid).Volume() > 0.5, label + ": the ball is held in");
    // Swung 20° to the side around the ball's center, nothing collides.
    const auto sb = pieces[1].solid.BoundingBox();
    (void)sb;
    double zc = 0;
    {
      // The ball's center: the middle of the sphere, which is the lowest 16 mm of the (upside down) ball part.
      const auto bb = pieces[0].solid.BoundingBox();
      zc = bb.min.z + 8.0;
    }
    for (double deg : {10.0, 20.0}) {
      const auto swung = pieces[0].solid.Translate({0, 0, -zc}).Rotate(deg, 0, 0).Translate({0, 0, zc});
      check((swung ^ pieces[1].solid).Volume() < 1e-6, label + ": swings " + std::to_string(int(deg)) + " deg");
    }
  }

  // Hose adapter: the bore is open end to end, a sleeve takes its pipe, and the wall holds.
  {
    struct Case { const char* a; double da; const char* b; double db; };
    for (const Case& k : {Case{"barb", 12, "sleeve", 32}, Case{"sleeve", 35, "barb", 12}, Case{"smooth", 20, "smooth", 25},
                          Case{"barb", 8, "barb", 6}, Case{"sleeve", 40, "sleeve", 32}}) {
      const std::string label = std::string("hose ") + k.a + " " + std::to_string(int(k.da)) + " / " + k.b + " " + std::to_string(int(k.db));
      QString warning;
      const auto pieces = gen::build("hose", QJsonObject{{"typeA", k.a}, {"diameterA", k.da}, {"typeB", k.b}, {"diameterB", k.db}}, &warning);
      check(pieces.size() == 1 && pieces[0].solid.Status() == manifold::Manifold::Error::NoError, label + ": builds");
      check(warning.isEmpty(), label + ": no warning (" + warning.toStdString() + ")");
      if (pieces.empty()) continue;
      const auto box = pieces[0].solid.BoundingBox();
      const QJsonObject v = gen::normalized("hose", {});
      const double wall = v["wall"].toDouble(), c = v["clearance"].toDouble();
      auto bore = [&](const char* type, double d) { return std::string(type) == "sleeve" ? d * 0.5 + c : d * 0.5 - wall; };
      const double narrowest = std::min(bore(k.a, k.da), bore(k.b, k.db));
      const auto rod = manifold::Manifold::Cylinder(box.max.z - box.min.z + 2, narrowest - 0.05, narrowest - 0.05, 96).Translate({0, 0, box.min.z - 1});
      check((pieces[0].solid ^ rod).Volume() < 1e-6, label + ": bore open end to end");
      check(pieces[0].solid.Genus() == 1, label + ": one tube");
      if (std::string(k.a) == "sleeve") {
        const auto pipe = manifold::Manifold::Cylinder(25, k.da * 0.5, k.da * 0.5, 128).Translate({0, 0, box.min.z - 1});
        check((pieces[0].solid ^ pipe).Volume() < 1e-6, label + ": the pipe fits sleeve A");
      }
    }
  }

  // Rack and pinion: they mesh, and turning the pinion while the rack slides along keeps them meshed.
  for (double helix : {0.0, 20.0})
    for (bool herring : {false, true}) {
      if (helix == 0 && herring) continue;
      for (double length : {100.0, 106.0}) {  // an even and an odd number of teeth
        const std::string label = "rack helix " + std::to_string(int(helix)) + (herring ? " herringbone" : "") + " length " + std::to_string(int(length));
        const auto pieces = make("rack", QJsonObject{{"helix", helix}, {"herringbone", herring}, {"length", length}}, label);
        if (pieces.size() != 2) continue;
        apart(pieces, label, 0.0);
        const auto pb = pieces[1].solid.BoundingBox();
        const manifold::vec3 center((pb.min.x + pb.max.x) * 0.5, (pb.min.y + pb.max.y) * 0.5, 0);
        const double r = 2.0 * 16 * 0.5;
        double worst = 0;
        for (double deg : {3.0, 7.0, 11.0, 15.0, 22.5, 30.0}) {
          const auto turned = pieces[1].solid.Translate(-center).Rotate(0, 0, deg).Translate(center);
          const auto slid = pieces[0].solid.Translate({r * deg * 3.14159265358979 / 180.0, 0, 0});
          worst = std::max(worst, (turned ^ slid).Volume());
        }
        check(worst < 1e-6, label + ": rolls along the rack (" + std::to_string(worst) + " mm3)");
        const auto stuck = pieces[1].solid.Translate(-center).Rotate(0, 0, 10).Translate(center);
        check((stuck ^ pieces[0].solid).Volume() > 0.5, label + ": the teeth really interlock");
      }
    }

  // Gridfinity.
  for (bool lip : {true, false})
    for (bool magnets : {false, true}) {
      const std::string label = std::string("gridbin lip=") + (lip ? "1" : "0") + " magnets=" + (magnets ? "1" : "0");
      const auto pieces = make("gridbin", QJsonObject{{"lip", lip}, {"magnets", magnets}, {"dividersX", 1}}, label);
      if (!pieces.empty()) {
        const auto b = pieces[0].solid.BoundingBox();
        check(std::abs((b.max.x - b.min.x) - 83.5) < 1e-3, label + ": 2 units wide = 83.5 mm");
        check(std::abs((b.max.z - b.min.z) - (lip ? 25.4 : 21.0)) < 1e-3, label + ": height");
      }
    }
  {
    // A 1x1 bin drops into a 1x1 baseplate pocket without touching it.
    const auto bin = make("gridbin", QJsonObject{{"unitsX", 1}, {"unitsY", 1}, {"unitsZ", 2}, {"lip", false}}, "gridbin 1x1");
    const auto plate = make("gridplate", QJsonObject{{"unitsX", 1}, {"unitsY", 1}}, "gridplate 1x1");
    if (!bin.empty() && !plate.empty()) {
      const auto pb = plate[0].solid.BoundingBox();
      const auto bb = bin[0].solid.BoundingBox();
      // Seat: the bin's 45° foot rests on the pocket's 45° wall (the plate is 4.65 tall, the foot 4.75).
      const auto seated = bin[0].solid.Translate(manifold::vec3(0, 0, pb.max.z - bb.min.z - 4.65 + 0.1));
      check((plate[0].solid ^ seated).Volume() < 1e-6, "gridfinity: bin fits the baseplate");
    }
    make("gridplate", QJsonObject{{"solid", true}, {"magnets", true}}, "gridplate solid magnets");
  }

  // Screw jar.
  for (const char* layout : {"print", "assembled"}) {
    const auto pieces = make("jar", QJsonObject{{"layout", layout}}, std::string("jar ") + layout);
    if (pieces.size() == 2) {
      if (std::string(layout) == "print") onBed(pieces, "jar print");
      else apart(pieces, "jar assembled", 0.005);  // the lid seats on the rim
    }
  }
  make("jar", QJsonObject{{"grip", false}, {"diameter", 30}, {"pitch", 3}}, "jar small plain");

  // Bolts and nuts.
  for (const char* size : {"M6", "M10", "M24"})
    for (const char* head : {"hex", "knurled", "none"}) {
      const std::string label = std::string("bolt ") + size + " " + head;
      const auto pieces = make("bolt", QJsonObject{{"size", size}, {"head", head}, {"layout", "assembled"}}, label);
      if (pieces.size() == 2) apart(pieces, label, 0.1);
    }
  {
    const auto pieces = make("bolt", QJsonObject{{"size", "M8"}}, "bolt M8 print");
    if (pieces.size() == 2) onBed(pieces, "bolt print");
    make("bolt", QJsonObject{{"size", "custom"}, {"diameter", 14}, {"pitch", 2.5}}, "bolt custom");
    make("nut", QJsonObject{{"size", "M3"}, {"thin", true}}, "nut M3 thin");
  }

  // Standoffs.
  for (const char* hole : {"insert", "screw", "through"})
    for (bool plate : {false, true}) {
      const std::string label = std::string("standoff ") + hole + (plate ? " plate" : "");
      const auto pieces = make("standoff", QJsonObject{{"hole", hole}, {"plate", plate}}, label);
      check(pieces.size() == (plate ? 1u : 4u), label + ": piece count");
    }

  // Gears mesh without touching.
  for (double helix : {0.0, 20.0})
    for (bool herring : {false, true}) {
      if (helix == 0 && herring) continue;
      const std::string label = "gear helix " + std::to_string(int(helix)) + (herring ? " herringbone" : "");
      const auto pieces = make("gear", QJsonObject{{"mate", true}, {"helix", helix}, {"herringbone", herring}}, label);
      check(pieces.size() == 2, label + ": two gears");
      if (pieces.size() == 2) apart(pieces, label, 0.0);
    }
  {
    // Without backlash, but turned half a tooth, the gears would collide: the mesh is real.
    const auto pieces = make("gear", QJsonObject{{"mate", true}, {"teeth", 15}, {"mateTeeth", 9}}, "gear 15/9");
    if (pieces.size() == 2) {
      apart(pieces, "gear 15/9", 0.0);
      const auto box = pieces[1].solid.BoundingBox();
      const manifold::vec3 c((box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2, 0);
      const auto turned = pieces[1].solid.Translate(-c).Rotate(0, 0, 20).Translate(c);
      check((pieces[0].solid ^ turned).Volume() > 0.5, "gear 15/9: teeth interlock");
    }
    make("gear", QJsonObject{{"teeth", 8}, {"bore", 0}}, "gear 8 teeth");
  }

  // Hinge: one print, two separate leaves.
  for (int knuckles : {3, 5, 6}) {
    const std::string label = "hinge " + std::to_string(knuckles);
    const auto pieces = make("hinge", QJsonObject{{"knuckles", knuckles}}, label);
    if (!pieces.empty()) {
      const auto parts = pieces[0].solid.Decompose();
      check(parts.size() == 2, label + ": two loose leaves (" + std::to_string(parts.size()) + ")");
      if (parts.size() == 2) {
        const double gap = parts[0].MinGap(parts[1], 1.0);
        check(gap > 0.3, label + ": leaves keep clearance (" + std::to_string(gap) + ")");
        // It must actually swing: turn one leaf up around the pin, all the way over, in 10° steps.
        manifold::Manifold a = parts[0], b = parts[1];
        if (a.BoundingBox().max.y > b.BoundingBox().max.y) std::swap(a, b);
        const double axisZ = pieces[0].solid.BoundingBox().min.z + 4.0;  // knuckle diameter 8
        double worst = 0;
        int worstAt = 0;
        for (int deg = 0; deg <= 180; deg += 10) {
          const auto turned = b.Translate({0, 0, -axisZ}).Rotate(deg, 0, 0).Translate({0, 0, axisZ});
          const double v = (a ^ turned).Volume();
          if (v > worst) worst = v, worstAt = deg;
        }
        check(worst < 1e-6, label + ": swings freely (jams at " + std::to_string(worstAt) + " deg, " + std::to_string(worst) + " mm3)");
      }
    }
  }

  // Honeycomb and fit test.
  {
    const auto panel = make("honeycomb", {}, "honeycomb");
    if (!panel.empty()) check(panel[0].solid.Genus() > 20, "honeycomb: has the cells");
    // Every hole shape, both arrangements, cut or whole at the border: the web between holes is
    // never thinner than asked, and "whole holes only" leaves only complete holes.
    for (const char* shape : {"hexagon", "circle", "square", "diamond", "triangle", "star", "slot"})
      for (const char* grid : {"staggered", "straight"})
        for (const char* edge : {"cut", "whole"})
          for (double angle : {0.0, 30.0}) {
            const double web = 2.0;
            const std::string label = std::string("pattern ") + shape + " " + grid + " " + edge + " " + std::to_string(int(angle));
            const auto p = make("honeycomb",
                                QJsonObject{{"shape", shape}, {"grid", grid}, {"edge", edge}, {"web", web}, {"angle", angle},
                                            {"outline", angle > 0 ? "round" : "rect"}},
                                label);
            if (p.empty()) continue;
            check(p[0].solid.Genus() > 3, label + ": has holes (" + std::to_string(p[0].solid.Genus()) + ")");
            // The holes, from a slice through the middle.
            manifold::Polygons slice = p[0].solid.Slice(0);
            manifold::Polygons holeLoops;
            double biggest = 0;
            size_t outer = 0;
            for (size_t i = 0; i < slice.size(); ++i) {
              double a = 0;
              for (size_t k = 0; k < slice[i].size(); ++k) {
                const auto& p0 = slice[i][k];
                const auto& p1 = slice[i][(k + 1) % slice[i].size()];
                a += p0.x * p1.y - p1.x * p0.y;
              }
              a = std::abs(a) * 0.5;
              if (a > biggest) biggest = a, outer = i;
            }
            for (size_t i = 0; i < slice.size(); ++i)
              if (i != outer) holeLoops.push_back(slice[i]);
            const manifold::CrossSection holes(holeLoops, manifold::CrossSection::FillRule::NonZero);
            const size_t n = holes.Decompose().size();
            const size_t grown = holes.Offset(web * 0.49, manifold::CrossSection::JoinType::Round, 2.0, 24).Decompose().size();
            check(n == size_t(p[0].solid.Genus()), label + ": holes in the slice match the genus");
            check(grown == n, label + ": web thinner than asked (" + std::to_string(n) + " holes, " + std::to_string(grown) +
                                  " after growing)");
            if (std::string(edge) == "whole") {
              // All complete: every hole has the same area.
              double lo = 1e18, hi = 0;
              for (const auto& h : holes.Decompose()) lo = std::min(lo, h.Area()), hi = std::max(hi, h.Area());
              check(hi - lo < hi * 0.01, label + ": only whole holes");
            }
          }
    // Holes only: exactly what the panel leaves out of its plate.
    {
      const auto panelSolid = make("honeycomb", {}, "pattern panel (for holes only)");
      const auto holesOnly = make("honeycomb", QJsonObject{{"make", "holes"}}, "pattern panel holes only");
      check(gen::makesHole("honeycomb", QJsonObject{{"make", "holes"}}) && !gen::makesHole("honeycomb", {}),
            "pattern panel: holes only is a hole shape");
      if (!panelSolid.empty() && !holesOnly.empty()) {
        const double plate = 120.0 * 80 * 3 - (4 - 3.14159265 ) * 4 * 4 * 3;  // rounded corners
        const double sum = panelSolid[0].solid.Volume() + holesOnly[0].solid.Volume();
        check(std::abs(sum - plate) < plate * 0.002, "pattern panel: panel + holes make the plate (" + std::to_string(sum) + " vs " +
                                                        std::to_string(plate) + ")");
        check(holesOnly[0].solid.Decompose().size() == size_t(panelSolid[0].solid.Genus()),
              "pattern panel: one hole solid per hole");
      }
    }
    const auto fit = make("fittest", QJsonObject{{"count", 5}}, "fit test 5");
    if (fit.size() == 2) {
      check(fit[0].solid.Genus() == 5, "fit test: five holes");
      onBed(fit, "fit test");
    }
    make("fittest", QJsonObject{{"shape", "round"}, {"count", 10}, {"size", 3}}, "fit test round 10");
  }

  // --- Pattern tube ------------------------------------------------------------------------------
  {
    const auto pot = make("patterntube", {}, "pattern tube");
    if (!pot.empty()) {
      const auto box = pot[0].solid.BoundingBox();
      check(std::abs(box.max.x - box.min.x - 100) < 0.5 && std::abs(box.max.z - box.min.z - 120) < 0.01,
            "pattern tube: 100 mm across at the top, 120 tall");
      check(pot[0].solid.Genus() > 40, "pattern tube: holes all round (" + std::to_string(pot[0].solid.Genus()) + ")");
      // The bands and the floor stay solid: below the holes a slice is one ring (the floor: one disc).
      const double z0 = box.min.z;
      check(pot[0].solid.Slice(z0 + 11.5).size() == 2, "pattern tube: the bottom band is a whole ring");
      check(pot[0].solid.Slice(z0 + 120 - 7.5).size() == 2, "pattern tube: the top band is a whole ring");
      check(pot[0].solid.Slice(z0 + 1.5).size() == 1, "pattern tube: a closed floor");
    }
    // Every shape, straight and tapered, cut and whole, both ways up: closed, with holes, and the web
    // round the middle no thinner than asked.
    for (const char* shape : {"hexagon", "circle", "square", "diamond", "triangle", "star", "slot"})
      for (const char* edge : {"cut", "whole"})
        for (const char* direction : {"upright", "sideways"}) {
          const std::string label = std::string("tube ") + shape + " " + edge + " " + direction;
          const auto t = make("patterntube",
                              QJsonObject{{"shape", shape}, {"edge", edge}, {"direction", direction}, {"bottomDiameter", 90},
                                          {"topDiameter", 90}, {"height", 80}, {"base", "open"}, {"web", 2.0}},
                              label);
          if (t.empty()) continue;
          check(t[0].solid.Genus() > 10, label + ": holes (" + std::to_string(t[0].solid.Genus()) + ")");
          // A straight tube unrolled at the middle height: a ring 2.4 thick whose gaps are the holes.
          const double zMid = (t[0].solid.BoundingBox().min.z + t[0].solid.BoundingBox().max.z) * 0.5;
          const manifold::Polygons ring = t[0].solid.Slice(zMid);
          double thinnest = 1e18;
          for (const auto& loop : ring) {
            // Pieces of the ring between holes: their size round the tube is the web there.
            double lo = 1e18, hi = -1e18;
            for (const auto& q : loop) {
              const double a = std::atan2(q.y, q.x);
              lo = std::min(lo, a), hi = std::max(hi, a);
            }
            if (hi - lo < 3) thinnest = std::min(thinnest, (hi - lo) * 45);
          }
          check(thinnest > 2.0 * 0.85 || thinnest > 1e17, label + ": web round the tube at least about 2 mm (" + std::to_string(thinnest) + ")");
        }
    // Tapered: the holes go square through the slanted wall, and none breaks the bands.
    for (const char* taper : {"grow", "same"})
      for (const char* edge : {"cut", "whole"}) {
        const std::string label = std::string("tube tapered, ") + taper + ", " + edge;
        const auto t = make("patterntube", QJsonObject{{"bottomDiameter", 60}, {"topDiameter", 140}, {"height", 100}, {"taper", taper},
                                                       {"edge", edge}},
                            label);
        if (t.empty()) continue;
        check(t[0].solid.Genus() > 30, label + ": holes (" + std::to_string(t[0].solid.Genus()) + ")");
        const double z0 = t[0].solid.BoundingBox().min.z;
        check(t[0].solid.Slice(z0 + 11.5).size() == 2 && t[0].solid.Slice(z0 + 100 - 7.5).size() == 2, label + ": bands whole");
      }
    // Narrow at the top works too.
    make("patterntube", QJsonObject{{"bottomDiameter", 120}, {"topDiameter", 70}, {"height", 90}}, "tube narrowing upwards");
    // Holes only: wrapped round, the same holes the tube has. Each build is moved to its own place,
    // so put both back into their own frame before cutting.
    const QJsonObject sleeve{{"base", "open"}, {"bottomDiameter", 70}, {"topDiameter", 90}, {"height", 90}};
    QJsonObject cut = sleeve;
    cut["make"] = "holes";
    QJsonObject plainValues = sleeve;
    plainValues["bandBottom"] = 90;
    plainValues["bandTop"] = 0;
    check(gen::makesHole("patterntube", cut) && !gen::makesHole("patterntube", sleeve), "pattern tube: holes only is a hole shape");
    manifold::vec3 cTube, cHoles, cPlain;
    QString warning;
    const auto tube = gen::build("patterntube", sleeve, &warning, -1, &cTube);
    const auto holes = gen::build("patterntube", cut, &warning, -1, &cHoles);
    const auto plain = gen::build("patterntube", plainValues, &warning, -1, &cPlain);
    check(tube.size() == 1 && holes.size() == 1 && plain.size() == 1, "pattern sleeve, its holes and a plain sleeve build");
    if (tube.size() == 1 && holes.size() == 1 && plain.size() == 1) {
      check(plain[0].solid.Genus() == 1, "plain sleeve: no holes");
      const manifold::Manifold own = tube[0].solid.Translate(cTube);
      const manifold::Manifold cutInto = plain[0].solid.Translate(cPlain) - holes[0].solid.Translate(cHoles);
      check(cutInto.Genus() == own.Genus(), "holes only cut into a plain sleeve: as many holes as the pattern sleeve (" +
                                                std::to_string(cutInto.Genus()) + " vs " + std::to_string(own.Genus()) + ")");
      check(std::abs(cutInto.Volume() - own.Volume()) < own.Volume() * 1e-4, "holes only cut into a plain sleeve: the pattern sleeve");
    }
  }

  // --- Threads on any cylinder ------------------------------------------------------------------
  for (const auto& size : gen::threadSizes()) {
    if (size.diameter > 12) continue;
    const std::string name = size.name.toStdString();
    // Same length, so both helices start at the same height: a real bolt turns until they do.
    const manifold::Manifold rod = gen::threadedCylinder(size.diameter, size.pitch, 12, false, 0);
    const manifold::Manifold cut = gen::threadedCylinder(size.diameter, size.pitch, 12, true, gen::threadPlay(size.pitch));
    check(rod.Status() == manifold::Manifold::Error::NoError && !rod.IsEmpty(), name + " rod builds");
    check(cut.Status() == manifold::Manifold::Error::NoError && !cut.IsEmpty(), name + " hole builds");
    if (rod.IsEmpty() || cut.IsEmpty()) continue;
    const auto rb = rod.BoundingBox();
    check(std::abs(rb.max.x - size.diameter / 2) < 0.02 && std::abs(rb.max.z - rb.min.z - 12) < 1e-6, name + " rod: its size");
    // The bolt turns in the hole without touching, and the hole's thread reaches inside the bolt's
    // outside diameter (otherwise nothing would grip).
    const manifold::Manifold block = manifold::Manifold::Cylinder(12, size.diameter, size.diameter, 64, true) - cut;
    const double clash = (block ^ rod).Volume();
    const double grip = (block ^ manifold::Manifold::Cylinder(12, size.diameter / 2 - 0.05, size.diameter / 2 - 0.05, 128, true)).Volume();
    check(clash < 1e-3, name + ": a bolt screws into the hole without touching (" + std::to_string(clash) + ")");
    check(grip > 0.05 * size.diameter, name + ": and the hole's thread grips it");
  }
  check(std::abs(gen::coarsePitch(8) - 1.25) < 1e-9 && std::abs(gen::coarsePitch(9) - 1.25 * 9 / 8) < 1e-9, "coarse pitch of a size, and between sizes");

  // --- O-ring seals ------------------------------------------------------------------------------
  {
    // Closed, the tongue squeezes the ring by the squeeze, and nothing else touches.
    auto squeezeOf = [&](const std::vector<gen::Piece>& p, double lift, const std::string& label) {
      if (p.size() < 3) return;
      const manifold::Manifold closed = p[1].solid.Translate({0, 0, -lift});
      check((p[0].solid ^ closed).Volume() < 1e-3, label + ": the lid's tongue clears the groove");
      check((p[0].solid ^ p[2].solid).Volume() < 1e-3, label + ": the gasket lies in the groove");
      const auto overlapBox = (p[2].solid ^ closed).BoundingBox();
      const double pressed = overlapBox.max.z - overlapBox.min.z;
      std::printf("  %s: ring pressed %.3f mm of 2\n", label.c_str(), pressed);
      check(std::abs(pressed - 0.5) < 0.03, label + ": closed, the ring is squeezed by a quarter");
    };
    const auto box = make("box", QJsonObject{{"lid", "seal"}, {"layout", "assembled"}, {"gasket", true}}, "sealed box");
    squeezeOf(box, 0.2, "sealed box");
    if (box.size() == 3) check(box[1].solid.Genus() == 4, "sealed box: four screws, outside the seal");
    const auto longBox = make("box", QJsonObject{{"lid", "seal"}, {"length", 200}, {"width", 60}, {"layout", "assembled"}}, "long sealed box");
    if (longBox.size() == 2) check(longBox[1].solid.Genus() == 8, "a 200 mm box gets two more screws on each long side");
    check(make("box", QJsonObject{{"lid", "seal"}}, "sealed box for printing").size() == 2, "box and lid laid out");
    QString w;
    check(gen::build("box", QJsonObject{{"lid", "seal"}, {"length", 14}, {"width", 14}}, &w).size() == 1 && w.contains("O-ring"),
          "too small for a groove: no lid, and it says why");
    const auto enclosure = make("enclosure", QJsonObject{{"seal", true}, {"layout", "assembled"}, {"gasket", true}}, "sealed enclosure");
    squeezeOf(enclosure, 0.2, "sealed enclosure");
    // 90 mm long outside: a screw at each corner and one more on each long side.
    if (enclosure.size() == 3) check(enclosure[1].solid.Genus() == 6, "sealed enclosure: six screws, outside the seal");
    make("enclosure", QJsonObject{{"seal", true}, {"hole", "screw"}, {"screw", "M2.5"}}, "sealed enclosure, self-tapping");
    QString small;
    // A fuzz case that used to give nothing: 15 x 15 inside, a 6 mm cord and 40 mm corners.
    const auto tiny = gen::build("enclosure", QJsonObject{{"seal", true}, {"screwPlace", "walls"}, {"length", 15}, {"width", 15}, {"cord", 6},
                                                          {"corner", 40}, {"wall", 8}, {"height", 200}, {"hole", "screw"},
                                                          {"usePrinter", false}, {"clearance", 1.5}},
                                 &small);
    std::printf("  tiny sealed enclosure: %s\n", small.toStdString().c_str());
    check(tiny.size() == 2 && small.contains("ears"), "too small for screws in the walls: ears instead, and it says so");
    // Screws in the walls: flat sides (the lid is exactly the box's size), the seal bent round
    // every screw, the inside corners filled.
    for (const char* id : {"enclosure", "box"}) {
      QJsonObject o{{"screwPlace", "walls"}, {"layout", "assembled"}, {"gasket", true}};
      if (std::string(id) == "box") o["lid"] = "seal";
      else o["seal"] = true;
      const std::string label = std::string("sealed ") + id + ", screws in the walls";
      const auto p = make(id, o, label);
      squeezeOf(p, 0.2, label);
      if (p.size() == 3) {
        const auto b = p[0].solid.BoundingBox(), l = p[1].solid.BoundingBox();
        check(std::abs(b.min.x - l.min.x) < 1e-3 && std::abs(b.max.x - l.max.x) < 1e-3 && std::abs(b.min.y - l.min.y) < 1e-3 &&
                  std::abs(b.max.y - l.max.y) < 1e-3,
              label + ": nothing sticks out, the lid is the box's size");
        check(p[1].solid.Genus() == (std::string(id) == "box" ? 4 : 6), label + ": every screw goes through the lid");
        check(p[2].solid.Genus() == 1, label + ": the gasket is one closed ring");
      }
    }
    const auto jar = make("jar", QJsonObject{{"seal", true}, {"layout", "assembled"}, {"gasket", true}}, "sealed jar");
    squeezeOf(jar, 0.0, "sealed jar");
    make("jar", QJsonObject{{"seal", true}, {"diameter", 20}}, "small sealed jar");
    make("jar", QJsonObject{{"seal", true}, {"wall", 8}}, "sealed jar, walls thick enough already");
  }

  // --- Mechanical, workshop and calibration generators --------------------------------------
  auto maxRadius = [](const manifold::Manifold& m) {
    const auto gl = m.GetMeshGL();
    double best = 0;
    for (size_t i = 0; i + 2 < gl.vertProperties.size(); i += gl.numProp)
      best = std::max(best, std::hypot(double(gl.vertProperties[i]), double(gl.vertProperties[i + 1])));
    return best;
  };
  {
    // GT2: the tips sit 0.254 mm inside the belt's pitch circle (teeth x 2 mm / pi).
    const auto p = make("pulley", QJsonObject{{"teeth", 20}, {"flanges", false}, {"hub", false}}, "GT2 20 teeth");
    if (!p.empty()) check(std::abs(maxRadius(p[0].solid) - (20 * 2.0 / M_PI * 0.5 - 0.254)) < 0.02, "GT2: outside diameter");
    const auto h = make("pulley", QJsonObject{{"teeth", 40}}, "GT2 40 teeth with hub");
    if (!h.empty()) check(h[0].solid.Genus() >= 1, "GT2: bored");
    const auto vb = make("pulley", QJsonObject{{"type", "vbelt"}, {"diameter", 50}}, "V pulley");
    check(vb.size() == 2, "V pulley: two halves");
    make("pulley", QJsonObject{{"type", "flat"}, {"boreShape", "d"}, {"setScrew", "insert"}}, "plain pulley, D bore");
  }
  for (const char* style : {"flange", "square", "pillow"}) {
    // A 608 (22 x 7) sits in its seat with the clearance all around, not more.
    const std::string label = std::string("bearing housing ") + style;
    const auto p = make("bearing", QJsonObject{{"style", style}, {"clearance", 0.1}}, label);
    if (p.empty()) continue;
    const auto box = p[0].solid.BoundingBox();
    manifold::Manifold ring, fat;
    if (std::string(style) == "pillow") {
      ring = manifold::Manifold::Cylinder(7, 11, 11, 128).Rotate(-90, 0, 0).Translate({0, box.max.y - 7, 0});
      fat = manifold::Manifold::Cylinder(7, 11.2, 11.2, 128).Rotate(-90, 0, 0).Translate({0, box.max.y - 7, 0});
    } else {
      ring = manifold::Manifold::Cylinder(7, 11, 11, 128).Translate({0, 0, box.max.z - 7});
      fat = manifold::Manifold::Cylinder(7, 11.2, 11.2, 128).Translate({0, 0, box.max.z - 7});
    }
    check((p[0].solid ^ ring).Volume() < 1e-3, label + ": the bearing fits its seat");
    check((p[0].solid ^ fat).Volume() > 0.1, label + ": and the seat is no bigger than the clearance");
  }
  {
    const auto p = make("coupler", QJsonObject{{"bore1", 5}, {"bore2", 8}, {"length", 25}, {"screws", 2}}, "coupler 5 to 8");
    if (!p.empty()) {
      const manifold::Manifold rods = manifold::Manifold::Cylinder(13, 2.5, 2.5, 64).Translate({0, 0, -13.5}) +
                                      manifold::Manifold::Cylinder(13, 4, 4, 64).Translate({0, 0, 0.5});
      check((p[0].solid ^ rods).Volume() < 1e-3, "coupler: both shafts fit");
    }
    make("coupler", QJsonObject{{"type", "collar"}, {"setScrew", "tap"}}, "shaft collar");
  }
  {
    const auto s = make("spring", {}, "coil spring");
    if (!s.empty()) check(s[0].solid.Decompose().size() == 1, "coil spring: one piece");
    make("spring", QJsonObject{{"type", "flat"}, {"folds", 7}}, "flat spring");
    QString w;
    check(gen::build("spring", QJsonObject{{"length", 8}, {"turns", 20}}, &w).empty() && !w.isEmpty(),
          "a coil too short for its turns: refused with a reason");
  }
  {
    const double c = 0.2;
    const auto p = make("snapfit", QJsonObject{{"layout", "assembled"}, {"clearance", c}}, "snap-fit assembled");
    if (p.size() == 2) {
      check(overlap(p[0], p[1]) < 1e-6, "snap-fit: the latch sits in the catch");
      const gen::Piece pulled{p[0].name, p[0].solid.Translate({-(c + 0.2), 0, 0})};
      check(overlap(pulled, p[1]) > 0.01, "snap-fit: pulled back, the hook catches the wall");
    }
    make("snapfit", {}, "snap-fit for printing");
    make("livinghinge", {}, "living hinge");
  }
  {
    const auto p = make("cablechain", QJsonObject{{"links", 4}}, "cable chain");
    if (!p.empty()) {
      auto links = p[0].solid.Decompose();
      check(links.size() == 4, "cable chain: four loose links (" + std::to_string(links.size()) + ")");
      std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.BoundingBox().min.x < b.BoundingBox().min.x; });
      if (links.size() == 4) {
        const auto box = links[0].BoundingBox();
        const double Rj = (box.max.z - box.min.z) * 0.5;
        const manifold::vec3 pivot(box.min.x + Rj + 20, 0, box.min.z + Rj);
        double worst = 0;
        int worstAt = 0;
        for (int deg = -80; deg <= 80; deg += 10) {
          const auto turned = links[1].Translate(-pivot).Rotate(0, deg, 0).Translate(pivot);
          const double v = (links[0] ^ turned).Volume();
          if (v > worst) worst = v, worstAt = deg;
        }
        check(worst < 1e-6, "cable chain: links turn freely (jam at " + std::to_string(worstAt) + " deg)");
      }
    }
  }
  {
    const double size = 20, c = 0.2, wall = 2.5, hub = size * 0.5 + c + wall;
    const auto p = make("tubeconnector", QJsonObject{{"layout", "corner3"}, {"clearance", c}}, "tube corner, round sockets");
    if (!p.empty()) {
      const auto box = p[0].solid.BoundingBox();
      const manifold::vec3 centre(box.min.x + hub, box.min.y + hub, box.min.z + hub);
      const auto rod = manifold::Manifold::Cylinder(24, size * 0.5, size * 0.5, 96).Translate({0, 0, hub + 0.01});
      const auto tubes = rod.Rotate(0, 90, 0) + rod.Rotate(-90, 0, 0) + rod;
      check((p[0].solid ^ tubes.Translate(centre)).Volume() < 1e-3, "tube corner: the tubes go into all three sockets");
    }
    make("tubeconnector", QJsonObject{{"shape", "square"}, {"layout", "six"}}, "square tube, six ways");
    make("tubeconnector", QJsonObject{{"fit", "plug"}, {"layout", "tee"}}, "tube T, plugs");
  }
  {
    const auto p = make("toolholder", QJsonObject{{"rows", 2}, {"columns", 6}}, "hex bit holder");
    if (!p.empty()) {
      const auto box = p[0].solid.BoundingBox();
      check(p[0].solid.Slice((box.min.z + box.max.z) * 0.5 + 2).size() == 13, "hex bit holder: twelve pockets");
    }
    make("toolholder", QJsonObject{{"type", "drill"}, {"from", 1}, {"to", 6}, {"step", 0.5}}, "drill index");
    make("toolholder", QJsonObject{{"type", "microsd"}, {"rows", 1}, {"columns", 10}}, "microSD holder");
  }
  {
    const auto p = make("dividers", QJsonObject{{"columns", 3}, {"rows", 2}, {"layout", "assembled"}}, "drawer dividers");
    check(p.size() == 5, "dividers: five strips");
    apart(p, "dividers assembled", 0);
    onBed(make("dividers", QJsonObject{{"columns", 2}, {"rows", 1}}, "dividers for printing"), "dividers for printing");
  }
  {
    const double d = 6, c = 0.3;
    const auto p = make("cable", QJsonObject{{"cables", 3}, {"cable", d}, {"clearance", c}}, "cable clip for three");
    if (!p.empty()) {
      const auto box = p[0].solid.BoundingBox();
      manifold::Manifold cables;
      for (int i = 0; i < 3; ++i)
        cables += manifold::Manifold::Cylinder(30, d * 0.5, d * 0.5, 64).Rotate(-90, 0, 0).Translate({(i - 1) * d, -15, box.min.z + d * 0.5});
      check((p[0].solid ^ cables).Volume() < 1e-3, "cable clip: three cables fit under it");
    }
    make("cable", QJsonObject{{"type", "snap"}}, "snap clip");
    check(make("cable", QJsonObject{{"type", "grommet"}}, "desk grommet").size() == 2, "grommet and cap");
    make("cable", QJsonObject{{"type", "label"}}, "cable label");
    make("nametag", QJsonObject{{"text", "Workshop"}, {"style", "engraved"}}, "name tag");
  }
  {
    const auto h = make("holetest", QJsonObject{{"from", 2}, {"to", 10}, {"step", 1}, {"pegs", true}}, "hole size test");
    if (h.size() == 2) check(h[0].solid.Genus() == 9, "hole test: nine holes");
    const auto t = make("threadtest", QJsonObject{{"size", "M8"}, {"count", 5}}, "thread fit test");
    if (t.size() == 2) check(t[0].solid.Genus() == 5, "thread test: five threaded holes");
    make("overhang", {}, "overhang and bridge test");
  }

  // Corner cases a fuzz run turned up: each one used to give nothing at all.
  {
    QString w;
    auto low = gen::build("box", QJsonObject{{"lid", "slide"}, {"height", 5}, {"floor", 9.4}, {"lidThickness", 10}}, &w);
    check(low.size() == 1 && !w.isEmpty(), "box too low for a lid: built without one, with a warning");
    w.clear();
    low = gen::build("box", QJsonObject{{"lid", "snap"}, {"height", 5}, {"floor", 3}}, &w);
    check(low.size() == 1 && !w.isEmpty(), "box too low for a snap lid: built without one");
    make("battery", QJsonObject{{"cell", "CR2032"}, {"columns", 4}, {"web", 10}, {"rim", 1}, {"clearance", 0}, {"depth", 20}},
         "battery shallow coins");
    w.clear();
    const auto gear = gen::build("gear", QJsonObject{{"teeth", 10}, {"module", 1}, {"bore", 40}}, &w);
    check(gear.size() == 1 && gear[0].solid.Volume() > 1 && !w.isEmpty(), "gear bore bigger than the gear: made smaller");
    w.clear();
    const auto hinge = gen::build("hinge", QJsonObject{{"length", 10}, {"knuckles", 31}}, &w);
    check(hinge.size() == 1 && hinge[0].solid.Decompose().size() == 2, "hinge with too many knuckles: uses fewer");
    // Count triangles rather than time it, so the check holds in a slow (sanitizer) build too.
    const auto bolt = make("bolt", QJsonObject{{"size", "custom"}, {"diameter", 60}, {"pitch", 0.3}, {"length", 100}, {"nut", false}},
                           "bolt 60 mm, pitch 0.3");
    if (!bolt.empty())
      check(bolt[0].solid.NumTri() < 1000000,
            "a very fine custom pitch stays light (" + std::to_string(bolt[0].solid.NumTri()) + " triangles)");
  }

  // Standard parts: the nominal sizes, and every part fits inside its own matching hole (they share
  // one origin, so after undoing build()'s centring they must not overlap outside it).
  {
    auto own = [](const char* id, QJsonObject values) {
      manifold::vec3 c;
      const auto pieces = gen::build(QString::fromLatin1(id), values, nullptr, -1, &c);
      manifold::Manifold all;
      for (const auto& piece : pieces) all = all + piece.solid;
      return all.Translate(c);
    };
    const auto m6 = own("stdscrew", QJsonObject{{"standard", "4762"}, {"size", "M6"}, {"length", 20}}).BoundingBox();
    check(std::abs(m6.max.z - 6) < 1e-6 && std::abs(m6.min.z + 20) < 1e-6 && std::abs(m6.max.x - 5) < 0.01,
          "M6x20 socket head: head 10 x 6 above the origin, 20 under it");
    const auto nut = own("stdnut", QJsonObject{{"standard", "934"}, {"size", "M10"}}).BoundingBox();
    check(std::abs(nut.max.z - 8) < 1e-6 && std::abs(nut.max.x - nut.min.x - 17) < 0.01, "DIN 934 M10: 17 across flats, 8 high");
    QString warning;
    gen::build("stdscrew", QJsonObject{{"standard", "7380"}, {"size", "M2"}}, &warning);
    check(warning.contains("M3"), "ISO 7380 has no M2: M3 is used, with a note");
    const std::map<std::string, std::vector<std::string>> standards = {
        {"stdscrew", {"4762", "7380", "10642", "7991", "4017", "933", "7045", "4026"}},
        {"stdnut", {"4032", "934", "4035", "985", "6923", "562"}},
        {"stdwasher", {"7089", "7093", "127"}},
        {"stdrod", {""}}};
    for (const auto& [id, list] : standards)
      for (const std::string& standard : list)
        for (const char* size : {"M3", "M8"}) {
          QJsonObject values{{"size", size}, {"length", 16}};
          if (!standard.empty()) values["standard"] = QString::fromStdString(standard);
          // A set screw goes into a tapped hole, so it is compared with its modelled thread.
          if (standard == "4026") values["thread"] = "modelled";
          const manifold::Manifold part = own(id.c_str(), values);
          values["make"] = "hole";
          const manifold::Manifold hole = own(id.c_str(), values);
          const double outside = (part - hole).Volume();
          const std::string label = id + " " + standard + " " + size;
          check(part.Volume() > 1 && hole.Volume() > part.Volume(), label + ": part and hole built");
          check(outside < 1e-3 + part.Volume() * 1e-4, label + ": part sits inside its hole (" + std::to_string(outside) + " mm3 outside)");
        }
    for (const char* d : {"3", "8"}) {
      QJsonObject values{{"diameter", d}, {"length", 20}, {"depth", 8}, {"fit", "slip"}};
      const manifold::Manifold pin = own("stdpin", values);
      values["make"] = "hole";
      const manifold::Manifold rest = pin - own("stdpin", values);
      check(rest.BoundingBox().min.z > -1e-3, std::string("dowel pin ") + d + ": the part in the hole is inside it");
    }
  }

  // Dice: every kind is one closed solid of the right size (sharp, without text here: the volume of
  // a regular solid with that inner radius), a pip d6 has its 21 pips cut, and an inlay d6 gives the
  // die and the marks filling it exactly.
  {
    const std::vector<std::pair<const char*, double>> kinds = {{"d4", 8 * std::sqrt(3.0)}, {"d6", 8.0},       {"d8", 4 * std::sqrt(3.0)},
                                                               {"d10", 0},                 {"d12", 5.5503}, {"d20", 5.0541}};
    for (const auto& [kind, ratio] : kinds) {
      const double size = 20;
      const double r = std::string(kind) == "d4" ? size / 4 : size / 2;
      QJsonObject values{{"kind", kind}, {"size", size}, {"rounding", 0.0}, {"marks", "numbers"}, {"style", "cut"}};
      const auto pieces = make("dice", values, std::string("dice ") + kind);
      check(pieces.size() == 1 && pieces[0].solid.Genus() == 0, std::string("dice ") + kind + ": one closed solid");
      if (pieces.empty()) continue;
      const double volume = pieces[0].solid.Volume();
      std::printf("dice %s: volume %.1f, %zu triangles\n", kind, volume, pieces[0].solid.NumTri());
      if (ratio > 0) check(std::abs(volume - ratio * r * r * r) < 0.002 * ratio * r * r * r, std::string("dice ") + kind + ": the right size");
      values["rounding"] = 1.5;
      const auto rounded = make("dice", values, std::string("dice ") + kind + " rounded");
      check(!rounded.empty() && rounded[0].solid.Volume() < volume && rounded[0].solid.Volume() > 0.85 * volume,
            std::string("dice ") + kind + ": rounded a little");
    }
    QJsonObject pipsDie{{"kind", "d6"}, {"size", 16.0}, {"rounding", 0.0}, {"marks", "pips"}, {"style", "cut"}, {"depth", 0.8}};
    const auto pip = make("dice", pipsDie, "dice pips");
    const double cube = 16.0 * 16 * 16;
    // 21 pips, each a cylinder of radius 0.17 * 8 * 0.55 / 0.55 ... as drawn: r = room * 0.17 with room 8.
    const double pipVolume = 21 * 3.14159 * (8 * 0.17) * (8 * 0.17) * 0.8;
    check(!pip.empty() && std::abs((cube - pip[0].solid.Volume()) - pipVolume) < 0.05 * pipVolume, "dice: 21 pips cut");
    pipsDie["style"] = "inlay";
    const auto inlay = make("dice", pipsDie, "dice inlay");
    check(inlay.size() == 2 && std::abs(inlay[0].solid.Volume() + inlay[1].solid.Volume() - cube) < 0.5 &&
              (inlay[0].solid ^ inlay[1].solid).Volume() < 0.01,
          "dice: inlay fills the pips exactly");
    pipsDie["style"] = "raised";
    const auto raised = make("dice", pipsDie, "dice raised");
    check(!raised.empty() && raised[0].solid.Volume() > cube, "dice: raised pips stand out");
  }

  // Lithophane: every shape and both kinds come out as one solid, thin where the picture is bright
  // (a lithophane) and inside the thickness range; a picture survives being stored in the settings.
  {
    QImage stripes(60, 40, QImage::Format_Grayscale8);
    for (int y = 0; y < 40; ++y)
      for (int x = 0; x < 60; ++x) stripes.scanLine(y)[x] = x < 30 ? 255 : 0;  // bright left, dark right
    const QString file = QDir::temp().filePath("daedal_litho_test.png");
    stripes.save(file);
    const QString value = gen::pictureValue(file);
    check(!value.isEmpty(), "lithophane: picture stored");
    for (const char* shape : {"panel", "framed", "keychain", "round"})
      for (const char* mode : {"litho", "relief"}) {
        QJsonObject values{{"picture", value}, {"shape", shape}, {"mode", mode}, {"width", 60.0}, {"thin", 0.8}, {"thick", 3.0}};
        const auto pieces = make("lithophane", values, std::string("lithophane ") + shape + " " + mode);
        check(pieces.size() == 1 && pieces[0].solid.Genus() == (std::string(shape) == "keychain" ? 1 : 0),
              std::string("lithophane ") + shape + " " + mode + ": one closed solid");
        if (pieces.empty()) continue;
        // The thickness over the left (bright) and right (dark) halves, from slices high up.
        const auto box = pieces[0].solid.BoundingBox();
        const double z0 = box.min.z;
        auto areaAt = [&](double z, double x0, double x1) {
          const auto cut = pieces[0].solid ^ manifold::Manifold::Cube({x1 - x0, 20, 20}).Translate({x0, -10, z0 + z});
          return cut.Volume();
        };
        const double brightHigh = areaAt(2.0, -25, -5), darkHigh = areaAt(2.0, 5, 25);
        const bool litho = std::string(mode) == "litho";
        check(litho ? darkHigh > 10 * (brightHigh + 1e-3) : brightHigh > 10 * (darkHigh + 1e-3),
              std::string("lithophane ") + shape + " " + mode + ": thick where it should be");
        if (std::string(shape) == "panel") check(std::abs(box.max.z - box.min.z - 3.0) < 0.05, "lithophane: thickest 3 mm");
      }
    QFile::remove(file);
  }

  std::printf(failures ? "%d failure(s)\n" : "all generator checks passed\n", failures);
  return failures ? 1 : 0;
}
