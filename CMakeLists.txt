# Minimal CMake build for libambix — designed to be add_subdirectory()'d
# from a parent project (e.g. reaper_ambix) that vendors both this fork
# and the WavPack SDK as git submodules.
#
# The autotools build (configure.ac + Makefile.am) remains the canonical
# build for standalone use; this file exists so libambix can be a clean
# CMake target inside a CMake-based parent.

cmake_minimum_required(VERSION 3.10)
project(libambix C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LIBAMBIX_BUILD_TESTS "Build libambix tests" OFF)
option(LIBAMBIX_ENABLE_WAVPACK "Enable the WavPack lossless backend" ON)

# --- libsndfile (required for the CAF backend) -----------------------------
find_package(PkgConfig QUIET)
set(_libambix_have_sndfile FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(SNDFILE QUIET sndfile)
  if(SNDFILE_FOUND)
    set(_libambix_have_sndfile TRUE)
  endif()
endif()

# Allow callers to point at an alternate libsndfile install via cache vars.
if(NOT _libambix_have_sndfile)
  find_path(SNDFILE_INCLUDE_DIR sndfile.h)
  find_library(SNDFILE_LIBRARY NAMES sndfile)
  if(SNDFILE_INCLUDE_DIR AND SNDFILE_LIBRARY)
    set(SNDFILE_INCLUDE_DIRS "${SNDFILE_INCLUDE_DIR}")
    set(SNDFILE_LIBRARIES    "${SNDFILE_LIBRARY}")
    set(_libambix_have_sndfile TRUE)
  endif()
endif()

if(NOT _libambix_have_sndfile)
  message(FATAL_ERROR
    "libambix CMake build requires libsndfile (development headers + library).")
endif()

# --- WavPack (optional, vendored or via parent) ----------------------------
set(_libambix_have_wavpack FALSE)
if(LIBAMBIX_ENABLE_WAVPACK)
  if(TARGET wavpack)
    # Parent project supplied the target (typical when WavPack is also vendored)
    set(_libambix_have_wavpack TRUE)
  elseif(PkgConfig_FOUND)
    pkg_check_modules(WAVPACK QUIET wavpack)
    if(WAVPACK_FOUND)
      set(_libambix_have_wavpack TRUE)
    endif()
  endif()
  if(NOT _libambix_have_wavpack)
    message(WARNING "LIBAMBIX_ENABLE_WAVPACK=ON but WavPack not found "
                    "(neither a 'wavpack' target nor pkg-config). "
                    "Building libambix without WavPack support.")
  endif()
endif()

# --- sources ---------------------------------------------------------------
set(_libambix_srcs
  libambix/src/libambix.c
  libambix/src/adaptor.c
  libambix/src/adaptor_acn.c
  libambix/src/adaptor_fuma.c
  libambix/src/matrix.c
  libambix/src/matrix_invert.c
  libambix/src/utils.c
  libambix/src/uuid_chunk.c
  libambix/src/marker_region_chunk.c
  libambix/src/sndfile.c
)
if(_libambix_have_wavpack)
  list(APPEND _libambix_srcs libambix/src/wavpack.c)
endif()

add_library(ambix STATIC ${_libambix_srcs})
set_target_properties(ambix PROPERTIES OUTPUT_NAME ambix)

# Public header lookup
target_include_directories(ambix
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libambix>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libambix/src
)

target_compile_definitions(ambix PRIVATE AMBIX_INTERNAL)

# CMake build does not generate config.h — fold the relevant defines in directly.
target_compile_definitions(ambix PRIVATE
  HAVE_STDLIB_H=1
  HAVE_STRING_H=1
  HAVE_SNDFILE_H=1
  HAVE_SNDFILE=1
  HAVE_SF_SET_CHUNK=1
  HAVE_SF_GET_CHUNK_ITERATOR=1
  HAVE_SF_CHUNK_INFO=1
)
if(_libambix_have_wavpack)
  target_compile_definitions(ambix PRIVATE HAVE_WAVPACK=1)
endif()

# libsndfile dependency
if(SNDFILE_INCLUDE_DIRS)
  target_include_directories(ambix PRIVATE ${SNDFILE_INCLUDE_DIRS})
endif()
if(SNDFILE_LIBRARY_DIRS)
  target_link_directories(ambix PRIVATE ${SNDFILE_LIBRARY_DIRS})
endif()
target_link_libraries(ambix PRIVATE ${SNDFILE_LIBRARIES})

# WavPack dependency
if(_libambix_have_wavpack)
  if(TARGET wavpack)
    target_link_libraries(ambix PRIVATE wavpack)
  else()
    if(WAVPACK_INCLUDE_DIRS)
      target_include_directories(ambix PRIVATE ${WAVPACK_INCLUDE_DIRS})
    endif()
    if(WAVPACK_LIBRARY_DIRS)
      target_link_directories(ambix PRIVATE ${WAVPACK_LIBRARY_DIRS})
    endif()
    target_link_libraries(ambix PRIVATE ${WAVPACK_LIBRARIES})
  endif()
endif()

# libm
if(NOT WIN32)
  target_link_libraries(ambix PRIVATE m)
endif()

message(STATUS "libambix: sndfile=${_libambix_have_sndfile} wavpack=${_libambix_have_wavpack}")
