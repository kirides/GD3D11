gd3d11_collect_vcxproj_sources("${CMAKE_CURRENT_SOURCE_DIR}/D3D11Engine.vcxproj" GD3D11_ENGINE_SOURCES)

function(gd3d11_get_package_version package_id out_var)
  set(_packages_config "${CMAKE_CURRENT_SOURCE_DIR}/packages.config")
  if(NOT EXISTS "${_packages_config}")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  file(STRINGS "${_packages_config}" _pkg_lines REGEX "id=\"${package_id}\"[^\"]*version=\"[^\"]+\"")
  if(NOT _pkg_lines)
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  list(GET _pkg_lines 0 _pkg_line)
  string(REGEX REPLACE ".*version=\"([^\"]+)\".*" "\\1" _pkg_version "${_pkg_line}")
  set(${out_var} "${_pkg_version}" PARENT_SCOPE)
endfunction()

gd3d11_get_package_version("directxmath" GD3D11_DIRECTXMATH_VERSION)
gd3d11_get_package_version("directxmesh_desktop_2019" GD3D11_DIRECTXMESH_VERSION)
gd3d11_get_package_version("directxtk_desktop_2019" GD3D11_DIRECTXTK_VERSION)
gd3d11_get_package_version("Microsoft.XAudio2.Redist" GD3D11_XAUDIO2_VERSION)

set(GD3D11_NUGET_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/packages" CACHE PATH "NuGet packages directory")

set(_directxmath_include "")
if(NOT GD3D11_DIRECTXMATH_VERSION STREQUAL "")
  set(_directxmath_include "${GD3D11_NUGET_PACKAGES_DIR}/directxmath.${GD3D11_DIRECTXMATH_VERSION}/include")
endif()

set(_directxmesh_root "")
if(NOT GD3D11_DIRECTXMESH_VERSION STREQUAL "")
  set(_directxmesh_root "${GD3D11_NUGET_PACKAGES_DIR}/directxmesh_desktop_2019.${GD3D11_DIRECTXMESH_VERSION}")
endif()

set(_directxtk_root "")
if(NOT GD3D11_DIRECTXTK_VERSION STREQUAL "")
  set(_directxtk_root "${GD3D11_NUGET_PACKAGES_DIR}/directxtk_desktop_2019.${GD3D11_DIRECTXTK_VERSION}")
endif()

set(_xaudio2_root "")
if(NOT GD3D11_XAUDIO2_VERSION STREQUAL "")
  set(_xaudio2_root "${GD3D11_NUGET_PACKAGES_DIR}/Microsoft.XAudio2.Redist.${GD3D11_XAUDIO2_VERSION}")
endif()

add_library(D3D11Engine SHARED ${GD3D11_ENGINE_SOURCES} ddraw.def)

set_target_properties(D3D11Engine PROPERTIES
  OUTPUT_NAME ddraw
)

target_include_directories(D3D11Engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/include/imgui/backends
)

if(EXISTS "${_directxmath_include}")
  target_include_directories(D3D11Engine BEFORE PRIVATE "${_directxmath_include}")
endif()

if(EXISTS "${_directxmesh_root}/include")
  target_include_directories(D3D11Engine BEFORE PRIVATE "${_directxmesh_root}/include")
endif()

if(EXISTS "${_directxtk_root}/include")
  target_include_directories(D3D11Engine BEFORE PRIVATE "${_directxtk_root}/include")
endif()

if(EXISTS "${_xaudio2_root}/build/native/include")
  target_include_directories(D3D11Engine BEFORE PRIVATE "${_xaudio2_root}/build/native/include")
endif()

gd3d11_apply_windows_sdk_includes(D3D11Engine)

gd3d11_apply_common_compile_settings(D3D11Engine)
target_compile_definitions(D3D11Engine PRIVATE D3D11ENGINE_EXPORTS)

target_link_libraries(D3D11Engine PRIVATE
  d3d11
  d3dcompiler
  dxgi
  dxguid
  gdi32
  imm32
  imagehlp
  ntdll
  shlwapi
  user32
  version
  winmm
)

target_link_directories(D3D11Engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GD3D11_VENDOR_LIB_DIR}
)

set(_gd3d11_nuget_cpp_lib_cfg "$<$<CONFIG:Debug>:Debug>$<$<NOT:$<CONFIG:Debug>>:Release>")
set(_gd3d11_nuget_xaudio_lib_cfg "$<$<CONFIG:Debug>:debug>$<$<NOT:$<CONFIG:Debug>>:release>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(_gd3d11_nuget_cpp_lib_cfg "Release")
  set(_gd3d11_nuget_xaudio_lib_cfg "release")
endif()

if(EXISTS "${_directxmesh_root}/native/lib/x86")
  target_link_directories(D3D11Engine PRIVATE "${_directxmesh_root}/native/lib/x86/${_gd3d11_nuget_cpp_lib_cfg}")
endif()

if(EXISTS "${_directxtk_root}/native/lib/x86")
  target_link_directories(D3D11Engine PRIVATE "${_directxtk_root}/native/lib/x86/${_gd3d11_nuget_cpp_lib_cfg}")
endif()

if(EXISTS "${_xaudio2_root}/build/native")
  target_link_directories(D3D11Engine PRIVATE "${_xaudio2_root}/build/native/${_gd3d11_nuget_xaudio_lib_cfg}/lib/x86")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/blobs/libs")
  target_link_directories(D3D11Engine PRIVATE "${CMAKE_SOURCE_DIR}/blobs/libs")
endif()

if(NOT GD3D11_DIRECTXTK_LIB_PATH STREQUAL "")
  target_link_directories(D3D11Engine PRIVATE "${GD3D11_DIRECTXTK_LIB_PATH}")
endif()

if(WIN32)
  target_link_libraries(D3D11Engine PRIVATE
    DirectXMesh.lib
    DirectXTK.lib
    DirectXTKAudioWin7.lib
    xaudio2_9redist.lib
    xapobaseredist.lib
    amd_ags_x86_2022_MD.lib
    assimp-vc143-mt.lib
    ffx_backend_dx11_x86.lib
    ffx_fsr2_x86.lib
    ffx_fsr3upscaler_x86.lib
    GFSDK_SSAO_D3D11.win32.lib
  )
endif()

if(GD3D11_USE_VCPKG)
  find_package(directxmath CONFIG QUIET)
  find_package(directxmesh CONFIG QUIET)
  find_package(directxtk CONFIG QUIET)

  gd3d11_link_if_target(D3D11Engine Microsoft::DirectXMath)
  gd3d11_link_if_target(D3D11Engine directxmath)
  gd3d11_link_if_target(D3D11Engine Microsoft::DirectXMesh)
  gd3d11_link_if_target(D3D11Engine directxmesh)
  gd3d11_link_if_target(D3D11Engine Microsoft::DirectXTK)
  gd3d11_link_if_target(D3D11Engine directxtk)
endif()

if(GD3D11_DEPLOY_AFTER_BUILD)
  gd3d11_resolve_deploy_root(_gd3d11_deploy_root)

  if(_gd3d11_deploy_root STREQUAL "")
    message(FATAL_ERROR "GD3D11_DEPLOY_AFTER_BUILD is ON but no game system path is configured")
  endif()

  add_custom_command(TARGET D3D11Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gd3d11_deploy_root}/GD3D11/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Shaders" "${_gd3d11_deploy_root}/GD3D11/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:D3D11Engine>" "${_gd3d11_deploy_root}/ddraw.dll"
    VERBATIM
  )
endif()
