if(NOT DEFINED GD3D11_SOURCE_DIR OR NOT DEFINED GD3D11_TARGET_DIR)
  message(FATAL_ERROR "GD3D11_SOURCE_DIR and GD3D11_TARGET_DIR are required")
endif()

if(NOT DEFINED GD3D11_SKIP_MATRIX_BUILD)
  set(GD3D11_SKIP_MATRIX_BUILD OFF)
endif()

if(NOT DEFINED GD3D11_REDIST_VERSION_SUFFIX)
  set(GD3D11_REDIST_VERSION_SUFFIX "")
endif()

function(gd3d11_fail message_text)
  message(FATAL_ERROR "[prepare_redist] ${message_text}")
endfunction()

function(gd3d11_find_file search_root file_name out_var)
  file(GLOB_RECURSE _all_files LIST_DIRECTORIES FALSE "${search_root}/*")
  set(_matches)

  foreach(_path IN LISTS _all_files)
    get_filename_component(_base_name "${_path}" NAME)
    if(_base_name STREQUAL "${file_name}"
      AND NOT _path MATCHES "/CMakeFiles/"
      AND NOT _path MATCHES "/_deps/")
      list(APPEND _matches "${_path}")
    endif()
  endforeach()

  if(NOT _matches)
    gd3d11_fail("Could not find ${file_name} under ${search_root}")
  endif()

  set(_release_matches)
  foreach(_candidate IN LISTS _matches)
    if(_candidate MATCHES "/Release/" OR _candidate MATCHES "/RelWithDebInfo/")
      list(APPEND _release_matches "${_candidate}")
    endif()
  endforeach()

  if(_release_matches)
    set(_matches "${_release_matches}")
  endif()

  list(LENGTH _matches _count)
  if(NOT _count EQUAL 1)
    string(REPLACE ";" "\n  " _formatted "${_matches}")
    gd3d11_fail("Ambiguous ${file_name} under ${search_root}. Candidates:\n  ${_formatted}")
  endif()

  list(GET _matches 0 _match)
  set(${out_var} "${_match}" PARENT_SCOPE)
endfunction()

function(gd3d11_require_exists path_to_check)
  if(NOT EXISTS "${path_to_check}")
    gd3d11_fail("Required path does not exist: ${path_to_check}")
  endif()
endfunction()

set(_workflow_file "${GD3D11_SOURCE_DIR}/.github/workflows/build-and-conditional-release.yml")
gd3d11_require_exists("${_workflow_file}")

set(_expected_matrix_configs
  Launcher
  Release
  Release_AVX
  Release_AVX2
  Release_G1
  Release_G1_12f
  Release_G1_AVX
  Release_G1_AVX2
  Spacer_NET
  Spacer_NET_G1
)

set(_expected_matrix_outputs
  ddraw.dll
  g2a.dll
  g2a_avx.dll
  g2a_avx2.dll
  g1.dll
  g1a.dll
  g1_avx.dll
  g1_avx2.dll
  g2_spacer.dll
  g1_spacer.dll
)

file(STRINGS "${_workflow_file}" _workflow_configs REGEX "^[ \t]*- configuration:[ \t]*[A-Za-z0-9_]+")
set(_workflow_matrix_configs)
foreach(_line IN LISTS _workflow_configs)
  string(REGEX REPLACE "^[ \t]*- configuration:[ \t]*([A-Za-z0-9_]+).*$" "\\1" _cfg "${_line}")
  list(APPEND _workflow_matrix_configs "${_cfg}")
endforeach()

file(STRINGS "${_workflow_file}" _workflow_outputs REGEX "^[ \t]*output-file:[ \t]*[A-Za-z0-9_]+\\.dll")
set(_workflow_matrix_outputs)
foreach(_line IN LISTS _workflow_outputs)
  string(REGEX REPLACE "^[ \t]*output-file:[ \t]*([A-Za-z0-9_]+\\.dll).*$" "\\1" _output_file "${_line}")
  list(APPEND _workflow_matrix_outputs "${_output_file}")
endforeach()

if(NOT "${_workflow_matrix_configs}" STREQUAL "${_expected_matrix_configs}")
  string(REPLACE ";" ", " _expected_configs_text "${_expected_matrix_configs}")
  string(REPLACE ";" ", " _workflow_configs_text "${_workflow_matrix_configs}")
  gd3d11_fail(
    "Workflow matrix configurations changed. Expected: [${_expected_configs_text}] but found: [${_workflow_configs_text}]"
  )
endif()

if(NOT "${_workflow_matrix_outputs}" STREQUAL "${_expected_matrix_outputs}")
  string(REPLACE ";" ", " _expected_outputs_text "${_expected_matrix_outputs}")
  string(REPLACE ";" ", " _workflow_outputs_text "${_workflow_matrix_outputs}")
  gd3d11_fail(
    "Workflow matrix output-file entries changed. Expected: [${_expected_outputs_text}] but found: [${_workflow_outputs_text}]"
  )
endif()

set(_matrix_entries
  "Launcher|Launcher|ddraw|root"
  "Release|Release|g2a|bin"
  "Release_AVX|Release_AVX|g2a_avx|bin"
  "Release_AVX2|Release_AVX2|g2a_avx2|bin"
  "Release_G1|Release_G1|g1|bin"
  "Release_G1_12f|Release_G1_12f|g1a|bin"
  "Release_G1_AVX|Release_G1_AVX|g1_avx|bin"
  "Release_G1_AVX2|Release_G1_AVX2|g1_avx2|bin"
  "Spacer_NET|Spacer_NET|g2_spacer|bin"
  "Spacer_NET_G1|Spacer_NET_G1|g1_spacer|bin"
)

if(NOT GD3D11_SKIP_MATRIX_BUILD)
  foreach(_entry IN LISTS _matrix_entries)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 1 _preset)

    message(STATUS "[prepare_redist] Configuring preset: ${_preset}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --preset "${_preset}"
      WORKING_DIRECTORY "${GD3D11_SOURCE_DIR}"
      RESULT_VARIABLE _configure_result
    )
    if(NOT _configure_result EQUAL 0)
      gd3d11_fail("Configure failed for preset ${_preset} with exit code ${_configure_result}")
    endif()

    message(STATUS "[prepare_redist] Building preset: ${_preset}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build --preset "${_preset}"
      WORKING_DIRECTORY "${GD3D11_SOURCE_DIR}"
      RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
      gd3d11_fail("Build failed for preset ${_preset} with exit code ${_build_result}")
    endif()
  endforeach()
endif()

if(NOT GD3D11_REDIST_VERSION_SUFFIX STREQUAL "")
  set(_version_suffix "${GD3D11_REDIST_VERSION_SUFFIX}")
elseif("$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
  set(_version_suffix "$ENV{GITHUB_REF_NAME}")
else()
  if(NOT "$ENV{GITHUB_SHA}" STREQUAL "")
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 9 _short_hash)
  else()
    execute_process(
      COMMAND git rev-parse --short=9 HEAD
      WORKING_DIRECTORY "${GD3D11_SOURCE_DIR}"
      RESULT_VARIABLE _git_result
      OUTPUT_VARIABLE _short_hash
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(NOT _git_result EQUAL 0 OR _short_hash STREQUAL "")
      set(_short_hash "unknown")
    endif()
  endif()
  set(_version_suffix "git-${_short_hash}")
endif()

set(_release_name "GD3D11-${_version_suffix}")
set(_release_dir "${GD3D11_TARGET_DIR}/${_release_name}")
set(_zip_path "${GD3D11_TARGET_DIR}/${_release_name}.zip")
set(_bin_dir "${_release_dir}/GD3D11/Bin")

file(MAKE_DIRECTORY "${GD3D11_TARGET_DIR}")
if(EXISTS "${_release_dir}")
  file(REMOVE_RECURSE "${_release_dir}")
endif()
if(EXISTS "${_zip_path}")
  file(REMOVE "${_zip_path}")
endif()

file(MAKE_DIRECTORY "${_release_dir}/GD3D11/shaders")
file(MAKE_DIRECTORY "${_release_dir}/GD3D11/Fonts")
file(MAKE_DIRECTORY "${_release_dir}/GD3D11/Meshes")
file(MAKE_DIRECTORY "${_release_dir}/GD3D11/Textures")
file(MAKE_DIRECTORY "${_bin_dir}")

gd3d11_require_exists("${GD3D11_SOURCE_DIR}/D3D11Engine/Shaders")
gd3d11_require_exists("${GD3D11_SOURCE_DIR}/blobs/Fonts")
gd3d11_require_exists("${GD3D11_SOURCE_DIR}/blobs/Meshes")
gd3d11_require_exists("${GD3D11_SOURCE_DIR}/blobs/Textures")
gd3d11_require_exists("${GD3D11_SOURCE_DIR}/blobs/libs")
gd3d11_require_exists("${GD3D11_SOURCE_DIR}/blobs/bin/wine-d2d1.dll")

file(COPY "${GD3D11_SOURCE_DIR}/D3D11Engine/Shaders/" DESTINATION "${_release_dir}/GD3D11/shaders")
file(COPY "${GD3D11_SOURCE_DIR}/blobs/Fonts/" DESTINATION "${_release_dir}/GD3D11/Fonts")
file(COPY "${GD3D11_SOURCE_DIR}/blobs/Meshes/" DESTINATION "${_release_dir}/GD3D11/Meshes")
file(COPY "${GD3D11_SOURCE_DIR}/blobs/Textures/" DESTINATION "${_release_dir}/GD3D11/Textures")
file(COPY "${GD3D11_SOURCE_DIR}/blobs/libs/" DESTINATION "${_release_dir}")
file(COPY_FILE "${GD3D11_SOURCE_DIR}/blobs/bin/wine-d2d1.dll" "${_bin_dir}/d2d1.dll" ONLY_IF_DIFFERENT)

set(_expected_paths
  "${_bin_dir}/d2d1.dll"
)

foreach(_entry IN LISTS _matrix_entries)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 1 _preset)
  list(GET _parts 2 _output_stem)
  list(GET _parts 3 _destination_kind)

  set(_preset_dir "${GD3D11_SOURCE_DIR}/out/build/${_preset}")
  gd3d11_require_exists("${_preset_dir}")

  gd3d11_find_file("${_preset_dir}" "ddraw.dll" _built_dll)
  gd3d11_find_file("${_preset_dir}" "ddraw.pdb" _built_pdb)

  if(_destination_kind STREQUAL "root")
    set(_destination_dll "${_release_dir}/ddraw.dll")
    set(_destination_pdb "${_release_dir}/ddraw.pdb")
  else()
    set(_destination_dll "${_bin_dir}/${_output_stem}.dll")
    set(_destination_pdb "${_bin_dir}/${_output_stem}.pdb")
  endif()

  file(COPY_FILE "${_built_dll}" "${_destination_dll}" ONLY_IF_DIFFERENT)
  file(COPY_FILE "${_built_pdb}" "${_destination_pdb}" ONLY_IF_DIFFERENT)

  list(APPEND _expected_paths "${_destination_dll}" "${_destination_pdb}")
endforeach()

foreach(_expected_path IN LISTS _expected_paths)
  if(NOT EXISTS "${_expected_path}")
    gd3d11_fail("Expected packaged artifact is missing: ${_expected_path}")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar cf "${_zip_path}" --format=zip "${_release_name}"
  WORKING_DIRECTORY "${GD3D11_TARGET_DIR}"
  RESULT_VARIABLE _zip_result
)
if(NOT _zip_result EQUAL 0)
  gd3d11_fail("Failed to create zip archive ${_zip_path} (exit code ${_zip_result})")
endif()

message(STATUS "[prepare_redist] Created release folder: ${_release_dir}")
message(STATUS "[prepare_redist] Created zip archive: ${_zip_path}")
