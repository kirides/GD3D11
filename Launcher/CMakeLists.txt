gd3d11_collect_vcxproj_sources("${CMAKE_CURRENT_SOURCE_DIR}/Launcher.vcxproj" GD3D11_LAUNCHER_SOURCES)

add_library(Launcher SHARED EXCLUDE_FROM_ALL ${GD3D11_LAUNCHER_SOURCES} ddraw.def)

set_target_properties(Launcher PROPERTIES
  OUTPUT_NAME ddraw
)

target_include_directories(Launcher PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

gd3d11_apply_windows_sdk_includes(Launcher)

if(MSVC)
  set_property(TARGET Launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_compile_options(Launcher PRIVATE
    /W3
    /MP
    /fp:fast
    /Zi
  )
  target_link_options(Launcher PRIVATE /DEBUG:FULL)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(WIN32)
    target_compile_definitions(Launcher PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    target_compile_options(Launcher PRIVATE -m32)
    target_link_options(Launcher PRIVATE -m32)
  endif()

  target_compile_options(Launcher PRIVATE -ffast-math)

  if(GD3D11_PROFILE STREQUAL "NOOPT")
    target_compile_options(Launcher PRIVATE -O0 -g)
  else()
    target_compile_options(Launcher PRIVATE -O2)
  endif()
endif()

target_compile_definitions(Launcher PRIVATE
  WIN32
  NDEBUG
  LAUNCHER_EXPORTS
  _WINDOWS
  _USRDLL
)
