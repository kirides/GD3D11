gd3d11_collect_vcxproj_sources("${CMAKE_CURRENT_SOURCE_DIR}/D3D11Engine.vcxproj" GD3D11_ENGINE_SOURCES)

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
find_package(directxtk CONFIG REQUIRED)
# directx-headers only supplies d3d12.h/d3dx12*.h here. Its compiled GUID/helper
# libs are deliberately not linked: dxguid.lib (already linked below) already
# supplies the D3D12 IIDs this project needs, and D3D12 stays dynamically loaded
# (LoadLibrary + GetProcAddress in D3D12Device.cpp) with no hard d3d12.lib link.
find_package(directx-headers CONFIG REQUIRED)

target_link_libraries(D3D11Engine PRIVATE
  Microsoft::DirectXMath
  Microsoft::DirectXTK
)

# The Windows SDK ships its own (older) DirectXMath/DirectXCollision/XAudio2/D3D12
# headers under Include/<ver>/um. Those must not shadow the vcpkg-provided ones
# pulled in above, so force the vcpkg include dirs to the front of the search order.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  target_include_directories(D3D11Engine BEFORE PRIVATE
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/directxtk"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/directx"
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
