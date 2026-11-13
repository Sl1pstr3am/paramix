# Findonnxruntime.cmake
#
# Locates ONNX Runtime for the optional stem separation backend (ADR-001).
#
# Honors:
#   STEM_SEPARATION_ORT_ROOT  an installation prefix, or the directory that
#                             contains include/ and lib/
#   CMAKE_PREFIX_PATH         the standard CMake prefix list
#
# On success, defines the imported target onnxruntime::onnxruntime and sets
# onnxruntime_FOUND. Nothing is downloaded at configure time.

if(TARGET onnxruntime::onnxruntime)
  set(onnxruntime_FOUND TRUE)
  return()
endif()

# Prefer a config package when the installation ships one.
find_package(onnxruntime CONFIG QUIET)
if(onnxruntime_FOUND)
  return()
endif()

set(_onnxruntime_HINTS)
if(STEM_SEPARATION_ORT_ROOT)
  list(APPEND _onnxruntime_HINTS "${STEM_SEPARATION_ORT_ROOT}")
endif()
if(DEFINED ENV{STEM_SEPARATION_ORT_ROOT})
  list(APPEND _onnxruntime_HINTS "$ENV{STEM_SEPARATION_ORT_ROOT}")
endif()

find_path(
  onnxruntime_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  HINTS ${_onnxruntime_HINTS}
  PATH_SUFFIXES include include/onnxruntime include/onnxruntime/core/session
)

find_library(
  onnxruntime_LIBRARY
  NAMES onnxruntime
  HINTS ${_onnxruntime_HINTS}
  PATH_SUFFIXES lib lib64
)

if(WIN32)
  find_file(
    onnxruntime_RUNTIME
    NAMES onnxruntime.dll
    HINTS ${_onnxruntime_HINTS}
    PATH_SUFFIXES lib bin .
  )
  mark_as_advanced(onnxruntime_RUNTIME)
endif()

include(FindPackageHandleStandardArgs)
set(_onnxruntime_REQUIRED_VARS onnxruntime_LIBRARY onnxruntime_INCLUDE_DIR)
if(WIN32)
  list(APPEND _onnxruntime_REQUIRED_VARS onnxruntime_RUNTIME)
endif()
find_package_handle_standard_args(
  onnxruntime
  REQUIRED_VARS ${_onnxruntime_REQUIRED_VARS}
)

if(onnxruntime_FOUND AND NOT TARGET onnxruntime::onnxruntime)
  add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
  set_target_properties(
    onnxruntime::onnxruntime
    PROPERTIES
      IMPORTED_LOCATION "${onnxruntime_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(onnxruntime_INCLUDE_DIR onnxruntime_LIBRARY)
