# DaedalCAD

DaedalCAD is a desktop 3D modeller for 3D printing, in the same spirit as TinkerCAD. You build parts out of simple shapes, cut holes with other shapes, group things, round the edges, and export a file for your slicer. It runs as a native Windows program — no browser, no account, no internet needed. Everything is in millimetres, Z up.

It’s written in C++20 with Qt 6 for the interface and OpenGL view, and Manifold for the solid geometry. There’s also an optional OpenCASCADE build option for exact STEP export and import; that version is included in the normal release. The project is free software under the GPL-3.0.

The name comes from Daedalus and the word “daedal” (skilfully and intricately made). Up to version 0.9 it was called ShapeSynth.

## Modelling

You get the usual shapes: cube, rounded box, sphere, dome, cylinder, cone, tube, capsule, torus, diamond (pointed or cut gem), heart, hex prism, pyramid, wedge, roof, and star (pointed or flat, any number of points). Drag one onto the workplane or click it.

Any shape can be a hole. Holes only cut solids in the same group (the bed itself counts as a group). You can give a hole clearance so it cuts slightly wider and a matching part fits; clearance is off by default, but you can use the printer clearance from Settings or set it per hole (optionally sides only).

Moving, rotating and scaling works with the gizmo or by typing numbers. Lay flat turns a clicked face onto the plate. You can set a workplane on any face (even slanted ones) so new parts stand upright on it. Section view cuts the view open along X, Y or Z so you can see inside. Overhangs that are too steep show striped red.

There’s mirror, align (pick parts then click a dot), measure, and drop-to-bed or drop-onto-part. Repeat makes stepped copies with a move, spin and scale each time. Grouping and ungrouping are there with full undo/redo.

Merge turns a group into one solid part so it draws and picks as the finished shape. Holes grouped with it stay inside it; Unmerge brings everything back. Hull shrink-wraps selected parts into one solid (or as a chain), and Intersect keeps only the overlapping volume as an editable group. Split cuts a part with a plane; you can optionally add connectors (pin holes, pegs, or a sliding dovetail) that use the printer clearance.

## Shape tools

Fillet rounds outside corners. Fillet surface rounds the edges of a face you click (pocket rims, tube ends, etc.). Detected Edge Filleting (still beta) finds sharp edges on anything — including merged groups, STLs and text — and lets you round or chamfer the ones you pick.

Hollow turns a part or group into a shell with even walls. Lattice turns something into a Voronoi skin, gyroid, honeycomb or Voronoi lattice (open or with a solid skin). It builds in the background on all cores, shows a progress bar with a Stop button, and smooths the result onto the true surface so slicers accept it.

Texture applies knurling, ribs, hexagons, dots, wood grain, leather, carbon weave or your own picture as real geometry, raised or cut in, wrapped around or from every side, leaving the bottom flat. Wrap puts text or a logo onto a curved surface. Articulate cuts a part into segments with print-in-place joints so you can print a flexi dragon or snake flat.

Simplify reduces triangle count on heavy sculpts or STLs within a tolerance you set. Restyle rebuilds something low-poly or from voxel cubes. Smooth join is a group setting that makes parts blend into each other like clay. Bend wraps a part into an arc; there are also Bend face and Extrude face tools.

## Sculpting

There’s a separate Sculpt window (similar to Blender’s Sculpt Mode). Start from a ball, cube, cylinder, slab or capsule, or sculpt an existing part. Brushes include Draw, Clay, Layer, Inflate, Blob, Crease, Smooth, Flatten, Fill, Scrape, Grab, Snake hook, Pinch, Nudge, Rotate, Mask and Stamp. Ctrl reverses the brush, Shift smooths, and you can mirror or use radial symmetry. The surface stays a closed solid — strokes that would push it through itself are held back. Remesh rebuilds even triangles at the detail you want. When you’re done you can replace the selected part or add the sculpt next to it; double-click a sculpted part to keep working on it. You can also save sculpts to a library.

Any cylinder can be an ISO metric thread (M2–M36), either as a threaded rod or as a threaded hole. Text uses any font installed on the computer, with style, size, spacing, alignment and curve quality.

## Libraries

- Your own parts, plus imported STLs.
- Sculpts you saved from the Sculpt window.
- 2D shapes (outlines with nodes, rectangles, circles, polygons, holes and layers) that extrude into solids. Placed shapes keep their own copy and can be updated when you change the library version.
- Generators — parametric parts that keep their settings. These include boxes with lids (various snap and seal options), Gridfinity bins and baseplates, screw-top jars, electronics enclosures, battery holders, watertight options with O-ring grooves, metric bolts/nuts/standoffs, spur/helical/herringbone gears, rack and pinion, hinges, ball joints, pulleys, bearing housings, shaft couplers, springs, latches, living hinges, cable chains, tube connectors, bit/tool holders, drawer dividers, cable clips, dice, lithophanes, hose adapters, pattern panels and tubes, and various fit/test pieces. Parts that fit together can optionally use your printer clearance.

There are also standard parts: ISO and DIN fasteners at real sizes (M2–M24), each available as the matching hole. These include various screw heads, nuts, washers, dowel pins, threaded rod and heat-set inserts. Threads can be plain (fast) or modelled. Holes include clearance sizes with counterbore/countersink, tapped holes, nut traps, etc. “Add matching hole” places the hole where the fastener is and groups it with the part it cuts.

## Export and other features

STL writes one merged mesh. OBJ and 3MF keep every part as its own named, coloured object. DXF puts each part on its own layer. Groups can keep part colours for multi-colour printing.

STEP export is available for other CAD programs. With OpenCASCADE (included in the normal release) many shapes, fillets, text, 2D editor shapes, threads, bends and recognised cylinders/cones from generators or STLs become true surfaces; without it the file is faceted. You can also import STEP files. Export slice writes a flat DXF of one cut. Technical drawing produces front/top/side/isometric views (first- or third-angle), hidden lines, dimensions, a title block and a filament estimate, saved as PDF, SVG, DXF or PNG.

The view has a view cube, Home, perspective or orthographic, and isometric. Every tool and command has a changeable keyboard shortcut. Projects save as a folder with a daedalcad.json, or you can export a portable .daedal file.

CloudSync is optional — it backs up projects to a DaedalSync server and lets you share designs. The app can also check an update server and install new versions.

## How it works

The scene is a flat list of nodes (shapes, holes, groups, text, 2D shapes, imported meshes, generators). The evaluator turns each into a Manifold solid, subtracts holes and merges groups, then applies fillets, bends and edge fillets. Results are cached so only changed parts rebuild. Heavy work runs in the background while the window stays responsive, and Manifold uses all cores for booleans. Groups that only assemble parts (no holes or group-level operations) draw straight from their parts, so grouping many overlapping pieces is instant; the real merge happens on export. The status bar shows the app’s own CPU and memory use.

Fillets are exact-radius, which makes them deliberately slow. Around 32 curve segments and 12 fillet segments is a reasonable balance. If a fillet is bigger than the part, it stays sharp.

## License

DaedalCAD is free software under the GNU General Public License version 3. Forks and changes are welcome; anything you distribute that is built from this code must be shared under the same license with its source.

The DaedalSync server (in webapp/) is under the AGPL-3.0. Qt is used under the LGPL-3.0, Manifold under Apache-2.0, and Clipper2 under the Boost Software License. Gridfinity is Zack Freedman’s design under the MIT license.
