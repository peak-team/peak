
add_executable(test_dgemm_mpi test_dgemm_mpi.c)
add_executable(test_mpi_exit_before_finalize test_mpi_exit_before_finalize.c)
add_executable(test_mpi_report_reduction test_mpi_report_reduction.c)
add_executable(test_mpi_report_transport
    test_mpi_report_transport.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/mpi_report_transport.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/report_formatter.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/report_snapshot.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/report_model.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/report_maxima.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/runtime_config.c
    ${PROJECT_SOURCE_DIR}/src/utils/timing.c
    ${PROJECT_SOURCE_DIR}/src/logging.c)

target_include_directories(test_dgemm_mpi
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_dgemm_mpi PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_dgemm_mpi PRIVATE ${BLAS_LIBRARIES} PUBLIC ${OpenMP_C_LIBRARIES} MPI::MPI_C)
else()
    target_link_libraries(test_dgemm_mpi PRIVATE ${BLAS_LIBRARIES} MPI::MPI_C)
endif()
target_link_libraries(test_mpi_exit_before_finalize PRIVATE MPI::MPI_C)
target_link_libraries(test_mpi_report_reduction PRIVATE peak MPI::MPI_C m)
target_compile_definitions(test_mpi_report_reduction
    PRIVATE PEAK_ENABLE_TEST_HOOKS=1 HAVE_MPI=1)
target_include_directories(test_mpi_report_transport PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/utils
    ${FRIDA_GUM_INCLUDE_DIRS})
target_compile_definitions(test_mpi_report_transport
    PRIVATE PEAK_ENABLE_TEST_HOOKS=1 HAVE_MPI=1)
target_link_libraries(test_mpi_report_transport PRIVATE MPI::MPI_C m)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_mpi_exit_before_finalize PRIVATE -Wl,--export-dynamic)
endif()
