gd3d11_collect_vcxproj_sources("${CMAKE_CURRENT_SOURCE_DIR}/Launcher.vcxproj" GD3D11_LAUNCHER_SOURCES)

add_library(Launcher SHARED EXCLUDE_FROM_ALL ${GD3D11_LAUNCHER_SOURCES} ddraw.def)

set_target_properties(Launcher PROPERTIES
  OUTPUT_NAME ddraw
)

target_include_directories(Launcher PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
  set_property(TARGET Launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_compile_options(Launcher PRIVATE
    /W3
    /MP
    /fp:fast
  )
endif()

target_compile_definitions(Launcher PRIVATE
  WIN32
  NDEBUG
  LAUNCHER_EXPORTS
  _WINDOWS
  _USRDLL
)
