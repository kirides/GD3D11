cmake_minimum_required(VERSION 3.24)

project(GD3D11 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(GD3D11Helpers)

option(GD3D11_BUILD_ENGINE "Build the D3D11Engine ddraw.dll target" ON)
option(GD3D11_BUILD_LAUNCHER "Build the Launcher ddraw.dll target" ON)
option(GD3D11_USE_VCPKG "Use vcpkg packages when available" ON)
option(GD3D11_DEPLOY_AFTER_BUILD "Copy build output into game system path after build" OFF)

set(GD3D11_GAME "G2" CACHE STRING "Game target (G2, G1, G1_12F)")
set_property(CACHE GD3D11_GAME PROPERTY STRINGS G2 G1 G1_12F)

set(GD3D11_SIMD "SSE2" CACHE STRING "SIMD target (SSE2, AVX, AVX2)")
set_property(CACHE GD3D11_SIMD PROPERTY STRINGS SSE2 AVX AVX2)

set(GD3D11_PROFILE "RELEASE" CACHE STRING "Build profile (RELEASE, NOOPT)")
set_property(CACHE GD3D11_PROFILE PROPERTY STRINGS RELEASE NOOPT)

set(GD3D11_COMPILER "AUTO" CACHE STRING "Compiler mode (AUTO, MSVC, CLANG)")
set_property(CACHE GD3D11_COMPILER PROPERTY STRINGS AUTO MSVC CLANG)

option(GD3D11_PUBLIC_RELEASE "Define PUBLIC_RELEASE" ON)
option(GD3D11_SPACER_NET "Define BUILD_SPACER_NET" OFF)
option(GD3D11_SPACER "Define BUILD_SPACER" OFF)
option(GD3D11_WIN32_LEAN_AND_MEAN "Define WIN32_LEAN_AND_MEAN" OFF)
option(GD3D11_TRACY_ENABLE "Define TRACY_ENABLE" OFF)
option(GD3D11_TRACY_ON_DEMAND "Define TRACY_ON_DEMAND" OFF)
set(GD3D11_TRACY_CALLSTACK "" CACHE STRING "Optional TRACY_CALLSTACK value")

set(GD3D11_SYSTEM_PATH_G1 "$ENV{G1_SYSTEM_PATH}" CACHE PATH "Gothic 1 deployment path")
set(GD3D11_SYSTEM_PATH_G2 "$ENV{G2_SYSTEM_PATH}" CACHE PATH "Gothic 2 deployment path")

set(GD3D11_VENDOR_LIB_DIR "${CMAKE_SOURCE_DIR}/lib" CACHE PATH "Directory with vendored .lib files")
set(GD3D11_DIRECTXTK_LIB_PATH "" CACHE PATH "Optional legacy DirectXTK lib path")

gd3d11_validate_enum(GD3D11_GAME "G2;G1;G1_12F")
gd3d11_validate_enum(GD3D11_SIMD "SSE2;AVX;AVX2")
gd3d11_validate_enum(GD3D11_PROFILE "RELEASE;NOOPT")
gd3d11_validate_enum(GD3D11_COMPILER "AUTO;MSVC;CLANG")
gd3d11_validate_compiler_selection()

if(CMAKE_CONFIGURATION_TYPES)
  if(GD3D11_PROFILE STREQUAL "NOOPT")
    set(_gd3d11_supported_configs "RelWithDebInfo;Release")
  else()
    set(_gd3d11_supported_configs "Release;RelWithDebInfo")
  endif()

  set(CMAKE_CONFIGURATION_TYPES "${_gd3d11_supported_configs}" CACHE STRING "Supported build configurations" FORCE)
else()
  if(GD3D11_PROFILE STREQUAL "NOOPT")
    set(_gd3d11_expected_build_type "RelWithDebInfo")
  else()
    set(_gd3d11_expected_build_type "Release")
  endif()

  set(_gd3d11_build_type_upper "")
  if(DEFINED CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _gd3d11_build_type_upper)
  endif()

  if(_gd3d11_build_type_upper STREQUAL "")
    set(CMAKE_BUILD_TYPE "${_gd3d11_expected_build_type}" CACHE STRING "Choose the type of build" FORCE)
  elseif(_gd3d11_build_type_upper STREQUAL "DEBUG")
    message(WARNING "Debug build type is not supported for GD3D11. Forcing CMAKE_BUILD_TYPE=${_gd3d11_expected_build_type}.")
    set(CMAKE_BUILD_TYPE "${_gd3d11_expected_build_type}" CACHE STRING "Choose the type of build" FORCE)
  elseif(GD3D11_PROFILE STREQUAL "NOOPT" AND NOT _gd3d11_build_type_upper STREQUAL "RELWITHDEBINFO")
    message(WARNING "GD3D11_PROFILE=NOOPT requires CMAKE_BUILD_TYPE=RelWithDebInfo. Forcing CMAKE_BUILD_TYPE=RelWithDebInfo.")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build" FORCE)
  endif()

  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release RelWithDebInfo)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(GD3D11_BUILD_ENGINE)
  add_subdirectory(D3D11Engine)
endif()

if(GD3D11_BUILD_LAUNCHER)
  add_subdirectory(Launcher)
endif()

set(GD3D11_REDIST_DIR "" CACHE PATH "Optional output directory for prepare_redist target")
option(GD3D11_REDIST_SKIP_MATRIX_BUILD "Skip matrix build and package already-built presets" OFF)
set(GD3D11_REDIST_VERSION_SUFFIX "" CACHE STRING "Optional release suffix override for prepare_redist")

if(NOT GD3D11_REDIST_DIR STREQUAL "")
  add_custom_target(prepare_redist
    COMMAND ${CMAKE_COMMAND}
      -DGD3D11_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DGD3D11_TARGET_DIR=${GD3D11_REDIST_DIR}
      -DGD3D11_SKIP_MATRIX_BUILD=${GD3D11_REDIST_SKIP_MATRIX_BUILD}
      -DGD3D11_REDIST_VERSION_SUFFIX=${GD3D11_REDIST_VERSION_SUFFIX}
      -P ${CMAKE_SOURCE_DIR}/cmake/PrepareRedist.cmake
    COMMENT "Build CI matrix and package CI-style redistribution artifacts"
    VERBATIM
  )
endif()
