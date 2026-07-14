add_test(NAME test_exec_chain_platform_contract
    COMMAND ${CMAKE_COMMAND}
            -DPEAK_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_exec_chain_platform_contract.cmake)
set_tests_properties(test_exec_chain_platform_contract
    PROPERTIES
        PASS_REGULAR_EXPRESSION "exec_chain_platform_contract_ok"
        TIMEOUT 10)

add_test(NAME test_exec_chain_platform_selection_contract
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_platform_selection_contract.py
            --cmake ${CMAKE_COMMAND}
            --c-compiler ${CMAKE_C_COMPILER}
            --source-root ${PROJECT_SOURCE_DIR}
            --probe-source ${CMAKE_CURRENT_SOURCE_DIR}/platform_probe)
set_tests_properties(test_exec_chain_platform_selection_contract
    PROPERTIES
        PASS_REGULAR_EXPRESSION
            "exec_chain_platform_selection_contract_ok"
        FAIL_REGULAR_EXPRESSION "Traceback|AssertionError"
        TIMEOUT 60)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_executable(test_exec_chain test_exec_chain.c)
    target_link_options(test_exec_chain PRIVATE -Wl,--export-dynamic -Wl,--as-needed)
    target_link_libraries(test_exec_chain PRIVATE pthread ${CMAKE_DL_LIBS})

    add_library(test_execve_observer SHARED test_execve_observer.c)
    target_link_options(test_execve_observer PRIVATE -Wl,--as-needed)
    target_link_libraries(test_execve_observer PRIVATE ${CMAKE_DL_LIBS})

    add_executable(test_exec_preinit test_exec_preinit.c)
    target_link_options(test_exec_preinit PRIVATE -Wl,--export-dynamic)
    target_link_libraries(test_exec_preinit PRIVATE pthread)

    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(execvpe "unistd.h" PEAK_TEST_HAVE_EXECVPE)
    check_symbol_exists(fexecve "unistd.h" PEAK_TEST_HAVE_FEXECVE)
    check_symbol_exists(execveat "unistd.h" PEAK_TEST_HAVE_EXECVEAT)
    check_symbol_exists(SYS_clone "sys/syscall.h" PEAK_TEST_HAVE_SYS_CLONE)
    check_symbol_exists(SYS_clone3 "sys/syscall.h" PEAK_TEST_HAVE_SYS_CLONE3)
    check_symbol_exists(POSIX_SPAWN_USEVFORK "spawn.h" PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
    unset(CMAKE_REQUIRED_DEFINITIONS)

    if(PEAK_TEST_HAVE_SYS_CLONE)
        add_executable(test_exec_preinit_clone test_exec_preinit.c)
        target_link_options(test_exec_preinit_clone PRIVATE -Wl,--export-dynamic)
        target_link_libraries(test_exec_preinit_clone PRIVATE pthread)
        target_compile_definitions(test_exec_preinit_clone
            PRIVATE PEAK_TEST_FORCE_CLONE_RAW_FORK=1)
    endif()

    if(PEAK_TEST_HAVE_EXECVEAT)
        target_compile_definitions(test_exec_chain
            PRIVATE PEAK_TEST_HAVE_EXECVEAT=1)
        target_compile_definitions(test_exec_preinit
            PRIVATE PEAK_TEST_HAVE_EXECVEAT=1)
        if(TARGET test_exec_preinit_clone)
            target_compile_definitions(test_exec_preinit_clone
                PRIVATE PEAK_TEST_HAVE_EXECVEAT=1)
        endif()
    endif()
    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        target_compile_definitions(test_exec_chain
            PRIVATE PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED=1)
        target_compile_definitions(test_exec_preinit
            PRIVATE PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED=1)
        if(TARGET test_exec_preinit_clone)
            target_compile_definitions(test_exec_preinit_clone
                PRIVATE PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED=1)
        endif()
    endif()
    if(PEAK_TEST_HAVE_EXECVPE)
        target_compile_definitions(test_exec_preinit
            PRIVATE PEAK_TEST_HAVE_EXECVPE=1)
        if(TARGET test_exec_preinit_clone)
            target_compile_definitions(test_exec_preinit_clone
                PRIVATE PEAK_TEST_HAVE_EXECVPE=1)
        endif()
    endif()
    if(PEAK_TEST_HAVE_FEXECVE)
        target_compile_definitions(test_exec_preinit
            PRIVATE PEAK_TEST_HAVE_FEXECVE=1)
        if(TARGET test_exec_preinit_clone)
            target_compile_definitions(test_exec_preinit_clone
                PRIVATE PEAK_TEST_HAVE_FEXECVE=1)
        endif()
    endif()
    if(PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK=1)
    endif()
    if(PEAK_TEST_HAVE_SYS_CLONE3)
        target_compile_definitions(test_exec_chain
            PRIVATE PEAK_TEST_HAVE_SYS_CLONE3=1)
    endif()

    set(_PEAK_EXEC_PREINIT_CASES
        execve execv execl execle execvp execvp_eacces_eloop execlp
        posix_spawn posix_spawnp)
    if(PEAK_TEST_HAVE_EXECVPE)
        list(APPEND _PEAK_EXEC_PREINIT_CASES execvpe)
    endif()
    if(PEAK_TEST_HAVE_FEXECVE)
        list(APPEND _PEAK_EXEC_PREINIT_CASES fexecve)
    endif()
    if(PEAK_TEST_HAVE_EXECVEAT)
        list(APPEND _PEAK_EXEC_PREINIT_CASES execveat)
    endif()
    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        list(APPEND _PEAK_EXEC_PREINIT_CASES raw_execve)
        if(PEAK_TEST_HAVE_EXECVEAT)
            list(APPEND _PEAK_EXEC_PREINIT_CASES raw_execveat)
        endif()
    endif()
    string(REPLACE ";" "," _PEAK_EXEC_PREINIT_CASES_ARG
        "${_PEAK_EXEC_PREINIT_CASES}")

    set(_PEAK_EXEC_DEFAULT_BYPASS_CASES
        execv execvpe execl execle fexecve posix_spawnp)
    if(PEAK_TEST_HAVE_EXECVEAT)
        list(APPEND _PEAK_EXEC_DEFAULT_BYPASS_CASES execveat)
    endif()
    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        list(APPEND _PEAK_EXEC_DEFAULT_BYPASS_CASES raw_execve)
        if(PEAK_TEST_HAVE_EXECVEAT)
            list(APPEND _PEAK_EXEC_DEFAULT_BYPASS_CASES raw_execveat)
        endif()
    endif()
    string(REPLACE ";" "," _PEAK_EXEC_DEFAULT_BYPASS_CASES_ARG
        "${_PEAK_EXEC_DEFAULT_BYPASS_CASES}")
    add_test(NAME test_exec_preinit_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_preinit_check.py
                --exe $<TARGET_FILE:test_exec_preinit>
                --libpeak $<TARGET_FILE:peak>
                --expected-cases ${_PEAK_EXEC_PREINIT_CASES_ARG})
    set_tests_properties(test_exec_preinit_contract
        PROPERTIES
            PASS_REGULAR_EXPRESSION "exec_preinit_contract_ok"
            FAIL_REGULAR_EXPRESSION "Traceback|AssertionError"
            TIMEOUT 20)
    if(TARGET test_exec_preinit_clone)
        add_test(NAME test_exec_preinit_clone_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_preinit_check.py
                    --exe $<TARGET_FILE:test_exec_preinit_clone>
                    --libpeak $<TARGET_FILE:peak>
                    --expected-cases ${_PEAK_EXEC_PREINIT_CASES_ARG})
        set_tests_properties(test_exec_preinit_clone_contract
            PROPERTIES
                PASS_REGULAR_EXPRESSION "exec_preinit_contract_ok"
                FAIL_REGULAR_EXPRESSION "Traceback|AssertionError"
                TIMEOUT 20)
    endif()

    set(_PEAK_EXEC_CHAIN_TESTS
        execv_success_checkpoint
        exec_checkpoint_write_target_no_reentry
        execv_zero_call_checkpoint
        exec_checkpoint_disabled
        exec_checkpoint_direct_disabled
        exec_bad_stats_path_nonfatal
        execve_custom_env_injection
        execve_call_env_optin_default_bypass
        raw_syscall_execve_custom_env_injection
        raw_syscall_execve_backend_disabled_injection
        raw_syscall_execve_failure_non_destructive
        raw_syscall_nonexec_passthrough
        execve_preflight_unavailable_injection
        execve_child_peak_env_only_injection
        execve_null_env_injection
        execve_loader_path_missing
        execve_loader_path_explicit
        execve_loader_path_chain_disabled
        execve_loader_path_secure_skip
        execve_large_env_injection
        execve_env_build_failure_passthrough
        execve_bad_env_failure_non_destructive
        execve_bad_env_preflight_unknown_non_destructive
        execve_bad_argv_failure_non_destructive
        execve_explicit_peak_env_preserved
        execve_child_chain_disabled
        execve_child_checkpoint_disabled
        execve_child_propagate_disabled
        execve_propagate_disabled
        execve_chain_disabled
        execve_secure_skip
        fexecve_custom_env_injection
        fexecve_preflight_unavailable_injection
        fexecve_bad_env_failure_non_destructive
        fexecve_bad_env_preflight_unknown_non_destructive
        fexecve_bad_argv_failure_non_destructive
        execl_success_checkpoint
        execlp_path_search
        execlp_default_bypass_without_execvpe
        execle_custom_env_injection
        exec_failure_non_destructive
        execvp_path_search
        execvp_default_bypass_without_execvpe
        execvp_empty_path_component
        helper_named_exec_still_checkpointed
        execvpe_caller_path_child_env_ignored
        no_duplicate_ld_preload_execvpe_path_fallback
        execvp_enoent_failure
        execvp_enotdir_enoent_failure
        execvp_enotdir_enoent_fallback
        execvp_eacces_failure
        execvp_eacces_fallback
        execvp_enoent_fallback
        execvp_enoexec_fallback
        fork_child_exec_parent_exec
        vfork_child_exec_parent_exec
        vfork_child_failed_exec_parent_exec
        clone_private_vm_exec
        clone_vm_exec
        clone_vfork_exec
        vfork_execl_parent_exec
        vfork_execlp_parent_exec
        vfork_execle_parent_exec
        fork_custom_env_execve_chain
        vfork_custom_env_execve_chain
        fork_loader_path
        vfork_loader_path
        fork_loader_path_secure_skip
        vfork_loader_path_secure_skip
        fork_parent_env_exhaustion
        vfork_custom_env_execle_chain
        fork_raw_syscall_custom_env_chain
        fork_raw_syscall_chain_disabled
        fork_custom_env_chain_disabled
        vfork_custom_env_chain_disabled
        vfork_custom_env_execvpe_chain
        vfork_custom_env_execvp_chain
        vfork_execvp_native_fallback
        vfork_custom_env_execlp_chain
        vfork_custom_env_fexecve_chain
        fork_bad_env_vector_efault
        vfork_bad_env_vector_efault
        fork_bad_env_string_efault
        vfork_bad_env_string_efault
        vfork_env_slots_fallback
        vfork_long_preload_fallback
        vfork_preload_entries_fallback
        no_duplicate_ld_preload
        no_duplicate_ld_preload_whitespace
        no_duplicate_ld_preload_env_entries
        posix_spawn_custom_env_injection
        posix_spawn_call_env_optin_default_bypass
        posix_spawn_env_build_failure_passthrough
        posix_spawn_null_env_injection
        posix_spawn_bad_env_failure_non_destructive
        posix_spawn_bad_env_preflight_unknown_non_destructive
        posix_spawn_bad_argv_failure_non_destructive
        posix_spawn_preflight_unavailable_injection
        posix_spawn_preflight_unavailable_bad_env_delegates
        posix_spawn_duplicate_ld_preload
        posix_spawn_actions_attrs_injection
        posix_spawn_actions_close_stderr
        posix_spawn_child_peak_env_only_injection
        posix_spawn_chain_disabled
        posix_spawn_failure_non_destructive
        posix_spawn_resolver_null_preserves_errno
        posix_spawnp_path_search
        posix_spawnp_env_build_failure_passthrough
        posix_spawnp_resolver_null_preserves_errno
        posix_spawnp_empty_path_component
        posix_spawnp_child_env_path_ignored
        posix_spawnp_preflight_unavailable_delegates
        posix_spawnp_preflight_unavailable_bad_env_delegates
        posix_spawnp_bad_env_failure_non_destructive
        posix_spawnp_bad_env_preflight_unknown_non_destructive
        posix_spawnp_bad_argv_failure_non_destructive
        posix_spawn_explicit_peak_env_preserved
        exec_checkpoint_concurrent_fini_callbacks
        exec_checkpoint_snapshot_lock_contention
        exec_checkpoint_statistics_snapshot_contention
        exec_checkpoint_statistics_shadow_invalidation
        exec_checkpoint_fork_child_fini
        signal_handler_execve_default_bypass
        default_disabled_family_bypass
        text_output_not_corrupted)

    if(PEAK_TEST_HAVE_EXECVEAT)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            execveat_custom_env_injection
            raw_syscall_execveat_custom_env_injection
            execveat_bad_env_failure_non_destructive
            execveat_bad_env_preflight_unknown_non_destructive
            execveat_bad_argv_failure_non_destructive)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            vfork_custom_env_execveat_chain
            vfork_raw_syscall_execveat_chain)
    endif()

    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED AND PEAK_TEST_HAVE_SYS_CLONE)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            raw_clone_libc_exec
            raw_clone_raw_exec)
    endif()

    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            raw_syscall_execve_observer_bypass)
    endif()

    if(PEAK_TEST_HAVE_SYS_CLONE3)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS clone3_exec)
    endif()

    if(NOT MPI_FOUND)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            execve_loader_path_empty
            execve_loader_path_duplicate
            execve_loader_path_preload_present
            vfork_loader_path_preload_present)
    endif()

    if(PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS posix_spawn_usevfork_custom_env)
    endif()

    set(_PEAK_EXEC_DEDICATED_DEFAULT_BYPASS_TESTS
        execve_call_env_optin_default_bypass
        execvp_default_bypass_without_execvpe
        execlp_default_bypass_without_execvpe
        posix_spawn_call_env_optin_default_bypass)
    foreach(_peak_exec_dedicated_test
            IN LISTS _PEAK_EXEC_DEDICATED_DEFAULT_BYPASS_TESTS)
        list(FIND _PEAK_EXEC_CHAIN_TESTS
            "${_peak_exec_dedicated_test}" _peak_exec_test_index)
        if(_peak_exec_test_index EQUAL -1)
            message(FATAL_ERROR
                "missing dedicated default-bypass test: "
                "${_peak_exec_dedicated_test}")
        endif()
    endforeach()

    add_test(NAME test_exec_chain_check_contracts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_exec_chain_check_contracts.py)
    set_tests_properties(test_exec_chain_check_contracts
        PROPERTIES
            PASS_REGULAR_EXPRESSION "OK"
            TIMEOUT 10)

    add_test(NAME test_exec_chain_checkpoint_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_chain_check.py
                --self-test-checkpoint-contract)
    set_tests_properties(test_exec_chain_checkpoint_contract
        PROPERTIES
            PASS_REGULAR_EXPRESSION "exec_chain_checkpoint_contract_ok"
            TIMEOUT 10)

    add_test(NAME test_exec_chain_timeout_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_chain_check.py
                --self-test-timeout-contract)
    set_tests_properties(test_exec_chain_timeout_contract
        PROPERTIES
            PASS_REGULAR_EXPRESSION "exec_chain_timeout_contract_ok"
            TIMEOUT 10)

    foreach(_peak_exec_test IN LISTS _PEAK_EXEC_CHAIN_TESTS)
        add_test(NAME test_${_peak_exec_test}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_chain_check.py
                    --mode ${_peak_exec_test}
                    --exe $<TARGET_FILE:test_exec_chain>
                    --libpeak $<TARGET_FILE:peak>
                    --execve-observer $<TARGET_FILE:test_execve_observer>
                    --expected-default-bypass-cases
                        ${_PEAK_EXEC_DEFAULT_BYPASS_CASES_ARG})
        set_tests_properties(test_${_peak_exec_test}
            PROPERTIES
                PASS_REGULAR_EXPRESSION "exec_chain_check_ok mode=${_peak_exec_test}"
                FAIL_REGULAR_EXPRESSION "Traceback|AssertionError|exec_chain_check_failed"
                TIMEOUT 30)
    endforeach()
endif()
