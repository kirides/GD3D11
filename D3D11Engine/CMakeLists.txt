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

gd3d11_get_package_version("Microsoft.Direct3D.D3D12" GD3D11_D3D12AGILITY_VERSION)

set(GD3D11_NUGET_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/packages" CACHE PATH "NuGet packages directory")

set(_d3d12agility_include "")
if(NOT GD3D11_D3D12AGILITY_VERSION STREQUAL "")
  set(_d3d12agility_include "${GD3D11_NUGET_PACKAGES_DIR}/Microsoft.Direct3D.D3D12.${GD3D11_D3D12AGILITY_VERSION}/build/native/include")
endif()

add_library(D3D11Engine SHARED ${GD3D11_ENGINE_SOURCES} ddraw.def)

set_target_properties(D3D11Engine PROPERTIES
  OUTPUT_NAME ddraw
)

target_include_directories(D3D11Engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/tracy
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/D3D12MemoryAllocator/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/include/imgui/backends
)

if(EXISTS "${_d3d12agility_include}")
  target_include_directories(D3D11Engine BEFORE PRIVATE "${_d3d12agility_include}" "${_d3d12agility_include}/d3dx12")
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

if(EXISTS "${CMAKE_SOURCE_DIR}/blobs/libs")
  target_link_directories(D3D11Engine PRIVATE "${CMAKE_SOURCE_DIR}/blobs/libs")
endif()

if(WIN32)
  target_link_libraries(D3D11Engine PRIVATE
    amd_ags_x86_2022_MD.lib
    assimp-vc143-mt.lib
    GFSDK_SSAO_D3D11.win32.lib
  )
endif()

find_package(directxmath CONFIG REQUIRED)
find_package(directxmesh CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(xaudio2redist CONFIG REQUIRED)

target_link_libraries(D3D11Engine PRIVATE
  Microsoft::DirectXMath
  Microsoft::DirectXMesh
  Microsoft::DirectXTK
  Microsoft::XAudio2Redist
)

# The Windows SDK ships its own (older) DirectXMath/DirectXCollision/XAudio2 headers
# under Include/<ver>/um. Those must not shadow the vcpkg-provided ones pulled in
# above, so force the vcpkg include dirs to the front of the search order.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  target_include_directories(D3D11Engine BEFORE PRIVATE
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/directxtk"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/xaudio2redist"
  )
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
