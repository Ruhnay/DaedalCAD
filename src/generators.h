#pragma once

// Generators: parts made from a few settings instead of drawn by hand (a box with a lid that fits,
// Gridfinity bins, bolts and nuts, gears...). A placed generator is one node (shape "generator")
// that keeps its settings, so it can be changed later; "Convert to parts" turns it into ordinary
// parts. Every generator returns one or more pieces (a box and its lid), already laid out for
// printing or assembled, and centered as a whole on their bounding box.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <manifold/cross_section.h>
#include <manifold/manifold.h>

#include <functional>
#include <utility>
#include <vector>

namespace gen {

struct Param {
  // Picture: a picture kept in the settings themselves (a small grey PNG, base64), so the project
  // carries it along; empty shows a sample picture.
  enum Type { Number, Integer, Choice, Toggle, Heading, Text, Picture };
  QString key;
  QString label;
  Type type = Number;
  double def = 0;
  double min = 0;
  double max = 100;
  double step = 1;
  int decimals = 1;
  QString suffix;
  // Choice: value and label pairs; the default is defChoice.
  std::vector<std::pair<QString, QString>> choices;
  QString defChoice;
  // Text: the default text and the longest allowed.
  QString defText;
  // Shown only when other settings match: "lid=lip|snap", "lid!=none", "nut=1"; several joined with
  // '&' must all match ("lid!=none&usePrinter=0").
  QString showIf;
  // A clearance that follows the printer clearance (Settings) while the generator's "usePrinter"
  // switch is on: it becomes printer clearance x printerFactor (thread flanks are slanted, so the
  // radial play that gives the same gap on them is larger). 0 = an ordinary setting.
  double printerFactor = 0;
  QString tip;
};

struct Spec {
  QString id;
  QString name;
  QString category;
  QString blurb;
  std::vector<Param> params;
};

const std::vector<Spec>& specs();
const Spec* spec(const QString& id);
QStringList categories();

// A picture file as a Picture setting's value (grey, at most `longest` pixels across); empty if
// it cannot be read.
QString pictureValue(const QString& path, int longest = 480);

// Every setting filled in (missing ones take the default) and kept inside its range.
QJsonObject normalized(const QString& id, const QJsonObject& values);
QJsonObject defaults(const QString& id);
bool visible(const Param& param, const QJsonObject& values);

struct Piece {
  QString name;
  manifold::Manifold solid;
};

// Builds the pieces. Empty when the id is unknown or nothing could be built; warning says why.
// printerClearance: the value from Settings, used where "Use printer clearance" is on (negative: never).
// centre: where the pieces' own origin was moved to (subtract it to get back their own frame;
// standard parts and their holes share that frame, so a part and its hole line up).
std::vector<Piece> build(const QString& id, const QJsonObject& values, QString* warning = nullptr,
                         double printerClearance = -1, manifold::vec3* centre = nullptr);

// The centre that build() gives for these settings (built quickly, with plain threads).
bool frameCentre(const QString& id, const QJsonObject& values, double printerClearance, manifold::vec3* centre);

// Makes a hole shape rather than a part: a standard part (ISO/DIN fastener) set to its matching
// hole, or a pattern panel or tube set to holes only.
bool makesHole(const QString& id, const QJsonObject& values);

// The settings as they are built: with "Use printer clearance" on, its clearances replaced.
QJsonObject resolved(const QString& id, const QJsonObject& values, double printerClearance);

// Binary STL of one piece, for "Convert to parts".
bool saveStl(const manifold::Manifold& solid, const QString& path, QString* error = nullptr);

// Default part name for a placed generator ("Box with lid").
QString displayName(const QString& id);

// Flat outlines of a line of text in mm (non-zero fill), centred on the origin, a capital letter
// `capHeight` tall, in a plain bold font. Generators that print labels (name tags, the drill index,
// test pieces) use it when the program has set one from its font engine; without it (the tests),
// labels are left out or shown as dots.
using TextOutlines = std::function<manifold::CrossSection(const QString& text, double capHeight)>;
void setTextOutlines(TextOutlines outlines);

// Metric threads for the thread option on cylinders: ISO sizes (name, diameter, coarse pitch).
struct ThreadSize {
  QString name;
  double diameter;
  double pitch;
};
const std::vector<ThreadSize>& threadSizes();
double coarsePitch(double diameter);  // the standard pitch of the nearest size
// A good starting play (radial clearance) for a printed threaded hole of this pitch: a quarter of
// the pitch, 0.1 to 0.25 mm. Fine threads are shallow, and more would leave nothing to grip.
double threadPlay(double pitch);

// A cylinder `length` long (centred on z = 0, along Z) with an ISO metric thread. internal: the
// cutter of a threaded hole instead, `clearance` wider all round, with a lead-in at both ends so
// the bolt finds the thread. Empty if it cannot be built.
manifold::Manifold threadedCylinder(double diameter, double pitch, double length, bool internal, double clearance);

}  // namespace gen
