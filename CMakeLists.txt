cmake_minimum_required(VERSION 3.24)
project(DaedalCAD VERSION 0.9.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt looks for the Vulkan headers on every configure ("Could NOT find WrapVulkanHeaders"). DaedalCAD
# draws with OpenGL only, so do not look.
set(CMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets OpenGL Gui Network)

include(FetchContent)
# Reconfigure must not git-fetch again. Manifold stays in build/_deps after the first configure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
# Clipper2 is stored in the source tree. Manifold looks this up and skips the clone.
set(FETCHCONTENT_SOURCE_DIR_CLIPPER2 "${CMAKE_SOURCE_DIR}/third_party/Clipper2" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_Clipper2 "${CMAKE_SOURCE_DIR}/third_party/Clipper2" CACHE PATH "" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
# Parallel booleans: Manifold runs its heavy steps on every CPU core through oneTBB, which is
# fetched once next to Manifold and linked in statically, so there is no extra DLL to ship.
# -DDAEDALCAD_PARALLEL=OFF builds without it (one core, as before 0.9).
option(DAEDALCAD_PARALLEL "Use all CPU cores for booleans (oneTBB, linked statically)" ON)
set(MANIFOLD_PAR ${DAEDALCAD_PARALLEL} CACHE BOOL "" FORCE)
set(MANIFOLD_USE_BUILTIN_TBB ${DAEDALCAD_PARALLEL} CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DOWNLOADS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  manifold
  GIT_REPOSITORY https://github.com/elalish/manifold.git
  GIT_TAG v3.5.3
  GIT_SHALLOW TRUE
)
# Manifold and oneTBB warn their own developers that a static oneTBB is not their usual setup, and
# oneTBB still sets an old CMake policy. DaedalCAD links oneTBB statically on purpose (no extra DLL
# to ship), so keep those notes out of every configure. Everything else still prints, unchanged.
set(DAEDALCAD_QUIET_DEPENDENCIES ON)
function(message)
  set(mode "")
  set(first 0)
  if(ARGV0 MATCHES "^(FATAL_ERROR|SEND_ERROR|WARNING|AUTHOR_WARNING|DEPRECATION|NOTICE|STATUS|VERBOSE|DEBUG|TRACE|CHECK_START|CHECK_PASS|CHECK_FAIL|CONFIGURE_LOG)$")
    set(mode "${ARGV0}")
    set(first 1)
  endif()
  set(text "")
  math(EXPR last "${ARGC} - 1")
  if(first LESS_EQUAL last)
    foreach(i RANGE ${first} ${last})
      string(APPEND text "${ARGV${i}}")
    endforeach()
  endif()
  if(DAEDALCAD_QUIET_DEPENDENCIES AND mode MATCHES "WARNING" AND text MATCHES "TBB")
    return()
  endif()
  if(mode STREQUAL "")
    _message("${text}")
  else()
    _message(${mode} "${text}")
  endif()
endfunction()
set(CMAKE_WARN_DEPRECATED OFF)
FetchContent_MakeAvailable(manifold)
unset(CMAKE_WARN_DEPRECATED)
set(DAEDALCAD_QUIET_DEPENDENCIES OFF)

add_executable(DaedalCAD
  src/main.cpp
  src/appdirs.cpp
  src/updater.cpp
  src/splashoverlay.cpp
  src/document.cpp
  src/evaluator.cpp
  src/generators.cpp
  src/surfacefillet.cpp
  src/edgefillet.cpp
  src/textshape.cpp
  src/shape2d.cpp
  src/sculpt.cpp
  src/sculptdialog.cpp
  src/lattice.cpp
  src/latticedialog.cpp
  src/effects.cpp
  src/effectdialog.cpp
  src/cloudsync.cpp
  src/exporter.cpp
  src/stepexport.cpp
  src/exactstep.cpp
  src/drawing.cpp
  src/drawingdialog.cpp
  src/usage.cpp
  src/viewport.cpp src/pickindex.cpp
  src/mainwindow.cpp
)

target_include_directories(DaedalCAD PRIVATE src)
# Where Help > Check for updates looks by default: the latest release of this GitHub repository.
# A fork sets its own: -DDAEDALCAD_UPDATE_REPO=owner/name.
set(DAEDALCAD_UPDATE_REPO "Ruhnay/DaedalCAD" CACHE STRING "GitHub repository (owner/name) whose releases are the updates")
target_compile_definitions(DaedalCAD PRIVATE DAEDALCAD_VERSION="${PROJECT_VERSION}" DAEDALCAD_GITHUB_REPO="${DAEDALCAD_UPDATE_REPO}")

target_link_libraries(DaedalCAD PRIVATE
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  Qt6::Gui
  Qt6::Network
  manifold
)

# OpenCASCADE, for exact STEP export and STEP import. AUTO uses it when CMake finds it (vcpkg:
# "vcpkg install opencascade:x64-windows", then point CMAKE_PREFIX_PATH or OCCT_DIR at
# <vcpkg>\installed\x64-windows); without it DaedalCAD writes the faceted STEP and has no STEP import.
set(DAEDALCAD_WITH_OCCT AUTO CACHE STRING "Use OpenCASCADE for exact STEP: AUTO, ON or OFF")
set_property(CACHE DAEDALCAD_WITH_OCCT PROPERTY STRINGS AUTO ON OFF)
set(DAEDALCAD_OCCT_LIBS "")
if(NOT DAEDALCAD_WITH_OCCT STREQUAL "OFF")
  if(DEFINED ENV{OCCT_DIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{OCCT_DIR}")
  endif()
  find_package(OpenCASCADE CONFIG QUIET)
  if(OpenCASCADE_FOUND)
    set(DAEDALCAD_OCCT_LIBS TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKOffset
                            TKShHealing TKMesh TKXSBase TKCDF TKLCAF TKCAF TKXCAF)
    if(TARGET TKDESTEP)  # OpenCASCADE 7.8 and later: all STEP in one toolkit
      list(APPEND DAEDALCAD_OCCT_LIBS TKDESTEP)
    else()
      list(APPEND DAEDALCAD_OCCT_LIBS TKXDESTEP TKSTEP TKSTEPBase TKSTEP209 TKSTEPAttr)
    endif()
    # Only toolkits this OpenCASCADE has (a few were merged over the versions).
    set(_occt_wanted ${DAEDALCAD_OCCT_LIBS})
    set(DAEDALCAD_OCCT_LIBS "")
    foreach(_toolkit IN LISTS _occt_wanted)
      if(TARGET ${_toolkit})
        list(APPEND DAEDALCAD_OCCT_LIBS ${_toolkit})
      endif()
    endforeach()
    # The headers: where the package says, else wherever Standard.hxx is under the install.
    set(DAEDALCAD_OCCT_INCLUDE "${OpenCASCADE_INCLUDE_DIR}")
    if(NOT EXISTS "${DAEDALCAD_OCCT_INCLUDE}/Standard.hxx")
      find_path(DAEDALCAD_OCCT_INCLUDE_FOUND Standard.hxx PATH_SUFFIXES opencascade include/opencascade
                HINTS "${OpenCASCADE_INSTALL_PREFIX}" "${OpenCASCADE_DIR}/../.." "$ENV{OCCT_DIR}")
      set(DAEDALCAD_OCCT_INCLUDE "${DAEDALCAD_OCCT_INCLUDE_FOUND}")
    endif()
    message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: exact STEP export and STEP import")
  elseif(DAEDALCAD_WITH_OCCT STREQUAL "ON")
    message(FATAL_ERROR "OpenCASCADE not found. Install it (vcpkg install opencascade:x64-windows) and set OCCT_DIR or "
                        "CMAKE_PREFIX_PATH to it, or configure with -DDAEDALCAD_WITH_OCCT=OFF.")
  else()
    message(STATUS "OpenCASCADE not found: STEP export is faceted, no STEP import (see docs/install.md)")
  endif()
endif()
if(DAEDALCAD_OCCT_LIBS)
  target_compile_definitions(DaedalCAD PRIVATE DAEDALCAD_WITH_OCCT)
  target_include_directories(DaedalCAD SYSTEM PRIVATE ${DAEDALCAD_OCCT_INCLUDE})
  target_link_libraries(DaedalCAD PRIVATE ${DAEDALCAD_OCCT_LIBS})
  if(WIN32)
    # Its DLLs (and the ones they need: freetype, zlib...) next to DaedalCAD.exe, so it starts
    # and package.ps1 ships them.
    # The folder TKernel.dll is in (with vcpkg that also holds freetype, zlib and the rest), and
    # the folders the package names.
    set(_occt_bins "${OpenCASCADE_BINARY_DIR}")
    if(OpenCASCADE_INSTALL_PREFIX)
      list(APPEND _occt_bins "${OpenCASCADE_INSTALL_PREFIX}/bin")
    endif()
    if(DEFINED ENV{OCCT_DIR})
      list(APPEND _occt_bins "$ENV{OCCT_DIR}/bin")
    endif()
    foreach(_property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION)
      get_target_property(_kernel TKernel ${_property})
      if(_kernel)
        get_filename_component(_kernel_dir "${_kernel}" DIRECTORY)
        list(APPEND _occt_bins "${_kernel_dir}")
        break()
      endif()
    endforeach()
    set(_occt_dlls "")
    foreach(_dir IN LISTS _occt_bins)
      file(GLOB _found "${_dir}/*.dll")
      list(APPEND _occt_dlls ${_found})
    endforeach()
    list(REMOVE_DUPLICATES _occt_dlls)
    list(FILTER _occt_dlls EXCLUDE REGEX "[Qq]t6")
    if(_occt_dlls)
      add_custom_command(TARGET DaedalCAD POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_occt_dlls} "$<TARGET_FILE_DIR:DaedalCAD>"
        COMMENT "Copy the OpenCASCADE DLLs next to DaedalCAD.exe")
    endif()
    # vcpkg keeps each library's license as share/<name>/copyright: they go along in licenses\.
    if(_kernel_dir)
      file(GLOB _copyrights "${_kernel_dir}/../share/*/copyright")
      foreach(_copyright IN LISTS _copyrights)
        get_filename_component(_port_dir "${_copyright}" DIRECTORY)
        get_filename_component(_port "${_port_dir}" NAME)
        if(_port MATCHES "^vcpkg")
          continue()
        endif()
        add_custom_command(TARGET DaedalCAD POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DaedalCAD>/licenses"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_copyright}" "$<TARGET_FILE_DIR:DaedalCAD>/licenses/${_port}.txt")
      endforeach()
    endif()
  endif()
endif()

if(WIN32)
  target_sources(DaedalCAD PRIVATE "${CMAKE_SOURCE_DIR}/appdata/icons/application.rc")
  # Publisher, product name and version in the exe (Explorer > Properties > Details).
  # Change the publisher with: cmake -DDAEDALCAD_PUBLISHER="Your Name"
  set(DAEDALCAD_PUBLISHER "DaedalCAD" CACHE STRING "Publisher name written into DaedalCAD.exe")
  string(TIMESTAMP DAEDALCAD_YEAR "%Y")
  configure_file("${CMAKE_SOURCE_DIR}/src/version.rc.in" "${CMAKE_BINARY_DIR}/version.rc" @ONLY)
  target_sources(DaedalCAD PRIVATE "${CMAKE_BINARY_DIR}/version.rc")
endif()

if(MSVC)
  target_compile_options(DaedalCAD PRIVATE /W3 /bigobj /EHsc)
  set_property(TARGET DaedalCAD PROPERTY WIN32_EXECUTABLE TRUE)
else()
  target_compile_options(DaedalCAD PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_custom_command(TARGET DaedalCAD POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/appdata"
          "$<TARGET_FILE_DIR:DaedalCAD>/appdata"
  COMMENT "Copy splash, icon, and toolbar icons next to DaedalCAD.exe")

# Surface fillet regression tests. Off by default; turn on with -DDAEDALCAD_FILLET_TESTS=ON and run
# `ctest -C Release` from the build folder. fillet_manifold_test runs the real Manifold kernel the way
# the app does and checks the finished fillet against a true rolling-ball fillet, drafted walls too.
option(DAEDALCAD_FILLET_TESTS "Build the surface fillet regression tests" OFF)
if(DAEDALCAD_FILLET_TESTS)
  enable_testing()
  add_executable(fillet_geom_test test/fillet_geom_test.cpp)
  add_executable(fillet_sweep_test test/sweep_test.cpp)
  add_executable(fillet_manifold_test test/manifold_fillet_test.cpp src/surfacefillet.cpp)
  target_include_directories(fillet_manifold_test PRIVATE src)
  target_link_libraries(fillet_manifold_test PRIVATE manifold)
  add_test(NAME fillet_geom COMMAND fillet_geom_test)
  add_test(NAME fillet_sweep COMMAND fillet_sweep_test)
  add_test(NAME fillet_manifold COMMAND fillet_manifold_test)
  add_executable(edgefillet_test test/edgefillet_test.cpp src/edgefillet.cpp)
  target_include_directories(edgefillet_test PRIVATE src)
  target_link_libraries(edgefillet_test PRIVATE manifold)
  add_test(NAME edgefillet COMMAND edgefillet_test)
  # Text shapes: needs fonts, so it runs Qt without a window.
  add_executable(text_test test/text_test.cpp src/textshape.cpp)
  target_include_directories(text_test PRIVATE src)
  target_link_libraries(text_test PRIVATE manifold Qt6::Gui)
  add_test(NAME text COMMAND text_test)
  set_tests_properties(text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # 2D shape editor: draws, measures, drags, undoes and saves through the real dialog.
  add_executable(shape2d_test test/shape2d_test.cpp src/shape2d.cpp)
  target_include_directories(shape2d_test PRIVATE src)
  target_link_libraries(shape2d_test PRIVATE Qt6::Widgets)
  add_test(NAME shape2d COMMAND shape2d_test)
  set_tests_properties(shape2d PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # Grouping precision: holes and solids a hair off the plate still cut and merge cleanly.
  add_executable(group_precision_test test/group_precision_test.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp
                 src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(group_precision_test PRIVATE src)
  target_link_libraries(group_precision_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME group_precision COMMAND group_precision_test)
  set_tests_properties(group_precision PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # Generators: every generator builds, and parts that go together (box and lid, bolt and nut, gears) fit.
  add_executable(sculpt_test test/sculpt_test.cpp src/sculpt.cpp)
  target_include_directories(sculpt_test PRIVATE src)
  target_link_libraries(sculpt_test PRIVATE manifold)
  add_test(NAME sculpt COMMAND sculpt_test)

  add_executable(lattice_test test/lattice_test.cpp src/lattice.cpp src/sculpt.cpp)
  target_include_directories(lattice_test PRIVATE src)
  target_link_libraries(lattice_test PRIVATE manifold)
  add_test(NAME lattice COMMAND lattice_test)
  add_executable(effects_test test/effects_test.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp)
  target_include_directories(effects_test PRIVATE src)
  target_link_libraries(effects_test PRIVATE manifold)
  add_test(NAME effects COMMAND effects_test)

  add_executable(generator_test test/generator_test.cpp src/generators.cpp)
  target_include_directories(generator_test PRIVATE src)
  target_link_libraries(generator_test PRIVATE manifold Qt6::Gui)
  add_test(NAME generators COMMAND generator_test)
  # Export: OBJ, 3MF and DXF keep parts apart, loose holes still cut, DXF is valid R12, slices are not mirrored.
  add_executable(export_test test/export_test.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp src/exporter.cpp
                 src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(export_test PRIVATE src)
  target_link_libraries(export_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME export COMMAND export_test)
  set_tests_properties(export PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # Exact STEP: written with OpenCASCADE, read back with it (only when DaedalCAD is built with it).
  if(DAEDALCAD_OCCT_LIBS)
    add_executable(exact_test test/exact_test.cpp src/exactstep.cpp src/stepexport.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp
                   src/generators.cpp src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
    target_include_directories(exact_test PRIVATE src)
    target_include_directories(exact_test SYSTEM PRIVATE ${DAEDALCAD_OCCT_INCLUDE})
    target_compile_definitions(exact_test PRIVATE DAEDALCAD_WITH_OCCT)
    target_link_libraries(exact_test PRIVATE manifold Qt6::Widgets ${DAEDALCAD_OCCT_LIBS})
    add_test(NAME exact_step COMMAND exact_test)
    set_tests_properties(exact_step PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
  # Modelling: intersection, hollow, multi-colour groups and the colours in 3MF and OBJ.
  # Timing the heavy geometry work, to compare compiler settings (not run as a test).
  option(DAEDALCAD_BENCHMARK "Build daedal_benchmark" OFF)
  if(DAEDALCAD_BENCHMARK)
    add_executable(daedal_benchmark tools/benchmark.cpp src/pickindex.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp
                   src/sculpt.cpp src/generators.cpp src/exporter.cpp src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
    target_include_directories(daedal_benchmark PRIVATE src)
    target_link_libraries(daedal_benchmark PRIVATE manifold Qt6::Widgets)
  endif()
  add_executable(pickindex_test test/pickindex_test.cpp src/pickindex.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp src/exporter.cpp
                 src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(pickindex_test PRIVATE src)
  target_link_libraries(pickindex_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME pickindex_test COMMAND pickindex_test)
  add_executable(modelling_test test/modelling_test.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp src/exporter.cpp
                 src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(modelling_test PRIVATE src)
  target_link_libraries(modelling_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME modelling COMMAND modelling_test)
  set_tests_properties(modelling PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # STEP: read back, every face flat, every edge used twice, the faces enclose the part's volume.
  add_executable(step_test test/step_test.cpp src/stepexport.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp
                 src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(step_test PRIVATE src)
  target_link_libraries(step_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME step COMMAND step_test)
  set_tests_properties(step PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  # Big groups: drawn without merging, built side by side, twists refined along their length.
  add_executable(group_speed_test test/group_speed_test.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp
                 src/exporter.cpp src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(group_speed_test PRIVATE src)
  target_link_libraries(group_speed_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME group_speed COMMAND group_speed_test)
  set_tests_properties(group_speed PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  # Technical drawings: views, hidden lines, holes, dimensions, filament estimate, files.
  add_executable(drawing_test test/drawing_test.cpp src/drawing.cpp src/document.cpp src/evaluator.cpp src/effects.cpp src/lattice.cpp src/sculpt.cpp src/generators.cpp
                 src/exporter.cpp src/surfacefillet.cpp src/edgefillet.cpp src/textshape.cpp src/shape2d.cpp)
  target_include_directories(drawing_test PRIVATE src)
  target_link_libraries(drawing_test PRIVATE manifold Qt6::Widgets)
  add_test(NAME drawing COMMAND drawing_test)
  set_tests_properties(drawing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  # The CPU and memory readout.
  add_executable(usage_test test/usage_test.cpp src/usage.cpp)
  target_include_directories(usage_test PRIVATE src)
  add_test(NAME usage COMMAND usage_test)

  # The rename from ShapeSynth: settings, data folder and old project files carried over.
  add_executable(rename_test test/rename_test.cpp src/appdirs.cpp)
  target_include_directories(rename_test PRIVATE src)
  target_link_libraries(rename_test PRIVATE Qt6::Core)
  add_test(NAME rename COMMAND rename_test)
endif()
