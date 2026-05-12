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

function(gd3d11_detect_compiler_family out_var)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_family "CLANG")
  elseif(MSVC)
    set(_family "MSVC")
  else()
    set(_family "OTHER")
  endif()

  set(${out_var} "${_family}" PARENT_SCOPE)
endfunction()

function(gd3d11_validate_compiler_selection)
  gd3d11_detect_compiler_family(_detected_family)

  if(NOT DEFINED GD3D11_COMPILER OR GD3D11_COMPILER STREQUAL "")
    set(_requested "AUTO")
  else()
    set(_requested "${GD3D11_COMPILER}")
  endif()

  if(_requested STREQUAL "AUTO")
    message(STATUS "GD3D11 compiler mode: AUTO (detected ${_detected_family}, CXX=${CMAKE_CXX_COMPILER_ID})")
    return()
  endif()

  if(_requested STREQUAL "MSVC" AND NOT _detected_family STREQUAL "MSVC")
    message(FATAL_ERROR "GD3D11_COMPILER=MSVC was requested, but detected CXX compiler is '${CMAKE_CXX_COMPILER_ID}'. Use an MSVC preset/toolchain or set GD3D11_COMPILER=AUTO.")
  endif()

  if(_requested STREQUAL "CLANG" AND NOT _detected_family STREQUAL "CLANG")
    message(FATAL_ERROR "GD3D11_COMPILER=CLANG was requested, but detected CXX compiler is '${CMAKE_CXX_COMPILER_ID}'. Use a clang preset/toolchain or set GD3D11_COMPILER=AUTO.")
  endif()

  message(STATUS "GD3D11 compiler mode: ${_requested} (CXX=${CMAKE_CXX_COMPILER_ID})")
endfunction()

function(gd3d11_apply_windows_sdk_includes target_name)
  gd3d11_detect_compiler_family(_compiler_family)

  set(_gnu_like_windows_toolchain FALSE)
  if(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_gnu_like_windows_toolchain TRUE)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_TARGET MATCHES "mingw|windows-gnu" OR CMAKE_CXX_SIMULATE_ID STREQUAL "GNU")
      set(_gnu_like_windows_toolchain TRUE)
    endif()
  endif()

  if(NOT WIN32 OR _compiler_family STREQUAL "MSVC" OR _gnu_like_windows_toolchain)
    if(_gnu_like_windows_toolchain)
      message(STATUS "Skipping Windows SDK include injection for target '${target_name}' on GNU-like Windows toolchain to avoid UCRT/MSVC header mixing.")
    endif()
    return()
  endif()

  set(_candidate_include_dirs)

  set(_env_include "$ENV{INCLUDE}")
  if(NOT _env_include STREQUAL "")
    string(REPLACE "\\" "/" _env_include "${_env_include}")
    foreach(_include_dir IN LISTS _env_include)
      if(EXISTS "${_include_dir}")
        list(APPEND _candidate_include_dirs "${_include_dir}")
      endif()
    endforeach()
  endif()

  set(_sdk_dir "$ENV{WindowsSdkDir}")
  if(_sdk_dir STREQUAL "")
    set(_sdk_dir "$ENV{WindowsSdkDir_10}")
  endif()
  string(REPLACE "\\" "/" _sdk_dir "${_sdk_dir}")

  set(_sdk_version "$ENV{WindowsSDKVersion}")
  string(REPLACE "\\" "/" _sdk_version "${_sdk_version}")
  string(REGEX REPLACE "/+$" "" _sdk_version "${_sdk_version}")

  if(_sdk_dir STREQUAL "" AND EXISTS "C:/Program Files (x86)/Windows Kits/10")
    set(_sdk_dir "C:/Program Files (x86)/Windows Kits/10")
  endif()

  if(NOT _sdk_dir STREQUAL "")
    set(_sdk_include_root "${_sdk_dir}/Include")

    if(_sdk_version STREQUAL "" AND EXISTS "${_sdk_include_root}")
      file(GLOB _sdk_version_dirs LIST_DIRECTORIES true "${_sdk_include_root}/*")
      list(SORT _sdk_version_dirs COMPARE NATURAL ORDER DESCENDING)
      foreach(_version_dir IN LISTS _sdk_version_dirs)
        if(EXISTS "${_version_dir}/um/windows.h" OR EXISTS "${_version_dir}/um/Windows.h")
          get_filename_component(_sdk_version "${_version_dir}" NAME)
          break()
        endif()
      endforeach()
    endif()

    if(NOT _sdk_version STREQUAL "")
      foreach(_component IN ITEMS shared um ucrt winrt)
        set(_component_dir "${_sdk_include_root}/${_sdk_version}/${_component}")
        if(EXISTS "${_component_dir}")
          list(APPEND _candidate_include_dirs "${_component_dir}")
        endif()
      endforeach()
    endif()
  endif()

  list(REMOVE_DUPLICATES _candidate_include_dirs)

  if(_candidate_include_dirs)
    target_include_directories(${target_name} SYSTEM PRIVATE ${_candidate_include_dirs})
  endif()

  set(_has_windows_h FALSE)
  set(_has_d3d11_h FALSE)

  foreach(_include_dir IN LISTS _candidate_include_dirs)
    if(EXISTS "${_include_dir}/windows.h" OR EXISTS "${_include_dir}/Windows.h")
      set(_has_windows_h TRUE)
    endif()
    if(EXISTS "${_include_dir}/d3d11.h")
      set(_has_d3d11_h TRUE)
    endif()
  endforeach()

  if(NOT _has_windows_h OR NOT _has_d3d11_h)
    message(WARNING "Best-effort Windows SDK include detection for non-MSVC target '${target_name}' could not verify both windows.h and d3d11.h. If compile fails, run from a Visual Studio Developer Prompt or set INCLUDE/WindowsSdkDir/WindowsSDKVersion.")
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
      /EHsc
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
    if(WIN32)
      target_compile_definitions(${target_name} PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
      target_compile_options(${target_name} PRIVATE
        -m32
        $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-fms-extensions>
        -Wno-switch
        -Wno-defaulted-function-deleted # DirectXMath uses = default for XMFLOAT4X4 which causes this warning
      )
      target_link_options(${target_name} PRIVATE -m32)
    endif()

    target_compile_options(${target_name} PRIVATE -ffast-math)

    if(GD3D11_SIMD STREQUAL "AVX")
      target_compile_options(${target_name} PRIVATE -mavx)
    elseif(GD3D11_SIMD STREQUAL "AVX2")
      target_compile_options(${target_name} PRIVATE -mavx2)
    else()
      target_compile_options(${target_name} PRIVATE -msse2)
    endif()

    if(GD3D11_PROFILE STREQUAL "NOOPT")
      target_compile_options(${target_name} PRIVATE -O0 -g -gcodeview)
      target_link_options(${target_name} PRIVATE -g -Wl)
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
