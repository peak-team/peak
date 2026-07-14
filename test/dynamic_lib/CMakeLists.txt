cmake_minimum_required(VERSION 3.16)

# ----- find 'make' to run your Makefile -----
find_program(MAKE_EXE NAMES make gmake REQUIRED)

# ----- find Python interpreter and development config used by the Makefile -----
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

set(_PY_INCLUDE_FLAGS)
foreach(_py_include_dir IN LISTS Python3_INCLUDE_DIRS)
  list(APPEND _PY_INCLUDE_FLAGS "-I${_py_include_dir}")
endforeach()
string(JOIN " " _PY_INCLUDE_FLAGS_STR ${_PY_INCLUDE_FLAGS})
execute_process(
  COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX') or '')"
  OUTPUT_VARIABLE _PY_EXT_SUFFIX
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Environment we pass to your Makefile so it picks compilers & python
# (Change CC/AR below if you want to force GCC instead of nvc)
set(_MAKE_ENV
  CC=${CMAKE_C_COMPILER}
  AR=${CMAKE_AR}
  PYTHON=${Python3_EXECUTABLE}
  PY_INCLUDE=${_PY_INCLUDE_FLAGS_STR}
  PY_LDFLAGS=
  PY_EXT_SUFFIX=${_PY_EXT_SUFFIX}
)

# Keep generated shared libraries, executables, objects, and Python extensions
# in the build tree so stale source-tree artifacts cannot affect test results.
set(DL_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DL_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# ----- build via Makefile: `make -f Makefile -j all` -----
add_custom_target(dynamic_lib_build ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${DL_SRC_DIR}/Makefile
          ${DL_SRC_DIR}/libA.c
          ${DL_SRC_DIR}/libB.c
          ${DL_SRC_DIR}/main.c
          ${DL_SRC_DIR}/customCpackage.c
          ${DL_SRC_DIR}/dlopen_flags_test.c
          ${DL_SRC_DIR}/dlopen_own_library.py
          ${DL_SRC_DIR}/import_customCpackage_variant.py
          ${DL_SRC_DIR}/test_customCpackage_script.sh
          ${DL_DIR}/
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile -j
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Building dynamic_lib (libA/libB/main + Python extensions) via Makefile"
  VERBATIM
)

# After building everything, pick the self-contained module as the canonical import name:
#   make use_staA_staB   (copies customCpackage_staA_staB<EXT> -> customCpackage<EXT>)
add_custom_command(TARGET dynamic_lib_build POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile use_staA_staB
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Selecting Python module variant: staA_staB (self-contained)"
  VERBATIM
)

set(_DLOPEN_DRAIN_SCRIPT
  "import ctypes, time; lib = ctypes.CDLL('./libB.so'); deadline = time.time() + 1.0\nwhile time.time() < deadline:\n    lib.b_dynamic()"
)
add_test(NAME test_dynamic_dlopen
  COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
)

add_test(NAME test_dynamic_dlopen_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libB.so'); lib.b_dynamic()"
)
set_tests_properties(test_dynamic_dlopen_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_first_call_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|"
)
set_tests_properties(test_dynamic_dlopen
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
)

add_test(NAME test_dynamic_customCpackage_script
  COMMAND ${CMAKE_COMMAND} -E env
          PYTHON_BIN=${Python3_EXECUTABLE}
          ${DL_DIR}/test_customCpackage_script.sh
)
set_tests_properties(test_dynamic_customCpackage_script
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    PASS_REGULAR_EXPRESSION "Imported customCpackage OK"
)

if(NOT PEAK_GUM_PEAK_API_AVAILABLE)
  add_test(NAME test_dynamic_dlopen_strict_stock_gum
    COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
  )
  set_tests_properties(test_dynamic_dlopen_strict_stock_gum
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_strict_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "skipping dlopen Gum replace: missing-gum-api"
      FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
  )
else()
  add_test(NAME test_dynamic_dlopen_strict_patched_gum
    COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
  )
  set_tests_properties(test_dynamic_dlopen_strict_patched_gum
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_strict_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
      TIMEOUT 60
  )

  set(_DYNAMIC_STRICT_ENV
      "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}")

  foreach(_a_variant IN ITEMS dynB staB)
    add_test(NAME test_dynamic_dlopen_libA_${_a_variant}_strict_patched_gum
      COMMAND ${Python3_EXECUTABLE}
              ${DL_DIR}/dlopen_own_library.py
              --delay 0
              --which A
              --a-variant ${_a_variant}
              --duration 1.0
    )
    set_tests_properties(test_dynamic_dlopen_libA_${_a_variant}_strict_patched_gum
      PROPERTIES
        WORKING_DIRECTORY ${DL_DIR}
        ENVIRONMENT "${_DYNAMIC_STRICT_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_libA_${_a_variant}_strict_statslog"
        PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
        TIMEOUT 60
    )
  endforeach()

  foreach(_py_variant IN ITEMS dynA_dynB dynA_staB staA_dynB staA_staB)
    add_test(NAME test_dynamic_customCpackage_${_py_variant}_strict_patched_gum
      COMMAND ${Python3_EXECUTABLE}
              ${DL_DIR}/import_customCpackage_variant.py
              customCpackage_${_py_variant}
              1.0
    )
    set_tests_properties(test_dynamic_customCpackage_${_py_variant}_strict_patched_gum
      PROPERTIES
        WORKING_DIRECTORY ${DL_DIR}
        ENVIRONMENT "${_DYNAMIC_STRICT_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_customCpackage_${_py_variant}_strict_statslog"
        PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
        TIMEOUT 60
    )
  endforeach()
endif()

# Optional: expose a `make-clean` helper in the CMake build
add_custom_target(dynamic_lib_clean
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile clean
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Cleaning dynamic_lib artifacts via Makefile"
  VERBATIM
)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_test(NAME test_dynamic_lib_dlopen_flags
    COMMAND ${DL_DIR}/dlopen_flags_test
  )
  set_tests_properties(test_dynamic_lib_dlopen_flags
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_HEARTBEAT_INTERVAL=0;${PRELOAD_VAR}"
  )

  add_test(NAME test_dynamic_lib_dlopen_target_no_crash
    COMMAND ${DL_DIR}/dlopen_flags_test
  )
  set_tests_properties(test_dynamic_lib_dlopen_target_no_crash
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=dlopen;PEAK_HEARTBEAT_INTERVAL=0;${PRELOAD_VAR}"
      FAIL_REGULAR_EXPRESSION "Segmentation fault|SIGSEGV|Caught signal|Fatal error"
      TIMEOUT 20
  )
endif()
