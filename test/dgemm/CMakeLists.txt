
add_executable(test_dgemm test_dgemm.f)
include_directories(
  ${Boost_INCLUDE_DIRS}
  ${pybind11_INCLUDE_DIR}
  ${PYTHON_INCLUDE_DIRS}
  ${PROJECT_SOURCE_DIR}/include/
)

target_link_libraries(test_dgemm PRIVATE ${BLAS_LIBRARIES})