#include "generators.h"

#include <manifold/cross_section.h>

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace gen {

namespace {

using manifold::CrossSection;
using manifold::Manifold;
using manifold::MeshGL64;
using manifold::OpType;
using manifold::Polygons;
using manifold::SimplePolygon;
using manifold::vec2;
using manifold::vec3;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------
// Settings

Param number(const QString& key, const QString& label, double def, double lo, double hi, double step = 1,
             int decimals = 1, const QString& suffix = QStringLiteral(" mm"), const QString& tip = QString()) {
  Param p;
  p.key = key;
  p.label = label;
  p.type = Param::Number;
  p.def = def;
  p.min = lo;
  p.max = hi;
  p.step = step;
  p.decimals = decimals;
  p.suffix = suffix;
  p.tip = tip;
  return p;
}

Param integer(const QString& key, const QString& label, int def, int lo, int hi, const QString& tip = QString()) {
  Param p = number(key, label, def, lo, hi, 1, 0, QString(), tip);
  p.type = Param::Integer;
  return p;
}

Param choice(const QString& key, const QString& label, std::vector<std::pair<QString, QString>> choices,
             const QString& def, const QString& tip = QString()) {
  Param p;
  p.key = key;
  p.label = label;
  p.type = Param::Choice;
  p.choices = std::move(choices);
  p.defChoice = def;
  p.tip = tip;
  return p;
}

Param toggle(const QString& key, const QString& label, bool def, const QString& tip = QString()) {
  Param p;
  p.key = key;
  p.label = label;
  p.type = Param::Toggle;
  p.def = def ? 1 : 0;
  p.tip = tip;
  return p;
}

Param text(const QString& key, const QString& label, const QString& def, int maxLength, const QString& tip = QString()) {
  Param p;
  p.key = key;
  p.label = label;
  p.type = Param::Text;
  p.defText = def;
  p.max = maxLength;
  p.tip = tip;
  return p;
}

Param picture(const QString& key, const QString& label, const QString& tip = QString()) {
  Param p;
  p.key = key;
  p.label = label;
  p.type = Param::Picture;
  p.tip = tip;
  return p;
}

Param heading(const QString& label) {
  Param p;
  p.key = QStringLiteral("_") + label;
  p.label = label;
  p.type = Param::Heading;
  return p;
}

Param when(Param p, const QString& condition) {
  p.showIf = condition;
  return p;
}

// The "Use printer clearance" switch, and a clearance that follows it.
Param usePrinter(bool def, const QString& tip) {
  return toggle("usePrinter", "Use printer clearance", def, tip);
}

Param followsPrinter(Param p, double factor) {
  p.printerFactor = factor;
  return p;
}

const std::vector<std::pair<QString, QString>>& metricSizes() {
  static const std::vector<std::pair<QString, QString>> sizes = {
      {"M3", "M3"},   {"M4", "M4"},   {"M5", "M5"},   {"M6", "M6"},   {"M8", "M8"},   {"M10", "M10"},
      {"M12", "M12"}, {"M16", "M16"}, {"M20", "M20"}, {"M24", "M24"}, {"custom", "Custom"}};
  return sizes;
}

std::vector<Spec> makeSpecs() {
  std::vector<Spec> all;
  const QString storage = QStringLiteral("Storage");
  const QString fasteners = QStringLiteral("Fasteners");
  const QString mechanical = QStringLiteral("Mechanical");
  const QString other = QStringLiteral("Panels");
  const QString workshop = QStringLiteral("Workshop");
  const QString calibration = QStringLiteral("Testing and calibration");
  // The O-ring settings shared by the box, the enclosure and the jar.
  auto sealParams = [](const QString& condition) {
    return std::vector<Param>{
        when(number("cord", "O-ring thickness", 2, 1, 6, 0.1, 1, " mm",
                    "The cross-section of the O-ring or cord (1.5, 2, 2.5 and 3 mm are common), or the thickness of a "
                    "printed gasket. The groove is sized from it."),
             condition),
        when(number("squeeze", "Squeeze", 25, 10, 40, 1, 0, QStringLiteral(" %"),
                    "How much the lid presses the ring flat when it is closed. 20 to 30 % seals well; more is harder to close."),
             condition),
        when(toggle("gasket", "Gasket to print in TPU", false,
                    "A flat ring that fits the groove, for when you have no cord. Print it in TPU or another flexible "
                    "filament."),
             condition)};
  };
  // Where the screws of a sealed lid go.
  auto screwPlace = [](const QString& condition) {
    return when(choice("screwPlace", "Screws sit",
                       {{"ears", "In ears outside the walls"}, {"walls", "In the walls (flat sides)"}}, "ears",
                       "Ears: the screws stand out from the box, and the walls stay thin. In the walls: the outside "
                       "stays a plain box; the screws go into the wall at the corners, the groove bends inward round "
                       "them and the inside corners fill in to make room. Either way no screw crosses the seal."),
                condition);
  };
  // Puts `extra` right after the parameter `key`.
  auto insertAfter = [](std::vector<Param>& params, const QString& key, const std::vector<Param>& extra) {
    auto at = std::find_if(params.begin(), params.end(), [&](const Param& p) { return p.key == key; });
    params.insert(at == params.end() ? at : at + 1, extra.begin(), extra.end());
  };
  const QString electronics = QStringLiteral("Electronics");
  const QString household = QStringLiteral("Household");

  {
    Spec s{QStringLiteral("box"), QStringLiteral("Box with lid"), storage,
           QStringLiteral("A box and a lid that fits it. Clearance is the gap between the parts; 0.2 mm suits most "
                          "printers. Print the fit test first if lids come out too tight or too loose."),
           {}};
    s.params = {heading(QStringLiteral("Size")),
                number("length", "Length X", 80, 10, 400),
                number("width", "Width Y", 60, 10, 400),
                number("height", "Height Z", 40, 5, 300, 1, 1, " mm", "The box without its lid."),
                number("wall", "Wall", 2, 0.8, 10, 0.2),
                number("floor", "Floor", 1.6, 0.6, 10, 0.2),
                number("corner", "Corner radius", 4, 0, 60, 0.5),
                heading(QStringLiteral("Lid")),
                choice("lid", "Lid",
                       {{"lip", "Lip (press fit)"}, {"snap", "Snap"}, {"slide", "Sliding"}, {"seal", "Sealed (O-ring, screws)"}, {"none", "No lid"}},
                       "lip",
                       "Lip: a rim on the lid drops into the box. Snap: the same with a small ridge that clicks into "
                       "a groove. Sliding: the lid slides along X, in grooves in the walls or over rails on their outside. Sealed: an O-ring in "
                       "a groove on top of the walls, a tongue on the lid that presses it, and screws in ears outside the walls."),
                when(number("lidThickness", "Lid thickness", 2, 0.8, 10, 0.2), "lid!=none"),
                when(number("lipDepth", "Lip depth", 6, 2, 40, 0.5, 1, " mm", "How far the lid's rim reaches into the box."),
                     "lid=lip|snap"),
                when(usePrinter(true, "Use the clearance from Settings (Printer). Off: set this box's own below."), "lid!=none"),
                when(followsPrinter(number("clearance", "Clearance", 0.2, 0, 1.5, 0.01, 2, " mm",
                                           "Gap between lid and box on every side."),
                                    1.0),
                     "lid!=none&usePrinter=0"),
                when(choice("slideStyle", "Slides", {{"inset", "In grooves (inside the walls)"}, {"outset", "Over rails (outside the walls)"}},
                            "inset",
                            "In grooves: the lid sits between the walls, flush with their tops. Over rails: the lid covers "
                            "the whole top and its rim wraps the outside of the walls, flush with the box's sides, hooked "
                            "under a step in them. Either way it slides out toward -X."),
                     "lid=slide"),
                when(toggle("slideSnap", "Snap lock", true,
                            "A small bump on the wall at the open end that clicks into a groove under the lid when it is "
                            "fully closed, so it does not slide open by itself."),
                     "lid=slide"),
                when(toggle("notch", "Finger notch", true, "A notch to get a finger under the lid."), "lid=lip|snap|slide"),
                when(choice("screw", "Screws", {{"M2.5", "M2.5"}, {"M3", "M3"}, {"M4", "M4"}}, "M3"), "lid=seal"),
                when(choice("hole", "Ears take", {{"insert", "Heat-set inserts"}, {"screw", "Self-tapping screws"}}, "insert"), "lid=seal"),
                heading(QStringLiteral("Inside")),
                integer("dividersX", "Dividers across X", 0, 0, 20),
                integer("dividersY", "Dividers across Y", 0, 0, 20),
                heading(QStringLiteral("Layout")),
                when(choice("layout", "Layout", {{"print", "Print: side by side"}, {"assembled", "Assembled"}}, "print",
                            "Print lays the lid next to the box, ready for the printer. Assembled shows the lid in "
                            "place to check the fit."),
                     "lid!=none")};
    insertAfter(s.params, QStringLiteral("hole"), sealParams(QStringLiteral("lid=seal")));
    insertAfter(s.params, QStringLiteral("hole"), {screwPlace(QStringLiteral("lid=seal"))});
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("gridbin"), QStringLiteral("Gridfinity bin"), storage,
           QStringLiteral("A bin for the Gridfinity system: 42 mm grid, 7 mm height units. Fits any Gridfinity "
                          "baseplate."),
           {}};
    s.params = {heading(QStringLiteral("Size")),
                integer("unitsX", "Units X", 2, 1, 10, "42 mm per unit."),
                integer("unitsY", "Units Y", 1, 1, 10, "42 mm per unit."),
                integer("unitsZ", "Height units", 3, 2, 20, "7 mm per unit, without the stacking lip."),
                number("wall", "Wall", 1.2, 0.8, 2.4, 0.1),
                number("floor", "Floor", 1.0, 0.6, 6, 0.2, 1, " mm", "Floor thickness above the feet."),
                heading(QStringLiteral("Options")),
                toggle("lip", "Stacking lip", true, "A lip on top, so bins stack on each other."),
                toggle("magnets", "Magnet holes", false, "6 x 2 mm magnet holes under every foot."),
                integer("dividersX", "Dividers across X", 0, 0, 20),
                integer("dividersY", "Dividers across Y", 0, 0, 20)};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("gridplate"), QStringLiteral("Gridfinity baseplate"), storage,
           QStringLiteral("The plate Gridfinity bins sit in. Open bottom is the light, standard version."), {}};
    s.params = {integer("unitsX", "Units X", 3, 1, 12, "42 mm per unit."),
                integer("unitsY", "Units Y", 3, 1, 12, "42 mm per unit."),
                toggle("solid", "Solid bottom", false),
                when(number("bottom", "Bottom thickness", 3.4, 1, 10, 0.2), "solid=1"),
                when(toggle("magnets", "Magnet holes", false, "6 x 2 mm magnet holes in every pocket."), "solid=1")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("jar"), QStringLiteral("Screw jar"), storage,
           QStringLiteral("A round jar with a screw-on lid. The thread has 45 degree flanks, so it prints without "
                          "supports."),
           {}};
    s.params = {number("diameter", "Inside diameter", 60, 15, 250),
                number("height", "Height", 70, 20, 300, 1, 1, " mm", "The jar without its lid."),
                number("wall", "Wall", 2.4, 1.2, 8, 0.2),
                number("pitch", "Thread pitch", 4, 2, 10, 0.5),
                number("turns", "Thread turns", 2, 1, 6, 0.25, 2, QString()),
                usePrinter(false, "Make the gap on the thread flanks the clearance from Settings (Printer). Threads often "
                                  "like a little more play than a lid, so this starts off."),
                when(followsPrinter(number("clearance", "Clearance", 0.4, 0.05, 1.5, 0.01, 2, " mm",
                                           "Radial play between the jar's thread and the lid's."),
                                    std::sqrt(2.0)),
                     "usePrinter=0"),
                toggle("grip", "Grip ridges on the lid", true),
                toggle("seal", "O-ring seal", false,
                       "An O-ring groove in the top of the neck and a tongue in the lid that presses it as you screw the "
                       "lid on. The mouth gets a little narrower to make room for the groove."),
                choice("layout", "Layout", {{"print", "Print: side by side"}, {"assembled", "Assembled"}}, "print")};
    insertAfter(s.params, QStringLiteral("seal"), sealParams(QStringLiteral("seal=1")));
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("enclosure"), QStringLiteral("Electronics enclosure"), electronics,
           QStringLiteral("A project box for a circuit board: screw posts or heat-set inserts in the corners for the lid, "
                          "optional standoffs for the board, vents, and openings for USB, power and buttons. Sizes are the "
                          "space inside."),
           {}};
    s.params = {heading(QStringLiteral("Inside")),
                number("length", "Length X", 80, 15, 400),
                number("width", "Width Y", 50, 15, 400),
                number("height", "Height Z", 30, 5, 300, 1, 1, " mm", "From the floor to the top of the walls."),
                number("wall", "Wall", 2, 1, 8, 0.2),
                number("floor", "Floor", 2, 0.8, 10, 0.2),
                number("corner", "Corner radius", 3, 0, 40, 0.5, 1, " mm", "Outside corners."),
                heading(QStringLiteral("Lid")),
                choice("lid", "Lid", {{"screws", "Screwed (corner posts)"}, {"lip", "Press fit (lip)"}}, "screws"),
                number("lidThickness", "Lid thickness", 2, 1, 10, 0.2),
                when(choice("screw", "Screws", {{"M2", "M2"}, {"M2.5", "M2.5"}, {"M3", "M3"}}, "M3"), "lid=screws"),
                when(choice("hole", "Posts take", {{"insert", "Heat-set inserts"}, {"screw", "Self-tapping screws"}}, "insert"),
                     "lid=screws"),
                when(toggle("seal", "O-ring seal", false,
                            "Watertight: an O-ring groove in the top of the walls and a tongue on the lid that presses it. "
                            "The screws move to ears outside the walls, so no hole crosses the seal. Walls grow to fit the groove."),
                     "lid=screws"),
                usePrinter(true, "Use the clearance from Settings (Printer) for the lid's locating rim. Off: set its own below."),
                when(followsPrinter(number("clearance", "Clearance", 0.2, 0, 1.5, 0.01, 2, " mm",
                                           "Gap between the lid's rim and the walls."),
                                    1.0),
                     "usePrinter=0"),
                choice("vents", "Lid vents", {{"none", "None"}, {"slots", "Slots"}, {"honeycomb", "Honeycomb"}}, "none"),
                toggle("sideVents", "Vent slots in the long walls", false),
                heading(QStringLiteral("Board standoffs")),
                toggle("pcb", "Standoffs for a board", false),
                when(number("pcbX", "Hole spacing X", 58, 5, 380, 0.5, 1, " mm", "Center to center. 58 x 49 fits a Raspberry Pi."), "pcb=1"),
                when(number("pcbY", "Hole spacing Y", 49, 5, 380, 0.5), "pcb=1"),
                when(number("pcbOffsetX", "Shift X", 0, -200, 200, 0.5, 1, " mm", "Move the board off the middle."), "pcb=1"),
                when(number("pcbOffsetY", "Shift Y", 0, -200, 200, 0.5), "pcb=1"),
                when(number("pcbHeight", "Standoff height", 5, 1, 50, 0.5), "pcb=1"),
                when(choice("pcbScrew", "Board screws", {{"M2", "M2"}, {"M2.5", "M2.5"}, {"M3", "M3"}}, "M2.5"), "pcb=1"),
                heading(QStringLiteral("Cut-out 1")),
                choice("cut1Wall", "Wall", {{"none", "None"}, {"front", "Front (-Y)"}, {"back", "Back (+Y)"}, {"left", "Left (-X)"}, {"right", "Right (+X)"}},
                       "front", "Which wall the opening goes through."),
                when(choice("cut1Type", "Opening",
                            {{"usbc", "USB-C"}, {"microusb", "Micro USB"}, {"usba", "USB-A"}, {"round", "Round"}, {"rect", "Rectangle"}},
                            "usbc", "USB openings are a little bigger than the plug, so it goes in at an angle too."),
                     "cut1Wall!=none"),
                when(number("cut1Diameter", "Diameter", 8, 1, 100, 0.5), "cut1Wall!=none&cut1Type=round"),
                when(number("cut1W", "Width", 12, 1, 200, 0.5), "cut1Wall!=none&cut1Type=rect"),
                when(number("cut1H", "Height", 6, 1, 200, 0.5), "cut1Wall!=none&cut1Type=rect"),
                when(number("cut1X", "Sideways", 0, -200, 200, 0.5, 1, " mm", "From the middle of the wall, to the right as you face it."),
                     "cut1Wall!=none"),
                when(number("cut1Z", "Height", 5, 0, 300, 0.5, 1, " mm", "Above the floor, to the middle of the opening."),
                     "cut1Wall!=none"),
                heading(QStringLiteral("Cut-out 2")),
                choice("cut2Wall", "Wall", {{"none", "None"}, {"front", "Front (-Y)"}, {"back", "Back (+Y)"}, {"left", "Left (-X)"}, {"right", "Right (+X)"}},
                       "none", "Which wall the opening goes through."),
                when(choice("cut2Type", "Opening",
                            {{"usbc", "USB-C"}, {"microusb", "Micro USB"}, {"usba", "USB-A"}, {"round", "Round"}, {"rect", "Rectangle"}},
                            "usbc", "USB openings are a little bigger than the plug, so it goes in at an angle too."),
                     "cut2Wall!=none"),
                when(number("cut2Diameter", "Diameter", 8, 1, 100, 0.5), "cut2Wall!=none&cut2Type=round"),
                when(number("cut2W", "Width", 12, 1, 200, 0.5), "cut2Wall!=none&cut2Type=rect"),
                when(number("cut2H", "Height", 6, 1, 200, 0.5), "cut2Wall!=none&cut2Type=rect"),
                when(number("cut2X", "Sideways", 0, -200, 200, 0.5, 1, " mm", "From the middle of the wall, to the right as you face it."),
                     "cut2Wall!=none"),
                when(number("cut2Z", "Height", 8, 0, 300, 0.5, 1, " mm", "Above the floor, to the middle of the opening."),
                     "cut2Wall!=none"),
                heading(QStringLiteral("Cut-out 3")),
                choice("cut3Wall", "Wall", {{"none", "None"}, {"front", "Front (-Y)"}, {"back", "Back (+Y)"}, {"left", "Left (-X)"}, {"right", "Right (+X)"}},
                       "none", "Which wall the opening goes through."),
                when(choice("cut3Type", "Opening",
                            {{"usbc", "USB-C"}, {"microusb", "Micro USB"}, {"usba", "USB-A"}, {"round", "Round"}, {"rect", "Rectangle"}},
                            "usbc", "USB openings are a little bigger than the plug, so it goes in at an angle too."),
                     "cut3Wall!=none"),
                when(number("cut3Diameter", "Diameter", 8, 1, 100, 0.5), "cut3Wall!=none&cut3Type=round"),
                when(number("cut3W", "Width", 12, 1, 200, 0.5), "cut3Wall!=none&cut3Type=rect"),
                when(number("cut3H", "Height", 6, 1, 200, 0.5), "cut3Wall!=none&cut3Type=rect"),
                when(number("cut3X", "Sideways", 0, -200, 200, 0.5, 1, " mm", "From the middle of the wall, to the right as you face it."),
                     "cut3Wall!=none"),
                when(number("cut3Z", "Height", 8, 0, 300, 0.5, 1, " mm", "Above the floor, to the middle of the opening."),
                     "cut3Wall!=none"),
                heading(QStringLiteral("Layout")),
                choice("layout", "Layout", {{"print", "Print: side by side"}, {"assembled", "Assembled"}}, "print")};
    insertAfter(s.params, QStringLiteral("seal"), sealParams(QStringLiteral("lid=screws&seal=1")));
    insertAfter(s.params, QStringLiteral("seal"), {screwPlace(QStringLiteral("lid=screws&seal=1"))});
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("battery"), QStringLiteral("Battery holder"), electronics,
           QStringLiteral("A block with a pocket for every battery. Cells stand upright; coin cells stand on their edge."), {}};
    s.params = {choice("cell", "Battery",
                       {{"AA", "AA"}, {"AAA", "AAA"}, {"C", "C"}, {"D", "D"}, {"18650", "18650"}, {"21700", "21700"},
                        {"CR2032", "CR2032 coin"}, {"9V", "9 V block"}},
                       "AA"),
                integer("columns", "Columns", 4, 1, 30),
                integer("rows", "Rows", 2, 1, 30),
                number("depth", "Pocket depth", 50, 20, 95, 5, 0, QStringLiteral(" %"), "How much of the battery sits in the pocket."),
                number("clearance", "Clearance", 0.3, 0, 2, 0.05, 2, " mm",
                       "Room around each battery. Batteries vary a little, so a bit more than a printed fit."),
                number("web", "Wall between", 1.6, 0.8, 10, 0.2),
                number("rim", "Outer wall", 2.4, 1, 20, 0.2),
                number("base", "Base", 2, 0.8, 20, 0.2),
                number("corner", "Corner radius", 3, 0, 20, 0.5)};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("bolt"), QStringLiteral("Bolt"), fasteners,
           QStringLiteral("A metric bolt or threaded rod, with a matching nut if you like. Printed threads work best "
                          "from M6 up."),
           {}};
    s.params = {choice("size", "Size", metricSizes(), "M10"),
                when(number("diameter", "Diameter", 10, 2, 100, 0.5), "size=custom"),
                when(number("pitch", "Pitch", 1.5, 0.3, 10, 0.05, 2), "size=custom"),
                number("length", "Thread length", 25, 3, 300),
                choice("head", "Head", {{"hex", "Hex"}, {"knurled", "Knurled (thumb screw)"}, {"none", "None (threaded rod)"}},
                       "hex"),
                toggle("nut", "Matching nut", true),
                when(usePrinter(false, "Make the gap on the thread flanks the clearance from Settings (Printer). Threads "
                                       "often like a little more play, so this starts off."),
                     "nut=1"),
                when(followsPrinter(number("clearance", "Nut clearance", 0.3, 0, 1.5, 0.01, 2, " mm",
                                           "Radial play between bolt and nut."),
                                    2.0),
                     "nut=1&usePrinter=0"),
                when(choice("layout", "Layout", {{"print", "Print: side by side"}, {"assembled", "Nut on the bolt"}}, "print"),
                     "nut=1")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("nut"), QStringLiteral("Nut"), fasteners,
           QStringLiteral("A metric hex nut. Use a little more clearance to fit a metal bolt."), {}};
    s.params = {choice("size", "Size", metricSizes(), "M10"),
                when(number("diameter", "Diameter", 10, 2, 100, 0.5), "size=custom"),
                when(number("pitch", "Pitch", 1.5, 0.3, 10, 0.05, 2), "size=custom"),
                toggle("thin", "Thin (jam) nut", false),
                usePrinter(false, "Make the gap on the thread flanks the clearance from Settings (Printer). For a metal "
                                  "bolt you usually want a little more, so this starts off."),
                when(followsPrinter(number("clearance", "Clearance", 0.3, 0, 1.5, 0.01, 2, " mm",
                                           "Radial play between bolt and nut."),
                                    2.0),
                     "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("standoff"), QStringLiteral("Standoffs"), fasteners,
           QStringLiteral("Screw posts for circuit boards and lids, for a screw, a heat-set insert or a bolt through."),
           {}};
    s.params = {choice("layout", "Layout", {{"single", "One post"}, {"pair", "Two posts"}, {"grid", "Four posts (2 x 2)"}},
                       "grid"),
                when(number("spacingX", "Spacing X", 58, 5, 400, 0.5, 1, " mm", "Center to center."), "layout=pair|grid"),
                when(number("spacingY", "Spacing Y", 49, 5, 400, 0.5, 1, " mm", "Center to center."), "layout=grid"),
                choice("screw", "Screw", {{"M2", "M2"}, {"M2.5", "M2.5"}, {"M3", "M3"}, {"M4", "M4"}, {"M5", "M5"}}, "M2.5"),
                choice("hole", "Hole",
                       {{"insert", "Heat-set insert"}, {"screw", "Screw (self tapping)"}, {"through", "Through hole"}},
                       "insert"),
                number("height", "Height", 8, 2, 100),
                number("postWall", "Wall around hole", 1.6, 0.8, 6, 0.2),
                toggle("flare", "Wide foot", true, "A cone at the foot for strength."),
                toggle("plate", "Base plate", false),
                when(number("plateThickness", "Plate thickness", 2, 0.8, 10, 0.2), "plate=1"),
                when(number("margin", "Plate margin", 5, 1, 50, 0.5), "plate=1")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("gear"), QStringLiteral("Spur gear"), mechanical,
           QStringLiteral("An involute gear, with a second gear that meshes with it if you like. Gears that mesh need "
                          "the same module."),
           {}};
    s.params = {number("module", "Module", 2, 0.5, 10, 0.25, 2, QString(), "Tooth size: pitch diameter = module x teeth."),
                integer("teeth", "Teeth", 20, 6, 200),
                number("thickness", "Thickness", 8, 1, 100),
                number("bore", "Bore diameter", 5, 0, 200, 0.5, 1, " mm", "0 = no hole."),
                number("pressure", "Pressure angle", 20, 14.5, 30, 0.5, 1, QStringLiteral("°")),
                number("backlash", "Backlash", 0.15, 0, 1, 0.05, 2),
                number("helix", "Helix angle", 0, 0, 45, 5, 0, QStringLiteral("°"), "0 = straight teeth."),
                when(toggle("herringbone", "Herringbone", false, "Two opposite helixes: no side push."), "helix!=0"),
                heading(QStringLiteral("Mating gear")),
                toggle("mate", "Mating gear", false),
                when(integer("mateTeeth", "Teeth", 12, 6, 200), "mate=1"),
                when(number("mateBore", "Bore diameter", 5, 0, 200, 0.5), "mate=1"),
                when(choice("layout", "Layout", {{"mesh", "Meshing"}, {"apart", "Apart"}}, "mesh"), "mate=1")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("rack"), QStringLiteral("Rack and pinion"), mechanical,
           QStringLiteral("A straight toothed rack, with a pinion gear that meshes with it if you like. Use the same "
                          "module as the spur gears to mix them."),
           {}};
    s.params = {number("module", "Module", 2, 0.5, 10, 0.25, 2, QString(), "Tooth size; must match the gear."),
                number("length", "Length", 100, 10, 1000, 1, 0, " mm", "Rounded to whole teeth."),
                number("thickness", "Thickness", 8, 1, 100),
                number("backing", "Backing", 6, 1, 50, 0.5, 1, " mm", "Solid material under the teeth."),
                number("pressure", "Pressure angle", 20, 14.5, 30, 0.5, 1, QStringLiteral("°")),
                number("backlash", "Backlash", 0.15, 0, 1, 0.05, 2),
                number("helix", "Helix angle", 0, 0, 45, 5, 0, QStringLiteral("°"), "0 = straight teeth."),
                when(toggle("herringbone", "Herringbone", false), "helix!=0"),
                toggle("holes", "Mounting holes", true),
                heading(QStringLiteral("Pinion")),
                toggle("pinion", "Pinion", true),
                when(integer("teeth", "Teeth", 16, 6, 200), "pinion=1"),
                when(number("bore", "Bore diameter", 5, 0, 200, 0.5), "pinion=1"),
                when(choice("layout", "Layout", {{"mesh", "Meshing"}, {"apart", "Apart"}}, "mesh"), "pinion=1")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("hinge"), QStringLiteral("Print-in-place hinge"), mechanical,
           QStringLiteral("Two leaves on a pin, printed as one piece that moves straight off the printer. Use 0.3 to "
                          "0.5 mm clearance."),
           {}};
    s.params = {number("length", "Length", 40, 10, 300),
                number("leaf", "Leaf width", 20, 4, 200),
                number("thickness", "Leaf thickness", 3, 1, 20, 0.2),
                integer("knuckles", "Knuckles", 5, 3, 31, "An odd number; the outer ones hold the pin."),
                number("knuckleDiameter", "Knuckle diameter", 8, 4, 40, 0.5),
                number("pin", "Pin size", 3.6, 1, 30, 0.2),
                number("clearance", "Clearance", 0.4, 0.15, 1.5, 0.05, 2),
                integer("holes", "Screw holes per leaf", 2, 0, 8),
                when(number("holeDiameter", "Hole diameter", 3.4, 1, 12, 0.1), "holes!=0"),
                when(toggle("countersink", "Countersunk", true), "holes!=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("balljoint"), QStringLiteral("Ball joint"), mechanical,
           QStringLiteral("A ball on a stem and a slotted socket it snaps into: for camera and phone arms, lamps, "
                          "posable mounts. The slots let the socket open as the ball goes in and give it its grip."),
           {}};
    s.params = {number("ball", "Ball diameter", 16, 6, 80, 0.5),
                number("stem", "Stem diameter", 8, 2, 60, 0.5, 1, " mm", "Thinner swings further."),
                number("stemLength", "Stem length", 15, 2, 200, 1),
                usePrinter(true, "Use the clearance from Settings (Printer) around the ball. Off: set its own below."),
                when(followsPrinter(number("clearance", "Clearance", 0.15, 0, 1, 0.01, 2, " mm",
                                           "Gap around the ball. Less is stiffer."),
                                    1.0),
                     "usePrinter=0"),
                choice("ballBase", "Ball end", {{"plate", "Plate with screw holes"}, {"foot", "Round foot"}, {"camera", "1/4\"-20 camera thread"}, {"none", "Plain stem"}},
                       "plate", "What the ball's stem ends in."),
                choice("socketBase", "Socket end", {{"plate", "Plate with screw holes"}, {"foot", "Round foot"}, {"camera", "1/4\"-20 camera thread"}, {"none", "Plain stem"}},
                       "camera", "What the socket's stem ends in."),
                choice("layout", "Layout", {{"print", "Print: side by side"}, {"assembled", "Assembled"}}, "print")};
    all.push_back(s);
  }
  // --- Mechanical, continued ------------------------------------------------------------------
  {
    Spec s{QStringLiteral("pulley"), QStringLiteral("Pulley"), mechanical,
           QStringLiteral("A GT2 timing pulley (2 mm pitch belts, as on 3D printers), a V-belt pulley or a plain pulley "
                          "with flanges. Bore round or with a flat for a D shaft; a hub with a set screw if you like. "
                          "A V-belt pulley comes in two halves split in the groove, so both flanks print cleanly."),
           {}};
    s.params = {heading(QStringLiteral("Pulley")),
                choice("type", "Type", {{"gt2", "GT2 timing"}, {"vbelt", "V-belt"}, {"flat", "Plain with flanges"}}, "gt2"),
                when(integer("teeth", "Teeth", 20, 10, 120), "type=gt2"),
                when(number("diameter", "Diameter", 30, 8, 300, 1, 1, " mm", "Where the belt runs: the bottom of a V groove "
                                                                                  "is the groove depth less."),
                     "type!=gt2"),
                number("beltWidth", "Belt width", 6, 2, 50, 0.5),
                when(number("grooveDepth", "Groove depth", 3, 1, 15, 0.5), "type=vbelt"),
                when(toggle("flanges", "Flanges", true, "Rims that keep the belt on. The top one leans out at 45°, so it "
                                                       "prints without support."),
                     "type!=vbelt"),
                heading(QStringLiteral("Bore and hub")),
                number("bore", "Bore", 5, 0, 60, 0.5, 1, " mm", "0 = no hole."),
                choice("boreShape", "Bore shape", {{"round", "Round"}, {"d", "D (flat side)"}}, "round"),
                when(toggle("hub", "Hub", true, "On top, with a set screw."), "type!=vbelt"),
                when(number("hubDiameter", "Hub diameter", 16, 6, 120, 0.5), "hub=1&type!=vbelt"),
                when(number("hubLength", "Hub length", 7, 3, 50, 0.5), "hub=1&type!=vbelt"),
                when(choice("setScrew", "Set screw (M3)",
                            {{"none", "None"}, {"nut", "With a nut pocket"}, {"tap", "Tapped into the plastic"}, {"insert", "Heat-set insert"}},
                            "nut"),
                     "hub=1&type!=vbelt"),
                usePrinter(false, QStringLiteral("Bore clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Bore clearance", 0.1, 0, 1, 0.05, 2, " mm", "Added all around the bore."), 1),
                     "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("bearing"), QStringLiteral("Bearing housing"), mechanical,
           QStringLiteral("A housing for a standard ball bearing: a round or square flange, or a pillow block with screw "
                          "slots. A small lip stops the bearing; the hole through it clears the inner ring."),
           {}};
    s.params = {heading(QStringLiteral("Bearing")),
                choice("bearing", "Bearing",
                       {{"623", "623 (3 x 10 x 4)"},
                        {"625", "625 (5 x 16 x 5)"},
                        {"688", "688 (8 x 16 x 5)"},
                        {"608", "608 (8 x 22 x 7, skateboard)"},
                        {"6000", "6000 (10 x 26 x 8)"},
                        {"6001", "6001 (12 x 28 x 8)"},
                        {"6002", "6002 (15 x 32 x 9)"},
                        {"6201", "6201 (12 x 32 x 10)"},
                        {"6202", "6202 (15 x 35 x 11)"},
                        {"6204", "6204 (20 x 47 x 14)"},
                        {"custom", "Custom"}},
                       "608"),
                when(number("innerDiameter", "Inner diameter", 8, 1, 100, 0.5), "bearing=custom"),
                when(number("outerDiameter", "Outer diameter", 22, 3, 150, 0.5), "bearing=custom"),
                when(number("bearingWidth", "Width", 7, 1, 50, 0.5), "bearing=custom"),
                heading(QStringLiteral("Housing")),
                choice("style", "Style", {{"flange", "Round flange"}, {"square", "Square flange"}, {"pillow", "Pillow block"}}, "flange"),
                number("wall", "Wall", 3, 1.2, 15, 0.2),
                choice("screw", "Screws", {{"M3", "M3"}, {"M4", "M4"}, {"M5", "M5"}}, "M4"),
                toggle("lip", "Lip that stops the bearing", true),
                usePrinter(false, QStringLiteral("The seat clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Seat clearance", 0.1, -0.2, 1, 0.05, 2, " mm",
                                           "Added all around the bearing. Around 0 to 0.1 gives a press fit on most printers; "
                                           "negative squeezes it in."),
                                    1),
                     "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("coupler"), QStringLiteral("Shaft coupler"), mechanical,
           QStringLiteral("A rigid coupler joining two shafts (for example 5 to 8 mm), or a shaft collar. M3 set screws with "
                          "a nut pocket, tapped straight into the plastic, or in heat-set inserts."),
           {}};
    s.params = {choice("type", "Type", {{"coupler", "Coupler (two shafts)"}, {"collar", "Shaft collar"}}, "coupler"),
                number("bore1", "Bore", 5, 1, 60, 0.5, 1, " mm", "The coupler's bottom bore, or the collar's."),
                when(number("bore2", "Second bore", 8, 1, 60, 0.5, 1, " mm", "The top bore."), "type=coupler"),
                when(number("length", "Length", 25, 10, 150, 1), "type=coupler"),
                when(number("width", "Width", 8, 8, 60, 0.5), "type=collar"),
                number("wall", "Wall", 6, 2, 20, 0.5, 1, " mm", "Around the widest bore; made wider when the nut pockets need it."),
                number("outerDiameter", "Outer diameter", 0, 0, 120, 0.5, 1, " mm", "0 = from the wall."),
                choice("setScrew", "Set screws (M3)", {{"nut", "With nut pockets"}, {"tap", "Tapped into the plastic"}, {"insert", "Heat-set inserts"}},
                       "nut"),
                integer("screws", "Screws per shaft", 1, 1, 2, "Two sit at 90° to each other."),
                usePrinter(false, QStringLiteral("Bore clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Bore clearance", 0.1, 0, 1, 0.05, 2), 1), "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("spring"), QStringLiteral("Spring"), mechanical,
           QStringLiteral("A compression spring of square wire, or a flat zig-zag spring. Print in PETG or nylon for "
                          "springs that last; PLA tires quickly."),
           {}};
    s.params = {choice("type", "Type", {{"coil", "Compression (coil)"}, {"flat", "Flat zig-zag"}}, "coil"),
                when(number("diameter", "Outer diameter", 20, 6, 200, 0.5), "type=coil"),
                when(number("wire", "Wire width", 2, 0.8, 20, 0.2, 1, " mm", "Across, from the outside in."), "type=coil"),
                when(number("coil", "Wire height", 1.6, 0.6, 20, 0.2, 1, " mm", "Each turn's thickness, up and down."), "type=coil"),
                number("length", "Length", 30, 5, 400, 1, 1, " mm", "Coil: free height. Flat: end to end."),
                when(integer("turns", "Turns", 6, 1, 60), "type=coil"),
                when(toggle("ends", "Flat closed ends", true, "A full ring at each end, so it stands straight."), "type=coil"),
                when(number("width", "Width", 15, 4, 200, 0.5, 1, " mm", "Across the zig-zag."), "type=flat"),
                when(number("strip", "Strip", 1.2, 0.4, 10, 0.2), "type=flat"),
                when(number("height", "Height", 5, 1, 100, 0.5), "type=flat"),
                when(integer("folds", "Folds", 6, 1, 60), "type=flat")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("snapfit"), QStringLiteral("Snap-fit"), mechanical,
           QStringLiteral("A cantilever snap: a latch whose hook clicks behind a wall with a window. Use it as a pair, or "
                          "copy the latch onto your own parts. A thicker or shorter beam is stiffer."),
           {}};
    s.params = {number("beamLength", "Beam length", 14, 5, 80, 0.5),
                number("beamThickness", "Beam thickness", 1.6, 0.6, 6, 0.1),
                number("beamWidth", "Beam width", 6, 2, 40, 0.5),
                number("hookDepth", "Hook depth", 1, 0.3, 4, 0.1, 1, " mm", "How far the hook catches behind the wall."),
                number("wallThickness", "Wall thickness", 2.5, 1, 10, 0.5),
                usePrinter(false, QStringLiteral("Clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Clearance", 0.2, 0, 1, 0.05, 2), 1), "usePrinter=0"),
                choice("layout", "Layout", {{"print", "Side by side"}, {"assembled", "Assembled"}}, "print")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("livinghinge"), QStringLiteral("Living hinge"), mechanical,
           QStringLiteral("Two plates joined by a thin strip that bends: lids, clips, boxes that fold. Two layers (0.4 mm) is "
                          "a good start; PETG and PP bend far more often than PLA."),
           {}};
    s.params = {number("plateLength", "Plate length", 30, 5, 200, 1, 1, " mm", "Each plate."),
                number("width", "Width", 40, 5, 300, 1),
                number("thickness", "Plate thickness", 3, 0.8, 20, 0.2),
                number("hingeThickness", "Hinge thickness", 0.4, 0.2, 2, 0.05, 2),
                number("gap", "Hinge length", 1.6, 0.4, 10, 0.2, 1, " mm", "The flexing strip between the plates.")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("cablechain"), QStringLiteral("Cable chain"), mechanical,
           QStringLiteral("A drag chain printed in one go: links on pins that turn up to 90° either way. Guides cables to a "
                          "moving part."),
           {}};
    s.params = {number("innerWidth", "Inside width", 15, 4, 100, 0.5),
                number("innerHeight", "Inside height", 10, 4, 60, 0.5),
                number("pitch", "Link length", 20, 8, 80, 0.5, 1, " mm", "Made longer when the height needs it."),
                integer("links", "Links", 8, 2, 60),
                number("wall", "Wall", 1.6, 1, 5, 0.2),
                number("floor", "Floor", 1.6, 0.8, 5, 0.2),
                toggle("topBars", "Bars across the top", true, "Keep the cables in."),
                usePrinter(false, QStringLiteral("Clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Clearance", 0.3, 0.1, 1, 0.05, 2, " mm", "Around the pins and between the links."), 1.5),
                     "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("tubeconnector"), QStringLiteral("Tube connector"), mechanical,
           QStringLiteral("Corner and cross pieces for round or square tube: frames, racks, tents, greenhouses. Sockets go "
                          "over the tube; plugs go into it."),
           {}};
    s.params = {choice("layout", "Arms",
                       {{"straight", "2: straight"},
                        {"corner", "2: corner (L)"},
                        {"tee", "3: T"},
                        {"corner3", "3: corner (X, Y and up)"},
                        {"cross", "4: cross"},
                        {"tee4", "4: T and up"},
                        {"five", "5: cross and up"},
                        {"six", "6: all ways"}},
                       "corner3"),
                choice("shape", "Tube", {{"round", "Round"}, {"square", "Square"}}, "round"),
                number("size", "Tube size", 20, 4, 100, 0.5, 1, " mm", "Outside diameter, or outside width of a square tube."),
                choice("fit", "Fits", {{"socket", "Over the tube (socket)"}, {"plug", "Into the tube (plug)"}}, "socket"),
                when(number("tubeWall", "Tube wall", 1.5, 0.3, 10, 0.1), "fit=plug"),
                number("depth", "Arm length", 25, 5, 150, 1),
                when(number("wall", "Wall", 2.5, 1, 10, 0.5), "fit=socket"),
                usePrinter(false, QStringLiteral("Clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Clearance", 0.2, -0.3, 1.5, 0.05, 2, " mm", "Around the tube; negative grips harder."), 1),
                     "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("hose"), QStringLiteral("Hose adapter"), household,
           QStringLiteral("Joins two hoses or pipes of different sizes: vacuum cleaners, shop vacs, aquarium and garden "
                          "hoses, fume extraction. Prints standing up without supports."),
           {}};
    const std::vector<std::pair<QString, QString>> ends = {
        {"barb", "Into a hose (barbed)"}, {"smooth", "Into a hose (smooth)"}, {"sleeve", "Over a pipe (sleeve)"}};
    s.params = {heading(QStringLiteral("End A (bottom)")),
                choice("typeA", "Fits", ends, "barb"),
                number("diameterA", "Diameter", 12, 2, 300, 0.5, 1, " mm",
                       "Into a hose: the hose's inside diameter. Over a pipe: the pipe's outside diameter."),
                number("lengthA", "Length", 25, 5, 200, 1),
                heading(QStringLiteral("End B (top)")),
                choice("typeB", "Fits", ends, "sleeve"),
                number("diameterB", "Diameter", 32, 2, 300, 0.5, 1, " mm",
                       "Into a hose: the hose's inside diameter. Over a pipe: the pipe's outside diameter."),
                number("lengthB", "Length", 30, 5, 200, 1),
                heading(QStringLiteral("Body")),
                number("wall", "Wall", 2, 0.8, 10, 0.2),
                number("transition", "Transition", 12, 2, 200, 1, 1, " mm", "Length of the taper between the ends; made longer when needed to stay printable."),
                toggle("collar", "Stop collar", true, "A ring between the ends that the hoses butt against."),
                number("clearance", "Sleeve clearance", 0.3, 0, 2, 0.05, 2, " mm", "Room around a pipe a sleeve slides over.")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("honeycomb"), QStringLiteral("Pattern panel"), other,
           QStringLiteral("A plate full of holes in a pattern: hexagons, circles, squares, diamonds, triangles, stars or "
                          "slots. Grilles, vents, fan covers, light covers."),
           {}};
    s.params = {choice("make", "Make", {{"panel", "The panel"}, {"holes", "Holes only, to cut into a part"}}, "panel",
                       "Holes only: just the holes, as a hole shape. Group it with a part to cut the pattern into that "
                       "part; the outline and border say where the holes go, the thickness how deep they cut."),
                heading(QStringLiteral("Plate")),
                choice("outline", "Outline", {{"rect", "Rectangle"}, {"round", "Round"}}, "rect"),
                when(number("length", "Length X", 120, 10, 600), "outline=rect"),
                when(number("width", "Width Y", 80, 10, 600), "outline=rect"),
                when(number("diameter", "Diameter", 100, 10, 600), "outline=round"),
                number("thickness", "Thickness", 3, 0.6, 50, 0.2),
                number("border", "Border", 5, 0, 100, 0.5, 1, " mm", "Solid rim around the pattern."),
                when(number("corner", "Corner radius", 4, 0, 100, 0.5), "outline=rect"),
                heading(QStringLiteral("Pattern")),
                choice("shape", "Holes",
                       {{"hexagon", "Hexagons (honeycomb)"},
                        {"circle", "Circles"},
                        {"square", "Squares"},
                        {"diamond", "Diamonds"},
                        {"triangle", "Triangles"},
                        {"star", "Stars"},
                        {"slot", "Slots"}},
                       "hexagon"),
                number("cell", "Hole size", 10, 2, 100, 0.5, 1, " mm",
                       "Hexagons: across the flats. Circles: diameter. Squares and triangles: side. Diamonds and stars: "
                       "point to point. Slots: width."),
                when(number("slotLength", "Slot length", 30, 4, 400, 1), "shape=slot"),
                when(integer("points", "Star points", 5, 3, 12), "shape=star"),
                number("web", "Web", 1.6, 0.4, 20, 0.2, 1, " mm", "Material between the holes."),
                when(choice("grid", "Arrangement", {{"staggered", "Staggered"}, {"straight", "Straight rows"}}, "staggered",
                            "Staggered shifts every other row by half a step, so more holes fit."),
                     "shape=circle|square|star|slot"),
                number("angle", "Turn pattern", 0, -180, 180, 15, 0, QStringLiteral("°")),
                choice("edge", "At the border", {{"cut", "Cut holes off"}, {"whole", "Whole holes only"}}, "cut",
                       "Cut off: the pattern runs up to the border. Whole holes only: holes that would be cut are left out.")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("patterntube"), QStringLiteral("Pattern tube"), other,
           QStringLiteral("A tube with a pattern of holes all the way round: a vase, a pot, a lamp shade, a pen holder, "
                          "a fan duct. Straight or tapered. Or just the holes, to cut the pattern into a round part."),
           {}};
    s.params = {choice("make", "Make", {{"tube", "The tube"}, {"holes", "Holes only, to cut into a round part"}}, "tube",
                       "Holes only: just the holes, wrapped round, as a hole shape. Size it like the round part (its "
                       "outside), group them, and the pattern is cut into the part."),
                heading(QStringLiteral("Tube")),
                number("bottomDiameter", "Bottom diameter", 80, 10, 600, 1, 1, " mm", "Outside, at the bottom."),
                number("topDiameter", "Top diameter", 100, 10, 600, 1, 1, " mm", "Outside, at the top. Different from the bottom: tapered."),
                number("height", "Height", 120, 5, 600, 1),
                number("wall", "Wall", 2.4, 0.6, 30, 0.2, 1, " mm", "Holes only: how deep the holes cut."),
                when(choice("base", "Bottom", {{"closed", "Closed (a pot)"}, {"open", "Open (a sleeve)"}}, "closed"), "make=tube"),
                when(number("floor", "Floor", 3, 0.6, 30, 0.2), "make=tube&base=closed"),
                number("bandBottom", "Solid band at bottom", 12, 0, 300, 1, 1, " mm", "No holes this high up from the bottom."),
                number("bandTop", "Solid band at top", 8, 0, 300, 1, 1, " mm", "No holes this far down from the top."),
                heading(QStringLiteral("Pattern")),
                choice("shape", "Holes",
                       {{"hexagon", "Hexagons (honeycomb)"},
                        {"circle", "Circles"},
                        {"square", "Squares"},
                        {"diamond", "Diamonds"},
                        {"triangle", "Triangles"},
                        {"star", "Stars"},
                        {"slot", "Slots"}},
                       "hexagon"),
                number("cell", "Hole size", 10, 2, 100, 0.5, 1, " mm",
                       "Hexagons: across the flats. Circles: diameter. Squares and triangles: side. Diamonds and stars: "
                       "point to point. Slots: width."),
                when(number("slotLength", "Slot length", 30, 4, 400, 1), "shape=slot"),
                when(integer("points", "Star points", 5, 3, 12), "shape=star"),
                number("web", "Web", 2, 0.4, 20, 0.2, 1, " mm", "Material between the holes (at the middle height; a taper makes it a little wider at the wide end)."),
                when(choice("grid", "Arrangement", {{"staggered", "Staggered"}, {"straight", "Straight rows"}}, "staggered",
                            "Staggered shifts every other row by half a step, so more holes fit."),
                     "shape=circle|square|star|slot"),
                choice("taper", "On a taper", {{"grow", "Holes grow with the width"}, {"same", "Holes keep their size"}}, "grow",
                       "Grow: the whole pattern widens with the tube, so it looks the same all the way up (sizes are "
                       "at the middle height). Keep their size: the holes crowd together towards the narrow end."),
                choice("direction", "Holes stand", {{"upright", "Upright (prints without supports)"}, {"sideways", "Turned a quarter"}}, "upright",
                       "Upright: hexagons and stars point up, slots run up and down."),
                choice("edge", "At the bands", {{"cut", "Cut holes off"}, {"whole", "Whole holes only"}}, "whole",
                       "Cut off: the pattern runs right up to the bands. Whole holes only: holes that would be cut are left out.")};
    all.push_back(s);
  }
  // --- Workshop ---------------------------------------------------------------------------------
  {
    Spec s{QStringLiteral("toolholder"), QStringLiteral("Bit and tool holder"), workshop,
           QStringLiteral("A block with pockets: 1/4\" hex bits, round tools, a drill index with the sizes written by "
                          "each hole, or SD and microSD cards."),
           {}};
    s.params = {choice("type", "Holds",
                       {{"hex", "1/4\" hex bits"}, {"round", "Round tools"}, {"drill", "Drill bits (index)"}, {"sd", "SD cards"},
                        {"microsd", "microSD cards"}},
                       "hex"),
                when(number("diameter", "Tool diameter", 6, 1, 60, 0.5), "type=round"),
                when(number("from", "Smallest drill", 1, 0.5, 30, 0.5), "type=drill"),
                when(number("to", "Largest drill", 10, 0.5, 30, 0.5), "type=drill"),
                when(number("step", "Step", 0.5, 0.1, 5, 0.1), "type=drill"),
                when(toggle("labels", "Sizes by the holes", true), "type=drill"),
                when(integer("rows", "Rows", 2, 1, 30), "type!=drill"),
                integer("columns", "Columns", 6, 1, 40, "Across. A drill index fills as many rows as it needs."),
                number("spacing", "Between holes", 3, 1, 30, 0.5),
                number("depth", "Pocket depth", 12, 2, 80, 0.5),
                number("base", "Floor", 2, 0.6, 20, 0.2),
                usePrinter(false, QStringLiteral("Clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Clearance", 0.2, 0, 1, 0.05, 2), 1), "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("dividers"), QStringLiteral("Drawer dividers"), workshop,
           QStringLiteral("Strips with half-height slots that slide together into a grid, sized to a drawer. Printed flat, "
                          "one behind the other."),
           {}};
    s.params = {heading(QStringLiteral("Drawer")),
                number("length", "Inside length X", 200, 40, 1000, 1),
                number("width", "Inside width Y", 150, 40, 1000, 1),
                number("height", "Divider height", 40, 10, 300, 1),
                heading(QStringLiteral("Dividers")),
                integer("columns", "Across X", 2, 0, 20, "Dividers that split the length."),
                integer("rows", "Across Y", 1, 0, 20, "Dividers that split the width."),
                number("thickness", "Thickness", 2, 0.8, 10, 0.2),
                number("gap", "Room to the drawer", 1, 0, 10, 0.5, 1, " mm", "The strips stop this short of each drawer wall."),
                usePrinter(false, QStringLiteral("Slot clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Slot clearance", 0.3, 0, 1.5, 0.05, 2), 1), "usePrinter=0"),
                choice("layout", "Layout", {{"print", "Flat, for printing"}, {"assembled", "Assembled"}}, "print")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("cable"), QStringLiteral("Cable management"), workshop,
           QStringLiteral("Screw-down cable clips, snap clips for double-sided tape, a desk grommet with a cap, and labels "
                          "that clip around a cable."),
           {}};
    s.params = {choice("type", "Type",
                       {{"clip", "Screw-down clip"}, {"snap", "Snap clip"}, {"grommet", "Desk grommet"}, {"label", "Cable label"}},
                       "clip"),
                when(number("cable", "Cable diameter", 6, 1, 40, 0.5), "type!=grommet"),
                when(integer("cables", "Cables side by side", 1, 1, 12), "type=clip"),
                when(number("width", "Clip width", 10, 3, 60, 0.5), "type!=grommet"),
                number("wall", "Wall", 2, 0.8, 8, 0.2),
                when(choice("screw", "Screw", {{"M3", "M3"}, {"M4", "M4"}, {"M5", "M5"}}, "M4"), "type=clip"),
                when(number("hole", "Desk hole", 60, 15, 150, 1), "type=grommet"),
                when(number("desk", "Desk thickness", 25, 5, 80, 0.5), "type=grommet"),
                when(number("flange", "Flange", 6, 2, 30, 0.5, 1, " mm", "How far the rim reaches over the desk."), "type=grommet"),
                when(number("slot", "Cable slot", 18, 4, 80, 0.5), "type=grommet"),
                when(number("tagLength", "Tag length", 25, 8, 100, 1), "type=label"),
                when(text("text", "Label", "LABEL", 24), "type=label"),
                usePrinter(false, QStringLiteral("Clearance follows the printer clearance from Settings.")),
                when(followsPrinter(number("clearance", "Clearance", 0.3, 0, 2, 0.05, 2), 1), "usePrinter=0")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("nametag"), QStringLiteral("Name tag"), workshop,
           QStringLiteral("Raised or engraved text on a plate that fits it, with a keyring hole: name tags, keychains, "
                          "labels for bins and shelves, small signs."),
           {}};
    s.params = {text("text", "Text", "DaedalCAD", 40),
                number("textHeight", "Letter height", 10, 2, 80, 0.5, 1, " mm", "The height of a capital letter."),
                choice("style", "Letters", {{"raised", "Raised"}, {"engraved", "Engraved"}, {"cutout", "Cut through"}}, "raised"),
                number("thickness", "Plate thickness", 2, 0.6, 20, 0.2),
                when(number("textDepth", "Letter depth", 1, 0.2, 10, 0.1), "style!=cutout"),
                number("padding", "Margin", 4, 0.5, 30, 0.5),
                number("corner", "Corner radius", 3, 0, 30, 0.5),
                toggle("hole", "Keyring hole", true),
                when(number("holeDiameter", "Hole diameter", 4, 1.5, 15, 0.5), "hole=1"),
                when(toggle("border", "Raised border", false), "style=raised")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("dice"), QStringLiteral("Dice"), workshop,
           QStringLiteral("A die to roll: d4, d6, d8, d10, d12 or d20, with numbers, pips or your own symbols, a picture "
                          "on the top face, rounded edges, cut in, raised, or as an inlay for a two-colour print. Opposite "
                          "faces add up the usual way."),
           {}};
    s.params = {choice("kind", "Die", {{"d4", "d4 (pyramid)"}, {"d6", "d6 (cube)"}, {"d8", "d8"}, {"d10", "d10"}, {"d12", "d12"}, {"d20", "d20"}},
                       "d6"),
                number("size", "Size", 16, 8, 80, 0.5, 1, " mm", "How tall the die stands on the table: across from one face to "
                                                               "the opposite one (a d4: from its tip to the table)."),
                number("rounding", "Rounded edges", 1.2, 0, 8, 0.1, 2, " mm", "0 keeps the edges sharp."),
                when(choice("marks", "Marks", {{"numbers", "Numbers"}, {"pips", "Pips (dots)"}}, "pips"), "kind=d6"),
                text("labels", "Own symbols", QString(), 200,
                     "Instead of numbers: what goes on each face, from 1 up, separated by commas (\"A, B, C\", \"★\", words). "
                     "Leave empty for numbers."),
                number("markSize", "Mark size", 55, 20, 95, 1, 0, " %", "How big the numbers or symbols are on their faces."),
                choice("style", "Marks are", {{"cut", "Cut in"}, {"raised", "Raised"}, {"inlay", "Inlay (two colours)"}}, "cut",
                       "Raised marks on the face it stands on keep it off the plate: print raised dice on a raft or with "
                       "support, or use Cut in. Inlay: the marks are cut in and filled flush by a separate part, for a two-colour print (Convert the "
                       "die to parts, then give the marks their own colour)."),
                number("depth", "Depth", 0.6, 0.2, 3, 0.1, 2, " mm", "How deep the marks are cut, or how far they stand out."),
                toggle("underline", "Mark 6 and 9", true, "A small line under 6 and 9, so they cannot be mixed up."),
                picture("picture", "Picture for the top number", "A logo or symbol in place of the highest number (the "
                                                                 "face on top). Dark parts become the mark."),
                toggle("invertPicture", "Swap dark and bright", false)};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("lithophane"), QStringLiteral("Lithophane / photo relief"), workshop,
           QStringLiteral("A picture as a thin panel. A lithophane shows the photo when you hold it up to the light (dark "
                          "parts are thick); a photo relief shows it in normal light (bright parts stand out). As a panel, "
                          "in a frame, a keychain or a round medallion."),
           {}};
    s.params = {picture("picture", "Picture", "Choose any photo or drawing. Strong contrast works best."),
                choice("mode", "Kind", {{"litho", "Lithophane (hold to the light)"}, {"relief", "Photo relief (bright stands out)"}},
                       "litho"),
                choice("shape", "Shape", {{"panel", "Panel"}, {"framed", "Panel in a frame"}, {"keychain", "Keychain"}, {"round", "Round"}},
                       "panel"),
                number("width", "Width", 100, 15, 300, 1, 1, " mm", "How wide the picture is printed."),
                number("thin", "Thinnest", 0.8, 0.3, 5, 0.1, 2, " mm",
                       "The brightest parts of a lithophane (the darkest of a relief). 0.6 to 0.8 mm lets light through well."),
                number("thick", "Thickest", 3, 0.8, 12, 0.1, 2, " mm", "The darkest parts of a lithophane (the brightest of a relief)."),
                number("detail", "Detail", 0.25, 0.1, 1, 0.05, 2, " mm", "The size of one step of the picture. Smaller is finer and slower."),
                toggle("invert", "Swap dark and bright", false),
                when(number("border", "Frame width", 4, 1, 30, 0.5), "shape=framed"),
                when(number("holeDiameter", "Hole diameter", 5, 2, 15, 0.5), "shape=keychain"),
                when(number("corner", "Corner radius", 2, 0, 30, 0.5), "shape=panel|framed|keychain")};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("fittest"), QStringLiteral("Fit test"), calibration,
           QStringLiteral("A strip of holes with growing clearance and one peg. Print it, find the tightest hole the "
                          "peg slides into, and use that clearance in the other generators. Hole 1 has one dot, hole "
                          "2 two dots, and so on."),
           {}};
    s.params = {choice("shape", "Peg", {{"square", "Square"}, {"round", "Round"}}, "square"),
                number("size", "Peg size", 10, 3, 40, 0.5),
                number("first", "First clearance", 0.1, 0, 1, 0.05, 2),
                number("step", "Clearance step", 0.05, 0.01, 0.5, 0.01, 2),
                integer("count", "Holes", 6, 2, 10),
                number("thickness", "Strip thickness", 4, 1.5, 20, 0.5)};
    all.push_back(s);
  }
  // --- Testing and calibration -----------------------------------------------------------------
  {
    Spec s{QStringLiteral("holetest"), QStringLiteral("Hole size test"), calibration,
           QStringLiteral("Round holes of exact sizes, labelled. Measure them after printing: holes usually come out a "
                          "little small, which tells you how much clearance to add. Optional pegs of the same sizes."),
           {}};
    s.params = {number("from", "Smallest", 2, 0.5, 40, 0.5),
                number("to", "Largest", 10, 0.5, 60, 0.5),
                number("step", "Step", 1, 0.1, 10, 0.1),
                number("thickness", "Plate thickness", 4, 1, 20, 0.5),
                toggle("pegs", "Pegs of the same sizes", false)};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("threadtest"), QStringLiteral("Thread fit test"), calibration,
           QStringLiteral("A plate with threaded holes of growing clearance and a short bolt. The hole the bolt turns into "
                          "smoothly gives the clearance to use for bolts, nuts and jars."),
           {}};
    s.params = {choice("size", "Size", metricSizes(), "M8"),
                when(number("diameter", "Diameter", 8, 2, 60, 0.5), "size=custom"),
                when(number("pitch", "Pitch", 1.25, 0.3, 6, 0.05, 2), "size=custom"),
                number("first", "First clearance", 0.1, 0, 1, 0.05, 2),
                number("step", "Clearance step", 0.1, 0.02, 0.5, 0.01, 2),
                integer("count", "Holes", 5, 2, 8),
                number("thickness", "Plate thickness", 6, 3, 20, 0.5)};
    all.push_back(s);
  }
  {
    Spec s{QStringLiteral("overhang"), QStringLiteral("Overhang and bridge test"), calibration,
           QStringLiteral("Fins leaning over at growing angles (from vertical) and bridges of growing length. Shows how far "
                          "your printer and settings can go without support."),
           {}};
    s.params = {heading(QStringLiteral("Overhangs")),
                number("from", "From", 30, 0, 80, 5, 0, QStringLiteral("°")),
                number("to", "To", 70, 5, 85, 5, 0, QStringLiteral("°")),
                number("step", "Step", 10, 2, 30, 1, 0, QStringLiteral("°")),
                number("height", "Fin height", 12, 4, 40, 1),
                heading(QStringLiteral("Bridges")),
                toggle("bridges", "Bridges", true),
                when(number("bridgeFrom", "Shortest", 10, 5, 100, 5), "bridges=1"),
                when(number("bridgeTo", "Longest", 50, 5, 150, 5), "bridges=1"),
                when(number("bridgeStep", "Step", 10, 5, 50, 5), "bridges=1")};
    all.push_back(s);
  }
  // Standard parts: ISO and DIN fasteners at their real sizes, each one also as its matching hole.
  {
    const QString standard = QStringLiteral("Standard parts");
    const std::vector<std::pair<QString, QString>> sizes = {
        {"M2", "M2"},   {"M2.5", "M2.5"}, {"M3", "M3"},   {"M4", "M4"},   {"M5", "M5"},   {"M6", "M6"},  {"M8", "M8"},
        {"M10", "M10"}, {"M12", "M12"},   {"M14", "M14"}, {"M16", "M16"}, {"M20", "M20"}, {"M24", "M24"}};
    auto make = [] {
      return choice("make", "Make", {{"part", "The part"}, {"hole", "Its matching hole"}}, "part",
                    "The part itself, or a hole to cut into your model where it goes. \"Add matching hole\" places "
                    "the hole for you, lined up with the part.");
    };
    auto thread = [] {
      return when(choice("thread", "Thread", {{"simple", "Plain (fast)"}, {"modelled", "Modelled"}}, "simple",
                         "Plain: a smooth cylinder at the thread's size, which keeps big scenes quick. Modelled: the "
                         "real ISO thread."),
                  "make=part");
    };
    {
      Spec s{QStringLiteral("stdscrew"), QStringLiteral("Screw (ISO/DIN)"), standard,
             QStringLiteral("Socket head, button head, countersunk, hex head, pan head and set screws to ISO and DIN "
                            "sizes. The head's underside sits at the part's origin, so the screw and its hole line up."),
             {}};
      s.params = {choice("standard", "Standard",
                         {{"4762", "ISO 4762 / DIN 912 socket head"},
                          {"7380", "ISO 7380 button head"},
                          {"10642", "ISO 10642 countersunk"},
                          {"7991", "DIN 7991 countersunk"},
                          {"4017", "ISO 4017 hex head"},
                          {"933", "DIN 933 hex head"},
                          {"7045", "ISO 7045 pan head (cross)"},
                          {"4026", "ISO 4026 set screw"}},
                         "4762"),
                  choice("size", "Size", sizes, "M3"),
                  number("length", "Length", 12, 2, 300, 1, 1, " mm",
                         "Under the head; overall for countersunk and set screws."),
                  thread(),
                  make(),
                  when(choice("fit", "Clearance (ISO 273)", {{"fine", "Fine"}, {"medium", "Medium"}, {"coarse", "Coarse"}},
                              "medium", "How much wider than the screw the hole is. Medium suits most prints."),
                       "make=hole&standard!=4026"),
                  when(choice("head", "Head", {{"flush", "Counterbored, flush"}, {"none", "On the surface"}}, "flush",
                              "Counterbored: room for the head, so a screw sunk until its head is level with the surface sits "
                              "flush. On the surface: a plain hole, the head standing on top."),
                       "make=hole&standard=4762|7380|4017|933|7045")};
      all.push_back(s);
    }
    {
      Spec s{QStringLiteral("stdnut"), QStringLiteral("Nut (ISO/DIN)"), standard,
             QStringLiteral("Hex nuts (ISO 4032, DIN 934, thin ISO 4035), nylon locking nuts (DIN 985), flange nuts "
                            "(DIN 6923) and square nuts (DIN 562). As a hole: a nut trap."),
             {}};
      s.params = {choice("standard", "Standard",
                         {{"4032", "ISO 4032 hex"},
                          {"934", "DIN 934 hex"},
                          {"4035", "ISO 4035 thin hex"},
                          {"985", "DIN 985 nylon locking"},
                          {"6923", "DIN 6923 flange"},
                          {"562", "DIN 562 square"}},
                         "4032"),
                  choice("size", "Size", sizes, "M3"),
                  thread(),
                  make(),
                  when(number("clearance", "Trap clearance", 0.2, 0, 1, 0.05, 2, " mm",
                              "Room all round the nut in the trap, so a real nut presses in."),
                       "make=hole"),
                  when(number("through", "Bolt hole beyond", 5, 0, 100, 1, 1, " mm",
                              "How far the bolt's clearance hole runs on past the trap, both ways."),
                       "make=hole")};
      all.push_back(s);
    }
    {
      Spec s{QStringLiteral("stdwasher"), QStringLiteral("Washer"), standard,
             QStringLiteral("Flat washers (ISO 7089 / DIN 125, wide ISO 7093) and spring washers (DIN 127). As a hole: "
                            "a recess for it."),
             {}};
      s.params = {choice("standard", "Standard",
                         {{"7089", "ISO 7089 / DIN 125 flat"}, {"7093", "ISO 7093 large"}, {"127", "DIN 127 spring"}}, "7089"),
                  choice("size", "Size", sizes, "M3"),
                  make(),
                  when(number("clearance", "Recess clearance", 0.2, 0, 1, 0.05, 2, " mm", "Room all round the washer."),
                       "make=hole"),
                  when(number("through", "Bolt hole beyond", 5, 0, 100, 1, 1, " mm",
                              "How far the bolt's clearance hole runs on, both ways."),
                       "make=hole")};
      all.push_back(s);
    }
    {
      Spec s{QStringLiteral("stdpin"), QStringLiteral("Dowel pin"), standard,
             QStringLiteral("Parallel pins, ISO 2338 / ISO 8734, with chamfered ends. As a hole: a pin hole with a "
                            "lead-in."),
             {}};
      s.params = {choice("diameter", "Diameter",
                         {{"2", "2 mm"}, {"2.5", "2.5 mm"}, {"3", "3 mm"}, {"4", "4 mm"}, {"5", "5 mm"}, {"6", "6 mm"},
                          {"8", "8 mm"}, {"10", "10 mm"}, {"12", "12 mm"}, {"16", "16 mm"}, {"20", "20 mm"}},
                         "3"),
                  number("length", "Length", 12, 2, 200),
                  make(),
                  when(choice("fit", "Fit", {{"press", "Press fit"}, {"slip", "Slip fit (+0.1 mm)"}}, "press",
                              "Printed holes come out a little small, so a press fit is usually drawn at the pin's size."),
                       "make=hole"),
                  number("depth", "Depth in the hole", 6, 0.5, 200, 0.5, 1, " mm",
                         "How far the pin goes into the hole; the rest stands out above it.")};
      all.push_back(s);
    }
    {
      Spec s{QStringLiteral("stdrod"), QStringLiteral("Threaded rod"), standard,
             QStringLiteral("Threaded rod, DIN 975. As a hole: a clearance hole along its length."), {}};
      s.params = {choice("size", "Size", sizes, "M8"),
                  number("length", "Length", 100, 2, 1000),
                  thread(),
                  make(),
                  when(choice("fit", "Clearance (ISO 273)", {{"fine", "Fine"}, {"medium", "Medium"}, {"coarse", "Coarse"}},
                              "medium"),
                       "make=hole")};
      all.push_back(s);
    }
    {
      Spec s{QStringLiteral("stdinsert"), QStringLiteral("Heat-set insert"), standard,
             QStringLiteral("Brass threaded inserts for plastic, at the common sizes sold for 3D printing. As a hole: "
                            "the pocket it is melted into, with a lead-in and room for the screw tip."),
             {}};
      s.params = {choice("size", "Size", {{"M2", "M2"}, {"M2.5", "M2.5"}, {"M3", "M3"}, {"M4", "M4"}, {"M5", "M5"}, {"M6", "M6"}, {"M8", "M8"}},
                         "M3"),
                  make()};
      all.push_back(s);
    }
  }
  return all;
}

// ---------------------------------------------------------------------------------------------
// Reading settings

class Values {
 public:
  Values(const Spec& spec, const QJsonObject& values) : values_(normalized(spec.id, values)) {}
  double num(const char* key) const { return values_.value(QLatin1String(key)).toDouble(); }
  int whole(const char* key) const { return static_cast<int>(std::lround(values_.value(QLatin1String(key)).toDouble())); }
  bool on(const char* key) const { return values_.value(QLatin1String(key)).toBool(); }
  QString pick(const char* key) const { return values_.value(QLatin1String(key)).toString(); }
  QString text(const char* key) const { return values_.value(QLatin1String(key)).toString(); }

 private:
  QJsonObject values_;
};

void note(QString* warning, const QString& text) {
  if (!warning || text.isEmpty() || warning->contains(text)) return;
  if (!warning->isEmpty()) *warning += QLatin1Char(' ');
  *warning += text;
}

// ---------------------------------------------------------------------------------------------
// Geometry helpers

bool ok(const Manifold& solid) { return solid.Status() == Manifold::Error::NoError && !solid.IsEmpty(); }

int quarterSegments(double radius) { return std::clamp(static_cast<int>(std::ceil(radius * 1.6)) + 3, 4, 24); }
int circleSegments(double radius) { return std::clamp(static_cast<int>(std::ceil(2 * kPi * radius / 0.6)), 32, 200); }

// Rounded rectangle around the origin with half sizes hx, hy. The point count only depends on
// `segments`, so outlines at different offsets can be lofted into one solid.
SimplePolygon roundRect(double hx, double hy, double r, int segments) {
  hx = std::max(hx, 0.1);
  hy = std::max(hy, 0.1);
  r = std::clamp(r, 0.05, std::min(hx, hy) - 0.02);
  SimplePolygon poly;
  poly.reserve(4 * (segments + 1));
  const double cx[4] = {hx - r, -(hx - r), -(hx - r), hx - r};
  const double cy[4] = {hy - r, hy - r, -(hy - r), -(hy - r)};
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i <= segments; ++i) {
      const double a = (c * 90.0 + 90.0 * i / segments) * kPi / 180.0;
      poly.push_back(vec2(cx[c] + r * std::cos(a), cy[c] + r * std::sin(a)));
    }
  }
  return poly;
}

SimplePolygon circle(double r, int segments) {
  SimplePolygon poly;
  poly.reserve(segments);
  for (int i = 0; i < segments; ++i) {
    const double a = 2 * kPi * i / segments;
    poly.push_back(vec2(r * std::cos(a), r * std::sin(a)));
  }
  return poly;
}

// Regular polygon; the first corner points along +X.
SimplePolygon regular(double circumradius, int sides, double startDegrees = 0) {
  SimplePolygon poly;
  for (int i = 0; i < sides; ++i) {
    const double a = (startDegrees + 360.0 * i / sides) * kPi / 180.0;
    poly.push_back(vec2(circumradius * std::cos(a), circumradius * std::sin(a)));
  }
  return poly;
}

Manifold prism(const SimplePolygon& poly, double z0, double z1) {
  return Manifold::Extrude(Polygons{poly}, z1 - z0).Translate(vec3(0, 0, z0));
}

Manifold prism(const CrossSection& section, double z0, double z1) {
  return Manifold::Extrude(section.ToPolygons(), z1 - z0).Translate(vec3(0, 0, z0));
}

Manifold block(double x0, double x1, double y0, double y1, double z0, double z1) {
  if (!(x1 > x0 && y1 > y0 && z1 > z0)) return {};  // an inverted box would poison every boolean it meets
  return Manifold::Cube(vec3(x1 - x0, y1 - y0, z1 - z0)).Translate(vec3(x0, y0, z0));
}

Manifold cylinderZ(double r, double z0, double z1, double x = 0, double y = 0) {
  return Manifold::Cylinder(z1 - z0, r, r, circleSegments(r)).Translate(vec3(x, y, z0));
}

Manifold coneZ(double r0, double r1, double z0, double z1, double x = 0, double y = 0) {
  return Manifold::Cylinder(z1 - z0, r0, r1, circleSegments(std::max(r0, r1))).Translate(vec3(x, y, z0));
}

// A closed solid through a stack of outlines (same point count, star shaped around the origin,
// counter clockwise), at rising heights.
Manifold stack(const std::vector<SimplePolygon>& rings, const std::vector<double>& heights) {
  const size_t levels = rings.size();
  if (levels < 2 || heights.size() != levels) return {};
  const size_t n = rings.front().size();
  for (const auto& ring : rings)
    if (ring.size() != n || n < 3) return {};
  MeshGL64 mesh;
  mesh.numProp = 3;
  mesh.vertProperties.reserve((levels * n + 2) * 3);
  for (size_t k = 0; k < levels; ++k) {
    for (const vec2& p : rings[k]) {
      mesh.vertProperties.push_back(p.x);
      mesh.vertProperties.push_back(p.y);
      mesh.vertProperties.push_back(heights[k]);
    }
  }
  const uint64_t bottom = levels * n;
  const uint64_t top = bottom + 1;
  for (double z : {heights.front(), heights.back()}) {
    mesh.vertProperties.push_back(0);
    mesh.vertProperties.push_back(0);
    mesh.vertProperties.push_back(z);
  }
  auto at = [n](size_t k, size_t i) { return static_cast<uint64_t>(k * n + (i % n)); };
  for (size_t k = 0; k + 1 < levels; ++k) {
    for (size_t i = 0; i < n; ++i) {
      const uint64_t a = at(k, i), b = at(k, i + 1), c = at(k + 1, i + 1), d = at(k + 1, i);
      mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
    }
  }
  for (size_t i = 0; i < n; ++i) {
    mesh.triVerts.insert(mesh.triVerts.end(), {bottom, at(0, i + 1), at(0, i)});
    mesh.triVerts.insert(mesh.triVerts.end(), {top, at(levels - 1, i), at(levels - 1, i + 1)});
  }
  Manifold solid(mesh);
  return ok(solid) ? solid : Manifold();
}

// A loft of one outline grown or shrunk by an offset at each height: (z, offset) pairs, rising z.
Manifold loft(const std::function<SimplePolygon(double)>& outline, std::vector<std::pair<double, double>> levels) {
  std::vector<SimplePolygon> rings;
  std::vector<double> heights;
  double last = -1e18;
  for (auto [z, offset] : levels) {
    if (z <= last + 1e-6) z = last + 1e-4;
    last = z;
    rings.push_back(outline(offset));
    heights.push_back(z);
  }
  return stack(rings, heights);
}

std::function<SimplePolygon(double)> roundRectOutline(double hx, double hy, double r) {
  const int segments = quarterSegments(std::max(r, 1.0));
  return [=](double offset) { return roundRect(hx + offset, hy + offset, r + offset, segments); };
}

std::function<SimplePolygon(double)> circleOutline(double r) {
  const int segments = circleSegments(r);
  return [=](double offset) { return circle(std::max(r + offset, 0.05), segments); };
}

// Ridged circle for grips.
SimplePolygon gripCircle(double r, double ridgeDepth) {
  const int ridges = std::max(12, static_cast<int>(std::round(2 * kPi * r / 3.0)));
  const int per = 8;
  SimplePolygon poly;
  for (int i = 0; i < ridges * per; ++i) {
    const double a = 2 * kPi * i / (ridges * per);
    const double rr = r + ridgeDepth * 0.5 * (1 + std::cos(ridges * a));
    poly.push_back(vec2(rr * std::cos(a), rr * std::sin(a)));
  }
  return poly;
}

// Round solid with its bottom and/or top edge chamfered: `radial` in, over `axial` height.
Manifold chamferedDisc(double r, double z0, double z1, double radial, double axial = -1, bool bottom = true,
                       bool top = true) {
  if (axial < 0) axial = radial;
  const double limit = (z1 - z0) * ((bottom && top) ? 0.45 : 0.9);
  if (axial > limit) {
    radial *= limit / axial;
    axial = limit;
  }
  if (radial < 0.01 || (!bottom && !top)) return cylinderZ(r, z0, z1);
  std::vector<std::pair<double, double>> levels;
  if (bottom) levels.insert(levels.end(), {{z0, -radial}, {z0 + axial, 0}});
  else levels.push_back({z0, 0});
  if (top) levels.insert(levels.end(), {{z1 - axial, 0}, {z1, -radial}});
  else levels.push_back({z1, 0});
  return loft(circleOutline(r), levels);
}

// ---------------------------------------------------------------------------------------------
// Threads

struct ThreadForm {
  double major = 5;  // outside radius
  double depth = 0.5;
  double pitch = 1;
  double crest = 0.125;  // part of a pitch flat at the outside
  double root = 0.25;    // part of a pitch flat at the inside
};

ThreadForm isoThread(double diameter, double pitch) {
  ThreadForm form;
  form.major = diameter * 0.5;
  form.pitch = pitch;
  form.depth = 0.5413 * pitch;  // 5H/8
  form.crest = 0.125;
  form.root = 0.25;
  return form;
}

// 45° flanks: prints without supports both ways up.
ThreadForm squareishThread(double major, double pitch, double depth) {
  ThreadForm form;
  form.major = major;
  form.pitch = pitch;
  form.depth = std::min(depth, pitch * 0.45);
  const double flank = form.depth / pitch;
  form.crest = form.root = std::max(0.05, (1 - 2 * flank) * 0.5);
  return form;
}

double threadRadius(const ThreadForm& form, double phase) {
  double d = phase - std::floor(phase);  // 0..1, crest centered on 0
  d = std::min(d, 1 - d);                // 0..0.5
  const double crestHalf = form.crest * 0.5;
  const double rootStart = 0.5 - form.root * 0.5;
  if (d <= crestHalf) return form.major;
  if (d >= rootStart) return form.major - form.depth;
  const double t = (d - crestHalf) / std::max(1e-9, rootStart - crestHalf);
  return form.major - form.depth * t;
}

// A right handed threaded rod from z0 to z1. `phaseZ` is the height the helix is measured from,
// so a nut cut with the same phaseZ lines up with the bolt.
Manifold threadedRod(const ThreadForm& form, double z0, double z1, double phaseZ, double radialGrow = 0) {
  ThreadForm grown = form;
  grown.major += radialGrow;
  const int around = std::clamp(static_cast<int>(std::ceil(2 * kPi * grown.major / 0.5)), 48, 180);
  // 20 rows per turn, fewer on a long fine thread (an M3 x 300 mm is 600 turns): keeps it under
  // about half a million triangles.
  const double turns = std::max(1.0, (z1 - z0) / form.pitch);
  const int perPitch = std::clamp(static_cast<int>(200000.0 / (around * turns)), 8, 20);
  const int rows = std::max(2, static_cast<int>(std::ceil((z1 - z0) / (form.pitch / perPitch))));
  std::vector<SimplePolygon> rings;
  std::vector<double> heights;
  rings.reserve(rows + 1);
  for (int j = 0; j <= rows; ++j) {
    const double z = z0 + (z1 - z0) * j / rows;
    SimplePolygon ring;
    ring.reserve(around);
    for (int i = 0; i < around; ++i) {
      const double a = 2 * kPi * i / around;
      const double phase = (z - phaseZ) / form.pitch - a / (2 * kPi);
      const double r = threadRadius(grown, phase);
      ring.push_back(vec2(r * std::cos(a), r * std::sin(a)));
    }
    rings.push_back(std::move(ring));
    heights.push_back(z);
  }
  return stack(rings, heights);
}

struct MetricSize {
  double diameter;
  double pitch;
  double flats;
};

MetricSize metric(const QString& size, double customDiameter, double customPitch, QString* warning) {
  static const std::map<QString, MetricSize> table = {
      {"M3", {3, 0.5, 5.5}},   {"M4", {4, 0.7, 7}},     {"M5", {5, 0.8, 8}},     {"M6", {6, 1.0, 10}},
      {"M8", {8, 1.25, 13}},   {"M10", {10, 1.5, 16}},  {"M12", {12, 1.75, 18}}, {"M16", {16, 2.0, 24}},
      {"M20", {20, 2.5, 30}},  {"M24", {24, 3.0, 36}}};
  if (const auto it = table.find(size); it != table.end()) return it->second;
  const double d = customDiameter;
  // Finer than d/60 is far below any standard pitch, and a long one runs into millions of triangles.
  double pitch = std::min(customPitch, d * 0.4);
  if (pitch < d / 60) {
    pitch = d / 60;
    note(warning, QStringLiteral("The pitch was raised to %1 mm; finer is impractical at this diameter.").arg(pitch, 0, 'f', 2));
  }
  return {d, pitch, std::round((1.5 * d + 1) * 2) / 2};
}

Manifold hexPrism(double flats, double z0, double z1) {
  return prism(regular(flats / std::sqrt(3.0), 6, 30), z0, z1);
}

// Hex with the usual 30° chamfer on both faces.
Manifold chamferedHex(double flats, double z0, double z1) {
  const double corner = flats / std::sqrt(3.0) + 0.01;
  const double inner = flats * 0.5 * 0.95;
  const double radial = corner - inner;
  return hexPrism(flats, z0, z1) ^ chamferedDisc(corner, z0, z1, radial, radial * std::tan(30 * kPi / 180));
}

// Threaded rod with the ends chamfered so the thread starts cleanly.
Manifold boltShank(const ThreadForm& form, double z0, double z1, double phaseZ, bool chamferBottom) {
  Manifold rod = threadedRod(form, z0, z1, phaseZ);
  if (!ok(rod)) return {};
  const double rMin = form.major - form.depth;
  const double edge = form.depth + 0.2;
  std::vector<std::pair<double, double>> levels;
  const double R = form.major + 0.5;
  // Loft of circles, given as offsets from radius R.
  if (chamferBottom) {
    levels.push_back({z0, rMin - 0.2 - R});
    levels.push_back({z0 + edge + 0.5, 0});
  } else {
    levels.push_back({z0, 0});
  }
  levels.push_back({z1 - edge - 0.5, 0});
  levels.push_back({z1, rMin - 0.2 - R});
  return rod ^ loft(circleOutline(R), levels);
}

Manifold nutBody(const MetricSize& size, double clearance, double z0, double z1, double phaseZ) {
  const ThreadForm form = isoThread(size.diameter, size.pitch);
  Manifold nut = chamferedHex(size.flats, z0, z1);
  const Manifold cut = threadedRod(form, z0 - form.pitch, z1 + form.pitch, phaseZ, clearance);
  if (!ok(cut)) return {};
  nut = nut - cut;
  // Countersinks on both faces, so the thread starts cleanly.
  const double outer = form.major + clearance + 0.3;
  const double inner = form.major - form.depth + clearance;
  const double sink = outer - inner;
  nut = nut - coneZ(outer + 0.5, inner, z0 - 0.5, z0 + sink) - coneZ(inner, outer + 0.5, z1 - sink, z1 + 0.5);
  return nut;
}

// ---------------------------------------------------------------------------------------------
// Placing pieces

// Lays pieces left to right along X, gap apart, all on z = 0 and centered on Y.
void sideBySide(std::vector<Piece>& pieces, double gap) {
  double x = 0;
  for (Piece& piece : pieces) {
    const auto box = piece.solid.BoundingBox();
    const double cy = (box.min.y + box.max.y) * 0.5;
    piece.solid = piece.solid.Translate(vec3(x - box.min.x, -cy, -box.min.z));
    x += (box.max.x - box.min.x) + gap;
  }
}

// ---------------------------------------------------------------------------------------------
// Screws and O-ring seals

struct ScrewHole {
  double pilot, clear, insert, insertLen;
};

ScrewHole screwHole(const QString& screw) {
  static const std::map<QString, ScrewHole> holes = {{"M2", {1.6, 2.4, 3.2, 3.0}},
                                                     {"M2.5", {2.1, 2.9, 3.6, 4.0}},
                                                     {"M3", {2.5, 3.4, 4.0, 5.7}},
                                                     {"M4", {3.3, 4.5, 5.6, 8.1}},
                                                     {"M5", {4.2, 5.5, 6.4, 9.5}}};
  const auto it = holes.find(screw);
  return it != holes.end() ? it->second : holes.at("M3");
}

// Groove and tongue for an O-ring, a length of cord or a printed gasket `cord` thick. The groove is
// cut into the top of a wall; the tongue stands under the lid and presses the ring down by
// `squeeze` (a quarter is usual) when the lid is closed. The groove is a little wider than the
// ring, so the squeezed ring has room to spread.
struct Seal {
  double cord = 2;
  double groove = 0, depth = 0;         // the slot in the wall
  double tongue = 0, tongueHeight = 0;  // the rib under the lid
  double land = 1.2;                    // wall left on each side of the groove
  double rim() const { return groove + 2 * land; }
};

Seal sealFor(const Values& v, double clearance) {
  Seal seal;
  seal.cord = v.num("cord");
  const double squeeze = std::clamp(v.num("squeeze"), 5.0, 50.0) / 100.0;
  seal.groove = seal.cord * 1.25 + 0.2;
  seal.depth = seal.cord * 1.5;
  seal.tongue = std::max(0.6, seal.groove - 2 * std::clamp(clearance, 0.15, 0.6));
  seal.tongueHeight = seal.depth - seal.cord * (1 - squeeze);
  return seal;
}

// A band `width` wide centred on `outline` shrunk by `middle`.
CrossSection band(const std::function<SimplePolygon(double)>& outline, double middle, double width) {
  return CrossSection(outline(-middle + width * 0.5)) - CrossSection(outline(-middle - width * 0.5));
}

struct SealedTop {
  Manifold box, lid, gasket;
};

// The top of a rectangular box sealed with an O-ring. The screws are always outside the seal, so
// no hole crosses it: either in ears standing out from the walls (`ears`), or inside the wall
// itself, with the groove bending inward round each hole and the inside corners filled to make
// room. Long sides get extra screws so the lid presses evenly (one at least every 80 mm or so).
// `floorZ`: the inside floor, where the corner fill starts.
SealedTop sealRect(Manifold box, double L, double W, double r, double wall, double H, double lidT, double floorZ, const Seal& seal,
                   const ScrewHole& sh, bool insert, bool gasket, bool ears, QString* warning) {
  const auto outer = roundRectOutline(L * 0.5, W * 0.5, r);
  const double rc = std::clamp(r, 0.05, std::min(L, W) * 0.5 - 0.02);
  const double holeR = (insert ? sh.insert : sh.pilot) * 0.5;
  const double widest = std::max(holeR, sh.clear * 0.5);
  std::vector<std::pair<double, double>> centres;
  std::vector<CrossSection> earList;
  const double bossR = std::max(holeR + 2.0, sh.clear + 1.4);  // room round the countersunk head in the lid
  const double reach = widest + 1.2;                             // ears: hole centre to the box's outside
  // In the wall: how far in from the outside the holes sit (the countersink keeps 0.6 mm of edge).
  const double inset = std::max(holeR + 1.2, sh.clear + 0.6);
  for (int sx : {-1, 1})
    for (int sy : {-1, 1}) {
      if (!ears) {
        // As close to the corner as the rounding allows, `inset` from every side.
        const double m = rc > inset ? rc - (rc - inset) / std::sqrt(2.0) : inset;
        centres.push_back({sx * (L * 0.5 - m), sy * (W * 0.5 - m)});
        continue;
      }
      const double cx = sx * (L * 0.5 - rc), cy = sy * (W * 0.5 - rc);
      const double d = (rc + reach) / std::sqrt(2.0);
      const double ex = cx + sx * d, ey = cy + sy * d;
      centres.push_back({ex, ey});
      const double s = std::max(bossR * 1.5, rc);
      // The ear's sides start a little inside the wall (g), not on its outside: corners on the
      // outside line came out a rounding error away from the wall's own after the union, and
      // those pairs of nearly equal corners break CAD exports.
      const double g = std::min(0.5, wall * 0.25);
      const SimplePolygon corner = {vec2(sx * L * 0.5, sy * W * 0.5), vec2(sx * (L * 0.5 - s), sy * (W * 0.5 - g)),
                                    vec2(sx * (L * 0.5 - s), sy * (W * 0.5 - s)), vec2(sx * (L * 0.5 - g), sy * (W * 0.5 - s))};
      earList.push_back(CrossSection::Hull({CrossSection(circle(bossR, 32)).Translate(vec2(ex, ey)), CrossSection(corner)}));
    }
  auto sideScrews = [&](double length, bool alongX) {
    const int extra = std::max(0, static_cast<int>(std::ceil(length / 80.0)) - 1);
    for (int i = 1; i <= extra; ++i) {
      const double u = -length * 0.5 + length * i / (extra + 1);
      for (int side : {-1, 1}) {
        const double half = alongX ? W * 0.5 : L * 0.5;
        const double e = side * (ears ? half + reach : half - inset);
        const vec2 at = alongX ? vec2(u, e) : vec2(e, u);
        centres.push_back({at.x, at.y});
        if (!ears) continue;
        const double inside = side * (half - 0.5);
        const SimplePolygon foot = alongX ? SimplePolygon{vec2(u - bossR, inside), vec2(u + bossR, inside), vec2(u + bossR, side * half),
                                                          vec2(u - bossR, side * half)}
                                          : SimplePolygon{vec2(inside, u - bossR), vec2(side * half, u - bossR), vec2(side * half, u + bossR),
                                                          vec2(inside, u + bossR)};
        CrossSection f(foot);
        if (f.Area() < 0) f = CrossSection(SimplePolygon(foot.rbegin(), foot.rend()));
        earList.push_back(CrossSection::Hull({CrossSection(circle(bossR, 32)).Translate(at), f}));
      }
    }
  };
  sideScrews(L, true);
  sideScrews(W, false);
  const CrossSection earShape = CrossSection::BatchBoolean(earList, OpType::Add);

  // The seal runs round the middle of the wall; with the screws in the wall it bends inward
  // round each of them, keeping a land of wall between the hole and the groove.
  const double middle = wall * 0.5;
  CrossSection sealLine(outer(-middle));
  if (!ears) {
    std::vector<CrossSection> keepOut;
    const double around = widest + seal.land + seal.groove * 0.5;
    for (auto [x, y] : centres) keepOut.push_back(CrossSection(circle(around, 48)).Translate(vec2(x, y)));
    sealLine = sealLine - CrossSection::BatchBoolean(keepOut, OpType::Add);
    // Round off the kinks where the line leaves the wall for a hole: a cord only bends so tight.
    const double bend = std::max(3.0, 1.5 * seal.cord);
    sealLine = sealLine.Offset(-bend, CrossSection::JoinType::Round).Offset(bend, CrossSection::JoinType::Round);
    // Too small to bend round the screws and still leave room inside: use ears instead.
    const CrossSection room = CrossSection(outer(-wall)) ^ sealLine.Offset(-(seal.groove * 0.5 + seal.land), CrossSection::JoinType::Round);
    if (sealLine.Decompose().size() != 1 || room.Area() < 0.25 * CrossSection(outer(-wall)).Area()) {
      note(warning, QStringLiteral("Too small for the screws in the walls, so they went in ears outside."));
      return sealRect(box, L, W, r, wall, H, lidT, floorZ, seal, sh, insert, gasket, true, warning);
    }
  }
  auto sealBand = [&](double width) {
    if (ears) return band(outer, middle, width);
    return sealLine.Offset(width * 0.5, CrossSection::JoinType::Round) - sealLine.Offset(-width * 0.5, CrossSection::JoinType::Round);
  };

  std::vector<Manifold> boxCuts, lidCuts;
  for (auto [x, y] : centres) {
    if (insert) {
      const double pocket = std::min(sh.insertLen + 0.5, H - 1);
      boxCuts.push_back(cylinderZ(sh.insert * 0.5, H - pocket, H + 1, x, y));
      boxCuts.push_back(coneZ(sh.insert * 0.5, sh.insert * 0.5 + 0.5, H - 0.5, H + 0.01, x, y));
      boxCuts.push_back(cylinderZ(sh.clear * 0.5, std::max(1.0, H - pocket - 4), H, x, y));
    } else {
      boxCuts.push_back(cylinderZ(sh.pilot * 0.5, std::max(1.0, H - 16), H + 1, x, y));
      boxCuts.push_back(coneZ(sh.pilot * 0.5, sh.pilot * 0.5 + 0.4, H - 0.4, H + 0.01, x, y));
    }
    lidCuts.push_back(cylinderZ(sh.clear * 0.5, H - 1, H + lidT + 1, x, y));
    lidCuts.push_back(coneZ(sh.clear * 0.5, sh.clear, H + lidT - sh.clear * 0.5, H + lidT + 0.01, x, y));
  }
  SealedTop top;
  if (ears) {
    box = box + prism(earShape, 0, H);
  } else {
    // Fill the inside corners (and the spots by the side screws) round the bent seal line. The
    // fill reaches 0.01 into the walls, and its edge by the seal 0.02 (along the straight walls
    // that edge is the wall's inside): flush, the union left corners a rounding error from the
    // walls' own, which breaks CAD exports.
    const CrossSection hollow(outer(-wall + 0.01));
    const CrossSection fill =
        hollow - sealLine.Offset(-(seal.groove * 0.5 + seal.land) + 0.02, CrossSection::JoinType::Round);
    if (fill.Area() > 1e-6) box = box + prism(fill, floorZ - 0.01, H);
  }
  top.box = box - prism(sealBand(seal.groove), H - seal.depth, H + 1) - Manifold::BatchBoolean(boxCuts, OpType::Add);
  const CrossSection plate = ears ? CrossSection(outer(0)) + earShape : CrossSection(outer(0));
  top.lid = (prism(plate, H, H + lidT) + prism(sealBand(seal.tongue), H - seal.tongueHeight, H + 0.01)) -
            Manifold::BatchBoolean(lidCuts, OpType::Add);
  if (gasket) top.gasket = prism(sealBand(seal.groove - 0.4), H - seal.depth, H - seal.depth + seal.cord);
  return top;
}

// ---------------------------------------------------------------------------------------------
// Box with lid

std::vector<Piece> buildBox(const Values& v, QString* warning) {
  const double L = v.num("length"), W = v.num("width"), H = v.num("height");
  double t = std::min(v.num("wall"), std::min(L, W) * 0.5 - 2);
  const double floorT = std::min(v.num("floor"), H - 1);
  const double r = std::clamp(v.num("corner"), 0.0, std::min(L, W) * 0.5 - 0.5);
  QString lid = v.pick("lid");
  const double T = v.num("lidThickness");
  const double c = v.num("clearance");
  {
    // Every lid style needs some wall height above the floor to hold on to.
    const double room = H - floorT - 1;
    const bool rails = lid == QLatin1String("slide") && v.pick("slideStyle") == QLatin1String("outset");
    const double need = rails ? 3.1 : lid == QLatin1String("slide") ? std::clamp(t * 0.5, 0.6, std::max(0.6, t - 0.6)) + 0.4 : 2.0;
    if (lid != QLatin1String("none") && room < need) {
      note(warning, QStringLiteral("The box is too low (or its floor too thick) for a lid, so it was left off."));
      lid = QStringLiteral("none");
    }
  }
  // A sealed lid needs a wall wide enough for the groove; the box keeps its outside size.
  const bool sealed = lid == QLatin1String("seal");
  Seal seal;
  if (sealed) {
    seal = sealFor(v, c);
    if (seal.rim() > std::min(L, W) * 0.5 - 3) {
      note(warning, QStringLiteral("The box is too small for an O-ring groove, so the lid was left off."));
      lid = QStringLiteral("none");
    } else if (t < seal.rim()) {
      note(warning, QStringLiteral("The walls were made %1 mm thick to hold the O-ring groove.").arg(seal.rim(), 0, 'f', 1));
      t = seal.rim();
    }
  }
  // A lid over rails: the top of the walls steps in by its rim (and a clearance), with an undercut
  // under that, and keeps a core of wall inside; the box keeps its outside size.
  const bool rails = lid == QLatin1String("slide") && v.pick("slideStyle") == QLatin1String("outset");
  const double rim = 1.0, undercut = 0.7, neck = 0.8;
  if (rails) {
    const double needed = rim + c + undercut + 0.8;
    if (needed > std::min(L, W) * 0.5 - 3) {
      note(warning, QStringLiteral("The box is too small for rails, so the lid was left off."));
      lid = QStringLiteral("none");
    } else if (t < needed) {
      note(warning, QStringLiteral("The walls were made %1 mm thick to hold the lid's rails.").arg(needed, 0, 'f', 1));
      t = needed;
    }
  }
  const bool assembled = v.pick("layout") == QLatin1String("assembled") && lid != QLatin1String("none");
  const auto outer = roundRectOutline(L * 0.5, W * 0.5, r);
  const auto inner = roundRectOutline(L * 0.5 - t, W * 0.5 - t, std::max(r - t, 0.05));

  Manifold box = prism(outer(0), 0, H) - prism(inner(0), floorT, H + 1);
  Manifold lidPart, gasketPart;
  double dividerTop = H;

  if (lid == QLatin1String("seal")) {
    SealedTop top = sealRect(box, L, W, r, t, H, T, floorT, seal, screwHole(v.pick("screw")), v.pick("hole") == QLatin1String("insert"),
                             v.on("gasket"), v.pick("screwPlace") != QLatin1String("walls"), warning);
    box = top.box;
    lidPart = top.lid;
    gasketPart = top.gasket;
    dividerTop = H - 0.5;
  }

  if (lid == QLatin1String("lip") || lid == QLatin1String("snap")) {
    const double depth = std::clamp(v.num("lipDepth"), 2.0, H - floorT - 1);
    const double lipWall = std::clamp(t, 1.2, 3.0);
    const auto lipOuter = [&](double off) { return inner(-c + off); };
    Manifold ring = prism(lipOuter(0), H - depth, H + 0.5) - prism(inner(-c - lipWall), H - depth - 1, H);
    if (lid == QLatin1String("snap")) {
      double bead = std::clamp(0.4 * t, 0.3, 0.8);
      if (bead + c > t - 0.5) bead = std::max(0.2, t - 0.5 - c);
      const double zb = H - depth + std::min(2.0, depth * 0.4) + bead;
      const Manifold ridge = loft(lipOuter, {{zb - bead, -0.05}, {zb, bead}, {zb + bead, -0.05}});
      ring = (ring + ridge) - prism(inner(-c - lipWall), H - depth - 1, H);
      const Manifold groove = loft(inner, {{zb - bead - c, -0.05}, {zb, bead + c}, {zb + bead + c, -0.05}});
      box = box - groove;
    }
    lidPart = prism(outer(0), H, H + T) + ring;
    dividerTop = H - depth - c - 0.3;
    if (v.on("notch")) {
      const double R = 7;
      const Manifold notch = Manifold::Cylinder(t + 2, R, R, circleSegments(R))
                                 .Rotate(-90, 0, 0)
                                 .Translate(vec3(0, -W * 0.5 - 1, H + 4));
      box = box - notch;
    }
  } else if (lid == QLatin1String("slide") && rails) {
    const double d = rim + c, g = undercut;
    const double Ts = std::clamp(neck + g + 1.2, neck + g + 0.6, H - floorT - 1);
    // The box: the top Ts of its walls cut back to a rail, d in from the outside, narrower still by g
    // at its foot (a neck, then a 45° slope out), so the lid's rim hooks under it.
    const Manifold rail = loft(outer, {{H - Ts - 0.01, -(d + g)}, {H - Ts + neck, -(d + g)}, {H - Ts + neck + g, -d}}) +
                          prism(outer(-d), H - Ts + neck + g - 0.01, H + 0.01);
    box = box - (prism(outer(1), H - Ts, H + 1) - rail);
    // The lid: a plate over the whole top (c above the walls) and a rim round the rail, everything
    // the rail passes through as the lid slides out toward -X taken away (so the rim stays across
    // the -X end only, which stops it there when closed). The rail grown by c: c out from its
    // upright faces, c out square to its 45° face.
    const double kink = c * (std::sqrt(2.0) - 1);
    auto passes = [L](const Manifold& layer) { return Manifold::Hull({layer, layer.Translate(vec3(L + 2, 0, 0))}); };
    // (Three layers, each convex, each swept on its own: the neck and the slope out of it together
    // are not convex, and a hull would fill the neck.)
    const Manifold room = passes(prism(outer(-(d + g) + c), H - Ts - 1, H - Ts + neck - kink + 0.01)) +
                          passes(loft(outer, {{H - Ts + neck - kink, -(d + g) + c}, {H - Ts + neck + g - kink, -d + c}})) +
                          passes(prism(outer(-d + c), H - Ts + neck + g - kink - 0.01, H + c));
    lidPart = prism(outer(0), H - Ts + c, H + c + T) - room;
    dividerTop = H - 0.3;
    if (v.on("slideSnap")) {
      // A low rounded bump across the top of the -X wall; the lid's plate rides over it (bending
      // by its height less the clearance) and drops onto it into a groove when closed.
      const double rise = 0.4, br = 0.6, x = -L * 0.5 + (d + t) * 0.5, span = W - 2 * t - 2;
      if (span > 2) {
        const Manifold bump = Manifold::Cylinder(span, br, br, circleSegments(br), true).Rotate(90, 0, 0).Translate(vec3(x, 0, H + rise - br));
        box = box + (bump ^ block(x - br - 0.1, x + br + 0.1, -W * 0.5, W * 0.5, H - 0.01, H + rise + 0.1));
        lidPart = lidPart - Manifold::Cylinder(span + 2, br + c, br + c, circleSegments(br + c), true)
                                .Rotate(90, 0, 0)
                                .Translate(vec3(x, 0, H + rise - br));
      }
    }
    if (v.on("notch")) {
      const double R = 10;
      const double dip = std::min(T * 0.5, 1.2);
      const Manifold scoop = Manifold::Cylinder(std::min(18.0, W * 0.4), R, R, circleSegments(R), true)
                                 .Rotate(90, 0, 0)
                                 .Translate(vec3(-L * 0.5 + std::min(10.0, L * 0.15), 0, H + c + T + R - dip));
      lidPart = lidPart - scoop;
    }
  } else if (lid == QLatin1String("slide")) {
    const double g = std::clamp(t * 0.5, 0.6, std::max(0.6, t - 0.6));
    const double Tz = std::clamp(T, g + 0.4, H - floorT - 1);
    // Dovetail groove: 45° under the top of the walls, so the lid cannot lift out.
    // A dovetail is not convex, so sweep its two convex layers toward -X one at a time.
    auto sweep = [L](const Manifold& end) { return Manifold::Hull({end, end.Translate(vec3(-L, 0, 0))}); };
    box = box - (sweep(loft(inner, {{H - Tz, g}, {H - Tz + g, 0}})) + sweep(prism(inner(0), H - Tz + g - 0.01, H + 1)));
    const double s = c * std::sqrt(2.0);
    const double low = std::min(g - c - s, g - 0.05);
    const double bend = H - Tz + g - (s - c);
    lidPart = (sweep(loft(inner, {{H - Tz + c, low}, {bend, -c}})) + sweep(prism(inner(-c), bend - 0.01, H)))
                  .TrimByPlane(vec3(1, 0, 0), -L * 0.5);
    dividerTop = H - Tz - 0.3;
    if (v.on("slideSnap")) {
      // A low rounded bump across the cut-down -X wall, which the lid slides over; it drops into a
      // groove under the lid's end when the lid is closed.
      const double rise = 0.4, br = 0.6, x = -L * 0.5 + t * 0.5, span = W - 2 * t - 2, z = H - Tz;
      if (span > 2 && t > 1.2) {
        const Manifold bump = Manifold::Cylinder(span, br, br, circleSegments(br), true).Rotate(90, 0, 0).Translate(vec3(x, 0, z + rise - br));
        box = box + (bump ^ block(x - br - 0.1, x + br + 0.1, -W * 0.5, W * 0.5, z - 0.01, z + rise + 0.1));
        lidPart = lidPart - Manifold::Cylinder(span + 2, br + c, br + c, circleSegments(br + c), true)
                                .Rotate(90, 0, 0)
                                .Translate(vec3(x, 0, z + rise - br));
      }
    }
    if (v.on("notch")) {
      const double R = 10;
      const double dip = std::min(Tz * 0.5, 1.2);
      const Manifold scoop = Manifold::Cylinder(std::min(18.0, W * 0.4), R, R, circleSegments(R), true)
                                 .Rotate(90, 0, 0)
                                 .Translate(vec3(-L * 0.5 + std::min(10.0, L * 0.15), 0, H + R - dip));
      lidPart = lidPart - scoop;
    }
  }

  const int nx = v.whole("dividersX"), ny = v.whole("dividersY");
  if ((nx > 0 || ny > 0) && dividerTop > floorT + 1) {
    const double dt = std::clamp(t * 0.75, 0.8, t);
    std::vector<Manifold> walls;
    const double innerL = L - 2 * t, innerW = W - 2 * t;
    for (int i = 1; i <= nx; ++i) {
      const double x = -innerL * 0.5 + innerL * i / (nx + 1);
      walls.push_back(block(x - dt * 0.5, x + dt * 0.5, -W * 0.5, W * 0.5, floorT - 0.01, dividerTop));
    }
    for (int i = 1; i <= ny; ++i) {
      const double y = -innerW * 0.5 + innerW * i / (ny + 1);
      walls.push_back(block(-L * 0.5, L * 0.5, y - dt * 0.5, y + dt * 0.5, floorT - 0.01, dividerTop));
    }
    box = box + (Manifold::BatchBoolean(walls, OpType::Add) ^ prism(inner(0.2), floorT - 0.01, dividerTop));
  } else if (nx > 0 || ny > 0) {
    note(warning, QStringLiteral("The box is too low for dividers under the lid."));
  }

  std::vector<Piece> pieces{{QStringLiteral("Box"), box}};
  if (lid != QLatin1String("none") && ok(lidPart)) {
    if (assembled) {
      if (lid != QLatin1String("slide")) lidPart = lidPart.Translate(vec3(0, 0, std::max(c, 0.05)));
      pieces.push_back({QStringLiteral("Lid"), lidPart});
      if (ok(gasketPart)) pieces.push_back({QStringLiteral("Gasket"), gasketPart});
      return pieces;
    }
    // Lip, snap, sealed and over-the-rails lids print upside down (flat top on the bed); a lid that
    // slides in grooves prints as it is.
    if (lid != QLatin1String("slide") || rails) lidPart = lidPart.Rotate(180, 0, 0);
    pieces.push_back({QStringLiteral("Lid"), lidPart});
    if (ok(gasketPart)) pieces.push_back({QStringLiteral("Gasket"), gasketPart});
    sideBySide(pieces, 10);
  }
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Gridfinity

constexpr double kGrid = 42.0;
constexpr double kBinRadius = 3.75;
constexpr double kBaseHeight = 4.75;

std::vector<Piece> buildGridBin(const Values& v, QString* warning) {
  const int nx = v.whole("unitsX"), ny = v.whole("unitsY"), nz = v.whole("unitsZ");
  const double w = v.num("wall");
  const double hx = kGrid * nx * 0.5 - 0.25, hy = kGrid * ny * 0.5 - 0.25;
  const double top = 7.0 * nz;
  const double floorZ = kBaseHeight + v.num("floor");
  bool lip = v.on("lip");
  const double lipBottom = top - (2.6 - w);
  if (lip && lipBottom < floorZ + 0.5) {
    note(warning, QStringLiteral("Too low for a stacking lip."));
    lip = false;
  }
  const auto outline = roundRectOutline(hx, hy, kBinRadius);
  Manifold body = prism(outline(0), kBaseHeight, lip ? top + 4.4 : top);
  std::vector<Manifold> feet;
  const auto foot = roundRectOutline(20.75, 20.75, kBinRadius);
  const Manifold footSolid = loft(foot, {{0, -2.95}, {0.8, -2.15}, {2.6, -2.15}, {kBaseHeight, 0}, {kBaseHeight + 0.2, 0}});
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
      feet.push_back(footSolid.Translate(vec3((i - (nx - 1) * 0.5) * kGrid, (j - (ny - 1) * 0.5) * kGrid, 0)));
  body = body + Manifold::BatchBoolean(feet, OpType::Add);
  Manifold cavity;
  if (lip) {
    cavity = loft(outline, {{floorZ, -w},
                            {lipBottom, -w},
                            {top, -2.6},
                            {top + 1.9, -0.7},
                            {top + 3.7, -0.7},
                            {top + 4.0, -0.4},
                            {top + 5.5, -0.4}});
  } else {
    cavity = prism(outline(-w), floorZ, top + 1);
  }
  body = body - cavity;
  const int dx = v.whole("dividersX"), dy = v.whole("dividersY");
  if (dx > 0 || dy > 0) {
    const double dividerTop = lip ? lipBottom - 0.5 : top;
    const double dt = 1.2;
    std::vector<Manifold> walls;
    const double innerX = 2 * (hx - w), innerY = 2 * (hy - w);
    for (int i = 1; i <= dx; ++i) {
      const double x = -innerX * 0.5 + innerX * i / (dx + 1);
      walls.push_back(block(x - dt * 0.5, x + dt * 0.5, -hy, hy, floorZ - 0.01, dividerTop));
    }
    for (int i = 1; i <= dy; ++i) {
      const double y = -innerY * 0.5 + innerY * i / (dy + 1);
      walls.push_back(block(-hx, hx, y - dt * 0.5, y + dt * 0.5, floorZ - 0.01, dividerTop));
    }
    body = body + (Manifold::BatchBoolean(walls, OpType::Add) ^ prism(outline(-w + 0.2), floorZ - 0.01, dividerTop));
  }
  if (v.on("magnets")) {
    std::vector<Manifold> holes;
    for (int i = 0; i < nx; ++i)
      for (int j = 0; j < ny; ++j)
        for (int k = 0; k < 4; ++k) {
          const double cx = (i - (nx - 1) * 0.5) * kGrid + ((k & 1) ? 13 : -13);
          const double cy = (j - (ny - 1) * 0.5) * kGrid + ((k & 2) ? 13 : -13);
          holes.push_back(cylinderZ(3.25, -1, 2.4, cx, cy));
        }
    body = body - Manifold::BatchBoolean(holes, OpType::Add);
  }
  return {{QStringLiteral("Bin"), body}};
}

std::vector<Piece> buildGridPlate(const Values& v, QString*) {
  const int nx = v.whole("unitsX"), ny = v.whole("unitsY");
  const bool solid = v.on("solid");
  const double bottom = solid ? v.num("bottom") : 0;
  const double topZ = bottom + 4.65;
  Manifold plate = prism(roundRectOutline(kGrid * nx * 0.5, kGrid * ny * 0.5, 4)(0), 0, topZ);
  const auto cell = roundRectOutline(21, 21, 4);
  std::vector<std::pair<double, double>> levels;
  if (!solid) levels.push_back({-1, -2.85});
  levels.insert(levels.end(), {{bottom, -2.85}, {topZ - 3.95, -2.15}, {topZ - 2.15, -2.15}, {topZ - 0.2, -0.2}, {topZ + 1, -0.2}});
  const Manifold pocket = loft(cell, levels);
  std::vector<Manifold> cuts;
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j) {
      const double cx = (i - (nx - 1) * 0.5) * kGrid, cy = (j - (ny - 1) * 0.5) * kGrid;
      cuts.push_back(pocket.Translate(vec3(cx, cy, 0)));
      if (solid && v.on("magnets") && bottom >= 2.6) {
        for (int k = 0; k < 4; ++k)
          cuts.push_back(cylinderZ(3.25, bottom - 2.4, bottom + 0.1, cx + ((k & 1) ? 13 : -13), cy + ((k & 2) ? 13 : -13)));
      }
    }
  plate = plate - Manifold::BatchBoolean(cuts, OpType::Add);
  return {{QStringLiteral("Baseplate"), plate}};
}

// ---------------------------------------------------------------------------------------------
// Screw jar

std::vector<Piece> buildJar(const Values& v, QString* warning) {
  const double rIn = v.num("diameter") * 0.5;
  const double H = v.num("height");
  const double wall = v.num("wall");
  const double pitch = v.num("pitch");
  const double c = v.num("clearance");
  const double depth = std::clamp(0.4 * pitch, 0.8, 2.5);
  const double neckRoot = rIn + wall;
  const double crest = neckRoot + depth;
  const double neckLen = std::min(v.num("turns") * pitch + pitch * 0.5, H * 0.6);
  const double neckZ = H - neckLen;
  const double floorT = wall;
  const ThreadForm neckForm = squareishThread(crest, pitch, depth);

  // Jar: plain body, threaded neck, hollow inside.
  Manifold neck = threadedRod(neckForm, neckZ - 0.01, H, neckZ);
  neck = neck ^ loft(circleOutline(crest + 0.5), {{neckZ - 0.01, 0}, {H - depth - 0.6, 0}, {H, -depth - 0.6}});
  Manifold jar = chamferedDisc(crest, 0, neckZ, 0.6, 0.6, true, false) + neck;
  // Sealed: the top of the neck is made wide enough for an O-ring groove (the mouth gets a little
  // narrower, with a 45 degree step inside so it prints), and the lid presses the ring down.
  bool sealed = v.on("seal");
  const Seal seal = sealFor(v, std::min(c, 0.3));
  const double rimOut = neckRoot - 0.6;  // where the neck's chamfer starts
  const double mouth = std::min(rIn, rimOut - seal.rim());
  if (sealed && mouth < std::max(3.0, rIn * 0.4)) {
    note(warning, QStringLiteral("The jar is too small for an O-ring groove, so it was left out."));
    sealed = false;
  }
  if (sealed) {
    const double step = rIn - mouth;
    const double zStep = neckZ - step;
    if (zStep < floorT + 1) {
      note(warning, QStringLiteral("The jar is too low for an O-ring groove, so it was left out."));
      sealed = false;
    } else {
      // (Thick walls already hold the groove: then the mouth stays as wide as the jar.)
      if (step > 0.01) jar = jar - cylinderZ(rIn, floorT, zStep + 0.01) - coneZ(rIn, mouth, zStep, neckZ) - cylinderZ(mouth, neckZ - 0.01, H + 1);
      else jar = jar - cylinderZ(rIn, floorT, H + 1);
      jar = jar - prism(band(circleOutline(rimOut), seal.land + seal.groove * 0.5, seal.groove), H - seal.depth, H + 1);
    }
  }
  if (!sealed) jar = jar - cylinderZ(rIn, floorT, H + 1);

  // Lid: the same helix with clearance, cut from a cup.
  const double lidWall = wall;
  const double rLid = crest + c + lidWall;
  const double lidTop = std::max(2.0, wall);
  const double lidBottom = neckZ + 0.4;
  const double lidInnerTop = H + 0.01;
  Manifold cup = v.on("grip") ? prism(gripCircle(rLid, 0.8), lidBottom, H + lidTop)
                              : chamferedDisc(rLid, lidBottom, H + lidTop, 0.6);
  const Manifold cut = threadedRod(neckForm, lidBottom - pitch, lidInnerTop, neckZ, c);
  cup = cup - (cut ^ cylinderZ(rLid, lidBottom - 1, lidInnerTop));
  // Lead-in for the thread at the open side.
  cup = cup - coneZ(crest + c + 0.6, neckRoot + c, lidBottom - 0.01, lidBottom + depth + 0.6);
  Manifold gasketPart;
  if (sealed) {
    const double middle = seal.land + seal.groove * 0.5;
    cup = cup + prism(band(circleOutline(rimOut), middle, seal.tongue), H - seal.tongueHeight, H + 0.02);
    if (v.on("gasket"))
      gasketPart = prism(band(circleOutline(rimOut), middle, seal.groove - 0.4), H - seal.depth, H - seal.depth + seal.cord);
  }

  std::vector<Piece> pieces{{QStringLiteral("Jar"), jar}, {QStringLiteral("Jar lid"), cup}};
  if (ok(gasketPart)) pieces.push_back({QStringLiteral("Gasket"), gasketPart});
  if (v.pick("layout") == QLatin1String("assembled")) return pieces;
  pieces[1].solid = pieces[1].solid.Rotate(180, 0, 0);
  sideBySide(pieces, 10);
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Bolt and nut

std::vector<Piece> buildBolt(const Values& v, QString* warning) {
  const MetricSize size = metric(v.pick("size"), v.num("diameter"), v.num("pitch"), warning);
  const ThreadForm form = isoThread(size.diameter, size.pitch);
  const double len = v.num("length");
  const QString head = v.pick("head");
  double headH = 0;
  Manifold bolt;
  if (head == QLatin1String("hex")) {
    headH = std::max(2.0, std::round(size.diameter * 0.65 * 10) / 10);
    bolt = chamferedHex(size.flats, 0, headH);
  } else if (head == QLatin1String("knurled")) {
    headH = std::max(3.0, size.diameter * 0.8);
    bolt = prism(gripCircle(size.flats * 0.6, std::max(0.6, size.diameter * 0.06)), 0, headH) ^
           chamferedDisc(size.flats * 0.6 + std::max(0.6, size.diameter * 0.06), 0, headH, std::min(1.0, headH * 0.2) + 0.6);
  }
  const Manifold shank = boltShank(form, headH - (headH > 0 ? 0.01 : 0), headH + len, headH, headH <= 0);
  bolt = headH > 0 ? bolt + shank : shank;
  std::vector<Piece> pieces{{QStringLiteral("Bolt"), bolt}};
  if (!v.on("nut")) return pieces;
  const double nutH = std::max(2.0, size.diameter * 0.8);
  const double nutZ = headH + std::max(1.0, std::min(len - nutH, len * 0.3));
  const Manifold nut = nutBody(size, v.num("clearance"), nutZ, nutZ + nutH, headH);
  pieces.push_back({QStringLiteral("Nut"), nut});
  if (v.pick("layout") == QLatin1String("assembled")) return pieces;
  sideBySide(pieces, 6);
  return pieces;
}

std::vector<Piece> buildNut(const Values& v, QString* warning) {
  const MetricSize size = metric(v.pick("size"), v.num("diameter"), v.num("pitch"), warning);
  const double h = std::max(2.0, size.diameter * (v.on("thin") ? 0.5 : 0.8));
  return {{QStringLiteral("Nut"), nutBody(size, v.num("clearance"), 0, h, 0)}};
}

// ---------------------------------------------------------------------------------------------
// Standoffs

// Hole sizes for small metric screws: pilot (self tapping), clearance (the screw slides through),
// heat-set insert hole and insert length.
std::vector<Piece> buildStandoff(const Values& v, QString*) {
  const ScrewHole hole = screwHole(v.pick("screw"));
  const QString kind = v.pick("hole");
  const double h = v.num("height");
  const double widest = kind == QLatin1String("insert") ? hole.insert : kind == QLatin1String("screw") ? hole.pilot : hole.clear;
  const double R = widest * 0.5 + v.num("postWall");
  const bool plate = v.on("plate");
  const double base = plate ? v.num("plateThickness") : 0;
  std::vector<std::pair<double, double>> spots;
  const QString layout = v.pick("layout");
  const double sx = v.num("spacingX") * 0.5, sy = v.num("spacingY") * 0.5;
  if (layout == QLatin1String("single")) spots = {{0, 0}};
  else if (layout == QLatin1String("pair")) spots = {{-sx, 0}, {sx, 0}};
  else spots = {{-sx, -sy}, {sx, -sy}, {-sx, sy}, {sx, sy}};

  std::vector<Manifold> solids;
  std::vector<Manifold> cuts;
  for (auto [x, y] : spots) {
    if (v.on("flare")) {
      const double flare = std::min(2.0, h * 0.4);
      solids.push_back(loft(circleOutline(R), {{base - 0.01, flare}, {base + flare, 0}, {base + h, 0}}).Translate(vec3(x, y, 0)));
    } else {
      solids.push_back(cylinderZ(R, base - 0.01, base + h, x, y));
    }
    const double topZ = base + h;
    if (kind == QLatin1String("insert")) {
      const double pocket = std::min(hole.insertLen + 0.5, h + base - 0.6);
      cuts.push_back(cylinderZ(hole.insert * 0.5, topZ - pocket, topZ + 1, x, y));
      cuts.push_back(coneZ(hole.insert * 0.5, hole.insert * 0.5 + 0.5, topZ - 0.5, topZ + 0.01, x, y));
      cuts.push_back(cylinderZ(hole.clear * 0.5, -1, topZ, x, y));
    } else {
      const double d = kind == QLatin1String("screw") ? hole.pilot : hole.clear;
      cuts.push_back(cylinderZ(d * 0.5, kind == QLatin1String("screw") && !plate ? 0.8 : -1, topZ + 1, x, y));
      cuts.push_back(coneZ(d * 0.5, d * 0.5 + 0.4, topZ - 0.4, topZ + 0.01, x, y));
    }
  }
  if (plate) {
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (auto [x, y] : spots) {
      x0 = std::min(x0, x - R);
      x1 = std::max(x1, x + R);
      y0 = std::min(y0, y - R);
      y1 = std::max(y1, y + R);
    }
    const double m = v.num("margin");
    solids.push_back(prism(roundRectOutline((x1 - x0) * 0.5 + m, (y1 - y0) * 0.5 + m, std::min(4.0, m + R))(0), 0, base)
                         .Translate(vec3((x0 + x1) * 0.5, (y0 + y1) * 0.5, 0)));
  }
  Manifold all = Manifold::BatchBoolean(solids, OpType::Add) - Manifold::BatchBoolean(cuts, OpType::Add);
  if (!plate) {
    // Separate posts: hand them out one by one.
    std::vector<Piece> pieces;
    int index = 1;
    for (const Manifold& post : all.Decompose()) pieces.push_back({QStringLiteral("Standoff %1").arg(index++), post});
    return pieces;
  }
  return {{QStringLiteral("Standoffs"), all}};
}

// ---------------------------------------------------------------------------------------------
// Gears

SimplePolygon gearOutline(int teeth, double module, double pressureDeg, double backlash) {
  const double r = module * teeth * 0.5;
  const double alpha = pressureDeg * kPi / 180.0;
  const double rb = r * std::cos(alpha);
  double ra = r + module;
  const double rf = std::max(r - 1.25 * module, module * 0.5);
  auto inv = [](double a) { return std::tan(a) - a; };
  auto phi = [&](double rho) { return rho <= rb ? 0.0 : inv(std::acos(rb / rho)); };
  const double half = kPi / (2.0 * teeth) - backlash / (4.0 * r) + inv(alpha);
  auto width = [&](double rho) { return half - phi(rho); };
  // Small gears end in a point before the outside circle; stop the tip just short of it.
  while (width(ra) < 0.15 / ra && ra > r + 0.1) ra -= 0.02 * module;
  const double start = std::max(rf, rb);
  const int flankSteps = 10;
  SimplePolygon poly;
  auto add = [&](double rho, double angle) { poly.push_back(vec2(rho * std::cos(angle), rho * std::sin(angle))); };
  for (int k = 0; k < teeth; ++k) {
    const double center = 2 * kPi * k / teeth;
    const double next = 2 * kPi * (k + 1) / teeth;
    // Right flank, root to tip.
    if (rf < rb) add(rf, center - width(rb));
    for (int i = 0; i <= flankSteps; ++i) {
      const double rho = start + (ra - start) * i / flankSteps;
      add(rho, center - width(rho));
    }
    // Tip.
    for (int i = 1; i < 4; ++i) add(ra, center - width(ra) + 2 * width(ra) * i / 4);
    // Left flank, tip to root.
    for (int i = flankSteps; i >= 0; --i) {
      const double rho = start + (ra - start) * i / flankSteps;
      add(rho, center + width(rho));
    }
    if (rf < rb) add(rf, center + width(rb));
    // Root, to the next tooth.
    const double from = center + width(start), to = next - width(start);
    for (int i = 1; i < 5; ++i) add(rf, from + (to - from) * i / 5);
  }
  return poly;
}

Manifold gearSolid(int teeth, double module, double pressure, double backlash, double thickness, double bore,
                   double helixDeg, bool herringbone, double rotateDeg, bool leftHand, QString* warning = nullptr) {
  CrossSection section = CrossSection(gearOutline(teeth, module, pressure, backlash)).Rotate(rotateDeg);
  // Keep a ring of material between the bore and the roots of the teeth.
  const double maxBore = 2 * (module * (teeth * 0.5 - 1.25) - std::max(1.0, module));
  if (bore > maxBore) {
    bore = std::max(0.0, maxBore);
    note(warning, QStringLiteral("The bore was made smaller to leave material under the teeth."));
  }
  if (bore > 0.1) section = section - CrossSection(circle(bore * 0.5, circleSegments(bore * 0.5)));
  const Polygons polys = section.ToPolygons();
  const double r = module * teeth * 0.5;
  double twist = 0;
  if (helixDeg > 0.01) twist = thickness * std::tan(helixDeg * kPi / 180.0) / r * 180.0 / kPi;
  if (leftHand) twist = -twist;
  if (std::abs(twist) < 1e-6) return Manifold::Extrude(polys, thickness);
  if (!herringbone) {
    const int steps = std::max(2, static_cast<int>(std::ceil(std::abs(twist) / 2)));
    return Manifold::Extrude(polys, thickness, steps, twist);
  }
  const double halfTwist = twist * 0.5;
  const int steps = std::max(2, static_cast<int>(std::ceil(std::abs(halfTwist) / 2)));
  const Manifold lower = Manifold::Extrude(polys, thickness * 0.5, steps, halfTwist);
  // Extrude twists counter clockwise like Rotate; carry on from where the lower half ended.
  const Manifold upper = Manifold::Extrude(polys, thickness * 0.5, steps, -halfTwist)
                             .Rotate(0, 0, halfTwist)
                             .Translate(vec3(0, 0, thickness * 0.5));
  return lower + upper;
}

std::vector<Piece> buildGear(const Values& v, QString* warning) {
  const double m = v.num("module");
  const int z1 = v.whole("teeth");
  const double th = v.num("thickness");
  const double pa = v.num("pressure");
  const double bl = v.num("backlash");
  const double helix = v.num("helix");
  const bool herring = v.on("herringbone") && helix > 0.01;
  std::vector<Piece> pieces;
  pieces.push_back({QStringLiteral("Gear"), gearSolid(z1, m, pa, bl, th, v.num("bore"), helix, herring, 0, false, warning)});
  if (!v.on("mate")) return pieces;
  const int z2 = v.whole("mateTeeth");
  // Turn the mate so a gap faces the first gear's tooth at +X.
  const double offset = 180.0 - 180.0 / z2;
  // The mate's helix runs the other way, scaled so the teeth stay in mesh through the thickness.
  // Same helix angle, other hand: gearSolid scales the twist by 1/radius, so the teeth stay in mesh.
  const Manifold mate = gearSolid(z2, m, pa, bl, th, v.num("mateBore"), helix, herring, offset, true, warning);
  const double distance = m * (z1 + z2) * 0.5;
  if (v.pick("layout") == QLatin1String("apart")) {
    pieces.push_back({QStringLiteral("Mating gear"), mate.Translate(vec3(distance + m * 3, 0, 0))});
  } else {
    pieces.push_back({QStringLiteral("Mating gear"), mate.Translate(vec3(distance, 0, 0))});
  }
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Print-in-place hinge

Manifold alongX(const Manifold& solidAlongZ, double x0, double zCenter) {
  // Extruded along +Z from 0: turn it to run along +X from x0, centered at height zCenter.
  return solidAlongZ.Rotate(0, 90, 0).Translate(vec3(x0, 0, zCenter));
}

std::vector<Piece> buildHinge(const Values& v, QString* warning) {
  const double L = v.num("length");
  const double W = v.num("leaf");
  const double c = v.num("clearance");
  double D = v.num("knuckleDiameter");
  const double t = std::min(v.num("thickness"), D);
  int n = v.whole("knuckles");
  if (n % 2 == 0) n += 1;
  double pin = v.num("pin");
  // The hole's pointed roof reaches (pin/2 + c) * sqrt(2) above the axis; keep 0.8 mm of knuckle over it.
  const double maxPin = 2 * ((D * 0.5 - 0.8) / std::sqrt(2.0) - c);
  if (pin > maxPin) {
    pin = std::max(1.0, maxPin);
    note(warning, QStringLiteral("Pin made smaller to leave a wall in the knuckles."));
  }
  const double R = D * 0.5;
  // Each knuckle needs 1.5 mm; use fewer (still an odd count) when the length is too short.
  const int fit = static_cast<int>(std::floor((L + c) / (1.5 + c)));
  if (n > fit) {
    n = fit % 2 == 0 ? fit - 1 : fit;
    if (n < 3) {
      note(warning, QStringLiteral("The hinge is too short for its knuckles."));
      return {};
    }
    note(warning, QStringLiteral("Too many knuckles for this length; using %1.").arg(n));
  }
  const double seg = (L - (n - 1) * c) / n;
  // A round pin, so the leaves turn freely. Its hole is round too, with a 45° pointed roof (a
  // teardrop) so the top of the hole prints without supports; the pin's underside bridges between
  // the outer knuckles. In alongX, the polygon's -x becomes up.
  auto teardrop = [](double r) {
    SimplePolygon pts = circle(r, std::clamp(circleSegments(r), 32, 96));
    pts.push_back(vec2(-r * std::sqrt(2.0), 0));
    return CrossSection::Hull(pts);
  };
  const int roundSegs = circleSegments(R);
  std::vector<Manifold> knA, knB, cutForA, cutForB;
  for (int k = 0; k < n; ++k) {
    const double x0 = -L * 0.5 + k * (seg + c);
    const Manifold knuckle = alongX(Manifold::Cylinder(seg, R, R, roundSegs), x0, R);
    const Manifold keepOut = alongX(Manifold::Cylinder(seg + 2 * c, R + c, R + c, circleSegments(R + c)), x0 - c, R);
    if (k % 2 == 0) {
      knA.push_back(knuckle);
      cutForB.push_back(keepOut);
    } else {
      knB.push_back(knuckle);
      cutForA.push_back(keepOut);
    }
  }
  const Manifold pinSolid =
      alongX(Manifold::Extrude(Polygons{circle(pin * 0.5, std::clamp(circleSegments(pin * 0.5), 32, 96))}, L), -L * 0.5, R);
  const Manifold pinHole = alongX(Manifold::Extrude(teardrop(pin * 0.5 + c).ToPolygons(), L + 2), -L * 0.5 - 1, R);
  Manifold plateA = block(-L * 0.5, L * 0.5, -(R + W), 0, 0, t);
  Manifold plateB = block(-L * 0.5, L * 0.5, 0, R + W, 0, t);
  const int holes = v.whole("holes");
  if (holes > 0) {
    const double hr = v.num("holeDiameter") * 0.5;
    std::vector<Manifold> cuts;
    for (int i = 0; i < holes; ++i) {
      const double x = -L * 0.5 + (i + 0.5) * L / holes;
      for (double y : {-(R + W * 0.5), R + W * 0.5}) {
        cuts.push_back(cylinderZ(hr, -1, t + 1, x, y));
        if (v.on("countersink")) cuts.push_back(coneZ(hr, hr * 2, t - hr, t + 0.01, x, y));
      }
    }
    const Manifold all = Manifold::BatchBoolean(cuts, OpType::Add);
    plateA = plateA - all;
    plateB = plateB - all;
  }
  Manifold a = (plateA - Manifold::BatchBoolean(cutForA, OpType::Add)) + Manifold::BatchBoolean(knA, OpType::Add) + pinSolid;
  Manifold b = (plateB - Manifold::BatchBoolean(cutForB, OpType::Add)) + Manifold::BatchBoolean(knB, OpType::Add);
  b = b - pinHole;
  // One piece on the printer: the leaves only come apart by breaking it.
  return {{QStringLiteral("Hinge"), a + b}};
}

// ---------------------------------------------------------------------------------------------
// Electronics enclosure

// The 2D outline of a wall opening, centered on the origin: x along the wall, y up.
CrossSection openingShape(const QString& type, double diameter, double w, double h) {
  if (type == QLatin1String("usbc")) return CrossSection(roundRect(4.7, 1.8, 1.6, 6));
  if (type == QLatin1String("microusb")) return CrossSection(roundRect(4.1, 1.6, 0.8, 4));
  if (type == QLatin1String("usba")) return CrossSection(roundRect(6.75, 3.1, 0.5, 3));
  if (type == QLatin1String("round")) return CrossSection(circle(diameter * 0.5, std::clamp(circleSegments(diameter * 0.5), 24, 96)));
  return CrossSection(roundRect(w * 0.5, h * 0.5, std::min(0.5, std::min(w, h) * 0.25), 3));
}

// Lid vents inside `area`: rounded slots across Y, or hexagons.
CrossSection ventHoles(const QString& kind, const CrossSection& area) {
  const manifold::Rect box = area.Bounds();
  const double spanX = box.max.x - box.min.x, spanY = box.max.y - box.min.y;
  std::vector<CrossSection> cells;
  if (kind == QLatin1String("slots")) {
    const double len = spanY * 0.8;
    const int n = std::max(1, static_cast<int>(std::floor((spanX * 0.8) / 4.0)));
    for (int i = 0; i < n; ++i) {
      const double x = (i - (n - 1) * 0.5) * 4.0 + (box.min.x + box.max.x) * 0.5;
      cells.push_back(CrossSection(roundRect(1.0, len * 0.5, 1.0, 4)).Translate(vec2(x, (box.min.y + box.max.y) * 0.5)));
    }
  } else {
    const double cell = 5, web = 1.4, pitch = cell + web;
    const double R = cell / std::sqrt(3.0), dx = pitch * std::sqrt(3.0) * 0.5;
    const CrossSection hex(regular(R, 6, 0));
    const vec2 c((box.min.x + box.max.x) * 0.5, (box.min.y + box.max.y) * 0.5);
    for (int i = -static_cast<int>(spanX / dx) - 1; i <= static_cast<int>(spanX / dx) + 1; ++i)
      for (int j = -static_cast<int>(spanY / pitch) - 1; j <= static_cast<int>(spanY / pitch) + 1; ++j)
        cells.push_back(hex.Translate(vec2(c.x + i * dx, c.y + j * pitch + ((i & 1) ? pitch * 0.5 : 0))));
  }
  CrossSection all = CrossSection::BatchBoolean(cells, OpType::Add) ^ area;
  // Only whole-enough holes: drop slivers cut off at the edge.
  std::vector<CrossSection> keep;
  for (const CrossSection& piece : all.Decompose())
    if (piece.Area() > 4) keep.push_back(piece);
  return CrossSection::BatchBoolean(keep, OpType::Add);
}

// A screw post in a corner of the inside (Li x Wi), joined to both walls: the round post plus its
// tangents out to the walls. `grow` makes it bigger all round (the lid's rim keeps that far away).
Manifold cornerPost(double Li, double Wi, double postR, double grow, double z0, double z1, int sx, int sy) {
  const double px = Li * 0.5 - postR, py = Wi * 0.5 - postR;
  const double ex = Li * 0.5 + 1, ey = Wi * 0.5 + 1;
  Manifold post = cylinderZ(postR + grow, z0, z1, px, py) + block(px, ex, py - postR - grow, ey, z0, z1) +
                  block(px - postR - grow, ex, py, ey, z0, z1);
  if (sx < 0) post = post.Mirror(vec3(1, 0, 0));
  if (sy < 0) post = post.Mirror(vec3(0, 1, 0));
  return post;
}

std::vector<Piece> buildEnclosure(const Values& v, QString* warning) {
  const double Li = v.num("length"), Wi = v.num("width"), Hi = v.num("height");
  double t = v.num("wall");
  const double f = v.num("floor");
  // Sealed: an O-ring groove in the top of the walls, the lid screws outside it.
  const bool sealed = v.pick("lid") == QLatin1String("screws") && v.on("seal");
  const Seal seal = sealFor(v, v.num("clearance"));
  if (sealed && t < seal.rim()) {
    note(warning, QStringLiteral("The walls were made %1 mm thick to hold the O-ring groove.").arg(seal.rim(), 0, 'f', 1));
    t = seal.rim();
  }
  const double L = Li + 2 * t, W = Wi + 2 * t, H = f + Hi;
  const double r = std::clamp(v.num("corner"), 0.0, std::min(L, W) * 0.5 - 0.5);
  const auto outer = roundRectOutline(L * 0.5, W * 0.5, r);
  const auto inner = roundRectOutline(Li * 0.5, Wi * 0.5, std::max(r - t, 0.3));
  const double T = v.num("lidThickness");
  const double c = v.num("clearance");
  const bool screws = v.pick("lid") == QLatin1String("screws");
  Manifold box = prism(outer(0), 0, H) - prism(inner(0), f, H + 1);
  Manifold lid = prism(outer(0), H, H + T);

  // Corner posts for the lid screws, filled into the corners so they are part of the walls.
  std::vector<std::pair<double, double>> posts;
  double postR = 0;
  Manifold gasketPart;
  if (sealed) {
    SealedTop top = sealRect(box, L, W, r, t, H, T, f, seal, screwHole(v.pick("screw")), v.pick("hole") == QLatin1String("insert"),
                             v.on("gasket"), v.pick("screwPlace") != QLatin1String("walls"), warning);
    box = top.box;
    lid = top.lid;
    gasketPart = top.gasket;
    if (v.pick("vents") != QLatin1String("none") || v.on("sideVents"))
      note(warning, QStringLiteral("Vents let water in past the seal."));
  } else if (screws) {
    const ScrewHole sh = screwHole(v.pick("screw"));
    const bool insert = v.pick("hole") == QLatin1String("insert");
    postR = (insert ? sh.insert : sh.pilot) * 0.5 + 1.6;
    if (postR * 2 > std::min(Li, Wi) * 0.45) note(warning, QStringLiteral("The box is small for corner posts."));
    std::vector<Manifold> solids, cuts, lidCuts;
    for (int sx : {-1, 1})
      for (int sy : {-1, 1}) {
        const double x = sx * (Li * 0.5 - postR), y = sy * (Wi * 0.5 - postR);
        posts.push_back({x, y});
        solids.push_back(cornerPost(Li, Wi, postR, 0, f - 0.01, H, sx, sy));
        if (insert) {
          const double pocket = std::min(sh.insertLen + 0.5, Hi - 1);
          cuts.push_back(cylinderZ(sh.insert * 0.5, H - pocket, H + 1, x, y));
          cuts.push_back(coneZ(sh.insert * 0.5, sh.insert * 0.5 + 0.5, H - 0.5, H + 0.01, x, y));
          cuts.push_back(cylinderZ(sh.clear * 0.5, std::max(f + 1, H - pocket - 4), H, x, y));  // room for the screw tip
        } else {
          cuts.push_back(cylinderZ(sh.pilot * 0.5, f + 1, H + 1, x, y));
          cuts.push_back(coneZ(sh.pilot * 0.5, sh.pilot * 0.5 + 0.4, H - 0.4, H + 0.01, x, y));
        }
        lidCuts.push_back(cylinderZ(sh.clear * 0.5, H - 1, H + T + 1, x, y));
        // Countersink for a flat-head screw.
        const double head = sh.clear;
        lidCuts.push_back(coneZ(sh.clear * 0.5, head, H + T - head * 0.5, H + T + 0.01, x, y));
      }
    box = (box + (Manifold::BatchBoolean(solids, OpType::Add) ^ prism(outer(-0.01), f - 0.01, H))) -
          Manifold::BatchBoolean(cuts, OpType::Add);
    lid = lid - Manifold::BatchBoolean(lidCuts, OpType::Add);
  }

  // The lid's rim that drops into the box (locates a screwed lid, holds a press-fit one). A sealed
  // lid has its tongue instead.
  if (!sealed) {
    const double depth = screws ? 2.0 : std::min(5.0, Hi * 0.4);
    Manifold rim = prism(inner(-c), H - depth, H + 0.01) - prism(inner(-c - 1.2), H - depth - 1, H + 0.02);
    for (auto [x, y] : posts)
      rim = rim - cornerPost(Li, Wi, postR, c, H - depth - 1, H + 1, x < 0 ? -1 : 1, y < 0 ? -1 : 1);
    lid = lid + rim;
  }

  // Lid vents, clear of the rim and the posts.
  const QString vents = v.pick("vents");
  if (vents != QLatin1String("none")) {
    CrossSection area(inner(-c - 3));
    for (auto [x, y] : posts) area = area - CrossSection(circle(postR + 3, 32)).Translate(vec2(x, y));
    const CrossSection holes = ventHoles(vents, area);
    if (holes.Area() > 0) lid = lid - prism(holes, H - 1, H + T + 1);
  }

  // Vent slots in the two long walls, high up.
  if (v.on("sideVents")) {
    const double hs = std::clamp(Hi * 0.35, 3.0, 15.0);
    const double zc = f + Hi * 0.65;
    const int n = std::max(1, static_cast<int>(std::floor(Li * 0.6 / 5.0)));
    std::vector<Manifold> ventCuts;  // ("slots" is a Qt keyword)
    for (int i = 0; i < n; ++i) {
      const double x = (i - (n - 1) * 0.5) * 5.0;
      for (int side : {-1, 1}) {
        const double y0 = side < 0 ? -W * 0.5 - 1 : Wi * 0.5 - 0.5, y1 = side < 0 ? -Wi * 0.5 + 0.5 : W * 0.5 + 1;
        const Manifold rod = Manifold::Cylinder(y1 - y0, 1.0, 1.0, 16).Rotate(-90, 0, 0).Translate(vec3(x, y0, 0));
        ventCuts.push_back(Manifold::Hull({rod.Translate(vec3(0, 0, zc - hs * 0.5 + 1)), rod.Translate(vec3(0, 0, zc + hs * 0.5 - 1))}));
      }
    }
    box = box - Manifold::BatchBoolean(ventCuts, OpType::Add);
  }

  // Openings through the walls.
  for (int i = 1; i <= 3; ++i) {
    const QByteArray k = QByteArray::number(i);
    const QString wall = v.pick(("cut" + k + "Wall").constData());
    if (wall == QLatin1String("none")) continue;
    const QString type = v.pick(("cut" + k + "Type").constData());
    const double u = v.num(("cut" + k + "X").constData());
    const double zc = f + v.num(("cut" + k + "Z").constData());
    CrossSection shape = openingShape(type, v.num(("cut" + k + "Diameter").constData()), v.num(("cut" + k + "W").constData()),
                                      v.num(("cut" + k + "H").constData()));
    const bool sideways = wall == QLatin1String("left") || wall == QLatin1String("right");
    // Front/back: shape x along X, shape y up. Left/right: the shape is turned so its x points up.
    if (sideways) shape = shape.Rotate(90);
    const Manifold slab = Manifold::Extrude(shape.ToPolygons(), t + 2).Translate(vec3(0, 0, -1));
    Manifold cut;
    if (wall == QLatin1String("front")) cut = slab.Rotate(90, 0, 0).Translate(vec3(u, -Wi * 0.5, zc));
    else if (wall == QLatin1String("back")) cut = slab.Rotate(-90, 0, 0).Translate(vec3(-u, Wi * 0.5, zc));
    else if (wall == QLatin1String("left")) cut = slab.Rotate(0, -90, 0).Translate(vec3(-Li * 0.5, -u, zc));
    else cut = slab.Rotate(0, 90, 0).Translate(vec3(Li * 0.5, u, zc));
    const auto cb = cut.BoundingBox();
    if (cb.min.z < f - 0.01 || cb.max.z > H + 0.01) note(warning, QStringLiteral("An opening runs past the floor or the top of the wall."));
    box = box - cut;
  }

  // Standoffs for a board.
  if (v.on("pcb")) {
    const ScrewHole ph = screwHole(v.pick("pcbScrew"));
    const double sr = ph.pilot * 0.5 + 1.6;
    const double h = v.num("pcbHeight");
    const double ox = v.num("pcbOffsetX"), oy = v.num("pcbOffsetY");
    std::vector<Manifold> solids, cuts;
    for (int sx : {-1, 1})
      for (int sy : {-1, 1}) {
        const double x = ox + sx * v.num("pcbX") * 0.5, y = oy + sy * v.num("pcbY") * 0.5;
        solids.push_back(loft(circleOutline(sr), {{f - 0.01, std::min(1.5, h * 0.4)}, {f + std::min(1.5, h * 0.4), 0}, {f + h, 0}})
                             .Translate(vec3(x, y, 0)));
        cuts.push_back(cylinderZ(ph.pilot * 0.5, f + 0.6, f + h + 1, x, y));
      }
    Manifold standoffs = Manifold::BatchBoolean(solids, OpType::Add) - Manifold::BatchBoolean(cuts, OpType::Add);
    standoffs = standoffs ^ prism(inner(0), f - 0.02, H);
    if (standoffs.IsEmpty()) note(warning, QStringLiteral("The board standoffs do not fit inside."));
    else box = box + standoffs;
  }

  std::vector<Piece> pieces{{QStringLiteral("Enclosure"), box}, {QStringLiteral("Lid"), lid}};
  if (ok(gasketPart)) pieces.push_back({QStringLiteral("Gasket"), gasketPart});
  if (v.pick("layout") == QLatin1String("assembled")) {
    pieces[1].solid = pieces[1].solid.Translate(vec3(0, 0, std::max(c, 0.05)));
    return pieces;
  }
  pieces[1].solid = pieces[1].solid.Rotate(180, 0, 0);
  sideBySide(pieces, 10);
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Battery holder

std::vector<Piece> buildBattery(const Values& v, QString*) {
  struct Cell {
    double d, len, w, h;  // round cells: diameter and length; 9 V: w x h footprint, len tall
    int kind;             // 0 round upright, 1 coin on edge, 2 block
  };
  static const std::map<QString, Cell> cells = {
      {"AA", {14.5, 50.5, 0, 0, 0}},     {"AAA", {10.5, 44.5, 0, 0, 0}},   {"C", {26.2, 50, 0, 0, 0}},
      {"D", {34.2, 61.5, 0, 0, 0}},      {"18650", {18.6, 65.2, 0, 0, 0}}, {"21700", {21.4, 70.2, 0, 0, 0}},
      {"CR2032", {20, 3.2, 0, 0, 1}},    {"9V", {0, 48.5, 26.5, 17.5, 2}}};
  const Cell cell = cells.count(v.pick("cell")) ? cells.at(v.pick("cell")) : cells.at("AA");
  const int nx = v.whole("columns"), ny = v.whole("rows");
  const double c = v.num("clearance"), web = v.num("web"), rim = v.num("rim"), base = v.num("base");
  const double share = v.num("depth") / 100.0;
  // Pocket footprint (x by y) and depth.
  double px, py, depth;
  if (cell.kind == 0) {
    px = py = cell.d + 2 * c;
    depth = cell.len * share;
  } else if (cell.kind == 1) {
    px = cell.len + 2 * c;  // the coin's thickness
    py = cell.d + 2 * c;
    depth = cell.d * share;
  } else {
    px = cell.w + 2 * c;
    py = cell.h + 2 * c;
    depth = cell.len * share;
  }
  const double pitchX = px + web, pitchY = py + web;
  const double sizeX = nx * pitchX - web + 2 * rim, sizeY = ny * pitchY - web + 2 * rim;
  const double top = base + depth;
  const double r = std::clamp(v.num("corner"), 0.0, std::min(sizeX, sizeY) * 0.5 - 0.5);
  Manifold holder = prism(roundRectOutline(sizeX * 0.5, sizeY * 0.5, r)(0), 0, top);
  const double lead = std::min(0.8, web * 0.4);
  Manifold pocket;
  if (cell.kind == 0) {
    pocket = loft(circleOutline(px * 0.5), {{base, 0}, {top - lead, 0}, {top + 0.01, lead}, {top + 1, lead}});
  } else {
    const double rr = cell.kind == 1 ? 0.3 : 1.0;  // small, or the corners pinch a coin's edge
    pocket = loft([&](double off) { return roundRect(px * 0.5 + off, py * 0.5 + off, rr + off, 6); },
                  {{base, 0}, {top - lead, 0}, {top + 0.01, lead}, {top + 1, lead}});
    if (cell.kind == 1) {
      // A round bottom that follows the coin, so it sits steady on its edge.
      const double rc = py * 0.5;
      pocket = pocket ^ (Manifold::Cylinder(px + 2, rc, rc, circleSegments(rc), true).Rotate(0, 90, 0).Translate(vec3(0, 0, base + rc)) +
                         block(-px, px, -py, py, base + rc, top + 2));
    }
  }
  std::vector<Manifold> cuts;
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
      cuts.push_back(pocket.Translate(vec3((i - (nx - 1) * 0.5) * pitchX, (j - (ny - 1) * 0.5) * pitchY, 0)));
  holder = holder - Manifold::BatchBoolean(cuts, OpType::Add);
  return {{QStringLiteral("Battery holder"), holder}};
}

// ---------------------------------------------------------------------------------------------
// Ball joint

// What a stem ends in, standing on z = 0; returns its top height through `top`.
Manifold jointBase(const QString& kind, double stemR, double clearance, double* top) {
  if (kind == QLatin1String("plate")) {
    *top = 4;
    Manifold plate = prism(roundRect(16, 12, 3, 6), 0, 4);
    for (double x : {-10.0, 10.0}) plate = plate - cylinderZ(1.7, -1, 5, x, 0) - coneZ(1.7, 3.4, 4 - 1.7, 4.01, x, 0);
    return plate;
  }
  if (kind == QLatin1String("foot")) {
    *top = 3;
    return chamferedDisc(std::max(stemR + 6, 10.0), 0, 3, 0.8, 0.8, false, true);
  }
  if (kind == QLatin1String("camera")) {
    // A boss with a 1/4"-20 UNC thread from below (same 60° form as metric), for a tripod screw.
    *top = 12;
    const ThreadForm form = isoThread(6.35, 1.27);
    Manifold boss = chamferedDisc(std::max(stemR + 2.5, 7.5), 0, 12, 0.6, 0.6, true, false);
    boss = boss - threadedRod(form, -1, 9, 0, clearance + 0.1) - coneZ(3.9, 3.2, -0.01, 0.8);
    return boss;
  }
  *top = 0;
  return {};
}

std::vector<Piece> buildBallJoint(const Values& v, QString* warning) {
  const double R = v.num("ball") * 0.5;
  const double c = v.num("clearance");
  double stemR = v.num("stem") * 0.5;
  const double socketTop = 0.55 * R;  // above the ball's center: holds it in
  const double opening = std::sqrt(std::max(0.0, (R + c) * (R + c) - socketTop * socketTop));
  if (stemR > opening - 0.8) {
    stemR = std::max(1.0, opening - 0.8);
    note(warning, QStringLiteral("Stem made thinner to pass the socket's opening."));
  }
  const double stemL = v.num("stemLength");
  const int segs = std::clamp(circleSegments(R) * 2, 64, 160);

  // Ball part: base, stem, a 45° cone under the ball so it prints without supports, the ball.
  double baseTop = 0;
  Manifold ballPart = jointBase(v.pick("ballBase"), stemR, c, &baseTop);
  const double zc = baseTop + stemL + R;
  Manifold stem = cylinderZ(stemR, baseTop - 0.01, zc);
  const double tangent = R / std::sqrt(2.0);
  if (tangent > stemR) stem = stem + coneZ(stemR, tangent, zc - tangent - (tangent - stemR), zc - tangent);
  const Manifold ball = Manifold::Sphere(R, segs).Translate(vec3(0, 0, zc));
  ballPart = ballPart.IsEmpty() ? stem + ball : ballPart + stem + ball;

  // Socket part: base, stem, a cup around the ball with four slots so it springs open.
  double socketBase = 0;
  Manifold socket = jointBase(v.pick("socketBase"), stemR, c, &socketBase);
  const double wall = std::max(2.0, R * 0.25);
  const double cupR = R + c + wall;
  const double sc = socketBase + stemL + cupR;  // the socket's center
  const double cupBottom = sc - R - c - 1.6;
  Manifold cup = cylinderZ(cupR, cupBottom, sc + socketTop);
  // 45° skirt from the stem up to the cup, so its underside needs no support.
  cup = cup + coneZ(stemR, cupR, cupBottom - (cupR - stemR), cupBottom + 0.01);
  Manifold sStem = cylinderZ(stemR, socketBase - 0.01, cupBottom - (cupR - stemR) + 0.01);
  cup = cup - Manifold::Sphere(R + c, segs).Translate(vec3(0, 0, sc));
  // Lead-in so the ball starts the opening.
  cup = cup - coneZ(opening - 0.2, opening + 1.2, sc + socketTop - 1.4, sc + socketTop + 0.01);
  const double slotDown = sc - 0.15 * R;
  cup = cup - block(-cupR - 1, cupR + 1, -0.6, 0.6, slotDown, sc + socketTop + 1) -
        block(-0.6, 0.6, -cupR - 1, cupR + 1, slotDown, sc + socketTop + 1);
  socket = socket.IsEmpty() ? sStem + cup : socket + sStem + cup;

  std::vector<Piece> pieces{{QStringLiteral("Ball"), ballPart}, {QStringLiteral("Socket"), socket}};
  if (v.pick("layout") == QLatin1String("assembled")) {
    // The ball part upside down, its ball in the socket.
    pieces[0].solid = ballPart.Translate(vec3(0, 0, -zc)).Rotate(180, 0, 0).Translate(vec3(0, 0, sc));
    return pieces;
  }
  sideBySide(pieces, 10);
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Hose adapter

std::vector<Piece> buildHose(const Values& v, QString* warning) {
  const double wall = v.num("wall"), c = v.num("clearance");
  struct End {
    QString type;
    double ro, ri, len;
  };
  auto endOf = [&](const char* type, const char* diameter, const char* length) {
    End e{v.pick(type), 0, 0, v.num(length)};
    const double d = v.num(diameter);
    if (e.type == QLatin1String("sleeve")) {
      e.ri = d * 0.5 + c;
      e.ro = e.ri + wall;
    } else {
      e.ro = d * 0.5;
      e.ri = e.ro - wall;
    }
    if (e.ri < 0.6) {
      e.ri = 0.6;
      note(warning, QStringLiteral("An end is too thin for this wall; its bore is left at 1.2 mm."));
    }
    return e;
  };
  const End a = endOf("typeA", "diameterA", "lengthA");
  const End b = endOf("typeB", "diameterB", "lengthB");
  // Outer and inner profiles as (radius, z), bottom to top.
  std::vector<vec2> out, in;
  // The barbs on an end: a slow ramp out, then 45° back, pointing away from the tip.
  auto barbs = [&](const End& e, double zTip, double dir) {
    const double h = std::clamp(e.ro * 0.08, 0.5, 1.5);
    const double pitch = std::max(3.0, h * 4);
    const int n = std::max(1, static_cast<int>(std::floor((e.len - 1) / pitch)));
    std::vector<vec2> pts;
    for (int k = 0; k < n; ++k) {
      pts.push_back(vec2(e.ro, k * pitch));
      pts.push_back(vec2(e.ro + h, k * pitch + pitch - h));
    }
    pts.push_back(vec2(e.ro, n * pitch));
    pts.push_back(vec2(e.ro, e.len));
    std::vector<vec2> placed;
    for (const vec2& p : pts) placed.push_back(vec2(p.x, zTip + dir * p.y));
    return placed;
  };
  auto endOuter = [&](const End& e, double zTip, double dir) {
    if (e.type == QLatin1String("barb")) return barbs(e, zTip, dir);
    if (e.type == QLatin1String("smooth"))
      return std::vector<vec2>{vec2(e.ro - 0.4, zTip), vec2(e.ro, zTip + dir * 1.0), vec2(e.ro, zTip + dir * e.len)};
    return std::vector<vec2>{vec2(e.ro, zTip), vec2(e.ro, zTip + dir * e.len)};
  };
  auto endInner = [&](const End& e, double zTip, double dir) {
    if (e.type == QLatin1String("sleeve"))
      return std::vector<vec2>{vec2(e.ri + 0.5, zTip), vec2(e.ri, zTip + dir * 0.5), vec2(e.ri, zTip + dir * e.len)};
    return std::vector<vec2>{vec2(e.ri, zTip), vec2(e.ri, zTip + dir * e.len)};
  };
  // The middle: long enough that nothing slopes more than 45°.
  const bool collar = v.on("collar");
  const double rc = std::max(a.ro, b.ro) + 3;
  double mid = std::max({v.num("transition"), std::abs(a.ri - b.ri) + 0.5, b.ro - a.ro + 0.5});
  if (collar) mid = std::max(mid, (rc - a.ro) + 2 + 0.5);
  const double z0 = a.len, z1 = a.len + mid, total = z1 + b.len;

  for (const vec2& p : endOuter(a, 0, 1)) out.push_back(p);
  if (collar) {
    out.push_back(vec2(rc, z0 + (rc - a.ro)));
    out.push_back(vec2(rc, z0 + (rc - a.ro) + 2));
  }
  std::vector<vec2> topOuter = endOuter(b, total, -1);
  std::reverse(topOuter.begin(), topOuter.end());
  for (const vec2& p : topOuter) out.push_back(p);
  for (const vec2& p : endInner(a, 0, 1)) in.push_back(p);
  std::vector<vec2> topInner = endInner(b, total, -1);
  std::reverse(topInner.begin(), topInner.end());
  for (const vec2& p : topInner) in.push_back(p);

  // Check the wall everywhere along the length.
  auto radiusAt = [](const std::vector<vec2>& prof, double z, bool widest) {
    double best = widest ? -1e9 : 1e9;
    for (size_t i = 0; i + 1 < prof.size(); ++i) {
      const vec2 p = prof[i], q = prof[i + 1];
      if (z < std::min(p.y, q.y) - 1e-9 || z > std::max(p.y, q.y) + 1e-9) continue;
      const double r = std::abs(q.y - p.y) < 1e-12 ? (widest ? std::max(p.x, q.x) : std::min(p.x, q.x))
                                                   : p.x + (q.x - p.x) * (z - p.y) / (q.y - p.y);
      best = widest ? std::max(best, r) : std::min(best, r);
    }
    return best;
  };
  for (double z = 0; z <= total; z += 0.25) {
    if (radiusAt(out, z, false) - radiusAt(in, z, true) < wall * 0.6) {
      note(warning, QStringLiteral("The wall gets thin in the taper; make the transition longer."));
      break;
    }
  }
  SimplePolygon poly;
  for (const vec2& p : out) poly.push_back(p);
  for (auto it = in.rbegin(); it != in.rend(); ++it) poly.push_back(*it);
  const CrossSection section = CrossSection(poly).Simplify(1e-6);
  const Manifold adapter = Manifold::Revolve(section.ToPolygons(), std::clamp(circleSegments(rc), 64, 256));
  return {{QStringLiteral("Hose adapter"), adapter}};
}

// ---------------------------------------------------------------------------------------------
// Rack and pinion

std::vector<Piece> buildRack(const Values& v, QString* warning) {
  const double m = v.num("module"), th = v.num("thickness"), back = v.num("backing");
  const double alpha = v.num("pressure") * kPi / 180.0, bl = v.num("backlash");
  const double p = kPi * m;
  const int n = std::max(2, static_cast<int>(std::round(v.num("length") / p)));
  const double half = n * p * 0.5;
  const double yRoot = -1.25 * m, yTip = m, yBottom = yRoot - back;
  // Half the tooth's width at height y (the pitch line is y = 0).
  auto w = [&](double y) { return p * 0.25 - bl * 0.25 - y * std::tan(alpha); };
  std::vector<vec2> topEdge;
  topEdge.push_back(vec2(-half, yRoot));
  for (int k = 0; k < n; ++k) {
    const double xc = -half + p * 0.5 + k * p;
    topEdge.push_back(vec2(xc - w(yRoot), yRoot));
    topEdge.push_back(vec2(xc - w(yTip), yTip));
    topEdge.push_back(vec2(xc + w(yTip), yTip));
    topEdge.push_back(vec2(xc + w(yRoot), yRoot));
  }
  topEdge.push_back(vec2(half, yRoot));
  SimplePolygon poly{vec2(-half, yBottom), vec2(half, yBottom)};
  for (auto it = topEdge.rbegin(); it != topEdge.rend(); ++it) poly.push_back(*it);
  const CrossSection section = CrossSection(poly).Simplify(1e-6);
  const double helix = v.num("helix");
  const bool herring = v.on("herringbone") && helix > 0.01;
  // nDivisions adds that many layers in between: one, at half the thickness, where a herringbone turns.
  Manifold rack = Manifold::Extrude(section.ToPolygons(), th, herring ? 1 : 0);
  if (helix > 0.01) {
    // Slant the teeth: the pinion's helix, unrolled.
    const double k = std::tan(helix * kPi / 180.0);
    rack = rack.Warp([=](vec3& q) { q.x += k * (herring ? std::min(q.z, th - q.z) : q.z); });
  }
  if (v.on("holes")) {
    const int holes = std::max(2, n / 6);
    const double yh = yRoot - back * 0.5;
    if (back >= 5) {
      for (int i = 0; i < holes; ++i) {
        const double x = -half + (i + 0.5) * (2 * half / holes);
        rack = rack - cylinderZ(1.7, -1, th + 1, x, yh) - coneZ(1.7, 3.4, th - 1.7, th + 0.01, x, yh);
      }
    }
  }
  std::vector<Piece> pieces{{QStringLiteral("Rack"), rack}};
  if (!v.on("pinion")) return pieces;
  const int z = v.whole("teeth");
  const double r = m * z * 0.5;
  // A tooth of the pinion goes into the rack's gap at x = 0 (n even) or straddles its tooth (n odd).
  const double turn = (n % 2 == 0) ? -90.0 : -90.0 + 180.0 / z;
  Manifold pinion = gearSolid(z, m, v.num("pressure"), bl, th, v.num("bore"), helix, herring, turn, false, warning);
  const double lift = v.pick("layout") == QLatin1String("apart") ? r + m * 3 + 2 : r;
  pieces.push_back({QStringLiteral("Pinion"), pinion.Translate(vec3(0, lift, 0))});
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Hole patterns (the pattern panel and the pattern tube)

// The holes of one repeat of a pattern, and how far it repeats along x and y: the holes sit at
// (i * period.x + at.x, j * period.y + at.y). Keys: shape, cell, web, grid, points, slotLength.
struct Motif {
  std::vector<std::pair<CrossSection, vec2>> holes;
  vec2 period{1, 1};
  double fullArea = 0;  // of one whole hole
};

Motif patternMotif(const Values& v) {
  const double cell = v.num("cell"), web = v.num("web");
  const QString shape = v.pick("shape");
  const bool staggered = v.pick("grid") != QLatin1String("straight");
  const double pitch = cell + web;
  Motif m;
  // One hole at the origin and the steps between holes: columns dx apart, rows dy apart; with
  // `shift` every other column moves half a row up, with `rowShift` (bricks) every other row half
  // a column over.
  CrossSection hole;
  double dx = pitch, dy = pitch;
  bool shift = false, rowShift = false;
  if (shape == QLatin1String("triangle")) {
    // Triangles pointing up and down in turn. Each sits in a tile whose side is longer by √3 x web,
    // which leaves the same web along every edge.
    const double T = cell + std::sqrt(3.0) * web;
    const double H = T * std::sqrt(3.0) * 0.5;
    const double R = cell / std::sqrt(3.0);
    const CrossSection up(regular(R, 3, 90));
    const CrossSection down(regular(R, 3, -90));
    m.holes = {{up, vec2(0, H / 3 - H * 0.5)},
               {down, vec2(T * 0.5, 2 * H / 3 - H * 0.5)},
               {down, vec2(0, H + 2 * H / 3 - H * 0.5)},
               {up, vec2(T * 0.5, H + H / 3 - H * 0.5)}};
    m.period = vec2(T, 2 * H);
    m.fullArea = std::sqrt(3.0) / 4 * cell * cell;
    return m;
  }
  if (shape == QLatin1String("circle")) {
    hole = CrossSection(circle(cell * 0.5, std::clamp(circleSegments(cell * 0.5), 16, 64)));
    dx = staggered ? pitch * std::sqrt(3.0) * 0.5 : pitch;
    shift = staggered;
  } else if (shape == QLatin1String("square")) {
    hole = CrossSection(SimplePolygon{vec2(-cell / 2, -cell / 2), vec2(cell / 2, -cell / 2), vec2(cell / 2, cell / 2), vec2(-cell / 2, cell / 2)});
    rowShift = staggered;
  } else if (shape == QLatin1String("diamond")) {
    // Squares turned 45°, on a grid turned 45°: the same web along every edge.
    hole = CrossSection(regular(cell * 0.5, 4, 0));
    const double step = cell / std::sqrt(2.0) + web;  // side + web, along the edges
    dx = step / std::sqrt(2.0);
    dy = step * std::sqrt(2.0);
    shift = true;
  } else if (shape == QLatin1String("star")) {
    const int n = v.whole("points");
    SimplePolygon star;
    for (int i = 0; i < 2 * n; ++i) {
      const double r = (i % 2 == 0) ? cell * 0.5 : cell * 0.5 * (n <= 4 ? 0.5 : 0.45);
      const double a = kPi / 2 + kPi * i / n;
      star.push_back(vec2(r * std::cos(a), r * std::sin(a)));
    }
    hole = CrossSection(star);
    dx = staggered ? pitch * std::sqrt(3.0) * 0.5 : pitch;
    shift = staggered;
  } else if (shape == QLatin1String("slot")) {
    const double len = std::max(v.num("slotLength"), cell);
    hole = CrossSection(roundRect(len * 0.5, cell * 0.5, cell * 0.5, 8));
    dx = len + web;
    rowShift = staggered;
  } else {  // hexagons
    hole = CrossSection(regular(cell / std::sqrt(3.0), 6, 0));
    dx = pitch * std::sqrt(3.0) * 0.5;
    shift = true;
  }
  m.fullArea = hole.Area();
  if (shift) {
    m.holes = {{hole, vec2(0, 0)}, {hole, vec2(dx, dy * 0.5)}};
    m.period = vec2(2 * dx, dy);
  } else if (rowShift) {
    m.holes = {{hole, vec2(0, 0)}, {hole, vec2(dx * 0.5, dy)}};
    m.period = vec2(dx, 2 * dy);
  } else {
    m.holes = {{hole, vec2(0, 0)}};
    m.period = vec2(dx, dy);
  }
  return m;
}

// The same pattern turned a quarter turn (x becomes y).
Motif turnedQuarter(const Motif& m) {
  Motif t;
  t.fullArea = m.fullArea;
  t.period = vec2(m.period.y, m.period.x);
  for (const auto& [h, at] : m.holes) t.holes.push_back({h.Rotate(90), vec2(-at.y, at.x)});
  return t;
}

// ---------------------------------------------------------------------------------------------
// Pattern panel (id "honeycomb", which it started as)

std::vector<Piece> buildHoneycomb(const Values& v, QString* warning) {
  const bool round = v.pick("outline") == QLatin1String("round");
  const double L = round ? v.num("diameter") : v.num("length");
  const double W = round ? v.num("diameter") : v.num("width");
  const double th = v.num("thickness");
  const double border = std::min(v.num("border"), std::min(L, W) * 0.5 - 1);
  const double cell = v.num("cell");
  const double turn = v.num("angle");
  const bool holesOnly = v.pick("make") == QLatin1String("holes");
  CrossSection plate, inner;
  if (round) {
    plate = CrossSection(circle(L * 0.5, circleSegments(L * 0.5)));
    inner = CrossSection(circle(L * 0.5 - border, circleSegments(L * 0.5)));
  } else {
    const double r = std::clamp(v.num("corner"), 0.0, std::min(L, W) * 0.5 - 0.5);
    const int segs = quarterSegments(std::max(r, 1.0));
    plate = CrossSection(roundRect(L * 0.5, W * 0.5, r, segs));
    inner = CrossSection(roundRect(L * 0.5 - border, W * 0.5 - border, std::max(r - border, 0.05), segs));
  }

  const Motif motif = patternMotif(v);
  // Reach: far enough to cover the plate whichever way the pattern is turned.
  const double reach = std::sqrt(L * L + W * W) * 0.5 + std::max(motif.period.x, motif.period.y);
  const bool whole = v.pick("edge") == QLatin1String("whole");
  std::vector<CrossSection> cells;
  size_t count = 0;
  const manifold::Rect bounds = inner.Bounds();
  const double c = std::cos(turn * kPi / 180), s = std::sin(turn * kPi / 180);
  const int cols = static_cast<int>(std::ceil(reach / motif.period.x)) + 1;
  const int rows = static_cast<int>(std::ceil(reach / motif.period.y)) + 1;
  for (int i = -cols; i <= cols && count <= 20000; ++i)
    for (int j = -rows; j <= rows && count <= 20000; ++j)
      for (const auto& [h, at] : motif.holes) {
        const double x = i * motif.period.x + at.x, y = j * motif.period.y + at.y;
        const double px = x * c - y * s, py = x * s + y * c;
        if (px < bounds.min.x - cell || px > bounds.max.x + cell || py < bounds.min.y - cell || py > bounds.max.y + cell) continue;
        if (++count > 20000) break;
        const CrossSection clipped = h.Rotate(turn).Translate(vec2(px, py)) ^ inner;
        const double area = clipped.Area();
        if (whole ? area > motif.fullArea * 0.999 : area > motif.fullArea * 0.2) cells.push_back(clipped);
      }
  if (count > 20000) {
    note(warning, QStringLiteral("Too many holes; make them bigger."));
    cells.clear();
  }
  if (cells.empty()) {
    if (holesOnly) note(warning, QStringLiteral("No holes fit inside the border."));
    return {{QStringLiteral("Panel"), prism(plate, 0, th)}};
  }
  const CrossSection holes = CrossSection::BatchBoolean(cells, OpType::Add);
  // Holes only: the holes themselves as solids, to cut the pattern into another part.
  if (holesOnly) return {{QStringLiteral("Pattern holes"), prism(holes, 0, th)}};
  return {{QStringLiteral("Panel"), prism(plate - holes, 0, th)}};
}

// ---------------------------------------------------------------------------------------------
// Pattern tube: the pattern wrapped round a straight or tapered tube

std::vector<Piece> buildPatternTube(const Values& v, QString* warning) {
  const bool holesOnly = v.pick("make") == QLatin1String("holes");
  const double Rb = v.num("bottomDiameter") * 0.5, Rt = v.num("topDiameter") * 0.5, H = v.num("height");
  const double wall = std::min(v.num("wall"), std::min(Rb, Rt) - 1);
  const bool closed = !holesOnly && v.pick("base") != QLatin1String("open");
  const double floor = closed ? std::min(v.num("floor"), H - 1) : 0;
  // Along the side: s runs up the slanted wall from the bottom edge.
  const double slant = std::hypot(H, Rt - Rb);
  const double up = H / slant, out = (Rt - Rb) / slant;  // the wall's direction: up and outwards
  const double bandB = std::min(v.num("bandBottom"), H), bandT = std::min(v.num("bandTop"), H - bandB);
  const double s0 = bandB / up, s1 = (H - bandT) / up;  // where the holes may go
  const bool whole = v.pick("edge") != QLatin1String("cut");
  const QString shape = v.pick("shape");

  // Upright is the way each shape prints without supports: hexagons and stars with a point on top,
  // slots running up and down.
  Motif motif = patternMotif(v);
  bool turn = shape == QLatin1String("hexagon") || shape == QLatin1String("slot");
  if (v.pick("direction") == QLatin1String("sideways")) turn = !turn;
  if (turn) motif = turnedQuarter(motif);

  // Round the tube a whole number of repeats, sized at the middle height.
  const double Rmid = (Rb + Rt) * 0.5;
  const int repeats = std::max(1, static_cast<int>(std::lround(2 * kPi * Rmid / motif.period.x)));
  const double perMm = 2 * kPi / (repeats * motif.period.x);  // radians per mm of pattern, round the middle
  // Growing with the width: the pattern drawn on the unrolled cone the way a map is drawn (every
  // hole the same shape, sized with the tube there). Row y of the pattern lands where the radius is
  // Rmid * exp(y * out / Rmid); on a straight tube that is just y up from the middle.
  const bool straight = std::abs(out) < 1e-9;
  const bool grow = v.pick("taper") != QLatin1String("same") && !straight;
  auto radiusAtRow = [&](double y) { return grow ? Rmid * std::exp(y * out / Rmid) : Rmid + y * out; };
  auto rowAtRadius = [&](double R) { return grow ? Rmid / out * std::log(R / Rmid) : (R - Rmid) / out; };
  const double sCentre = (s0 + s1) * 0.5;
  auto sAtRow = [&](double y) { return straight ? sCentre + y : (radiusAtRow(y) - Rb) / out; };
  double yLo = s0 - sCentre, yHi = s1 - sCentre;
  if (!straight) {
    const double a = rowAtRadius(Rb + s0 * out), b = rowAtRadius(Rb + s1 * out);
    yLo = std::min(a, b);
    yHi = std::max(a, b);
  }
  if (!grow) {
    const double Rnarrow = std::min(Rb + s0 * out, Rb + s1 * out);
    const double squeeze = Rnarrow / Rmid;  // how much closer the holes stand at the narrow end
    if (motif.period.x * (1 - squeeze) > v.num("web") * 0.75)
      note(warning, QStringLiteral("At the narrow end the holes nearly touch: make them smaller, the web wider, the taper less "
                                   "or let the holes grow with the width."));
  }

  std::vector<Manifold> cutters;
  const double depth = wall + 1;  // from 1 mm outside the outer surface to 1 mm past the inner one
  // Rows from the middle of the patterned part, so it sits evenly between the bands.
  const double yMid = (yLo + yHi) * 0.5;
  const int rowsDown = static_cast<int>(std::ceil((yMid - yLo) / motif.period.y)) + 2;
  const int rowsUp = static_cast<int>(std::ceil((yHi - yMid) / motif.period.y)) + 2;
  size_t count = 0;
  bool tooMany = false;
  for (int j = -rowsDown; j <= rowsUp && !tooMany; ++j)
    for (int i = 0; i < repeats && !tooMany; ++i)
      for (const auto& [shape2d, at] : motif.holes) {
        const double y = yMid + j * motif.period.y + at.y;
        const double sAt = sAtRow(y);
        const double scale = grow ? radiusAtRow(y) / Rmid : 1.0;
        const CrossSection h = scale == 1.0 ? shape2d : shape2d.Scale(vec2(scale, scale));
        const manifold::Rect box = h.Bounds();
        if (whole ? (sAt + box.min.y < s0 - 1e-9 || sAt + box.max.y > s1 + 1e-9) : (sAt + box.max.y <= s0 || sAt + box.min.y >= s1))
          continue;
        if (++count > 6000) {
          tooMany = true;
          break;
        }
        const double theta = (i * motif.period.x + at.x) * perMm;
        const double R = Rb + sAt * out, z = sAt * up;
        const vec3 radial(std::cos(theta), std::sin(theta), 0);
        const vec3 tangent(-std::sin(theta), std::cos(theta), 0);
        const vec3 along = radial * out + vec3(0, 0, up);    // up the wall
        const vec3 normal = radial * up - vec3(0, 0, out);   // out of the wall
        const vec3 at3 = radial * R + vec3(0, 0, z);
        // The hole's x round the tube, y up the wall, and straight through the wall.
        const manifold::mat3x4 place(tangent, along, normal, at3 - normal * depth);
        cutters.push_back(Manifold::Extrude(h.ToPolygons(), depth + 1).Transform(place));
      }
  if (tooMany) {
    note(warning, QStringLiteral("Too many holes; make them bigger."));
    cutters.clear();
  }
  Manifold holes = cutters.empty() ? Manifold() : Manifold::BatchBoolean(cutters, OpType::Add);
  if (!whole && !holes.IsEmpty()) {
    // Cut off at the bands.
    const double reach = std::max(Rb, Rt) + 5;
    holes = holes ^ Manifold::Cylinder(H - bandT - bandB, reach, reach, 16).Translate(vec3(0, 0, bandB));
  }
  if (holesOnly) {
    if (holes.IsEmpty()) {
      note(warning, QStringLiteral("No holes fit between the bands."));
      return {};
    }
    return {{QStringLiteral("Pattern holes"), holes}};
  }

  const int segs = std::max(circleSegments(std::max(Rb, Rt)), 96);
  // The wall measured square to its surface: a little wider across when tapered.
  const double across = wall / up;
  auto innerAt = [&](double z) { return Rb - across + (Rt - Rb) * z / H; };
  const double z0 = closed ? floor : -1, z1 = H + 1;
  if (innerAt(z0) < 0.5 || innerAt(z1) < 0.5) {
    note(warning, QStringLiteral("The wall is too thick for this diameter."));
    return {};
  }
  const Manifold outer = Manifold::Cylinder(H, Rb, Rt, segs);
  const Manifold inner = Manifold::Cylinder(z1 - z0, innerAt(z0), innerAt(z1), segs).Translate(vec3(0, 0, z0));
  Manifold tube = outer - inner;
  if (!holes.IsEmpty()) tube = tube - holes;
  else note(warning, QStringLiteral("No holes fit between the bands."));
  return {{QStringLiteral("Tube"), tube}};
}

// ---------------------------------------------------------------------------------------------
// Fit test

std::vector<Piece> buildFitTest(const Values& v, QString*) {
  const bool round = v.pick("shape") == QLatin1String("round");
  const double s = v.num("size");
  const int count = v.whole("count");
  const double first = v.num("first"), step = v.num("step");
  const double th = v.num("thickness");
  const double widest = s + 2 * (first + step * (count - 1));
  const double pitch = widest + 4;
  const double lengthX = count * pitch + 2;
  const double markRow = 3.0;
  const double widthY = widest + 6 + markRow;
  Manifold strip = prism(roundRectOutline(lengthX * 0.5, widthY * 0.5, 2)(0), 0, th);
  std::vector<Manifold> cuts;
  for (int i = 0; i < count; ++i) {
    const double clearance = first + step * i;
    const double x = -lengthX * 0.5 + 1 + pitch * (i + 0.5);
    const double y = markRow * 0.5;
    const double half = s * 0.5 + clearance;
    if (round) {
      cuts.push_back(cylinderZ(half, -1, th + 1, x, y));
    } else {
      cuts.push_back(prism(roundRect(half, half, 0.3 + clearance, 3), -1, th + 1).Translate(vec3(x, y, 0)));
    }
    // i + 1 dots under the hole.
    const double dotGap = std::min(1.8, (pitch - 1.4) / std::max(1, i));
    for (int d = 0; d <= i; ++d) {
      const double mx = x + (d - i * 0.5) * dotGap;
      cuts.push_back(cylinderZ(0.55, th - 0.6, th + 1, mx, -widthY * 0.5 + markRow * 0.5 + 1));
    }
  }
  strip = strip - Manifold::BatchBoolean(cuts, OpType::Add);
  const double pegH = th + 6;
  const double chamfer = 0.4;
  Manifold peg = round ? loft(circleOutline(s * 0.5), {{0, -chamfer}, {chamfer, 0}, {pegH, 0}})
                       : loft([&](double off) { return roundRect(s * 0.5 + off, s * 0.5 + off, 0.3 + off, 3); },
                              {{0, -chamfer}, {chamfer, 0}, {pegH, 0}});
  std::vector<Piece> pieces{{QStringLiteral("Fit strip"), strip}, {QStringLiteral("Peg"), peg}};
  sideBySide(pieces, 6);
  return pieces;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

const std::vector<Spec>& specs() {
  static const std::vector<Spec> all = makeSpecs();
  return all;
}

const Spec* spec(const QString& id) {
  for (const Spec& s : specs())
    if (s.id == id) return &s;
  return nullptr;
}

QStringList categories() {
  QStringList out;
  for (const Spec& s : specs())
    if (!out.contains(s.category)) out << s.category;
  // Standard parts next to the printed fasteners.
  const QString standard = QStringLiteral("Standard parts");
  const int fasteners = static_cast<int>(out.indexOf(QStringLiteral("Fasteners")));
  if (out.contains(standard) && fasteners >= 0) {
    out.removeAll(standard);
    out.insert(fasteners + 1, standard);
  }
  return out;
}

bool saveStl(const Manifold& solid, const QString& path, QString* error) {
  const manifold::MeshGL64 mesh = solid.GetMeshGL64();
  const size_t triangles = mesh.triVerts.size() / 3;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = file.errorString();
    return false;
  }
  QByteArray data(80, '\0');
  const char header[] = "DaedalCAD generator";
  std::memcpy(data.data(), header, sizeof(header) - 1);
  auto putU32 = [&data](uint32_t value) { data.append(reinterpret_cast<const char*>(&value), 4); };
  auto putF = [&data](double value) {
    const float f = static_cast<float>(value);
    data.append(reinterpret_cast<const char*>(&f), 4);
  };
  putU32(static_cast<uint32_t>(triangles));
  const size_t stride = mesh.numProp;
  for (size_t t = 0; t < triangles; ++t) {
    vec3 p[3];
    for (int k = 0; k < 3; ++k) {
      const size_t v = mesh.triVerts[t * 3 + k];
      p[k] = vec3(mesh.vertProperties[v * stride], mesh.vertProperties[v * stride + 1], mesh.vertProperties[v * stride + 2]);
    }
    vec3 n = manifold::la::cross(p[1] - p[0], p[2] - p[0]);
    const double len = manifold::la::length(n);
    if (len > 0) n = n / len;
    putF(n.x), putF(n.y), putF(n.z);
    for (const vec3& q : p) putF(q.x), putF(q.y), putF(q.z);
    data.append(2, '\0');
  }
  if (file.write(data) != data.size()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

QString displayName(const QString& id) {
  const Spec* s = spec(id);
  return s ? s->name : QStringLiteral("Generator");
}

QJsonObject normalized(const QString& id, const QJsonObject& values) {
  QJsonObject out;
  const Spec* s = spec(id);
  if (!s) return values;
  for (const Param& p : s->params) {
    const QJsonValue value = values.value(p.key);
    switch (p.type) {
      case Param::Heading:
        break;
      case Param::Number: {
        const double x = value.isDouble() ? value.toDouble() : p.def;
        out.insert(p.key, std::clamp(std::isfinite(x) ? x : p.def, p.min, p.max));
        break;
      }
      case Param::Integer: {
        const double x = value.isDouble() ? std::round(value.toDouble()) : p.def;
        out.insert(p.key, std::clamp(std::isfinite(x) ? x : p.def, p.min, p.max));
        break;
      }
      case Param::Toggle:
        out.insert(p.key, value.isBool() ? value.toBool() : (value.isDouble() ? value.toDouble() != 0 : p.def != 0));
        break;
      case Param::Text: {
        QString text = value.isString() ? value.toString() : p.defText;
        text = text.left(static_cast<int>(p.max)).simplified();
        out.insert(p.key, text);
        break;
      }
      case Param::Picture:
        out.insert(p.key, value.isString() ? value.toString() : QString());
        break;
      case Param::Choice: {
        QString pick = value.toString();
        bool known = false;
        for (const auto& option : p.choices) known = known || option.first == pick;
        out.insert(p.key, known ? pick : p.defChoice);
        break;
      }
    }
  }
  return out;
}

QJsonObject defaults(const QString& id) { return normalized(id, {}); }

bool visible(const Param& param, const QJsonObject& values) {
  for (const QString& condition : param.showIf.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
    const bool negate = condition.contains(QStringLiteral("!="));
    const QStringList parts = condition.split(negate ? QStringLiteral("!=") : QStringLiteral("="));
    if (parts.size() != 2) continue;
    const QJsonValue value = values.value(parts[0]);
    QString text;
    if (value.isBool()) text = value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    else if (value.isDouble()) text = QString::number(value.toDouble());
    else text = value.toString();
    const bool match = parts[1].split(QLatin1Char('|')).contains(text);
    if (negate == match) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Shared bits for the mechanical, workshop and calibration generators

namespace {

TextOutlines& textOutlinesHook() {
  static TextOutlines hook;
  return hook;
}

// A line of text as a flat shape centred on the origin (empty without a text engine).
CrossSection textShape(const QString& text, double capHeight) {
  if (!textOutlinesHook() || text.trimmed().isEmpty()) return {};
  try {
    return textOutlinesHook()(text, capHeight);
  } catch (...) {
    return {};
  }
}

bool haveText() { return static_cast<bool>(textOutlinesHook()); }

// A cylinder along +X from x0 to x1, its axis at (y, z).
Manifold barX(double r, double x0, double x1, double y, double z) {
  return Manifold::Cylinder(x1 - x0, r, r, circleSegments(r)).Rotate(0, 90, 0).Translate(vec3(x0, y, z));
}

// A cylinder along +Y from y0 to y1, its axis at (x, z).
Manifold barY(double r, double y0, double y1, double x, double z) {
  return Manifold::Cylinder(y1 - y0, r, r, circleSegments(r)).Rotate(-90, 0, 0).Translate(vec3(x, y0, z));
}

// A horizontal hole along Y with a 45° pointed roof, so it prints without support.
Manifold teardropY(double r, double y0, double y1, double x, double z) {
  const CrossSection round(circle(r, circleSegments(r)));
  const CrossSection tip(SimplePolygon{vec2(-0.01, r * std::sqrt(2.0)), vec2(0.01, r * std::sqrt(2.0)), vec2(0, 0)});
  const CrossSection drop = CrossSection::Hull({round, tip});
  return Manifold::Extrude(drop.ToPolygons(), y1 - y0).Rotate(90, 0, 0).Translate(vec3(x, y1, z));
}

// A flat profile drawn in X (across) and Z (up), made `width` deep along Y and centred on y = 0.
Manifold profileXZ(const CrossSection& profile, double width) {
  return Manifold::Extrude(profile.ToPolygons(), width).Rotate(90, 0, 0).Translate(vec3(0, width * 0.5, 0));
}

// Text cut `depth` into the top face at height `top`, centred on (x, y). Nothing without a text engine.
Manifold engraved(const QString& text, double capHeight, double x, double y, double top, double depth) {
  const CrossSection shape = textShape(text, capHeight);
  if (shape.IsEmpty()) return {};
  return Manifold::Extrude(shape.ToPolygons(), depth + 1).Translate(vec3(x, y, top - depth));
}

// `count` small dots in a row centred on (x, y), cut into the top face: a label without text.
Manifold dots(int count, double x, double y, double top, double gap = 1.8) {
  std::vector<Manifold> marks;
  for (int i = 0; i < count; ++i) marks.push_back(cylinderZ(0.55, top - 0.6, top + 1, x + (i - (count - 1) * 0.5) * gap, y));
  return marks.empty() ? Manifold() : Manifold::BatchBoolean(marks, OpType::Add);
}

QString mm(double value) {
  QString text = QString::number(value, 'f', 2);
  while (text.contains(QLatin1Char('.')) && (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) text.chop(1);
  return text;
}

// Stacks pieces front to back along Y (long strips stay side by side instead of in one long row).
void stackAlongY(std::vector<Piece>& pieces, double gap) {
  double y = 0;
  for (Piece& piece : pieces) {
    const auto box = piece.solid.BoundingBox();
    const double cx = (box.min.x + box.max.x) * 0.5;
    piece.solid = piece.solid.Translate(vec3(-cx, y - box.min.y, -box.min.z));
    y += (box.max.y - box.min.y) + gap;
  }
}

// An M3 nut pocket: a hex slot `thick` deep along X starting at x0, open along Z from z0 to z1.
Manifold nutSlotM3(double x0, double thick, double z0, double z1, double clearance) {
  const double half = 5.5 * 0.5 + clearance;
  return block(x0, x0 + thick + clearance, -half, half, z0, z1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Pulley

std::vector<Piece> buildPulley(const Values& v, QString* warning) {
  const QString type = v.pick("type");
  const double belt = v.num("beltWidth");
  const double c = v.num("clearance");
  const bool gt2 = type == QLatin1String("gt2");
  const bool vbelt = type == QLatin1String("vbelt");
  const bool hub = v.on("hub");
  const double hubR = v.num("hubDiameter") * 0.5;
  const double hubL = hub ? v.num("hubLength") : 0;
  double R;
  CrossSection body;
  if (gt2) {
    // GT2: 2 mm pitch; the belt's pitch line sits 0.254 mm outside the pulley.
    const int teeth = v.whole("teeth");
    R = teeth * 2.0 / kPi * 0.5 - 0.254;
    const double rg = 0.555;
    const CrossSection gap = CrossSection::Hull({CrossSection(circle(rg, 24)).Translate(vec2(R - 0.75 + rg, 0)),
                                                 CrossSection::Square(vec2(1.0, rg * 2.3), true).Translate(vec2(R + 0.4, 0))});
    std::vector<CrossSection> gaps;
    for (int i = 0; i < teeth; ++i) gaps.push_back(gap.Rotate(360.0 * i / teeth));
    body = CrossSection(circle(R, std::max(teeth * 12, 96))) - CrossSection::BatchBoolean(gaps, OpType::Add);
  } else {
    R = v.num("diameter") * 0.5;
  }
  const bool flanges = !vbelt && v.on("flanges");
  const bool hasHub = hub && !vbelt;
  // As wide as the hub at least, so the hub on top never hangs over the flange.
  const double flangeR = std::max(R + (gt2 ? 1.5 : 2.0), hasHub ? hubR : 0.0);
  const double flange = flanges ? 1.0 : 0;
  const double rim = vbelt ? 1.5 : 0.5;
  const double bodyH = belt + 2 * rim;
  // From the bed up: the flat flange, the belt surface, the top flange leaning out at 45° (no
  // support needed), then the hub, which is narrower than everything under it.
  const double z0 = flange;
  const double z1 = z0 + bodyH;
  const double flangeTop = flanges ? z1 + (flangeR - R) : z1;
  const double hubTop = flangeTop + (hasHub ? hubL : 0);
  std::vector<Manifold> parts;
  if (flanges) parts.push_back(cylinderZ(flangeR, 0, z0 + 0.01));
  if (gt2) {
    parts.push_back(Manifold::Extrude(body.ToPolygons(), bodyH).Translate(vec3(0, 0, z0)));
  } else if (vbelt) {
    // The groove: 40° between its flanks, as V-belts are made.
    const double depth = std::min(v.num("grooveDepth"), R * 0.6);
    const double bottom = std::max(0.6, belt - 2 * depth * std::tan(20 * kPi / 180));
    const double zc = bodyH * 0.5;
    const SimplePolygon profile{vec2(0, 0), vec2(R, 0), vec2(R, zc - belt * 0.5), vec2(R - depth, zc - bottom * 0.5),
                                vec2(R - depth, zc + bottom * 0.5), vec2(R, zc + belt * 0.5), vec2(R, bodyH), vec2(0, bodyH)};
    parts.push_back(Manifold::Revolve(Polygons{profile}, circleSegments(R)).Translate(vec3(0, 0, z0)));
  } else {
    parts.push_back(cylinderZ(R, z0, z1));
  }
  // The top flange's cone starts a little inside the body: a ring of exactly radius R at z1 - 0.01
  // put its corners on the body's side, a rounding error from the body's own, which breaks CAD
  // exports. Started 0.05 in and 0.03 down on the same 45° line, it leaves the belt surface
  // 0.02 mm above z1.
  if (flanges) parts.push_back(coneZ(R - 0.05, flangeR, z1 - 0.03, flangeTop));
  if (hasHub) parts.push_back(chamferedDisc(hubR, flangeTop - 0.01, hubTop, 0.5, 0.5, false, true));
  Manifold pulley = Manifold::BatchBoolean(parts, OpType::Add);
  // Bore, round or with a flat for a D shaft.
  const double boreR = v.num("bore") * 0.5 + c;
  if (boreR > 0.4) {
    CrossSection bore(circle(boreR, circleSegments(boreR)));
    if (v.pick("boreShape") == QLatin1String("d")) {
      const double flat = boreR - std::max(0.3, v.num("bore") * 0.1);
      bore = bore ^ CrossSection::Square(vec2(flat + boreR, 2 * boreR + 2), true).Translate(vec2((flat - boreR) * 0.5, 0));
    }
    pulley = pulley - prism(bore, -1, hubTop + 1);
  }
  if (boreR + 0.8 > std::min(R - (gt2 ? 0.75 : vbelt ? v.num("grooveDepth") : 0), hasHub ? hubR : R) - 0.8)
    note(warning, QStringLiteral("The bore leaves almost no wall; use a smaller bore or a bigger pulley."));
  const QString screw = v.pick("setScrew");
  if (hasHub && screw != QLatin1String("none")) {
    const double z = flangeTop + hubL * 0.5;
    const double holeR = screw == QLatin1String("tap") ? 1.25 : screw == QLatin1String("insert") ? 2.0 : 1.65;
    pulley = pulley - barX(holeR, 0, hubR + 1, 0, z);
    if (screw == QLatin1String("nut")) {
      const double x0 = std::max(boreR + 1.0, (boreR + hubR) * 0.5 - 1.5);
      if (x0 + 2.6 + c > hubR - 0.8) note(warning, QStringLiteral("The hub is too small for a nut pocket; make it wider."));
      pulley = pulley - nutSlotM3(x0, 2.6, z - 3.3, hubTop + 1, c);
    }
  }
  if (vbelt) {
    // A V groove's upper flank is too flat to print: two halves, split in the groove, each printed
    // with its outer face down so both flanks narrow going up. Glue them, or pin them on the shaft.
    const double cut = z0 + bodyH * 0.5;
    const Manifold lower = pulley.TrimByPlane(vec3(0, 0, -1), -cut);
    const Manifold upper = pulley.TrimByPlane(vec3(0, 0, 1), cut).Mirror(vec3(0, 0, 1));
    std::vector<Piece> halves{{QStringLiteral("V-belt pulley, half 1"), lower}, {QStringLiteral("V-belt pulley, half 2"), upper}};
    sideBySide(halves, 6);
    return halves;
  }
  return {{gt2 ? QStringLiteral("GT2 pulley") : vbelt ? QStringLiteral("V-belt pulley") : QStringLiteral("Pulley"), pulley}};
}

// ---------------------------------------------------------------------------------------------
// Bearing housing

struct BearingSize {
  double id, od, width;
};

BearingSize bearingSize(const Values& v) {
  static const std::map<QString, BearingSize> table = {
      {"623", {3, 10, 4}},     {"625", {5, 16, 5}},     {"688", {8, 16, 5}},     {"608", {8, 22, 7}},
      {"6000", {10, 26, 8}},   {"6001", {12, 28, 8}},   {"6002", {15, 32, 9}},   {"6201", {12, 32, 10}},
      {"6202", {15, 35, 11}},  {"6204", {20, 47, 14}}};
  const auto it = table.find(v.pick("bearing"));
  if (it != table.end()) return it->second;
  return {v.num("innerDiameter"), std::max(v.num("outerDiameter"), v.num("innerDiameter") + 2), v.num("bearingWidth")};
}

std::vector<Piece> buildBearing(const Values& v, QString*) {
  const BearingSize b = bearingSize(v);
  const double c = v.num("clearance");
  const double wall = v.num("wall");
  const double seatR = b.od * 0.5 + c;
  const double lip = v.on("lip") ? 1.0 : 0;
  // The hole through the lip clears the inner ring, which turns with the shaft.
  const double throughR = std::min(seatR - 0.8, b.id * 0.5 + (b.od - b.id) * 0.22);
  const double length = b.width + lip;
  const ScrewHole sh = screwHole(v.pick("screw"));
  const QString style = v.pick("style");
  Manifold housing;
  if (style == QLatin1String("pillow")) {
    // Shaft along Y, the block standing on its base with two screw slots.
    const double R = seatR + wall;
    const double base = std::max(4.0, wall + 1);
    const double reach = R + 2 * wall + sh.clear;
    Manifold body = Manifold::Hull({barY(R, 0, length, 0, R), block(-R, R, 0, length, 0, base)});
    body = body + block(-reach, reach, 0, length, 0, base);
    const double slotX = R + wall + sh.clear * 0.5;
    for (double side : {-1.0, 1.0}) {
      const Manifold slot = Manifold::Hull({cylinderZ(sh.clear * 0.5, -1, base + 1, side * (slotX - 1), length * 0.5),
                                            cylinderZ(sh.clear * 0.5, -1, base + 1, side * (slotX + 1), length * 0.5)});
      body = body - slot;
    }
    body = body - teardropY(seatR, lip, length + 1, 0, R) - teardropY(throughR, -1, lip + 0.01, 0, R);
    housing = body;
  } else {
    const double R = seatR + wall;
    const double holeAt = R + sh.clear * 0.5 + 0.6;
    const double plate = std::max(3.0, std::min(length, wall + 1.5));
    Manifold body = chamferedDisc(R, 0, length, 0.4, 0.4, false, true);
    std::vector<std::pair<double, double>> holes;
    if (style == QLatin1String("square")) {
      const double a = holeAt / std::sqrt(2.0) + 0.5;
      const double half = a + sh.clear * 0.5 + wall;
      body = body + prism(roundRect(half, half, std::min(3.0, half * 0.3), 6), 0, plate);
      holes = {{a, a}, {-a, a}, {-a, -a}, {a, -a}};
    } else {
      body = body + cylinderZ(holeAt + sh.clear * 0.5 + wall, 0, plate);
      for (int i = 0; i < 3; ++i) {
        const double ang = 2 * kPi * i / 3 + kPi / 2;
        holes.push_back({holeAt * std::cos(ang), holeAt * std::sin(ang)});
      }
    }
    for (const auto& [x, y] : holes) body = body - cylinderZ(sh.clear * 0.5, -1, plate + 1, x, y);
    body = body - cylinderZ(seatR, lip, length + 1) - cylinderZ(throughR, -1, lip + 0.01);
    // A small chamfer leads the bearing into its seat.
    body = body - coneZ(seatR, seatR + 0.5, length - 0.5, length + 0.01);
    housing = body;
  }
  return {{QStringLiteral("Bearing housing"), housing}};
}

// ---------------------------------------------------------------------------------------------
// Shaft coupler and collar

std::vector<Piece> buildCoupler(const Values& v, QString* warning) {
  const bool collar = v.pick("type") == QLatin1String("collar");
  const double c = v.num("clearance");
  const double b1 = v.num("bore1") * 0.5 + c;
  const double b2 = collar ? b1 : v.num("bore2") * 0.5 + c;
  const double length = collar ? std::max(8.0, v.num("width")) : v.num("length");
  const QString screw = v.pick("setScrew");
  const bool nut = screw == QLatin1String("nut");
  const double widest = std::max(b1, b2);
  double R = std::max(v.num("outerDiameter") * 0.5, widest + v.num("wall"));
  if (nut && R < widest + 1.2 + 2.6 + c + 1.2) {
    R = widest + 1.2 + 2.6 + c + 1.2;
    note(warning, QStringLiteral("Made wider to fit the nut pockets."));
  }
  Manifold part = chamferedDisc(R, 0, length, 0.6);
  if (collar || std::abs(b1 - b2) < 1e-6) {
    part = part - cylinderZ(b1, -1, length + 1);
  } else {
    part = part - cylinderZ(b1, -1, length * 0.5 + 0.01) - cylinderZ(b2, length * 0.5, length + 1);
  }
  const double holeR = screw == QLatin1String("tap") ? 1.25 : screw == QLatin1String("insert") ? 2.0 : 1.65;
  const int perSide = v.whole("screws");
  std::vector<std::pair<double, double>> levels;  // (height, bore radius)
  if (collar) levels = {{length * 0.5, b1}};
  else levels = {{length * 0.25, b1}, {length * 0.75, b2}};
  for (const auto& [z, boreR] : levels) {
    for (int k = 0; k < perSide; ++k) {
      Manifold cut = barX(holeR, 0, R + 1, 0, z);
      if (nut) {
        const double x0 = boreR + 1.2;
        // The pocket opens toward the nearer end face, so the nut drops in from there.
        const bool fromTop = z > length * 0.5 || collar;
        cut = cut + (fromTop ? nutSlotM3(x0, 2.6, z - 3.3, length + 1, c) : nutSlotM3(x0, 2.6, -1, z + 3.3, c));
      }
      part = part - cut.Rotate(0, 0, 90.0 * k);
    }
  }
  return {{collar ? QStringLiteral("Shaft collar") : QStringLiteral("Shaft coupler"), part}};
}

// ---------------------------------------------------------------------------------------------
// Springs

std::vector<Piece> buildSpring(const Values& v, QString* warning) {
  if (v.pick("type") == QLatin1String("flat")) {
    // A zig-zag strip: straight runs across, joined by U-turns at alternate ends.
    const double L = v.num("length"), W = v.num("width"), t = v.num("strip"), h = v.num("height");
    const int folds = v.whole("folds");
    double s = L / folds;
    if (s < t + 0.8) {
      s = t + 0.8;
      note(warning, QStringLiteral("Too many folds for this length; the runs are spaced out."));
    }
    const double r = s * 0.5;
    const double reach = std::max(0.5, W * 0.5 - r - t * 0.5);
    std::vector<CrossSection> bits;
    const double x0 = -s * folds * 0.5;
    for (int i = 0; i <= folds; ++i)
      bits.push_back(CrossSection::Square(vec2(t, 2 * reach), true).Translate(vec2(x0 + i * s, 0)));
    const int seg = circleSegments(r + t);
    for (int i = 0; i < folds; ++i) {
      const bool upper = i % 2 == 0;
      const CrossSection ring = CrossSection(circle(r + t * 0.5, seg)) - CrossSection(circle(std::max(0.05, r - t * 0.5), seg));
      const CrossSection half = ring ^ CrossSection::Square(vec2(2 * (r + t), r + t)).Translate(vec2(-(r + t), upper ? 0 : -(r + t)));
      bits.push_back(half.Translate(vec2(x0 + i * s + r, upper ? reach : -reach)));
    }
    // Eyes at both ends to fix it with a screw or a pin.
    const double eyeR = std::max(3.0, t + 1.8);
    for (double x : {x0, x0 + folds * s}) {
      // The free end of the first run is at the bottom; the last run's depends on the number of folds.
      const bool last = x != x0;
      const double y = (!last || folds % 2 == 1) ? -reach - eyeR + 0.5 : reach + eyeR - 0.5;
      bits.push_back(CrossSection(circle(eyeR, 32)).Translate(vec2(x, y)) - CrossSection(circle(1.7, 24)).Translate(vec2(x, y)));
    }
    // Simplify: where a run meets its turn the union leaves pairs of corners a rounding error
    // apart, which break CAD exports.
    const CrossSection shape = CrossSection::BatchBoolean(bits, OpType::Add).Simplify(1e-5);
    return {{QStringLiteral("Flat spring"), Manifold::Extrude(shape.ToPolygons(), h)}};
  }
  // A coil of square wire: a twisted extrusion of an annulus sector. The sector's angle sets how
  // thick each turn is; the rest of the pitch is the gap between turns.
  const double R = v.num("diameter") * 0.5;
  const double w = std::min(v.num("wire"), R - 1);
  const double t = v.num("coil");
  const double L = v.num("length");
  const int turns = v.whole("turns");
  const double end = v.on("ends") ? t : 0;
  const double active = L - 2 * end;
  double pitch = active / turns;
  if (pitch < t + 0.4) {
    note(warning, QStringLiteral("The turns would touch: make it longer, use fewer turns or a thinner coil."));
    return {};
  }
  const double sweep = 360.0 * t / pitch;
  const int seg = std::max(8, static_cast<int>(std::ceil(circleSegments(R) * sweep / 360.0)));
  SimplePolygon sector;
  for (int i = 0; i <= seg; ++i) {
    const double a = sweep * i / seg * kPi / 180;
    sector.push_back(vec2(R * std::cos(a), R * std::sin(a)));
  }
  for (int i = seg; i >= 0; --i) {
    const double a = sweep * i / seg * kPi / 180;
    sector.push_back(vec2((R - w) * std::cos(a), (R - w) * std::sin(a)));
  }
  Manifold coil = Manifold::Extrude(Polygons{sector}, active, turns * 72, 360.0 * turns).Translate(vec3(0, 0, end));
  if (end > 0) {
    const Manifold ring = cylinderZ(R, 0, end) - cylinderZ(R - w, -1, end + 1);
    coil = coil + ring + ring.Translate(vec3(0, 0, L - end));
  }
  return {{QStringLiteral("Spring"), coil}};
}

// ---------------------------------------------------------------------------------------------
// Snap-fit

std::vector<Piece> buildSnapFit(const Values& v, QString* warning) {
  const double t = v.num("beamThickness"), bw = v.num("beamWidth"), hd = v.num("hookDepth");
  const double wallT = v.num("wallThickness"), c = v.num("clearance");
  const double hl = hd / std::tan(30 * kPi / 180);  // the lead-in ramp, 30° to the beam
  double Lb = v.num("beamLength");
  const double need = hl + c + wallT + 3;
  if (Lb < need) {
    Lb = need;
    note(warning, QStringLiteral("The beam was made longer to reach through the wall."));
  }
  // Latch: an anchor block, the beam along X and the hook on its end, pointing up.
  const double anchor = 6;
  Manifold latch = block(-anchor, 0.01, -bw * 0.5 - 2, bw * 0.5 + 2, 0, t + 3) + block(0, Lb, -bw * 0.5, bw * 0.5, 0, t);
  const CrossSection hook(SimplePolygon{vec2(Lb - hl, t - 0.01), vec2(Lb, t - 0.01), vec2(Lb - hl, t + hd)});
  latch = latch + profileXZ(hook, bw);
  // Catch: a wall with a window the beam passes through; the hook snaps behind it.
  const double xw = Lb - hl - c - wallT;
  const double low = -hd - c - 3, high = t + hd + 3;
  Manifold wall = block(xw, xw + wallT, -bw * 0.5 - 4, bw * 0.5 + 4, low, high) -
                  block(xw - 1, xw + wallT + 1, -bw * 0.5 - c, bw * 0.5 + c, -hd - c, t + c);
  std::vector<Piece> pieces{{QStringLiteral("Latch"), latch}, {QStringLiteral("Catch"), wall}};
  if (v.pick("layout") == QLatin1String("assembled")) return pieces;
  sideBySide(pieces, 6);
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Living hinge

std::vector<Piece> buildLivingHinge(const Values& v, QString*) {
  const double L = v.num("plateLength"), W = v.num("width"), T = v.num("thickness");
  const double h = std::min(v.num("hingeThickness"), T), gap = v.num("gap");
  const Manifold a = block(-gap * 0.5 - L, -gap * 0.5, -W * 0.5, W * 0.5, 0, T);
  const Manifold b = block(gap * 0.5, gap * 0.5 + L, -W * 0.5, W * 0.5, 0, T);
  const Manifold web = block(-gap * 0.5 - 0.01, gap * 0.5 + 0.01, -W * 0.5, W * 0.5, 0, h);
  return {{QStringLiteral("Living hinge"), a + b + web}};
}

// ---------------------------------------------------------------------------------------------
// Cable chain

// One link, its rear pivot at x = 0, its front pivot at x = pitch, both at half height.
// Across (y, one side): tongue layer [half, half + t], a gap of c, then the outer wall.
Manifold chainLink(double pitch, double H, double half, double t, double floorT, double c, double pinR, bool topBar) {
  const double Rj = H * 0.5;
  const int seg = circleSegments(Rj);
  auto side = [&](double y0, double y1, const CrossSection& shape) {
    return Manifold::Extrude(shape.ToPolygons(), y1 - y0).Rotate(90, 0, 0).Translate(vec3(0, y1, 0));
  };
  const CrossSection rearDisk = CrossSection(circle(Rj, seg)).Translate(vec2(0, Rj));
  const CrossSection frontDisk = CrossSection(circle(Rj, seg)).Translate(vec2(pitch, Rj));
  const double bodyFrom = Rj + c, bodyTo = pitch - Rj - c;
  const CrossSection outer = rearDisk + CrossSection::Square(vec2(bodyTo, H)).Translate(vec2(0, 0));
  const CrossSection tongue = frontDisk + CrossSection::Square(vec2(pitch - bodyFrom, H)).Translate(vec2(bodyFrom, 0));
  // The filler stops 0.01 short of where the tongue starts and the outer wall ends: flush with
  // them, the union left corners a rounding error from theirs, which breaks CAD exports.
  const CrossSection filler = CrossSection::Square(vec2(bodyTo - bodyFrom - 0.02, H)).Translate(vec2(bodyFrom + 0.01, 0));
  const double yTongue = half, yGap = half + t, yOuter = half + t + c, yEdge = half + 2 * t + c;
  Manifold one = side(yTongue, yGap, tongue) + side(yGap - 0.01, yOuter + 0.01, filler) + side(yOuter, yEdge, outer);
  // The pin on the tongue reaches out through the gap into the next link's outer wall.
  one = one + barY(pinR, yGap - 0.01, yEdge, pitch, Rj);
  one = one - teardropY(pinR + c, yOuter - 0.01, yEdge + 0.01, 0, Rj);
  Manifold link = one + one.Mirror(vec3(0, 1, 0));
  // The floor and the top bars stop 0.01 inside the walls' ends and outsides: flush with them, the
  // union left corners a rounding error from the walls' own, which breaks CAD exports.
  const double e = 0.01;
  link = link + block(bodyFrom + e, bodyTo - e, -yEdge + e, yEdge - e, 0, floorT);
  if (topBar) link = link + block(bodyFrom + e, bodyTo - e, -yEdge + e, yEdge - e, H - t, H);
  return link;
}

std::vector<Piece> buildCableChain(const Values& v, QString* warning) {
  const double t = v.num("wall"), floorT = v.num("floor"), c = v.num("clearance");
  const bool top = v.on("topBars");
  const double H = floorT + v.num("innerHeight") + (top ? t : 0);
  const double Rj = H * 0.5;
  double pitch = v.num("pitch");
  if (pitch < 2 * (Rj + c) + 3) {
    pitch = 2 * (Rj + c) + 3;
    note(warning, QStringLiteral("The links were made longer so they can turn (at least %1 mm for this height).").arg(mm(pitch)));
  }
  const double pinR = std::clamp(Rj / std::sqrt(2.0) - c - 0.6, 1.0, 3.0);
  const Manifold link = chainLink(pitch, H, v.num("innerWidth") * 0.5, t, floorT, c, pinR, top);
  std::vector<Manifold> links;
  for (int i = 0; i < v.whole("links"); ++i) links.push_back(link.Translate(vec3(i * pitch, 0, 0)));
  return {{QStringLiteral("Cable chain"), Manifold::BatchBoolean(links, OpType::Add)}};
}

// ---------------------------------------------------------------------------------------------
// Tube connector

std::vector<Piece> buildTubeConnector(const Values& v, QString*) {
  const bool round = v.pick("shape") == QLatin1String("round");
  const bool socket = v.pick("fit") == QLatin1String("socket");
  const double size = v.num("size"), c = v.num("clearance"), wall = v.num("wall"), depth = v.num("depth");
  static const std::map<QString, std::vector<int>> layouts = {
      // 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
      {"straight", {0, 1}}, {"corner", {0, 2}}, {"tee", {0, 1, 2}}, {"corner3", {0, 2, 4}},
      {"cross", {0, 1, 2, 3}}, {"tee4", {0, 1, 2, 4}}, {"five", {0, 1, 2, 3, 4}}, {"six", {0, 1, 2, 3, 4, 5}}};
  const auto found = layouts.find(v.pick("layout"));
  const std::vector<int> arms = found != layouts.end() ? found->second : std::vector<int>{0, 1};
  const double hub = socket ? size * 0.5 + c + wall : size * 0.5;
  Manifold part = Manifold::Cube(vec3(2 * hub, 2 * hub, 2 * hub), true);
  // Arms are made along +Z and turned into place.
  Manifold arm, cavity;
  if (socket) {
    const double outer = size * 0.5 + c + wall, inner = size * 0.5 + c;
    arm = round ? cylinderZ(outer, hub - 0.01, hub + depth) : block(-outer, outer, -outer, outer, hub - 0.01, hub + depth);
    cavity = round ? cylinderZ(inner, hub, hub + depth + 1) : block(-inner, inner, -inner, inner, hub, hub + depth + 1);
    // A lead-in chamfer at the mouth.
    cavity = cavity + (round ? coneZ(inner, inner + 0.8, hub + depth - 0.8, hub + depth + 0.01)
                             : loft([&](double off) { return roundRect(inner + off, inner + off, 0.2 + off, 2); },
                                    {{hub + depth - 0.8, 0}, {hub + depth + 0.01, 0.8}}));
  } else {
    const double plug = size * 0.5 - v.num("tubeWall") - c;
    arm = round ? loft(circleOutline(plug), {{hub - 0.01, 0}, {hub + depth - 1, 0}, {hub + depth, -0.8}})
                : loft([&](double off) { return roundRect(plug + off, plug + off, 0.6 + off, 4); },
                       {{hub - 0.01, 0}, {hub + depth - 1, 0}, {hub + depth, -0.8}});
  }
  auto turned = [](const Manifold& m, int dir) {
    switch (dir) {
      case 0: return m.Rotate(0, 90, 0);
      case 1: return m.Rotate(0, -90, 0);
      case 2: return m.Rotate(-90, 0, 0);
      case 3: return m.Rotate(90, 0, 0);
      case 5: return m.Rotate(180, 0, 0);
      default: return m;
    }
  };
  std::vector<Manifold> adds{part}, cuts;
  for (int dir : arms) {
    adds.push_back(turned(arm, dir));
    if (socket) cuts.push_back(turned(cavity, dir));
  }
  part = Manifold::BatchBoolean(adds, OpType::Add);
  if (!cuts.empty()) part = part - Manifold::BatchBoolean(cuts, OpType::Add);
  return {{QStringLiteral("Tube connector"), part}};
}

// ---------------------------------------------------------------------------------------------
// Bit and tool holder

std::vector<Piece> buildToolHolder(const Values& v, QString*) {
  const QString type = v.pick("type");
  const double c = v.num("clearance"), spacing = v.num("spacing"), base = v.num("base");
  double depth = v.num("depth");
  std::vector<double> sizes;  // per hole: the drill size (drill index), else one entry per hole
  int columns = v.whole("columns");
  int rows = v.whole("rows");
  const bool drill = type == QLatin1String("drill");
  if (drill) {
    const double from = v.num("from"), to = std::max(v.num("from"), v.num("to")), step = v.num("step");
    for (double d = from; d <= to + 1e-6 && sizes.size() < 200; d += step) sizes.push_back(d);
    rows = static_cast<int>((sizes.size() + columns - 1) / columns);
  } else {
    sizes.assign(static_cast<size_t>(rows * columns), 0.0);
  }
  // The hole's footprint (half sizes) and its outline.
  auto footprint = [&](double d) -> std::pair<vec2, CrossSection> {
    if (type == QLatin1String("hex")) {
      const double flats = 6.35 + 2 * c;  // 1/4" hex bits
      return {vec2(flats / std::sqrt(3.0), flats * 0.5), CrossSection(regular(flats / std::sqrt(3.0), 6, 0))};
    }
    // Cards stand side by side, their long edge front to back.
    if (type == QLatin1String("sd")) return {vec2(1.2 + c, 12.1 + c), CrossSection::Square(vec2(2.4 + 2 * c, 24.2 + 2 * c), true)};
    if (type == QLatin1String("microsd"))
      return {vec2(0.7 + c, 5.6 + c), CrossSection::Square(vec2(1.4 + 2 * c, 11.2 + 2 * c), true)};
    const double r = (drill ? d : v.num("diameter")) * 0.5 + c;
    return {vec2(r, r), CrossSection(circle(r, circleSegments(r)))};
  };
  double cellX = 0, cellY = 0;
  for (double d : sizes) {
    const vec2 half = footprint(d).first;
    cellX = std::max(cellX, 2 * half.x + spacing);
    cellY = std::max(cellY, 2 * half.y + spacing);
  }
  const bool labels = drill && v.on("labels");
  const double labelRow = labels ? 5.0 : 0;
  cellY += labelRow;
  const double lengthX = columns * cellX + spacing, widthY = rows * cellY + spacing;
  const double topZ = base + depth;
  Manifold holder = prism(roundRect(lengthX * 0.5, widthY * 0.5, std::min(3.0, std::min(lengthX, widthY) * 0.25), 6), 0, topZ);
  std::vector<Manifold> cuts;
  for (size_t i = 0; i < sizes.size(); ++i) {
    const int col = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
    const double x = -lengthX * 0.5 + spacing * 0.5 + cellX * (col + 0.5);
    const double y = widthY * 0.5 - spacing * 0.5 - cellY * (row + 0.5) + labelRow * 0.5;
    cuts.push_back(prism(footprint(sizes[i]).second, base, topZ + 1).Translate(vec3(x, y, 0)));
    if (labels) {
      const Manifold text = engraved(mm(sizes[i]), 2.6, x, y - cellY * 0.5 + labelRow * 0.5, topZ, 0.6);
      if (!text.IsEmpty()) cuts.push_back(text);
    }
  }
  holder = holder - Manifold::BatchBoolean(cuts, OpType::Add);
  return {{drill ? QStringLiteral("Drill index") : QStringLiteral("Tool holder"), holder}};
}

// ---------------------------------------------------------------------------------------------
// Drawer dividers

std::vector<Piece> buildDividers(const Values& v, QString* warning) {
  const double L = v.num("length"), W = v.num("width"), H = v.num("height");
  const double t = v.num("thickness"), c = v.num("clearance"), gap = v.num("gap");
  const int cols = v.whole("columns"), rows = v.whole("rows");
  const double slot = t + 2 * c;
  std::vector<double> xs, ys;  // where the strips stand
  for (int i = 1; i <= cols; ++i) xs.push_back(-L * 0.5 + L * i / (cols + 1));
  for (int j = 1; j <= rows; ++j) ys.push_back(-W * 0.5 + W * j / (rows + 1));
  const double lx = L - 2 * gap, ly = W - 2 * gap;
  std::vector<Piece> pieces;
  // Strips along X (one per row) get their slots from below, strips along Y from above.
  for (size_t j = 0; j < ys.size(); ++j) {
    Manifold strip = block(-lx * 0.5, lx * 0.5, ys[j] - t * 0.5, ys[j] + t * 0.5, 0, H);
    for (double x : xs) strip = strip - block(x - slot * 0.5, x + slot * 0.5, ys[j] - t, ys[j] + t, -1, H * 0.5 + c * 0.5);
    pieces.push_back({QStringLiteral("Strip along X %1").arg(j + 1), strip});
  }
  for (size_t i = 0; i < xs.size(); ++i) {
    Manifold strip = block(xs[i] - t * 0.5, xs[i] + t * 0.5, -ly * 0.5, ly * 0.5, 0, H);
    for (double y : ys) strip = strip - block(xs[i] - t, xs[i] + t, y - slot * 0.5, y + slot * 0.5, H * 0.5 - c * 0.5, H + 1);
    pieces.push_back({QStringLiteral("Strip along Y %1").arg(i + 1), strip});
  }
  if (pieces.empty()) {
    note(warning, QStringLiteral("Set at least one divider."));
    return {};
  }
  if (std::max(xs.empty() ? 0.0 : ly, ys.empty() ? 0.0 : lx) > 250)
    note(warning, QStringLiteral("Some strips are longer than most print beds (250 mm); print them diagonally or in two parts."));
  if (v.pick("layout") == QLatin1String("assembled")) return pieces;
  // Lay every strip flat, its thickness up, one behind the other.
  for (Piece& piece : pieces) {
    const auto box = piece.solid.BoundingBox();
    const bool alongX = (box.max.x - box.min.x) > (box.max.y - box.min.y);
    piece.solid = alongX ? piece.solid.Rotate(-90, 0, 0) : piece.solid.Rotate(0, 90, 0).Rotate(0, 0, 90);
  }
  stackAlongY(pieces, 4);
  return pieces;
}

// ---------------------------------------------------------------------------------------------
// Cable management

std::vector<Piece> buildCable(const Values& v, QString* warning) {
  const QString type = v.pick("type");
  const double d = v.num("cable"), c = v.num("clearance"), t = v.num("wall"), w = v.num("width");
  if (type == QLatin1String("grommet")) {
    const double OD = v.num("hole") - c, desk = v.num("desk"), flange = v.num("flange");
    const double ID = OD - 2 * std::max(t, 1.6);
    if (ID < 6) {
      note(warning, QStringLiteral("The hole is too small for a grommet with this wall."));
      return {};
    }
    // Printed upside down: flange on the bed, the sleeve standing up.
    const Manifold sleeve = (cylinderZ(OD * 0.5 + flange, 0, 2) + cylinderZ(OD * 0.5, 1.99, 2 + desk)) - cylinderZ(ID * 0.5, -1, desk + 3);
    // The cap: a disc on the bed, a ring that drops into the sleeve, and a slot for the cables.
    const double plugR = ID * 0.5 - c;
    Manifold cap = cylinderZ(OD * 0.5 + flange, 0, 2) + (cylinderZ(plugR, 1.99, 6) - cylinderZ(plugR - 1.6, 1.9, 7));
    const double slot = std::min(v.num("slot"), ID * 0.8);
    cap = cap - block(-slot * 0.5, slot * 0.5, 0, OD, -1, 8);
    std::vector<Piece> pieces{{QStringLiteral("Grommet"), sleeve}, {QStringLiteral("Grommet cap"), cap}};
    sideBySide(pieces, 6);
    return pieces;
  }
  const double r = d * 0.5 + c * 0.5;
  if (type == QLatin1String("clip")) {
    // A saddle over the cables with two screw tabs; drawn across (X, up Z), `w` deep along Y.
    const int n = v.whole("cables");
    const double cw = n * d + 2 * c, ch = d + c;
    const ScrewHole sh = screwHole(v.pick("screw"));
    const double tab = sh.clear + 2 * t + 1;
    const CrossSection arch = CrossSection(roundRect(cw * 0.5 + t, (ch + t) * 0.5, std::min(r + t, (ch + t) * 0.5 - 0.05), 8))
                                  .Translate(vec2(0, (ch + t) * 0.5));
    const CrossSection channel = CrossSection(roundRect(cw * 0.5, ch * 0.5 + 0.5, std::min(r, ch * 0.5), 8)).Translate(vec2(0, ch * 0.5 - 0.5));
    const CrossSection tabs = CrossSection::Square(vec2(cw + 2 * t + 2 * tab, t)).Translate(vec2(-(cw * 0.5 + t + tab), 0));
    Manifold clip = profileXZ((arch + tabs) - channel, w);
    const double hx = cw * 0.5 + t + tab * 0.5;
    for (double x : {-hx, hx}) clip = clip - cylinderZ(sh.clear * 0.5, -1, t + 1, x, 0) - coneZ(sh.clear * 0.5, sh.clear, t - sh.clear * 0.5, t + 0.01, x, 0);
    return {{QStringLiteral("Cable clip"), clip}};
  }
  // A C-shaped ring that snaps around the cable, opening at the top.
  const double ro = r + t;
  const double open = d * 0.7;
  const CrossSection ring = CrossSection(circle(ro, circleSegments(ro))) - CrossSection(circle(r, circleSegments(r)));
  const CrossSection gapCut = CrossSection::Square(vec2(open, ro + 1)).Translate(vec2(-open * 0.5, 0));
  if (type == QLatin1String("snap")) {
    const CrossSection pad = CrossSection::Square(vec2(2 * ro + 4, t)).Translate(vec2(-ro - 2, -ro - t + 0.5));
    const CrossSection shape = (ring - gapCut) + pad;
    Manifold snap = profileXZ(shape.Translate(vec2(0, ro + t - 0.5)), w);
    return {{QStringLiteral("Snap clip"), snap}};
  }
  // Cable label: the ring with a flat tag sticking out to the side, text on the tag.
  const double tagL = v.num("tagLength");
  const CrossSection tag = CrossSection::Square(vec2(ro + tagL, t)).Translate(vec2(0, -ro));
  Manifold label = profileXZ(((ring - gapCut) + tag).Translate(vec2(0, ro)), w);
  const QString text = v.text("text");
  const Manifold letters = engraved(text, std::min(w * 0.55, 6.0), ro + tagL * 0.5, 0, t, 0.5);
  if (!letters.IsEmpty()) label = label - letters;
  return {{QStringLiteral("Cable label"), label}};
}

// ---------------------------------------------------------------------------------------------
// Name tag, keychain or sign

// ---------------------------------------------------------------------------------------------
// Lithophane / photo relief

// A sample picture while none is chosen: a sun over hills.
QImage samplePicture() {
  QImage image(320, 220, QImage::Format_Grayscale8);
  image.fill(QColor(225, 225, 225));
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  QRadialGradient sky(QPointF(210, 70), 200);
  sky.setColorAt(0, QColor(255, 255, 255));
  sky.setColorAt(1, QColor(150, 150, 150));
  painter.fillRect(image.rect(), sky);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(250, 250, 250));
  painter.drawEllipse(QPointF(210, 70), 30, 30);
  QPainterPath far;
  far.moveTo(0, 150);
  far.cubicTo(70, 95, 130, 120, 180, 135);
  far.cubicTo(230, 150, 280, 110, 320, 125);
  far.lineTo(320, 220);
  far.lineTo(0, 220);
  painter.setBrush(QColor(95, 95, 95));
  painter.drawPath(far);
  QPainterPath near;
  near.moveTo(0, 185);
  near.cubicTo(80, 160, 160, 200, 230, 180);
  near.cubicTo(270, 170, 300, 175, 320, 168);
  near.lineTo(320, 220);
  near.lineTo(0, 220);
  painter.setBrush(QColor(40, 40, 40));
  painter.drawPath(near);
  painter.end();
  return image;
}

QImage pictureOf(const QString& value) {
  if (value.isEmpty()) return samplePicture();
  QImage image = QImage::fromData(QByteArray::fromBase64(value.toLatin1()));
  if (image.isNull()) return samplePicture();
  return image.convertToFormat(QImage::Format_Grayscale8);
}

std::vector<Piece> buildLithophane(const Values& v, QString* warning) {
  const QImage image = pictureOf(v.text("picture"));
  if (v.text("picture").isEmpty()) note(warning, QStringLiteral("A sample picture: choose your own under Picture."));
  const QString shape = v.pick("shape");
  const bool round = shape == QLatin1String("round");
  const bool litho = v.pick("mode") == QLatin1String("litho");
  const double width = v.num("width");
  const double thin = v.num("thin");
  const double thick = std::max(v.num("thick"), thin + 0.2);
  // A round one uses the middle square of the picture.
  double u0 = 0, v0 = 0, uw = image.width(), vh = image.height();
  if (round) {
    const double side = std::min(uw, vh);
    u0 = (uw - side) * 0.5;
    v0 = (vh - side) * 0.5;
    uw = vh = side;
  }
  const double height = width * vh / uw;
  const double step = std::max(v.num("detail"), std::max(width, height) / 900.0);
  const int nx = std::clamp(static_cast<int>(std::ceil(width / step)), 2, 1200);
  const int ny = std::clamp(static_cast<int>(std::ceil(height / step)), 2, 1200);
  // Brightness 0 .. 1 at a point of the picture (bilinear), top row first.
  auto bright = [&](double fx, double fy) {
    const double px = std::clamp(u0 + fx * uw - 0.5, 0.0, image.width() - 1.0);
    const double py = std::clamp(v0 + fy * vh - 0.5, 0.0, image.height() - 1.0);
    const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
    const int x1 = std::min(x0 + 1, image.width() - 1), y1 = std::min(y0 + 1, image.height() - 1);
    const double ax = px - x0, ay = py - y0;
    auto at = [&](int x, int y) { return image.constScanLine(y)[x] / 255.0; };
    return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
  };
  const bool invert = v.on("invert");
  MeshGL64 mesh;
  mesh.numProp = 3;
  auto vertex = [&](double x, double y, double z) {
    mesh.vertProperties.insert(mesh.vertProperties.end(), {x, y, z});
    return static_cast<uint64_t>(mesh.vertProperties.size() / 3 - 1);
  };
  auto tri = [&](uint64_t a, uint64_t b, uint64_t c) { mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c}); };
  // The top: one point per grid corner.
  for (int j = 0; j <= ny; ++j)
    for (int i = 0; i <= nx; ++i) {
      double b = bright(static_cast<double>(i) / nx, 1.0 - static_cast<double>(j) / ny);
      if (invert) b = 1 - b;
      const double t = thin + (litho ? 1 - b : b) * (thick - thin);
      vertex(-width * 0.5 + width * i / nx, -height * 0.5 + height * j / ny, t);
    }
  auto top = [&](int i, int j) { return static_cast<uint64_t>(j * (nx + 1) + i); };
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i) {
      tri(top(i, j), top(i + 1, j), top(i + 1, j + 1));
      tri(top(i, j), top(i + 1, j + 1), top(i, j + 1));
    }
  // The rim, counter-clockwise seen from above, and the flat back: a fan from the middle.
  std::vector<uint64_t> rim;
  for (int i = 0; i < nx; ++i) rim.push_back(top(i, 0));
  for (int j = 0; j < ny; ++j) rim.push_back(top(nx, j));
  for (int i = nx; i > 0; --i) rim.push_back(top(i, ny));
  for (int j = ny; j > 0; --j) rim.push_back(top(0, j));
  std::vector<uint64_t> base;
  for (uint64_t r : rim) base.push_back(vertex(mesh.vertProperties[r * 3], mesh.vertProperties[r * 3 + 1], 0));
  const uint64_t middle = vertex(0, 0, 0);
  for (size_t k = 0; k < rim.size(); ++k) {
    const size_t n = (k + 1) % rim.size();
    tri(rim[k], base[k], base[n]);
    tri(rim[k], base[n], rim[n]);
    tri(middle, base[n], base[k]);
  }
  Manifold slab(mesh);
  if (!ok(slab)) {
    note(warning, QStringLiteral("The picture could not be made into a solid."));
    return {};
  }
  const double hx = width * 0.5, hy = height * 0.5;
  const double corner = std::clamp(v.num("corner"), 0.1, std::min(hx, hy) - 0.2);
  const double rimTop = thick + 0.4;
  Manifold part;
  if (round) {
    const double r = hx;
    part = slab ^ prism(circle(r - 0.01, circleSegments(r)), -1, thick + 1);
    part = part + (prism(circle(r + 1.6, circleSegments(r + 1.6)), 0, rimTop) - prism(circle(r - 0.2, circleSegments(r)), -1, rimTop + 1));
  } else {
    part = slab ^ prism(roundRect(hx, hy, corner, 8), -1, thick + 1);
    if (shape == QLatin1String("framed")) {
      const double border = v.num("border");
      part = part + (prism(roundRect(hx + border, hy + border, corner + border, 8), 0, thick + 1.5) -
                     prism(roundRect(hx + 0.1, hy + 0.1, corner, 8), -1, thick + 3));
    } else if (shape == QLatin1String("keychain")) {
      const double holeR = v.num("holeDiameter") * 0.5;
      const double tabR = holeR + 2.5;
      const double tabY = hy + holeR + 1.2;
      // A thin rim keeps the edge from being as fragile as the brightest parts.
      part = part + (prism(roundRect(hx + 1.6, hy + 1.6, corner + 1.6, 8), 0, rimTop) - prism(roundRect(hx - 0.1, hy - 0.1, corner, 8), -1, rimTop + 1));
      Manifold tab = prism(circle(tabR, circleSegments(tabR)), 0, rimTop).Translate(vec3(0, tabY, 0)) +
                     block(-tabR, tabR, hy - 0.5, tabY, 0, rimTop);
      part = part + tab - cylinderZ(holeR, -1, rimTop + 1, 0, tabY);
    }
  }
  return {{litho ? QStringLiteral("Lithophane") : QStringLiteral("Photo relief"), part}};
}

QString pictureValue(const QString& path, int longest) {
  QImage image(path);
  if (image.isNull()) return {};
  image = image.convertToFormat(QImage::Format_Grayscale8);
  if (std::max(image.width(), image.height()) > longest)
    image = image.scaled(longest, longest, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return QString::fromLatin1(bytes.toBase64());
}

// ---------------------------------------------------------------------------------------------
// Dice

namespace {

// A picture as a flat shape, `side` mm across at most, centred: its dark pixels (bright ones if
// inverted), as unions of pixel runs, smoothed a little.
CrossSection pictureShape(const QString& value, double side, bool invert) {
  if (value.isEmpty()) return {};
  QImage image = QImage::fromData(QByteArray::fromBase64(value.toLatin1()));
  if (image.isNull()) return {};
  image = image.convertToFormat(QImage::Format_Grayscale8);
  if (std::max(image.width(), image.height()) > 128) image = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  const int w = image.width(), h = image.height();
  const double px = side / std::max(w, h);
  std::vector<CrossSection> runs;
  for (int y = 0; y < h; ++y) {
    const uchar* line = image.constScanLine(y);
    int x = 0;
    while (x < w) {
      auto dark = [&](int i) { return (line[i] < 128) != invert; };
      if (!dark(x)) {
        ++x;
        continue;
      }
      const int from = x;
      while (x < w && dark(x)) ++x;
      runs.push_back(CrossSection::Square(vec2((x - from) * px, px * 1.001))
                         .Translate(vec2((from - w * 0.5) * px, (h * 0.5 - y - 1) * px)));
    }
  }
  if (runs.empty()) return {};
  const CrossSection shape = CrossSection::BatchBoolean(runs, manifold::OpType::Add);
  return shape.Offset(px * 0.35, CrossSection::JoinType::Round).Offset(-px * 0.35, CrossSection::JoinType::Round).Simplify(px * 0.05);
}

// The pips of a d6 face showing `value`, in a square `half` mm from the middle to the side.
CrossSection pips(int value, double half) {
  static const std::vector<std::vector<vec2>> layout = {
      {{0, 0}},
      {{-1, 1}, {1, -1}},
      {{-1, 1}, {0, 0}, {1, -1}},
      {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}},
      {{-1, 1}, {1, 1}, {0, 0}, {-1, -1}, {1, -1}},
      {{-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}}};
  if (value < 1 || value > 6) return {};
  const double spread = half * 0.52, r = half * 0.17;
  std::vector<CrossSection> dots;
  for (const vec2& at : layout[static_cast<size_t>(value - 1)]) dots.push_back(CrossSection::Circle(r, 32).Translate(at * spread));
  return CrossSection::BatchBoolean(dots, manifold::OpType::Add);
}

}  // namespace

std::vector<Piece> buildDice(const Values& v, QString* warning) {
  namespace la = manifold::la;
  const QString kind = v.pick("kind");
  const double phi = (1 + std::sqrt(5.0)) / 2;
  // Each die as the faces' outward directions (it is the solid inside all of them).
  std::vector<vec3> normals;
  if (kind == QLatin1String("d4")) {
    normals = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
  } else if (kind == QLatin1String("d6")) {
    normals = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  } else if (kind == QLatin1String("d8")) {
    for (int i = 0; i < 8; ++i) normals.push_back(vec3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1));
  } else if (kind == QLatin1String("d10")) {
    // A pentagonal trapezohedron: ten kites, five round each tip.
    const double tilt = 48.0 * kPi / 180;
    for (int k = 0; k < 10; ++k)
      normals.push_back(vec3(std::cos(k * kPi / 5) * std::sin(tilt), std::sin(k * kPi / 5) * std::sin(tilt), (k % 2 ? -1 : 1) * std::cos(tilt)));
  } else if (kind == QLatin1String("d12")) {
    for (int a : {-1, 1})
      for (int b : {-1, 1}) {
        normals.push_back(vec3(0, a, b * phi));
        normals.push_back(vec3(a, b * phi, 0));
        normals.push_back(vec3(b * phi, 0, a));
      }
  } else {  // d20
    for (int i = 0; i < 8; ++i) normals.push_back(vec3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1));
    for (int a : {-1, 1})
      for (int b : {-1, 1}) {
        normals.push_back(vec3(0, a / phi, b * phi));
        normals.push_back(vec3(a / phi, b * phi, 0));
        normals.push_back(vec3(b * phi, 0, a / phi));
      }
  }
  for (vec3& n : normals) n = la::normalize(n);
  const int faces = static_cast<int>(normals.size());
  const bool d4 = faces == 4;
  const double r = v.num("size") * (d4 ? 0.25 : 0.5);  // from the middle to each face
  // The sharp die: a big cube cut down by every face's plane.
  auto frameFor = [](const vec3& n) {
    const vec3 hint = std::abs(n.z) < 0.9 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    const vec3 u = la::normalize(la::cross(hint, n));
    return std::pair<vec3, vec3>(u, la::cross(n, u));
  };
  const double big = r * 20;
  Manifold sharp = Manifold::Cube(vec3(big, big, big), true);
  for (const vec3& n : normals) {
    const auto [a, b] = frameFor(n);
    const Manifold beyond = Manifold::Cube(vec3(big * 2, big * 2, big), true).Translate(vec3(0, 0, big * 0.5));
    sharp = sharp - beyond.Transform(manifold::mat3x4(a, b, n, n * r));
  }
  // Its corners (each once).
  std::vector<vec3> corners;
  {
    const manifold::MeshGL64 mesh = sharp.GetMeshGL64();
    for (size_t i = 0; i < mesh.vertProperties.size(); i += mesh.numProp) {
      const vec3 p(mesh.vertProperties[i], mesh.vertProperties[i + 1], mesh.vertProperties[i + 2]);
      bool seen = false;
      for (const vec3& c : corners) seen = seen || la::length(c - p) < r * 1e-6;
      if (!seen) corners.push_back(p);
    }
  }
  // Rounded: every face moved in by the radius, and a ball of that radius rolled round it (the
  // faces are all the same distance from the middle, so moving them in is a plain scale).
  const double round = std::clamp(v.num("rounding"), 0.0, r * 0.45);
  Manifold die = sharp;
  if (round > 0.01) {
    std::vector<Manifold> balls;
    const int segs = std::clamp(static_cast<int>(round * 12), 16, 40);
    for (const vec3& c : corners) balls.push_back(Manifold::Sphere(round, segs).Translate(c * ((r - round) / r)));
    die = Manifold::Hull(balls);
  }
  // Each face's corners, in order round it; its middle (where the inner ball touches) and the room
  // there (the distance to its nearest side).
  struct Face {
    vec3 n, centre, up;
    double room = 0;
    std::vector<vec3> ring;
  };
  std::vector<Face> sides(static_cast<size_t>(faces));
  for (int i = 0; i < faces; ++i) {
    Face& f = sides[static_cast<size_t>(i)];
    f.n = normals[static_cast<size_t>(i)];
    f.centre = f.n * r;
    for (const vec3& c : corners)
      if (std::abs(la::dot(c, f.n) - r) < r * 1e-4) f.ring.push_back(c);
    const auto [a, b] = frameFor(f.n);
    std::sort(f.ring.begin(), f.ring.end(), [&, a = a, b = b](const vec3& p, const vec3& q) {
      return std::atan2(la::dot(p - f.centre, b), la::dot(p - f.centre, a)) < std::atan2(la::dot(q - f.centre, b), la::dot(q - f.centre, a));
    });
    // The face's own middle (its centroid: a d10's kites are not centred on the inner ball's touch).
    if (f.ring.size() >= 3) {
      vec3 sum(0, 0, 0);
      double area = 0;
      for (size_t k = 1; k + 1 < f.ring.size(); ++k) {
        const double piece = la::length(la::cross(f.ring[k] - f.ring[0], f.ring[k + 1] - f.ring[0])) * 0.5;
        sum += (f.ring[0] + f.ring[k] + f.ring[k + 1]) / 3.0 * piece;
        area += piece;
      }
      if (area > 0) f.centre = sum / area;
    }
    f.room = 1e300;
    for (size_t k = 0; k < f.ring.size(); ++k) {
      const vec3 p = f.ring[k], q = f.ring[(k + 1) % f.ring.size()];
      const vec3 e = q - p;
      const double t = std::clamp(la::dot(f.centre - p, e) / la::dot(e, e), 0.0, 1.0);
      f.room = std::min(f.room, la::length(f.centre - (p + e * t)));
    }
  }
  // Numbers: opposite faces add up to faces + 1 (a d10: 0 to 9, adding up to 9, odd ones on top).
  std::vector<int> value(static_cast<size_t>(faces), 0);
  if (!d4) {
    std::vector<int> order(static_cast<size_t>(faces));
    for (int i = 0; i < faces; ++i) order[static_cast<size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      const vec3 &p = normals[static_cast<size_t>(a)], &q = normals[static_cast<size_t>(b)];
      return p.z != q.z ? p.z > q.z : (p.y != q.y ? p.y > q.y : p.x > q.x);
    });
    std::vector<char> done(static_cast<size_t>(faces), 0);
    int next = faces == 10 ? 1 : 1;
    int nextEven = 0;
    for (int i : order) {
      if (done[static_cast<size_t>(i)]) continue;
      int opposite = -1;
      for (int j = 0; j < faces; ++j)
        if (la::dot(normals[static_cast<size_t>(i)], normals[static_cast<size_t>(j)]) < -0.999) opposite = j;
      if (opposite < 0) continue;
      done[static_cast<size_t>(i)] = done[static_cast<size_t>(opposite)] = 1;
      if (faces == 10) {
        // The upper face of the pair takes the odd number.
        const int odd = next, even = 9 - odd;
        const bool iUp = normals[static_cast<size_t>(i)].z > 0;
        value[static_cast<size_t>(iUp ? i : opposite)] = odd;
        value[static_cast<size_t>(iUp ? opposite : i)] = even;
        next += 2;
        (void)nextEven;
      } else {
        value[static_cast<size_t>(i)] = next;
        value[static_cast<size_t>(opposite)] = faces + 1 - next;
        ++next;
      }
    }
  }
  // The face that ends up on top (the highest number; a d4 stands on its first face), and which way
  // is up on each face: towards the top on a d6's sides, towards the tip on a d10, towards the corner
  // that stands highest on the rest.
  int top = 0;
  for (int i = 0; i < faces; ++i)
    if (value[static_cast<size_t>(i)] > value[static_cast<size_t>(top)]) top = i;
  const vec3 upward = d4 ? -normals[0] : normals[static_cast<size_t>(top)];
  const vec3 aside = frameFor(upward).second;
  for (Face& f : sides) {
    vec3 up;
    if (faces == 6) {
      up = std::abs(la::dot(f.n, upward)) > 0.5 ? aside : upward;
    } else if (faces == 10) {
      up = vec3(0, 0, f.n.z > 0 ? 1.0 : -1.0);
    } else {
      vec3 best = f.ring.empty() ? aside : f.ring.front();
      for (const vec3& c : f.ring) {
        const double h = la::dot(c, upward), hb = la::dot(best, upward);
        if (h > hb + 1e-9 || (std::abs(h - hb) < 1e-9 && la::dot(c, aside) > la::dot(best, aside))) best = c;
      }
      up = best - f.centre;
    }
    up = up - f.n * la::dot(up, f.n);
    f.up = la::dot(up, up) > 1e-12 ? la::normalize(up) : frameFor(f.n).first;
  }
  const QStringList own = v.text("labels").split(QLatin1Char(','), Qt::SkipEmptyParts);
  auto labelFor = [&](int number) -> QString {
    const int index = faces == 10 ? (number == 0 ? 9 : number - 1) : number - 1;
    if (index >= 0 && index < own.size() && !own[index].trimmed().isEmpty()) return own[index].trimmed();
    return QString::number(number);
  };
  const bool usePips = faces == 6 && v.pick("marks") == QLatin1String("pips") && own.isEmpty();
  const bool underline = v.on("underline");
  const double scale = v.num("markSize") / 100.0;
  const QString pictureValue = v.text("picture");
  const int topValue = d4 ? 4 : (faces == 10 ? 9 : faces);
  bool missingText = false;
  // A mark as a flat shape for one face (or a d4 corner), `room` mm from its middle to the edge.
  auto markShape = [&](int number, double room) -> CrossSection {
    if (number == topValue && !pictureValue.isEmpty()) {
      const CrossSection picture = pictureShape(pictureValue, room * 1.45 * scale / 0.55, v.on("invertPicture"));
      if (!picture.IsEmpty()) return picture;
    }
    if (usePips) return pips(number, room);
    const QString text = labelFor(number);
    const double cap = room * 1.1 * scale * (text.size() > 1 ? 0.8 : 1.0);
    CrossSection shape = textShape(text, cap);
    if (shape.IsEmpty()) {
      missingText = true;
      return {};
    }
    // A little bolder than the font, so the strokes print solidly at die sizes.
    shape = shape.Offset(cap * 0.05, CrossSection::JoinType::Round);
    // Fit within the face, however wide the text came out.
    const manifold::Rect box = shape.Bounds();
    const double wide = box.max.x - box.min.x, limit = room * 1.5 * scale / 0.55;
    if (wide > limit) shape = shape.Scale(vec2(limit / wide, limit / wide));
    if (underline && own.isEmpty() && (text == QLatin1String("6") || text == QLatin1String("9"))) {
      const manifold::Rect b = shape.Bounds();
      const double w = (b.max.x - b.min.x) * 0.8, t = std::max(0.5, cap * 0.09);
      shape = shape + CrossSection::Square(vec2(w, t), true).Translate(vec2((b.min.x + b.max.x) * 0.5, b.min.y - t * 1.6));
    }
    return shape;
  };
  // All marks, each a thin solid through its face; `from` and `to` along the face's normal.
  const QString style = v.pick("style");
  const double depth = v.num("depth");
  auto marks = [&](double from, double to) {
    std::vector<Manifold> all;
    auto place = [&](const CrossSection& shape, const vec3& centre, const vec3& up, const vec3& n) {
      if (shape.IsEmpty()) return;
      const vec3 right = la::cross(up, n);
      all.push_back(Manifold::Extrude(shape.ToPolygons(), to - from).Translate(vec3(0, 0, from)).Transform(manifold::mat3x4(right, up, n, centre)));
    };
    for (const Face& f : sides) {
      if (d4) {
        // A d4 shows the number of the corner at the top: each face has one by each of its corners.
        for (const vec3& corner : f.ring) {
          int number = 0;
          for (size_t k = 0; k < corners.size(); ++k)
            if (la::length(corners[k] - corner) < r * 1e-6) number = static_cast<int>(k) + 1;
          const vec3 toward = la::normalize(corner - f.centre);
          const double room = f.room * 0.55;
          place(markShape(number, room), f.centre + toward * la::length(corner - f.centre) * 0.52, toward, f.n);
        }
      } else {
        place(markShape(value[static_cast<size_t>(&f - sides.data())], f.room), f.centre, f.up, f.n);
      }
    }
    return Manifold::BatchBoolean(all, manifold::OpType::Add);
  };
  std::vector<Piece> pieces;
  if (style == QLatin1String("raised")) {
    pieces.push_back({QStringLiteral("Die"), die + marks(-0.4, depth)});
  } else {
    const Manifold cut = marks(-depth, 1.0);
    pieces.push_back({QStringLiteral("Die"), die - cut});
    if (style == QLatin1String("inlay")) pieces.push_back({QStringLiteral("Marks"), marks(-depth, 0) ^ die});
  }
  // Standing on a face, flat on the plate: the highest number (or the picture) on top, upright seen
  // from the front; a d4 on one face with a corner up.
  {
    const vec3 z = upward;
    vec3 y = d4 ? aside : sides[static_cast<size_t>(top)].up;
    y = la::normalize(y - z * la::dot(y, z));
    const vec3 x = la::cross(y, z);
    // Rows x, y, z: the face's frame becomes the plate's; then up so the bottom face is at 0.
    const manifold::mat3x4 turn(vec3(x.x, y.x, z.x), vec3(x.y, y.y, z.y), vec3(x.z, y.z, z.z), vec3(0, 0, r));
    for (Piece& piece : pieces) piece.solid = piece.solid.Transform(turn);
  }
  if (missingText) note(warning, QStringLiteral("Numbers need the text engine; they are left off here."));
  if (!pictureValue.isEmpty() && pictureShape(pictureValue, 10, v.on("invertPicture")).IsEmpty())
    note(warning, QStringLiteral("The picture has nothing dark enough to show: try Swap dark and bright."));
  return pieces;
}

std::vector<Piece> buildNameTag(const Values& v, QString* warning) {
  const QString text = v.text("text");
  const double cap = v.num("textHeight"), thick = v.num("thickness"), depth = v.num("textDepth");
  const double pad = v.num("padding");
  const QString style = v.pick("style");
  CrossSection letters = textShape(text, cap);
  double minX, maxX, minY, maxY;
  if (letters.IsEmpty()) {
    // No text engine (or no text): size the plate as if the text were there.
    const double guess = std::max(1, static_cast<int>(text.size())) * cap * 0.7;
    minX = -guess * 0.5, maxX = guess * 0.5, minY = -cap * 0.5, maxY = cap * 0.5;
    if (!text.trimmed().isEmpty() && !haveText()) note(warning, QStringLiteral("The text shows in DaedalCAD."));
  } else {
    const manifold::Rect box = letters.Bounds();
    minX = box.min.x, maxX = box.max.x, minY = box.min.y, maxY = box.max.y;
  }
  const bool hole = v.on("hole");
  const double holeR = v.num("holeDiameter") * 0.5;
  const double left = minX - pad - (hole ? 2 * holeR + 3 : 0);
  const double right = maxX + pad, bottom = minY - pad, top = maxY + pad;
  const double cx = (left + right) * 0.5, cy = (bottom + top) * 0.5;
  const double hx = (right - left) * 0.5, hy = (top - bottom) * 0.5;
  const double corner = std::min(v.num("corner"), std::min(hx, hy) - 0.1);
  Manifold plate = prism(roundRect(hx, hy, std::max(0.1, corner), 8), 0, thick).Translate(vec3(cx, cy, 0));
  if (hole) plate = plate - cylinderZ(holeR, -1, thick + 1, left + holeR + 2, cy);
  if (v.on("border") && style == QLatin1String("raised")) {
    const double rim = 1.2;
    const Manifold frame = prism(roundRect(hx, hy, std::max(0.1, corner), 8), 0, depth) -
                           prism(roundRect(hx - rim, hy - rim, std::max(0.1, corner - rim), 8), -1, depth + 1);
    plate = plate + frame.Translate(vec3(cx, cy, thick - 0.01));
  }
  if (!letters.IsEmpty()) {
    if (style == QLatin1String("raised")) {
      plate = plate + Manifold::Extrude(letters.ToPolygons(), depth + 0.01).Translate(vec3(0, 0, thick - 0.01));
    } else if (style == QLatin1String("engraved")) {
      plate = plate - Manifold::Extrude(letters.ToPolygons(), depth + 1).Translate(vec3(0, 0, thick - depth));
    } else {
      plate = plate - Manifold::Extrude(letters.ToPolygons(), thick + 2).Translate(vec3(0, 0, -1));
      note(warning, QStringLiteral("Cut-out letters lose their inner islands (the middle of an O); a stencil font avoids that."));
    }
  }
  return {{QStringLiteral("Name tag"), plate}};
}

// ---------------------------------------------------------------------------------------------
// Testing and calibration

std::vector<Piece> buildHoleTest(const Values& v, QString*) {
  const double from = v.num("from"), to = std::max(v.num("from"), v.num("to")), step = v.num("step");
  std::vector<double> sizes;
  for (double d = from; d <= to + 1e-6 && sizes.size() < 30; d += step) sizes.push_back(d);
  const double th = v.num("thickness"), gapX = 3;
  const double widest = sizes.back();
  const double labelRow = 5;
  double lengthX = gapX;
  for (double d : sizes) lengthX += d + gapX;
  const double widthY = widest + 2 * gapX + labelRow;
  Manifold plate = prism(roundRect(lengthX * 0.5, widthY * 0.5, 2, 6), 0, th);
  std::vector<Manifold> cuts;
  double x = -lengthX * 0.5 + gapX;
  for (size_t i = 0; i < sizes.size(); ++i) {
    const double d = sizes[i];
    const double cxh = x + d * 0.5;
    cuts.push_back(cylinderZ(d * 0.5, -1, th + 1, cxh, labelRow * 0.5));
    const double labelY = -widthY * 0.5 + labelRow * 0.5 + 0.5;
    Manifold mark = haveText() ? engraved(mm(d), 2.4, cxh, labelY, th, 0.6) : dots(static_cast<int>(i) + 1, cxh, labelY, th, 1.4);
    if (!mark.IsEmpty()) cuts.push_back(mark);
    x += d + gapX;
  }
  plate = plate - Manifold::BatchBoolean(cuts, OpType::Add);
  std::vector<Piece> pieces{{QStringLiteral("Hole test"), plate}};
  if (v.on("pegs")) {
    // The same sizes as pegs: measure them too, and see which peg goes into which hole.
    Manifold strip = prism(roundRect(lengthX * 0.5, (widest + 2 * gapX) * 0.5, 2, 6), 0, 2);
    x = -lengthX * 0.5 + gapX;
    std::vector<Manifold> pegs{strip};
    for (double d : sizes) {
      pegs.push_back(loft(circleOutline(d * 0.5), {{1.99, 0}, {9.6, 0}, {10, -std::min(0.4, d * 0.1)}}).Translate(vec3(x + d * 0.5, 0, 0)));
      x += d + gapX;
    }
    pieces.push_back({QStringLiteral("Pegs"), Manifold::BatchBoolean(pegs, OpType::Add)});
    stackAlongY(pieces, 5);
  }
  return pieces;
}

std::vector<Piece> buildThreadTest(const Values& v, QString* warning) {
  const MetricSize size = metric(v.pick("size"), v.num("diameter"), v.num("pitch"), warning);
  const ThreadForm form = isoThread(size.diameter, size.pitch);
  const int count = v.whole("count");
  const double first = v.num("first"), step = v.num("step"), th = v.num("thickness");
  const double cell = size.flats + 4;
  const double lengthX = count * cell + 2, widthY = cell + 5;
  Manifold plate = prism(roundRect(lengthX * 0.5, widthY * 0.5, 2, 6), 0, th);
  std::vector<Manifold> cuts;
  for (int i = 0; i < count; ++i) {
    const double grow = first + step * i;
    const double x = -lengthX * 0.5 + 1 + cell * (i + 0.5);
    const double y = 2.5;
    const Manifold thread = threadedRod(form, -form.pitch, th + form.pitch, 0, grow);
    if (!ok(thread)) continue;
    const double outer = form.major + grow + 0.3, inner = form.major - form.depth + grow;
    cuts.push_back((thread + coneZ(outer + 0.5, inner, -0.5, outer - inner) + coneZ(inner, outer + 0.5, th - (outer - inner), th + 0.5))
                       .Translate(vec3(x, y, 0)));
    const double labelY = -widthY * 0.5 + 2.5;
    Manifold mark = haveText() ? engraved(mm(grow), 2.2, x, labelY, th, 0.6) : dots(i + 1, x, labelY, th, 1.4);
    if (!mark.IsEmpty()) cuts.push_back(mark);
  }
  plate = plate - Manifold::BatchBoolean(cuts, OpType::Add);
  // A short bolt to try in every hole.
  const double headH = std::max(2.0, std::round(size.diameter * 0.65 * 10) / 10);
  const Manifold bolt = chamferedHex(size.flats, 0, headH) + boltShank(form, headH - 0.01, headH + th + 4, headH, false);
  std::vector<Piece> pieces{{QStringLiteral("Thread test plate"), plate}, {QStringLiteral("Test bolt"), bolt}};
  sideBySide(pieces, 6);
  return pieces;
}

std::vector<Piece> buildOverhangTest(const Values& v, QString*) {
  std::vector<double> angles;
  for (double a = v.num("from"); a <= v.num("to") + 1e-6 && angles.size() < 12; a += v.num("step")) angles.push_back(a);
  const double h = v.num("height"), fin = 3, depthY = 10, gapX = 3, base = 1.6, labelRow = 5;
  std::vector<Manifold> parts;
  std::vector<Manifold> cuts;
  double x = gapX;
  // Overhangs: fins leaning over at each angle (from vertical).
  for (size_t i = 0; i < angles.size(); ++i) {
    const double lean = h * std::tan(angles[i] * kPi / 180);
    const CrossSection fins(SimplePolygon{vec2(x, base - 0.01), vec2(x + fin, base - 0.01), vec2(x + fin + lean, base + h), vec2(x + lean, base + h)});
    parts.push_back(profileXZ(fins, depthY).Translate(vec3(0, labelRow * 0.5, 0)));
    const double labelX = x + fin * 0.5 + lean * 0.5;
    Manifold mark = haveText() ? engraved(QString::number(angles[i]), 2.6, labelX, -depthY * 0.5, base, 0.5)
                               : dots(static_cast<int>(i) + 1, labelX, -depthY * 0.5, base, 1.4);
    if (!mark.IsEmpty()) cuts.push_back(mark);
    x += fin + lean + gapX;
  }
  double lengthX = x;
  // Bridges: pairs of pillars with a thin deck across, behind the fins.
  std::vector<double> spans;
  if (v.on("bridges"))
    for (double s = v.num("bridgeFrom"); s <= v.num("bridgeTo") + 1e-6 && spans.size() < 8; s += v.num("bridgeStep")) spans.push_back(s);
  const double bridgeY0 = depthY * 0.5 + labelRow * 0.5 + gapX;
  double bx = gapX;
  for (double s : spans) {
    const double pillar = 3, bh = std::min(h, 8.0);
    parts.push_back(block(bx, bx + pillar, bridgeY0, bridgeY0 + 6, base - 0.01, base + bh + 1));
    parts.push_back(block(bx + pillar + s, bx + 2 * pillar + s, bridgeY0, bridgeY0 + 6, base - 0.01, base + bh + 1));
    parts.push_back(block(bx + pillar - 0.01, bx + pillar + s + 0.01, bridgeY0, bridgeY0 + 6, base + bh, base + bh + 1));
    bx += 2 * pillar + s + gapX;
  }
  lengthX = std::max(lengthX, bx);
  const double y0 = -depthY * 0.5 - labelRow * 0.5 - 1.5;
  const double y1 = spans.empty() ? depthY * 0.5 + labelRow * 0.5 + gapX : bridgeY0 + 6 + gapX;
  parts.push_back(block(0, lengthX + gapX, y0, y1, 0, base));
  Manifold test = Manifold::BatchBoolean(parts, OpType::Add);
  if (!cuts.empty()) test = test - Manifold::BatchBoolean(cuts, OpType::Add);
  return {{QStringLiteral("Overhang and bridge test"), test}};
}

void setTextOutlines(TextOutlines outlines) { textOutlinesHook() = std::move(outlines); }

QJsonObject resolved(const QString& id, const QJsonObject& values, double printerClearance) {
  QJsonObject out = normalized(id, values);
  const Spec* s = spec(id);
  if (!s || printerClearance < 0 || !out.value(QStringLiteral("usePrinter")).toBool()) return out;
  for (const Param& p : s->params)
    if (p.printerFactor > 0) out.insert(p.key, std::clamp(printerClearance * p.printerFactor, p.min, p.max));
  return out;
}

// ---------------------------------------------------------------------------------------------
// Standard parts: ISO and DIN screws, nuts, washers, pins, rod and heat-set inserts, each with a
// matching hole. Dimensions are the standards' nominal values (millimetres).

namespace {

double sizeDiameter(const QString& size) { return size.mid(1).toDouble(); }

// The row for `size`, or the nearest size the standard has (and a note saying so).
template <class Row>
const Row* rowFor(const std::map<QString, Row>& table, const QString& size, const QString& standard, QString* warning,
                  QString* used) {
  if (table.empty()) return nullptr;
  const auto it = table.find(size);
  if (it != table.end()) {
    *used = size;
    return &it->second;
  }
  const double want = sizeDiameter(size);
  auto best = table.begin();
  for (auto i = table.begin(); i != table.end(); ++i)
    if (std::abs(sizeDiameter(i->first) - want) < std::abs(sizeDiameter(best->first) - want)) best = i;
  note(warning, QStringLiteral("%1 has no %2; %3 is used.").arg(standard, size, best->first));
  *used = best->first;
  return &best->second;
}

struct HeadRow {
  double dk;  // head diameter; across flats for a hex head
  double k;   // head height
  double s;   // hex key (socket) size; unused for hex and cross heads
  double t;   // socket depth
};

const std::map<QString, std::map<QString, HeadRow>>& screwTables() {
  static const std::map<QString, std::map<QString, HeadRow>> tables = {
      // Socket head cap screw, ISO 4762 (DIN 912 is the same).
      {"4762", {{"M2", {3.8, 2, 1.5, 1}},     {"M2.5", {4.5, 2.5, 2, 1.1}}, {"M3", {5.5, 3, 2.5, 1.3}},  {"M4", {7, 4, 3, 2}},
                {"M5", {8.5, 5, 4, 2.5}},     {"M6", {10, 6, 5, 3}},        {"M8", {13, 8, 6, 4}},       {"M10", {16, 10, 8, 5}},
                {"M12", {18, 12, 10, 6}},     {"M14", {21, 14, 12, 7}},     {"M16", {24, 16, 14, 8}},    {"M20", {30, 20, 17, 10}},
                {"M24", {36, 24, 19, 12}}}},
      // Button head, ISO 7380-1.
      {"7380", {{"M3", {5.7, 1.65, 2, 1.04}},  {"M4", {7.6, 2.2, 2.5, 1.3}},  {"M5", {9.5, 2.75, 3, 1.56}}, {"M6", {10.5, 3.3, 4, 2.08}},
                {"M8", {14, 4.4, 5, 2.6}},     {"M10", {17.5, 5.5, 6, 3.12}}, {"M12", {21, 6.6, 8, 4.16}},  {"M16", {28, 8.8, 10, 5.2}}}},
      // Countersunk socket, ISO 10642 (90 degree head, its full theoretical diameter).
      {"10642", {{"M3", {6.72, 1.86, 2, 1.1}},  {"M4", {8.96, 2.48, 2.5, 1.5}}, {"M5", {11.2, 3.1, 3, 1.9}},  {"M6", {13.44, 3.72, 4, 2.2}},
                 {"M8", {17.92, 4.96, 5, 3}},   {"M10", {22.4, 6.2, 6, 3.6}},   {"M12", {26.88, 7.44, 8, 4.3}}, {"M16", {33.6, 8.8, 10, 4.5}},
                 {"M20", {40.32, 10.16, 12, 5.6}}}},
      // Countersunk socket, DIN 7991.
      {"7991", {{"M3", {6, 1.7, 2, 1.2}},     {"M4", {8, 2.3, 2.5, 1.8}},   {"M5", {10, 2.8, 3, 2.3}},  {"M6", {12, 3.3, 4, 2.5}},
                {"M8", {16, 4.4, 5, 3.5}},    {"M10", {20, 5.5, 6, 4.4}},   {"M12", {24, 6.5, 8, 4.6}}, {"M16", {30, 7.5, 10, 5.3}},
                {"M20", {36, 8.5, 12, 5.9}}}},
      // Hex head, ISO 4017 (dk = across flats).
      {"4017", {{"M3", {5.5, 2, 0, 0}},   {"M4", {7, 2.8, 0, 0}},   {"M5", {8, 3.5, 0, 0}},   {"M6", {10, 4, 0, 0}},
                {"M8", {13, 5.3, 0, 0}},  {"M10", {16, 6.4, 0, 0}}, {"M12", {18, 7.5, 0, 0}}, {"M14", {21, 8.8, 0, 0}},
                {"M16", {24, 10, 0, 0}},  {"M20", {30, 12.5, 0, 0}}, {"M24", {36, 15, 0, 0}}}},
      // Hex head, DIN 933: wider spanners at M10, M12 and M14.
      {"933", {{"M3", {5.5, 2, 0, 0}},   {"M4", {7, 2.8, 0, 0}},   {"M5", {8, 3.5, 0, 0}},   {"M6", {10, 4, 0, 0}},
               {"M8", {13, 5.5, 0, 0}},  {"M10", {17, 7, 0, 0}},   {"M12", {19, 8, 0, 0}},   {"M14", {22, 9, 0, 0}},
               {"M16", {24, 10, 0, 0}},  {"M20", {30, 13, 0, 0}},  {"M24", {36, 15, 0, 0}}}},
      // Pan head, cross recess, ISO 7045.
      {"7045", {{"M2", {4, 1.6, 0, 0}}, {"M2.5", {5, 2.1, 0, 0}}, {"M3", {5.6, 2.4, 0, 0}}, {"M4", {8, 3.1, 0, 0}},
                {"M5", {9.5, 3.7, 0, 0}}, {"M6", {12, 4.6, 0, 0}}, {"M8", {16, 6, 0, 0}}}},
      // Set screw (grub screw), hex socket, flat point, ISO 4026 (no head: s and t only).
      {"4026", {{"M2", {0, 0, 0.9, 0.8}}, {"M2.5", {0, 0, 1.3, 1.2}}, {"M3", {0, 0, 1.5, 1.2}}, {"M4", {0, 0, 2, 1.5}},
                {"M5", {0, 0, 2.5, 2}},   {"M6", {0, 0, 3, 2}},       {"M8", {0, 0, 4, 3}},      {"M10", {0, 0, 5, 4}},
                {"M12", {0, 0, 6, 4.8}},  {"M16", {0, 0, 8, 6.4}},    {"M20", {0, 0, 10, 8}}}},
  };
  return tables;
}

struct NutRow {
  double s;  // across flats (square: the side)
  double m;  // height
  double extra;  // nyloc: total height; flange: flange diameter
};

const std::map<QString, std::map<QString, NutRow>>& nutTables() {
  static const std::map<QString, std::map<QString, NutRow>> tables = {
      {"4032", {{"M2", {4, 1.6, 0}},   {"M2.5", {5, 2, 0}},   {"M3", {5.5, 2.4, 0}},  {"M4", {7, 3.2, 0}},   {"M5", {8, 4.7, 0}},
                {"M6", {10, 5.2, 0}},  {"M8", {13, 6.8, 0}},  {"M10", {16, 8.4, 0}},  {"M12", {18, 10.8, 0}}, {"M14", {21, 12.8, 0}},
                {"M16", {24, 14.8, 0}}, {"M20", {30, 18, 0}}, {"M24", {36, 21.5, 0}}}},
      {"934", {{"M2", {4, 1.6, 0}},   {"M2.5", {5, 2, 0}},   {"M3", {5.5, 2.4, 0}}, {"M4", {7, 3.2, 0}},  {"M5", {8, 4, 0}},
               {"M6", {10, 5, 0}},    {"M8", {13, 6.5, 0}},  {"M10", {17, 8, 0}},   {"M12", {19, 10, 0}}, {"M14", {22, 11, 0}},
               {"M16", {24, 13, 0}},  {"M20", {30, 16, 0}},  {"M24", {36, 19, 0}}}},
      {"4035", {{"M3", {5.5, 1.8, 0}}, {"M4", {7, 2.2, 0}},  {"M5", {8, 2.7, 0}},  {"M6", {10, 3.2, 0}}, {"M8", {13, 4, 0}},
                {"M10", {16, 5, 0}},   {"M12", {18, 6, 0}},  {"M14", {21, 7, 0}},  {"M16", {24, 8, 0}},  {"M20", {30, 10, 0}},
                {"M24", {36, 12, 0}}}},
      // Nyloc, DIN 985: extra = total height with the nylon ring.
      {"985", {{"M3", {5.5, 2.4, 4}}, {"M4", {7, 2.9, 5}}, {"M5", {8, 3.2, 5}}, {"M6", {10, 4, 6}},  {"M8", {13, 5.5, 8}},
               {"M10", {17, 6.5, 10}}, {"M12", {19, 8, 12}}, {"M14", {22, 9.5, 14}}, {"M16", {24, 10.5, 16}}, {"M20", {30, 14, 20}},
               {"M24", {36, 15, 24}}}},
      // Flange nut, DIN 6923: extra = flange diameter.
      {"6923", {{"M5", {8, 5, 11.8}},  {"M6", {10, 6, 14.2}}, {"M8", {13, 8, 17.9}}, {"M10", {15, 10, 21.8}},
                {"M12", {18, 12, 26}}, {"M14", {21, 14, 29.9}}, {"M16", {24, 16, 34.5}}, {"M20", {30, 20, 42.8}}}},
      // Square nut, thin, DIN 562.
      {"562", {{"M3", {5.5, 1.8, 0}}, {"M4", {7, 2.2, 0}}, {"M5", {8, 2.7, 0}}, {"M6", {10, 3.2, 0}}, {"M8", {13, 4, 0}},
               {"M10", {17, 5, 0}},   {"M12", {19, 6, 0}}}},
  };
  return tables;
}

struct WasherRow {
  double d1, d2, h;
};

const std::map<QString, std::map<QString, WasherRow>>& washerTables() {
  static const std::map<QString, std::map<QString, WasherRow>> tables = {
      {"7089", {{"M2", {2.2, 5, 0.3}},   {"M2.5", {2.7, 6, 0.5}}, {"M3", {3.2, 7, 0.5}},    {"M4", {4.3, 9, 0.8}},  {"M5", {5.3, 10, 1}},
                {"M6", {6.4, 12, 1.6}},  {"M8", {8.4, 16, 1.6}},  {"M10", {10.5, 20, 2}},   {"M12", {13, 24, 2.5}}, {"M14", {15, 28, 2.5}},
                {"M16", {17, 30, 3}},    {"M20", {21, 37, 3}},    {"M24", {25, 44, 4}}}},
      {"7093", {{"M3", {3.2, 9, 0.8}},  {"M4", {4.3, 12, 1}},   {"M5", {5.3, 15, 1.2}}, {"M6", {6.4, 18, 1.6}}, {"M8", {8.4, 24, 2}},
                {"M10", {10.5, 30, 2.5}}, {"M12", {13, 37, 3}}, {"M14", {15, 44, 3}},   {"M16", {17, 50, 3}},  {"M20", {22, 60, 4}},
                {"M24", {26, 72, 5}}}},
      // Spring washer, DIN 127 B.
      {"127", {{"M3", {3.1, 6.2, 0.8}},   {"M4", {4.1, 7.6, 0.9}},  {"M5", {5.1, 9.2, 1.2}},  {"M6", {6.1, 11.8, 1.6}}, {"M8", {8.2, 14.8, 2}},
               {"M10", {10.2, 18.1, 2.2}}, {"M12", {12.2, 21.1, 2.5}}, {"M14", {14.2, 24.1, 3}}, {"M16", {16.2, 27.4, 3.5}},
               {"M20", {20.2, 33.6, 4}},   {"M24", {24.5, 40, 5}}}},
  };
  return tables;
}

// Clearance holes, ISO 273: fine, medium, coarse.
const std::map<QString, std::array<double, 3>>& clearanceTable() {
  static const std::map<QString, std::array<double, 3>> table = {
      {"M2", {2.2, 2.4, 2.6}},  {"M2.5", {2.7, 2.9, 3.1}}, {"M3", {3.2, 3.4, 3.6}},  {"M4", {4.3, 4.5, 4.8}},  {"M5", {5.3, 5.5, 5.8}},
      {"M6", {6.4, 6.6, 7}},    {"M8", {8.4, 9, 10}},      {"M10", {10.5, 11, 12}},  {"M12", {13, 13.5, 14.5}}, {"M14", {15, 15.5, 16.5}},
      {"M16", {17, 17.5, 18.5}}, {"M20", {21, 22, 24}},    {"M24", {25, 26, 28}}};
  return table;
}

double clearanceHole(const QString& size, const QString& fit) {
  const auto it = clearanceTable().find(size);
  const int i = fit == QLatin1String("fine") ? 0 : fit == QLatin1String("coarse") ? 2 : 1;
  if (it != clearanceTable().end()) return it->second[static_cast<size_t>(i)];
  return sizeDiameter(size) * (1.06 + 0.04 * i);
}

// Counterbores for socket heads, DIN 974-1; other heads: their diameter plus 1.
double counterbore(const QString& standard, const QString& size, const HeadRow& head) {
  static const std::map<QString, double> socket = {{"M2", 4.3},  {"M2.5", 5},  {"M3", 6.5},  {"M4", 8},   {"M5", 10},
                                                   {"M6", 11},   {"M8", 15},   {"M10", 18},  {"M12", 20}, {"M14", 24},
                                                   {"M16", 26},  {"M20", 33},  {"M24", 40}};
  if (standard == QLatin1String("4762"))
    if (const auto it = socket.find(size); it != socket.end()) return it->second;
  if (standard == QLatin1String("4017") || standard == QLatin1String("933")) return head.dk * 2 / std::sqrt(3.0) + 1;  // round the corners
  return head.dk + 1;
}

// Heat-set inserts as sold for 3D printing: the hole they go in and their length.
struct InsertRow {
  double hole, length;
};
const std::map<QString, InsertRow>& insertTable() {
  static const std::map<QString, InsertRow> table = {{"M2", {3.2, 3}},     {"M2.5", {3.6, 4}}, {"M3", {4.0, 5.7}},
                                                     {"M4", {5.6, 8.1}},   {"M5", {6.4, 9.5}}, {"M6", {8.0, 12.7}},
                                                     {"M8", {9.6, 12.7}}};
  return table;
}

double pitchOf(const QString& size) {
  for (const ThreadSize& t : threadSizes())
    if (t.name == size) return t.pitch;
  return coarsePitch(sizeDiameter(size));
}

// A shank from z0 down to z1 (z1 < z0): a plain rod with a chamfered tip, or a modelled thread.
Manifold shank(const QString& size, double z0, double z1, bool threaded) {
  const double d = sizeDiameter(size);
  const double length = z0 - z1;
  if (length <= 0.05) return {};
  if (threaded) {
    const Manifold rod = threadedCylinder(d, pitchOf(size), length, false, 0);
    if (ok(rod)) return rod.Translate(vec3(0, 0, (z0 + z1) * 0.5));
  }
  const double tip = std::min(0.1 * d + 0.2, length * 0.3);
  return chamferedDisc(d * 0.5, z1, z0, tip, tip, true, false);
}

// A hex socket (or a cross recess) cut into the top at z = top.
Manifold hexSocket(double s, double depth, double top) {
  if (s <= 0 || depth <= 0) return {};
  const double r = s / std::sqrt(3.0);
  return prism(regular(r, 6, 30), top - depth, top + 1) + coneZ(r, 0.01, top - depth - r * 0.55, top - depth + 0.001);
}

Manifold crossRecess(double headD, double depth, double top) {
  const double arm = headD * 0.28, width = headD * 0.09;
  const Manifold bar = block(-arm, arm, -width, width, top - depth, top + 1);
  return bar + bar.Rotate(0, 0, 90) + coneZ(width * 2.2, 0.01, top - depth - width, top - depth + 0.001);
}

// --- The parts ---------------------------------------------------------------------------------

// A screw: the head's bearing face at z = 0 and the shank down to -length (a countersunk or set
// screw: its top at z = 0, length overall).
std::vector<Piece> buildStdScrew(const Values& v, QString* warning) {
  const QString standard = v.pick("standard");
  QString size;
  const auto& table = screwTables().at(standard);
  const HeadRow* head = rowFor(table, v.pick("size"), QStringLiteral("This standard"), warning, &size);
  if (!head) return {};
  const double d = sizeDiameter(size);
  const double L = std::max(v.num("length"), d * 0.5);
  const bool threaded = v.pick("thread") == QLatin1String("modelled");
  const bool hole = v.pick("make") == QLatin1String("hole");
  const bool countersunk = standard == QLatin1String("10642") || standard == QLatin1String("7991");
  const bool setScrew = standard == QLatin1String("4026");
  const bool hex = standard == QLatin1String("4017") || standard == QLatin1String("933");
  if (hole) {
    // The hole the screw goes through: clearance down past its tip, and room for the head so its
    // top sits flush (or no room: the head stands on the surface).
    const bool flush = v.pick("head") == QLatin1String("flush");
    if (setScrew) {
      // Tapped, running on a whole number of turns past both ends so its thread meets the screw's.
      const double pitch = pitchOf(size);
      const double past = pitch * std::ceil(1.0 / pitch);
      const Manifold tapped = threadedCylinder(d, pitch, L + 2 * past, true, threadPlay(pitch));
      return {{QStringLiteral("Screw hole"), tapped.Translate(vec3(0, 0, -L * 0.5))}};
    }
    const double clear = clearanceHole(size, v.pick("fit")) * 0.5;
    Manifold cut = cylinderZ(clear, -L - 1, 1);
    if (countersunk) {
      // A 90 degree countersink, sunk by the head's straight rim (DIN 7991 has one) so its top is flush.
      const double top = head->dk * 0.5 + 0.25;
      const double rim = std::max(0.0, head->k - (head->dk - d) * 0.5);
      cut = cut + coneZ(clear, top, -rim - (top - clear), -rim) + cylinderZ(top, -rim - 0.001, 1);
    } else if (flush) {
      const double r = counterbore(standard, size, *head) * 0.5;
      cut = cut + cylinderZ(r, 0, head->k + 0.4 + 1);
    } else {
      cut = cylinderZ(clear, -L - 1, 1);
    }
    return {{QStringLiteral("Screw hole"), cut}};
  }
  Manifold screw;
  if (setScrew) {
    screw = shank(size, 0, -L, threaded) - hexSocket(head->s, head->t, 0);
  } else if (countersunk) {
    const double cone = (head->dk - d) * 0.5;
    const double rim = std::max(0.0, head->k - cone);
    Manifold capHead = coneZ(d * 0.5, head->dk * 0.5, -head->k, -rim);
    if (rim > 0.01) capHead = capHead + cylinderZ(head->dk * 0.5, -rim, 0);
    screw = (capHead + shank(size, -head->k + 0.01, -L, threaded)) - hexSocket(head->s, head->t, 0);
  } else if (hex) {
    screw = chamferedHex(head->dk, 0, head->k) + shank(size, 0.01, -L, threaded);
  } else if (standard == QLatin1String("7380")) {
    // A spherical cap over the head's diameter.
    const double a = head->dk * 0.5, k = head->k;
    const double R = (a * a + k * k) / (2 * k);
    SimplePolygon profile{vec2(0, 0)};
    const int steps = 24;
    for (int i = 0; i <= steps; ++i) {
      const double x = a * (1 - static_cast<double>(i) / steps);
      profile.push_back(vec2(x, std::sqrt(std::max(0.0, R * R - x * x)) - (R - k)));
    }
    const Manifold dome = Manifold::Revolve(Polygons{profile}, circleSegments(a));
    screw = (dome + shank(size, 0.01, -L, threaded)) - hexSocket(head->s, head->t, k);
  } else if (standard == QLatin1String("7045")) {
    const Manifold pan = chamferedDisc(head->dk * 0.5, 0, head->k, head->k * 0.35, head->k * 0.45, false, true);
    screw = (pan + shank(size, 0.01, -L, threaded)) - crossRecess(head->dk, head->k * 0.55, head->k);
  } else {
    // Socket head cap: a slightly chamfered top, the hex socket in it.
    const Manifold capHead = chamferedDisc(head->dk * 0.5, 0, head->k, head->dk * 0.05, head->dk * 0.05, false, true);
    screw = (capHead + shank(size, 0.01, -L, threaded)) - hexSocket(head->s, head->t, head->k);
  }
  return {{QStringLiteral("%1 x %2").arg(size, QString::number(L, 'g', 4)), screw}};
}

// A nut, standing on z = 0.
std::vector<Piece> buildStdNut(const Values& v, QString* warning) {
  const QString standard = v.pick("standard");
  QString size;
  const NutRow* row = rowFor(nutTables().at(standard), v.pick("size"), QStringLiteral("This standard"), warning, &size);
  if (!row) return {};
  const double d = sizeDiameter(size);
  const bool threaded = v.pick("thread") == QLatin1String("modelled");
  const double height = standard == QLatin1String("985") ? row->extra : row->m;
  if (v.pick("make") == QLatin1String("hole")) {
    // A nut trap: the nut's shape with room all round, and the bolt's clearance through it.
    const double c = std::max(0.0, v.num("clearance"));
    Manifold trap = standard == QLatin1String("562") ? block(-row->s * 0.5 - c, row->s * 0.5 + c, -row->s * 0.5 - c, row->s * 0.5 + c, -0.1, height + c)
                                                     : prism(regular((row->s + 2 * c) / std::sqrt(3.0), 6, 30), -0.1, height + c);
    if (standard == QLatin1String("6923")) trap = trap + cylinderZ(row->extra * 0.5 + c, -0.1, row->m * 0.3 + c);  // the flange's cone
    const double reach = std::max(0.0, v.num("through"));
    trap = trap + cylinderZ(clearanceHole(size, "medium") * 0.5, -reach, height + reach);
    return {{QStringLiteral("Nut trap"), trap}};
  }
  Manifold nut;
  if (standard == QLatin1String("562")) {
    nut = block(-row->s * 0.5, row->s * 0.5, -row->s * 0.5, row->s * 0.5, 0, row->m);
  } else {
    nut = chamferedHex(row->s, 0, row->m);
    if (standard == QLatin1String("985")) {
      // The nylon ring's collar on top, a little narrower.
      nut = nut + chamferedDisc(row->s * 0.47, row->m - 0.01, row->extra, row->s * 0.06, row->s * 0.06, false, true);
    } else if (standard == QLatin1String("6923")) {
      const double c = row->m * 0.15;
      nut = nut + coneZ(row->extra * 0.5, row->s * 0.5, 0, c * 2) + cylinderZ(row->extra * 0.5, 0, c * 0.5);
    }
  }
  Manifold bore;
  if (threaded) {
    const Manifold cut = threadedCylinder(d, pitchOf(size), height + 0.02, true, 0);
    bore = ok(cut) ? cut.Translate(vec3(0, 0, height * 0.5)) : Manifold();
  }
  if (!ok(bore)) bore = cylinderZ(d * 0.5, -1, height + 1);  // plain: the thread's outside diameter
  if (standard == QLatin1String("985") && !threaded) bore = cylinderZ(d * 0.5, -1, row->m + 0.01) + cylinderZ(d * 0.42, row->m, height + 1);
  return {{QStringLiteral("%1 nut").arg(size), nut - bore}};
}

// A washer, lying on z = 0.
std::vector<Piece> buildStdWasher(const Values& v, QString* warning) {
  const QString standard = v.pick("standard");
  QString size;
  const WasherRow* row = rowFor(washerTables().at(standard), v.pick("size"), QStringLiteral("This standard"), warning, &size);
  if (!row) return {};
  const bool spring = standard == QLatin1String("127");
  const double rise = spring ? row->h * 0.8 : 0;  // a spring washer's split ends stand apart
  if (v.pick("make") == QLatin1String("hole")) {
    const double c = std::max(0.0, v.num("clearance"));
    const double reach = std::max(0.0, v.num("through"));
    return {{QStringLiteral("Washer recess"), cylinderZ(row->d2 * 0.5 + c, -0.1, row->h + rise + c) +
                                                   cylinderZ(clearanceHole(size, "medium") * 0.5, -reach, row->h + reach)}};
  }
  Manifold washer = cylinderZ(row->d2 * 0.5, 0, row->h) - cylinderZ(row->d1 * 0.5, -1, row->h + 1);
  if (spring) {
    // Split, and the two ends turned apart along a helix.
    washer = washer - block(0, row->d2, -row->h * 0.4, row->h * 0.4, -1, row->h + 1);
    washer = washer.Refine(4).Warp([rise](vec3& p) {
      const double a = std::atan2(p.y, p.x);
      p.z += rise * (a < 0 ? a + 2 * kPi : a) / (2 * kPi);
    });
  } else {
    // The usual small chamfer on the outside edge.
    washer = (chamferedDisc(row->d2 * 0.5, 0, row->h, std::min(0.3, row->h * 0.3), -1, false, true)) - cylinderZ(row->d1 * 0.5, -1, row->h + 1);
  }
  return {{QStringLiteral("%1 washer").arg(size), washer}};
}

// A dowel pin, ISO 2338 / ISO 8734, standing in its hole: `depth` of it below z = 0, the rest above.
std::vector<Piece> buildStdPin(const Values& v, QString*) {
  const double d = v.pick("diameter").toDouble();
  const double L = std::max(v.num("length"), d * 0.5);
  const double depth = std::clamp(v.num("depth"), 0.5, L);
  if (v.pick("make") == QLatin1String("hole")) {
    const double r = d * 0.5 + (v.pick("fit") == QLatin1String("slip") ? 0.1 : 0.0);
    return {{QStringLiteral("Pin hole"),
             cylinderZ(r, -depth - 0.2, 1) + coneZ(r, r + 0.4, -0.4, 0.001) + cylinderZ(r + 0.4, 0, 1)}};
  }
  const double c = std::min(0.15 * d, L * 0.2);
  return {{QStringLiteral("Pin %1 x %2").arg(QString::number(d, 'g', 3), QString::number(L, 'g', 4)),
           chamferedDisc(d * 0.5, -depth, L - depth, c, c)}};
}

// Threaded rod, DIN 975, from z = 0 up to length.
std::vector<Piece> buildStdRod(const Values& v, QString*) {
  const QString size = v.pick("size");
  const double L = std::max(v.num("length"), 1.0);
  if (v.pick("make") == QLatin1String("hole")) return {{QStringLiteral("Rod hole"), cylinderZ(clearanceHole(size, v.pick("fit")) * 0.5, -1, L + 1)}};
  Manifold rod = shank(size, L, 0, v.pick("thread") == QLatin1String("modelled"));
  return {{QStringLiteral("%1 rod").arg(size), rod}};
}

// A heat-set insert, its top at z = 0, going down.
std::vector<Piece> buildStdInsert(const Values& v, QString* warning) {
  QString size;
  const InsertRow* row = rowFor(insertTable(), v.pick("size"), QStringLiteral("Heat-set inserts"), warning, &size);
  if (!row) return {};
  const double d = sizeDiameter(size);
  const double L = row->length;
  if (v.pick("make") == QLatin1String("hole")) {
    // The pocket it is pressed into, a lead-in at the top, and room for the screw beyond.
    const double r = row->hole * 0.5;
    return {{QStringLiteral("Insert pocket"), cylinderZ(r, -L - 0.5, 1) + coneZ(r, r + 0.4, -0.4, 0.001) + cylinderZ(r + 0.4, 0, 1) +
                                                   cylinderZ(clearanceHole(size, "medium") * 0.5, -L - 4, -L)}};
  }
  // Knurled brass: two grip bands round a slightly slimmer body, a plain bore.
  const double outer = row->hole * 0.5 + 0.3;
  Manifold insert = cylinderZ(outer - 0.25, -L, 0);
  for (double z0 : {-L * 0.95, -L * 0.48}) {
    const Manifold band = prism(gripCircle(outer - 0.25, 0.25), z0, z0 + L * 0.4);
    insert = insert + band;
  }
  insert = insert - cylinderZ(d * 0.5, -L - 1, 1);
  return {{QStringLiteral("%1 insert").arg(size), insert}};
}

}  // namespace

const std::vector<ThreadSize>& threadSizes() {
  static const std::vector<ThreadSize> sizes = {
      {"M2", 2, 0.4},   {"M2.5", 2.5, 0.45}, {"M3", 3, 0.5},     {"M4", 4, 0.7},   {"M5", 5, 0.8},
      {"M6", 6, 1.0},   {"M8", 8, 1.25},     {"M10", 10, 1.5},   {"M12", 12, 1.75}, {"M14", 14, 2.0},
      {"M16", 16, 2.0}, {"M20", 20, 2.5},    {"M24", 24, 3.0},   {"M30", 30, 3.5}, {"M36", 36, 4.0}};
  return sizes;
}

double coarsePitch(double diameter) {
  const ThreadSize* best = &threadSizes().front();
  for (const ThreadSize& size : threadSizes())
    if (std::abs(size.diameter - diameter) < std::abs(best->diameter - diameter)) best = &size;
  return best->pitch * std::clamp(diameter / best->diameter, 0.5, 2.0);
}

double threadPlay(double pitch) { return std::clamp(0.25 * pitch, 0.1, 0.25); }

Manifold threadedCylinder(double diameter, double pitch, double length, bool internal, double clearance) {
  const double hz = length * 0.5;
  pitch = std::clamp(pitch, std::max(0.2, diameter / 60), std::max(0.3, diameter * 0.4));
  const ThreadForm form = isoThread(diameter, pitch);
  if (!internal) {
    // Chamfered ends when there is room for them, so the thread starts cleanly.
    if (length > 2 * form.depth + 1.6) return boltShank(form, -hz, hz, -hz, true);
    return threadedRod(form, -hz, hz, -hz);
  }
  const Manifold rod = threadedRod(form, -hz - pitch, hz + pitch, -hz, clearance);
  if (!ok(rod)) return {};
  Manifold cut = rod ^ cylinderZ(form.major + clearance + 1, -hz, hz);
  const double outer = form.major + clearance + 0.3;
  const double inner = form.major - form.depth + clearance;
  const double sink = std::min(outer - inner, hz * 0.5);
  cut = cut + coneZ(outer, inner, -hz, -hz + sink) + coneZ(inner, outer, hz - sink, hz);
  return ok(cut) ? cut : Manifold();
}

std::vector<Piece> build(const QString& id, const QJsonObject& values, QString* warning, double printerClearance,
                         manifold::vec3* centre) {
  const Spec* s = spec(id);
  if (!s) {
    note(warning, QStringLiteral("Unknown generator."));
    return {};
  }
  const Values v(*s, resolved(id, values, printerClearance));
  std::vector<Piece> pieces;
  try {
    if (id == QLatin1String("box")) pieces = buildBox(v, warning);
    else if (id == QLatin1String("gridbin")) pieces = buildGridBin(v, warning);
    else if (id == QLatin1String("gridplate")) pieces = buildGridPlate(v, warning);
    else if (id == QLatin1String("jar")) pieces = buildJar(v, warning);
    else if (id == QLatin1String("bolt")) pieces = buildBolt(v, warning);
    else if (id == QLatin1String("nut")) pieces = buildNut(v, warning);
    else if (id == QLatin1String("standoff")) pieces = buildStandoff(v, warning);
    else if (id == QLatin1String("gear")) pieces = buildGear(v, warning);
    else if (id == QLatin1String("hinge")) pieces = buildHinge(v, warning);
    else if (id == QLatin1String("enclosure")) pieces = buildEnclosure(v, warning);
    else if (id == QLatin1String("battery")) pieces = buildBattery(v, warning);
    else if (id == QLatin1String("balljoint")) pieces = buildBallJoint(v, warning);
    else if (id == QLatin1String("hose")) pieces = buildHose(v, warning);
    else if (id == QLatin1String("rack")) pieces = buildRack(v, warning);
    else if (id == QLatin1String("honeycomb")) pieces = buildHoneycomb(v, warning);
    else if (id == QLatin1String("patterntube")) pieces = buildPatternTube(v, warning);
    else if (id == QLatin1String("fittest")) pieces = buildFitTest(v, warning);
    else if (id == QLatin1String("pulley")) pieces = buildPulley(v, warning);
    else if (id == QLatin1String("bearing")) pieces = buildBearing(v, warning);
    else if (id == QLatin1String("coupler")) pieces = buildCoupler(v, warning);
    else if (id == QLatin1String("spring")) pieces = buildSpring(v, warning);
    else if (id == QLatin1String("snapfit")) pieces = buildSnapFit(v, warning);
    else if (id == QLatin1String("livinghinge")) pieces = buildLivingHinge(v, warning);
    else if (id == QLatin1String("cablechain")) pieces = buildCableChain(v, warning);
    else if (id == QLatin1String("tubeconnector")) pieces = buildTubeConnector(v, warning);
    else if (id == QLatin1String("toolholder")) pieces = buildToolHolder(v, warning);
    else if (id == QLatin1String("dividers")) pieces = buildDividers(v, warning);
    else if (id == QLatin1String("cable")) pieces = buildCable(v, warning);
    else if (id == QLatin1String("nametag")) pieces = buildNameTag(v, warning);
    else if (id == QLatin1String("lithophane")) pieces = buildLithophane(v, warning);
    else if (id == QLatin1String("dice")) pieces = buildDice(v, warning);
    else if (id == QLatin1String("holetest")) pieces = buildHoleTest(v, warning);
    else if (id == QLatin1String("threadtest")) pieces = buildThreadTest(v, warning);
    else if (id == QLatin1String("overhang")) pieces = buildOverhangTest(v, warning);
    else if (id == QLatin1String("stdscrew")) pieces = buildStdScrew(v, warning);
    else if (id == QLatin1String("stdnut")) pieces = buildStdNut(v, warning);
    else if (id == QLatin1String("stdwasher")) pieces = buildStdWasher(v, warning);
    else if (id == QLatin1String("stdpin")) pieces = buildStdPin(v, warning);
    else if (id == QLatin1String("stdrod")) pieces = buildStdRod(v, warning);
    else if (id == QLatin1String("stdinsert")) pieces = buildStdInsert(v, warning);
  } catch (const std::exception&) {
    pieces.clear();
  }
  std::vector<Piece> good;
  for (Piece& piece : pieces)
    if (ok(piece.solid)) good.push_back(std::move(piece));
  if (good.size() != pieces.size() || good.empty()) note(warning, QStringLiteral("Some of this could not be built with these settings."));
  if (good.empty()) return good;
  // Center the whole set on its bounding box, like every other part.
  double lo[3] = {1e18, 1e18, 1e18}, hi[3] = {-1e18, -1e18, -1e18};
  for (const Piece& piece : good) {
    const auto box = piece.solid.BoundingBox();
    lo[0] = std::min(lo[0], box.min.x);
    lo[1] = std::min(lo[1], box.min.y);
    lo[2] = std::min(lo[2], box.min.z);
    hi[0] = std::max(hi[0], box.max.x);
    hi[1] = std::max(hi[1], box.max.y);
    hi[2] = std::max(hi[2], box.max.z);
  }
  const vec3 center((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5);
  for (Piece& piece : good) piece.solid = piece.solid.Translate(-center);
  if (centre) *centre = center;
  return good;
}

bool frameCentre(const QString& id, const QJsonObject& values, double printerClearance, manifold::vec3* centre) {
  // A modelled thread has the same extent as a plain one, and is far slower to build.
  QJsonObject plain = values;
  if (plain.contains(QStringLiteral("thread"))) plain[QStringLiteral("thread")] = QStringLiteral("simple");
  return !build(id, plain, nullptr, printerClearance, centre).empty();
}

bool makesHole(const QString& id, const QJsonObject& values) {
  const QString make = values.value(QStringLiteral("make")).toString();
  if (id == QLatin1String("honeycomb") || id == QLatin1String("patterntube")) return make == QLatin1String("holes");
  return id.startsWith(QLatin1String("std")) && make == QLatin1String("hole");
}

}  // namespace gen
