# DaedalCAD

DaedalCAD is a desktop 3D modeller for 3D printing, in the spirit of TinkerCAD:
you build parts from simple shapes, cut holes with other shapes, group them, round the edges and
export a file for your slicer. It runs locally as a native Windows program, with no browser, no
account and no internet connection needed. Everything is in millimetres, with Z pointing up.

It is written in C++20 on [Qt 6](https://www.qt.io/) (window, widgets, OpenGL view) and
[Manifold](https://github.com/elalish/manifold) (the solid geometry kernel), and it is free
software under the GPL-3.0.

The name comes from Daedalus, the master craftsman of Greek myth, and "daedal": skilfully and
intricately made. Up to version 0.9 it was called ShapeSynth.

## What you can do with it

### Model

- **Shapes:** cube, round box, sphere, dome, cylinder, cone, tube, capsule, torus, diamond (pointed, or a cut gem with a flat top), heart, hex prism, pyramid, wedge, roof and star (pointed or flat, any number of points). Drag one onto the workplane or click it.
- **Holes:** any shape can be a hole. Holes cut solids in the same group; the bed itself counts as a group. A hole can have a clearance, so it cuts slightly wider than drawn and a part of the same size fits into it. The clearance is off by default; you can use the printer clearance from Settings or give the hole its own value, optionally on the sides only.
- **Arrange:**
  - Move, rotate and scale with the gizmo or by typing numbers in the inspector.
  - **Lay flat:** click a face and the part turns so that face lies on the plate.
  - **Workplane:** click any face, slanted ones too, and new parts stand on it, upright to it.
  - **Section view:** cut the view open along X, Y or Z with a slider to see inside walls, holes and seals; cut faces show hatched.
  - **Overhangs:** faces that hang over too steeply to print without support show striped red (the angle is in Settings).
  - Mirror, align (pick the parts, then click a dot, like TinkerCAD), measure, and drop parts onto the bed or onto another part.
  - Repeat: makes copies that step by a move, a spin and a scale each time.
  - Group and ungroup, with undo and redo for everything.
  - **Merge** makes a group one solid part: drawn and picked as its finished shape, its parts hidden. Holes grouped with it go inside it, and Unmerge brings every part back as it was. Exports still use the parts, so STEP keeps exact surfaces.
  - Hull: shrink-wraps the selected parts into one smooth solid (two spheres make a capsule), or as a chain from each part to the next for bars and paths. The parts stay editable inside it.
  - **Intersect:** keeps only what the selected parts have in common, as an editable group.
  - **Split:** cuts a part in two with a level or upright plane, or along the workplane's face; each half stays an editable group. Optional connectors line the halves up again: pin holes with loose pins, pegs on one half, or a sliding dovetail, all fitted with the printer clearance.
- **Shape tools:**
  - **Fillet** rounds outside corners; **Fillet surface** rounds the edges of a face you click, such as a pocket or a tube rim.
  - **Detected Edge Filleting (Beta)** finds the sharp edges of any object, including merged groups, STL files and text, and rounds or chamfers the ones you pick.
  - **Hollow** makes a part or group a shell with even walls, closed or open on the sides you choose (cups, boxes, sleeves).
  - **Lattice** turns a part into a Voronoi skin, a gyroid, a honeycomb or a Voronoi lattice, open or with a solid skin: light, striking and printable, with a preview first. It builds in the background with a progress bar (and a Stop button), on every CPU core, and smooths the result onto the true surface with rounded edges and no self-intersections, so slicers take it as is.
  - **Texture** puts knurling, ribs, hexagons, dots, wood grain, leather, a carbon weave or your own picture on a part as real geometry, raised or cut in, wrapped from every side or round the part, leaving the bottom flat on the plate.
  - **Wrap** puts text or a logo onto a curved part (round a mug, a pen holder, a ball, or straight onto a surface), raised as a part of its own or cut in.
  - **Articulate** cuts a part into segments with print-in-place joints: a flexi dragon, snake or fish that prints flat in one go and wiggles off the plate.
  - **Simplify** gives a heavy sculpt or STL far fewer triangles for the same shape (within a tolerance you set, 0.02 mm by default).
  - **Restyle** rebuilds a part low-poly (big flat facets, optionally nudged for a hand-cut look) or from voxel cubes.
  - **Smooth join** (a group setting) makes grouped parts flow into each other like clay, with a soft blend where they meet; holes cut with the same soft edge.
  - **Bend** wraps a part into an arc. **Bend face** and **Extrude face** work on a single face.
- **Sculpt:** a window for freeform sculpting, like Blender's Sculpt Mode. Start from a ball, cube, cylinder, slab or capsule, or sculpt the selected part. Brushes: Draw, Clay, Layer, Inflate, Blob, Crease, Smooth, Flatten, Fill, Scrape, Grab, Snake hook, Pinch, Nudge, Rotate, Mask (protect areas from the other brushes) and Stamp (press in or raise a picture: rivets, scales, knurling, a logo of your own), with Ctrl for the other way (dig, carve), Shift to smooth, mirror on X/Y/Z, radial symmetry (2 to 16 copies around an axis) and undo. The surface always stays a closed, printable solid (a stroke that would push it through itself is held back), and Remesh rebuilds it as even triangles at the detail you choose. Done replaces the selected part or adds the sculpt beside it; double-click a sculpted part to sculpt it again. **Save** keeps a sculpt in the Sculpts library to place or keep working on later, apart from any project.
- **Threads:** any cylinder can be an ISO metric thread (M2 to M36): a threaded rod, or as a hole a threaded hole a bolt of that size screws into.
- **Text** in any font installed on the computer: style, size, letter spacing, alignment and how smooth the curves are.

### Libraries

- **Parts:** your own parts, plus STL files you import.
- **Sculpts:** sculpts you saved from the Sculpt window, with a picture of each. Click or drag one onto the bed to place a copy, double-click to keep sculpting it.
- **2D shapes:** a 2D editor for outlines with nodes and curve handles, rectangles, circles and polygons, holes and layers. The result is extruded into a solid. Parts on the bed keep their own copy, and DaedalCAD offers to update them when you change the shape in the library.
- **Generators:** parametric parts that keep their settings, so you can change them after placing them. **Convert to parts** turns a generator into ordinary parts. The generators:
  - Box with lid (lip, snap, sliding in grooves or over rails with an optional snap lock, or sealed with an O-ring), Gridfinity bins and baseplates, a screw-top jar.
  - Electronics enclosure, battery holder (AA to D, 18650, 21700, coin cells, 9 V).
  - Watertight options for the box, the enclosure and the jar: an O-ring groove in the top of the walls, a tongue on the lid that squeezes the ring by a set amount, screws outside the seal (in ears, or inside the wall with the groove curving round them so the sides stay flat), and an optional gasket to print in TPU.
  - Metric bolts, nuts and standoffs.
  - Spur, helical and herringbone gears, rack and pinion, a print-in-place hinge, a snap-in ball joint.
  - GT2, V-belt and plain pulleys, bearing housings (flange or pillow block), shaft couplers and collars, coil and flat springs, a snap-fit latch, a living hinge, a print-in-place cable chain, and tube connectors for round or square tube.
  - Workshop: bit and tool holders (hex bits, round tools, a labelled drill index, SD and microSD cards), drawer dividers that slot together, cable clips, a desk grommet, cable labels, name tags or keychains, dice (d4 to d20, numbers, pips, your own symbols or a picture, cut in, raised or as a two-colour inlay), and lithophanes or photo reliefs from your own picture (a panel, a framed panel, a keychain or a round medallion).
  - A hose adapter, a pattern panel with hexagon, circle, square, diamond, triangle, star or slot holes, and a pattern tube with the same holes all the way round a straight or tapered tube (vases, pots, lamp shades). Both can make the holes only, as a hole shape, to cut the pattern into another part.
  - Testing and calibration: a clearance fit test, a hole size test, a thread fit test, and an overhang and bridge test.

  Parts that fit together can use your printer clearance, and that is optional per generator.

- **Standard parts:** ISO and DIN fasteners at their real sizes (M2 to M24), each one also as its matching hole:
  - Screws: socket head (ISO 4762 / DIN 912), button head (ISO 7380), countersunk (ISO 10642, DIN 7991), hex head (ISO 4017, DIN 933), pan head (ISO 7045) and set screws (ISO 4026).
  - Nuts: hex (ISO 4032, DIN 934, thin ISO 4035), nylon locking (DIN 985), flange (DIN 6923) and square (DIN 562).
  - Washers (ISO 7089 / DIN 125, ISO 7093, spring DIN 127), dowel pins (ISO 2338 / 8734), threaded rod (DIN 975) and heat-set inserts.
  - Threads plain (fast) or modelled.
  - Holes: ISO 273 clearance (fine, medium, coarse) with a counterbore or countersink so heads sit flush, tapped holes for set screws, nut traps, washer recesses, pin holes and insert pockets.
  - **Add matching hole** puts the hole exactly where the fastener is and groups it with the part it sits in, so it cuts only that part.

### Export and share

- **STL** writes one merged mesh. **OBJ** and **3MF** keep every part as its own named object, in its colour. **DXF** puts each part on its own layer.
- **Multi-colour:** a group with **Keep part colours** shows and exports its parts' colours, one closed piece per colour; in a 3MF they form one model with a part per colour, ready to give each its own filament.
- **STEP** for Fusion, FreeCAD, SolidWorks and other CAD programs: every part a closed, named, coloured solid in millimetres. With OpenCASCADE (see docs/install.md) boxes, cylinders, cones, tubes, spheres, domes, rings and capsules, and the holes, groups, intersections, repeats and closed hollows made from them, are true surfaces, and so are rounded and chamfered edges (a shape's own Fillet and Edge fillet picks, on shapes and groups), text (the font's own curves), shapes from the 2D editor and threads (helical sweeps); bends and twists become smooth spline surfaces; in generated parts and imported STLs the cylinders and cones their triangles describe are recognised and made exact, and the rest goes in as flat faces inside the same solid. Without OpenCASCADE the whole file is faceted: flat areas as single faces with shared edges, round surfaces as bands of narrow flat faces.
- **File > Import STEP** (with OpenCASCADE) puts every solid of a STEP file on the plate with its names and colours; a STEP export writes them exactly again.
- **Export slice** writes a flat DXF of one cut through the model: at a height you choose, or at a face you click. It is meant for laser cutters and CAD programs.
- **Technical drawing** (File menu) puts the selection, or the whole plate, on an A4, A3 or A2 sheet:
  - Front, top and side views in first-angle (ISO) or third-angle projection, plus an isometric view.
  - Hidden edges dashed; round holes and pegs drawn as true circles with centre marks.
  - Dimensions: overall sizes, and the diameter and position of every round hole seen square on.
  - A title block, and a filament estimate in the top-left corner: weight, length and cost from the material, infill, wall thickness and price per kg.
  - A scale too big for the paper adds sheets: the whole part at a scale that fits first, then each view at your scale, in overlapping pieces if one sheet is too small.
  - Saved as PDF (all sheets in one file), SVG, DXF (on layers) or PNG.
- **View:** a view cube with arrows, a **Home** view of the whole plate, perspective or **orthographic**, and an isometric view.
- **Keyboard:** every tool, menu command and view has a key you can change in **Settings › Keyboard** (with a search box and a reset to defaults).
- **Projects** save as a folder with a `daedalcad.json`. **Project Files** exports one portable `.daedal`.
- **CloudSync** (optional) backs up your projects to a DaedalSync server and shares designs with other people on that server. Those people can send update requests for a design, and its author accepts or declines them.
- **Updates:** when an update server is set, the app checks it for new versions and installs them.

## Where your files live

DaedalCAD keeps the program and your own work apart:

| What | Installed with the setup | Portable (unzipped) copy |
|---|---|---|
| Program, icons, help pictures | `%LOCALAPPDATA%\Programs\DaedalCAD` | The folder you unzipped |
| Projects, part and 2D shape libraries, imported and placed 2D shapes | `Documents\DaedalCAD` | `appdata\` next to `DaedalCAD.exe` |
| Settings (printer clearance, CloudSync, recent projects) | Registry, `HKEY_CURRENT_USER\Software\DaedalCAD\DaedalCAD` | Same |

Installing, updating or uninstalling never touches `Documents\DaedalCAD`. Your data is in these
folders:

- `projects`: your projects.
- `parts`: the parts library.
- `2d_shapes`: the 2D shape library.
- `Imported`: imported 2D shapes.
- `sculpts`: sculpted parts placed in the workspace (a project keeps its own copy when you save it).
- `sculpt_library`: the Sculpts library (an STL and a picture per sculpt).
- `placed_2d`: the copies kept by 2D shapes placed on the bed.

Coming from ShapeSynth: on its first start DaedalCAD copies ShapeSynth's settings and moves
`Documents\ShapeSynth` to `Documents\DaedalCAD`. Old projects (`shapesynth.json`, `.shapezip`) and
2D shapes still open, and are saved in the new format from then on.

## What lives where in this repository

```text
DaedalCAD/
├── src/                     The desktop program
│   ├── main.cpp             Start-up
│   ├── mainwindow.*         The main window: tool bar, libraries, inspector, dialogs, save/load, export menus
│   ├── viewport.*           The 3D view: OpenGL drawing, picking, gizmo, camera
│   ├── pickindex.*          Box tree and edge neighbours for picking on big meshes
│   ├── document.*           The scene: nodes, groups, holes and their settings
│   ├── evaluator.*          Turns the scene into solids with Manifold: shapes, holes, groups, fillets, bends, caches
│   ├── generators.*         The parametric generators (boxes, bolts, gears, hinges, ...)
│   ├── surfacefillet.*      Surface fillets (with fillet_geom.h, which holds the pure geometry)
│   ├── edgefillet.*         Edge fillets (Beta)
│   ├── textshape.*          Font outlines to flat shapes for 3D text
│   ├── shape2d.*            The 2D shape editor
│   ├── sculpt.*             Sculpting: brushes, the guard against self-intersection, voxel remesh
│   ├── sculptdialog.*       The Sculpt window
│   ├── lattice.*            Lattices: Voronoi skin, gyroid, honeycomb, Voronoi struts as distance fields
│   ├── latticedialog.*      The Lattice window
│   ├── effects.*            Textures, the smooth join, low-poly and voxel restyles
│   ├── effectdialog.*       The window for Texture and Restyle
│   ├── exporter.*           STL, OBJ, 3MF and DXF writers
│   ├── stepexport.*         STEP writer (faceted solids)
│   ├── exactstep.*          Exact STEP export and STEP import (OpenCASCADE)
│   ├── drawing.*            Technical drawings: views, hidden lines, circles, dimensions, filament estimate, PDF/SVG/DXF/PNG
│   ├── drawingdialog.*      The Technical drawing window
│   ├── cloudsync.*          The CloudSync window and client
│   ├── updater.*            Update checks and installs
│   ├── nethost.h            Which server addresses may use http (local network) and which need https
│   ├── appdirs.*            Where program files and user data are kept
│   └── splashoverlay.*      The start-up picture
├── test/                    Automated tests: fillets, text, 2D editor, sculpting, lattices, grouping precision, generators, export, STEP
├── appdata/                 Files shipped with the program
│   ├── icons/               Tool and shape icons (SVG and PNG) and the program icon
│   ├── help/                Pictures for the Help pages (names in help/README.txt)
│   └── splash.png
├── webapp/                  DaedalSync: the Node.js server for CloudSync and update downloads (see webapp/README.md)
├── docs/
│   ├── install.md           Building, packaging and the installer on Windows
│   └── cloudsync.md         How CloudSync, public projects and update requests behave, and the server API
├── installer/               Inno Setup script for the Windows installer
├── third_party/Clipper2/    2D polygon library used by Manifold (kept here so builds don't download it)
├── tools/                   Python scripts that draw the icons
├── package.ps1              Makes the release zip and installer
├── build-windows.bat        One-step build with Visual Studio 2022
├── CMakeLists.txt           The build: the program, Manifold and the tests
├── LICENSE                  GPL-3.0
└── THIRD_PARTY_LICENSES.md  Qt, Manifold, Clipper2 and the others DaedalCAD builds on
```

Manifold is not stored in the repository: CMake downloads the pinned version (3.5.3) the first
time you configure the build. Building is described in [docs/install.md](docs/install.md).

## How it works, briefly

The scene is a flat list of nodes: shapes, holes, groups, text, 2D shapes, imported meshes and
generators. The evaluator turns each node into a Manifold solid, then subtracts holes and
merges groups. It applies fillets, bends and edge fillets afterwards, and caches every result so
that only what you changed is rebuilt. The view draws those solids, and export writes the same
solids out. Heavy work runs on a background thread while the window keeps drawing, and Manifold
spreads its booleans over all CPU cores. A group that only puts parts together (no holes inside,
nothing done to the group itself) is drawn straight from its parts, so grouping many overlapping
parts is instant; the real merge happens on export. The status bar shows how much CPU and memory DaedalCAD itself is
using, on the left of the messages.

Fillets are exact-radius, which makes them deliberately slow. Around 32 curve segments and 12
fillet segments is a good balance. If a fillet is bigger than the part, the part stays sharp.

## License

DaedalCAD is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE). Forks and changes are welcome; anything you
distribute that is built from this code must be shared under the same license, with its source.

The DaedalSync server in `webapp/` is licensed under the GNU Affero General Public License,
version 3. If you run a changed version of it for other people, you share those changes too.

Qt is used under the LGPL-3.0, Manifold under Apache-2.0 and Clipper2 under the Boost Software
License. Details and full texts are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
Gridfinity is Zack Freedman's design, used under the MIT license.
