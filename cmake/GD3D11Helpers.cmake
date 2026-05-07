function(gd3d11_validate_enum var_name allowed_values)
  list(FIND allowed_values "${${var_name}}" _idx)
  if(_idx EQUAL -1)
    message(FATAL_ERROR "${var_name}='${${var_name}}' is invalid. Allowed values: ${allowed_values}")
  endif()
endfunction()

function(gd3d11_collect_vcxproj_sources vcxproj out_var)
  if(NOT EXISTS "${vcxproj}")
    message(FATAL_ERROR "Missing vcxproj file: ${vcxproj}")
  endif()

  file(STRINGS "${vcxproj}" _compile_lines REGEX "<ClCompile Include=\"[^\"]+\"")
  set(_sources)

  foreach(_line IN LISTS _compile_lines)
    string(REGEX REPLACE ".*<ClCompile Include=\"([^\"]+)\".*" "\\1" _source "${_line}")
    string(REPLACE "\\" "/" _source "${_source}")
    list(APPEND _sources "${_source}")
  endforeach()

  list(REMOVE_DUPLICATES _sources)
  set(${out_var} "${_sources}" PARENT_SCOPE)
endfunction()

function(gd3d11_link_if_target target_name dependency_name)
  if(TARGET "${dependency_name}")
    target_link_libraries("${target_name}" PRIVATE "${dependency_name}")
  endif()
endfunction()

function(gd3d11_apply_common_compile_settings target_name)
  target_compile_definitions(${target_name} PRIVATE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS
    WIN32
    NDEBUG
    _WINDOWS
    _USRDLL
    NOMINMAX
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    NTDDI_VERSION=0x06010000
    _WIN7_PLATFORM_UPDATE=1
    _XM_DISABLE_INTEL_SVML_
  )

  if(GD3D11_PUBLIC_RELEASE)
    target_compile_definitions(${target_name} PRIVATE PUBLIC_RELEASE)
  endif()

  if(GD3D11_WIN32_LEAN_AND_MEAN)
    target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN)
  endif()

  if(GD3D11_GAME STREQUAL "G2")
    target_compile_definitions(${target_name} PRIVATE BUILD_GOTHIC_2_6_fix)
  elseif(GD3D11_GAME STREQUAL "G1")
    target_compile_definitions(${target_name} PRIVATE BUILD_GOTHIC_1_08k)
  elseif(GD3D11_GAME STREQUAL "G1_12F")
    target_compile_definitions(${target_name} PRIVATE BUILD_GOTHIC_1_08k BUILD_1_12F)
  endif()

  if(GD3D11_SPACER_NET)
    target_compile_definitions(${target_name} PRIVATE BUILD_SPACER_NET)
  endif()

  if(GD3D11_SPACER)
    target_compile_definitions(${target_name} PRIVATE BUILD_SPACER)
  endif()

  if(GD3D11_TRACY_ENABLE)
    target_compile_definitions(${target_name} PRIVATE TRACY_ENABLE)
  endif()

  if(GD3D11_TRACY_ON_DEMAND)
    target_compile_definitions(${target_name} PRIVATE TRACY_ON_DEMAND)
  endif()

  if(NOT GD3D11_TRACY_CALLSTACK STREQUAL "")
    target_compile_definitions(${target_name} PRIVATE "TRACY_CALLSTACK=${GD3D11_TRACY_CALLSTACK}")
  endif()

  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W3
      /MP
      /fp:fast
      /Zi
      /EHs-c-
      /wd4005
      /wd4530
      /wd4577
      /wd6246
      /wd6322
      /wd26812
    )

    set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

    if(GD3D11_SIMD STREQUAL "AVX")
      target_compile_options(${target_name} PRIVATE /arch:AVX)
    elseif(GD3D11_SIMD STREQUAL "AVX2")
      target_compile_options(${target_name} PRIVATE /arch:AVX2)
    else()
      target_compile_options(${target_name} PRIVATE /arch:SSE2)
    endif()

    if(GD3D11_PROFILE STREQUAL "NOOPT")
      target_compile_options(${target_name} PRIVATE /Od /Ob1)
      target_link_options(${target_name} PRIVATE /OPT:NOREF /OPT:NOICF /DEBUG:FULL)
    else()
      target_compile_options(${target_name} PRIVATE /O2 /Ob2 /GL)
      target_link_options(${target_name} PRIVATE /LTCG /OPT:REF /OPT:ICF /DEBUG:FULL)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE -ffast-math)

    if(GD3D11_SIMD STREQUAL "AVX")
      target_compile_options(${target_name} PRIVATE -mavx)
    elseif(GD3D11_SIMD STREQUAL "AVX2")
      target_compile_options(${target_name} PRIVATE -mavx2)
    else()
      target_compile_options(${target_name} PRIVATE -msse2)
    endif()

    if(GD3D11_PROFILE STREQUAL "NOOPT")
      target_compile_options(${target_name} PRIVATE -O0 -g)
    else()
      target_compile_options(${target_name} PRIVATE -O2)
    endif()
  endif()
endfunction()

function(gd3d11_resolve_deploy_root out_var)
  if(GD3D11_GAME STREQUAL "G2")
    set(_deploy_root "${GD3D11_SYSTEM_PATH_G2}")
  else()
    set(_deploy_root "${GD3D11_SYSTEM_PATH_G1}")
  endif()

  set(${out_var} "${_deploy_root}" PARENT_SCOPE)
endfunction()
