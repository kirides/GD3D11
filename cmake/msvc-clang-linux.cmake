set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(triple i686-pc-windows-msvc)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)

set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(XWIN_DIR "$ENV{HOME}/.xwin" CACHE PATH "Path to xwin directory")
message(STATUS "Using XWIN_DIR: ${XWIN_DIR}")

# Use the elegant winsysroot flag now that xwin is formatted correctly
set(CMAKE_CXX_FLAGS "--target=${triple} /winsysroot ${XWIN_DIR} /EHsc" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS "--target=${triple} /winsysroot ${XWIN_DIR}" CACHE STRING "" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "/machine:X86 /MANIFEST:NO /winsysroot:${XWIN_DIR}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "/machine:X86 /MANIFEST:NO /winsysroot:${XWIN_DIR}" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "/machine:X86 /MANIFEST:NO /winsysroot:${XWIN_DIR}" CACHE STRING "" FORCE)
