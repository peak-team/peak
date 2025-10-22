cmake_minimum_required(VERSION 3.16)

# ----- find 'make' to run your Makefile -----
find_program(MAKE_EXE NAMES make gmake REQUIRED)

# ----- find Python (interpreter is enough) -----
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Environment we pass to your Makefile so it picks compilers & python
# (Change CC/AR below if you want to force GCC instead of nvc)
set(_MAKE_ENV
  CC=${CMAKE_C_COMPILER}
  AR=${CMAKE_AR}
  PYTHON=${Python3_EXECUTABLE}
)

# Directory that contains your Makefile (this CMakeLists.txt is in the same dir)
set(DL_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ----- build via Makefile: `make -f Makefile -j all` -----
add_custom_target(dynamic_lib_build ALL
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

# Optional: expose a `make-clean` helper in the CMake build
add_custom_target(dynamic_lib_clean
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile clean
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Cleaning dynamic_lib artifacts via Makefile"
  VERBATIM
)
