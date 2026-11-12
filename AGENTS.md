# AGENTS.md

Notes for AI coding agents (Claude, Grok and others) working on DaedalCAD. Read this before
changing anything. The owner's word in the conversation wins over this file.

## What DaedalCAD is

A desktop 3D modeller for 3D printing, in the spirit of TinkerCAD, for Windows. C++20, Qt 6 for
the interface and the OpenGL view, Manifold for the solid geometry, optional OpenCASCADE for exact
STEP. See `README.md` for the features.

It is meant to be **fun**: lots of playful, useful features for people who print things. It is
**not** a "true" CAD program and **not** a slicer. When in doubt, choose what is simple to use over
what is technically complete.

## Rules

- **Version:** stays **0.9.8** (`project(DaedalCAD VERSION …)` in `CMakeLists.txt`) until the
  owner says otherwise. Do not bump it as part of a change.
- **Do not mention other CAD applications, aside from TinkerCAD**, anywhere: code, comments, docs,
  commit messages.
- **Branches:** `main` is pinned at the last finished state; do not commit to it. Work happens on
  the `0.9.8` branch, until the owner says otherwise.
- **Commits** go to that branch in small, focused steps, each with a clear message saying what
  changed and why. Sign them if your setup can (the owner wants them shown as Verified on GitHub).
  No attribution or session-link lines (such as Co-Authored-By): the author already shows who made it.
- **Pull before you start**, and rebase on top of what is there. Never force-push, rewrite history
  or delete branches or tags unless the owner asks for exactly that.
- **Tags and GitHub releases** are made by the owner.
- **Do not break what works.** Run the tests (below) before you push. If a change needs a test,
  add one.
- **Keep the way of working the same** for the user unless the change is about that: same menus,
  same tools, same shortcuts.
- **Old projects must keep opening.** A new field in the project file gets a default that gives the
  old behaviour when it is missing (see how `faceInnerRadius` is loaded in `mainwindow.cpp`).
- **No new third-party libraries** without asking. Everything shipped must be GPL-3.0 compatible
  and listed in `THIRD_PARTY_LICENSES.md`.

## Ideas the owner has ruled out

Do not suggest or build these:

- AMS / multi-colour painting or photo-to-colour relief (Bambu's own tools do this better)
- print checks such as thin walls or clearance (a slicer shows this better)
- slicer features, print cost or time estimates beyond what exists
- puzzle splits, cookie cutters or stamps, fidget toys, marble run pieces
- tabletop gaming generators
- assemblies or exploded views
- sharing or workflow extras such as generator presets or "surprise me"
- home or lighting generators

## Layout

| Where | What |
|---|---|
| `src/document.h/.cpp` | The scene: a flat list of `Node`s (shapes, holes, groups, text, 2D shapes, meshes, generators) |
| `src/evaluator.*` | Turns the scene into Manifold solids: holes, groups, fillets, bends; cached per node |
| `src/mainwindow.*` | The main window: menus, tools, panels, project save and load, help texts |
| `src/viewport.*` | The OpenGL view, picking, gizmos |
| `src/generators.*` | Parametric parts (boxes, Gridfinity, fasteners, gears, …) |
| `src/surfacefillet.*`, `edgefillet.*`, `fillet_geom.h` | Fillets |
| `src/sculpt*`, `lattice*`, `effects*`, `shape2d.*`, `textshape.*` | Sculpt window, lattices, textures and other effects, 2D shapes, text |
| `src/exporter.*`, `stepexport.*`, `exactstep.*`, `drawing*` | STL/OBJ/3MF/DXF, STEP (faceted and exact), technical drawings |
| `src/cloudsync.*` | CloudSync client for the DaedalSync server |
| `src/updater.*` | Updates from GitHub releases |
| `test/` | Regression tests |
| `webapp/` | DaedalSync, the Node.js project sync server (AGPL-3.0) |
| `appdata/` | Help images, icons, splash screen |
| `docs/` | `install.md` (building), `cloudsync.md` (CloudSync requirements) |
| `installer/`, `package.ps1` | Inno Setup installer and the release zip |
| `setup-windows.ps1`, `build-windows.bat` | One-step Windows setup and build |

## Building and testing

Windows: `build-windows.bat` (see `docs/install.md`). By hand, or on Linux with Qt 6.8:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDAEDALCAD_FILLET_TESTS=ON -DCMAKE_PREFIX_PATH=<Qt>
cmake --build build --parallel
ctest --test-dir build
```

`-DDAEDALCAD_FILLET_TESTS=ON` builds the tests. The `usage` test depends on timing and can fail on
a busy machine; run it again before worrying.

DaedalSync: `cd webapp && npm install && npm test`.

## Style

- Follow the code around you: naming, comments, `QStringLiteral` for UI text.
- Write user-facing text (labels, tooltips, help, messages) in plain, short English that says what
  happens. No jargon where an everyday word works.
- Comments explain why, not what.
- Sizes are in millimetres, Z is up.
