cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(Hasegawa CXX)

# VSTGUI on macOS compiles Objective-C++ sources.
if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

# pffft is a static lib linked into the (shared) plugin module; on Linux that
# requires position-independent code or the link fails with "recompile with
# -fPIC".
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Max/MSP externals link the static CRT (/MT) on Windows, as the official
# max-sdk-base requires. Set it for the whole addon -- before Avendish and the
# object library are created -- so every target shares one runtime; mixing /MT
# and /MD trips MSVC with LNK2038. (CMP0091 is NEW via cmake_minimum_required.)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The VSTGUI editor uses RTTI + exceptions (dynamic_cast, throw); avnd strips
# both by default, which makes VSTGUI's dynamic_cast crash. Keep them on. Must
# be set before Avendish is configured (its DisableExceptions target reads it).
set(AVND_USES_EXCEPTIONS ON CACHE BOOL "" FORCE)

# Use the Avendish the host already provides (e.g. ossia score); otherwise fetch it.
# The same CMakeLists builds this as an ossia/score add-on or as a standalone object.
find_package(Avendish QUIET)
if(NOT Avendish_FOUND)
  include(FetchContent)
  # TODO: revert to celtera/avendish once the VSTGUI editor is merged upstream.
  # Using the fork branch that adds a self-contained VSTGUI VST3 editor.
  FetchContent_Declare(
    Avendish
    GIT_REPOSITORY "https://github.com/edumeneses/avendish"
    GIT_TAG vst3-vstgui-editor)
  FetchContent_Populate(Avendish)
  list(APPEND CMAKE_PREFIX_PATH "${avendish_SOURCE_DIR}")
  find_package(Avendish REQUIRED)
endif()

# PFFFT: the FFT behind the phase-vocoder harmonizer engine. Single precision
# only; skip its tests/benchmarks/examples/install.
include(FetchContent)
cmake_policy(SET CMP0135 NEW)
set(PFFFT_USE_TYPE_FLOAT   ON  CACHE BOOL "" FORCE)
set(PFFFT_USE_TYPE_DOUBLE  OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_FFTPACK      OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(INSTALL_PFFFT          OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  pffft
  GIT_REPOSITORY "https://github.com/marton78/pffft"
  GIT_TAG a4b03590cc2a4bea56f9721996e3057835799179)
FetchContent_MakeAvailable(pffft)

avnd_addon_init(NAME HasegawaAddon)

# CATEGORY audioplug: audio plug-in back-ends only (VST3 / CLAP / VST2). Keeps
# the godot/pd/max/python object back-ends -- and godot-cpp -- out of the build.
# Building the VST3 SDK from source carries funknown.cpp in its pluginterfaces
# lib, so the module's FUID symbols resolve; -Wl,--no-undefined below guards it.
avnd_addon_object(
  BASE HasegawaAddon
  C_NAME avnd_hasegawa
  CLASS Hasegawa
  CATEGORY audioplug
  MAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Hasegawa/Hasegawa.hpp"
  SOURCES
    Hasegawa/Hasegawa.cpp
    Hasegawa/Hasegawa.hpp
  DEPENDENCIES PFFFT::PFFFT)

avnd_addon_finalize(NAME HasegawaAddon
  UUID b4d2c8f1-6a3e-4b5d-9c7f-2e8a1d0b3f6c VERSION 1)

# On Linux the static link order leaves funknown.cpp's symbols
# (FUID::generate()/FUID()/FUnknownPrivate::atomicAdd, referenced by the SDK's
# base lib) unresolved, so the module fails to dlopen and hosts silently skip
# it. Compile funknown.cpp straight into the module target -- which already has
# the VST3 SDK include dirs -- and fail the link on any remaining undefined
# symbol. macOS/Windows resolve these on their own.
if(UNIX AND NOT APPLE AND VST3_SDK_ROOT AND TARGET avnd_hasegawa_vst3)
  target_sources(avnd_hasegawa_vst3
    PRIVATE "${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp")
  target_link_options(avnd_hasegawa_vst3 PRIVATE "-Wl,--no-undefined")
endif()
