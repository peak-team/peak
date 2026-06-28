if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_executable(test_exec_chain test_exec_chain.c)
    target_link_options(test_exec_chain PRIVATE -Wl,--export-dynamic)

    include(CheckSymbolExists)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(execveat "unistd.h" PEAK_TEST_HAVE_EXECVEAT)
    check_symbol_exists(AT_EMPTY_PATH "fcntl.h" PEAK_TEST_HAVE_AT_EMPTY_PATH)
    check_symbol_exists(POSIX_SPAWN_USEVFORK "spawn.h" PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
    check_c_source_compiles("
        #include <sys/syscall.h>
        int main(void) {
        #ifndef SYS_execveat
        #error SYS_execveat unavailable
        #endif
          return 0;
        }" PEAK_TEST_HAVE_SYS_EXECVEAT)
    check_c_source_compiles("
        #define _GNU_SOURCE
        #include <linux/sched.h>
        #include <sys/syscall.h>
        int main(void) {
        #ifndef SYS_clone3
        #error SYS_clone3 unavailable
        #endif
          struct clone_args args;
          args.flags = 0;
          return (int)args.flags;
        }" PEAK_TEST_HAVE_SYS_CLONE3)
    unset(CMAKE_REQUIRED_DEFINITIONS)

    if(PEAK_TEST_HAVE_EXECVEAT)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_EXECVEAT=1)
    endif()
    if(PEAK_TEST_HAVE_AT_EMPTY_PATH)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_AT_EMPTY_PATH=1)
    endif()
    if(PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK=1)
    endif()
    if(PEAK_TEST_HAVE_SYS_EXECVEAT)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_SYS_EXECVEAT=1)
    endif()
    if(PEAK_TEST_HAVE_SYS_CLONE3)
        target_compile_definitions(test_exec_chain PRIVATE PEAK_TEST_HAVE_SYS_CLONE3=1)
    endif()

    set(_PEAK_EXEC_CHAIN_TESTS
        execv_success_checkpoint
        exec_checkpoint_write_target_no_reentry
        execv_zero_call_checkpoint
        exec_checkpoint_disabled
        execve_custom_env_injection
        execve_preflight_unavailable_injection
        execve_child_peak_env_only_injection
        syscall_execve_custom_env_injection
        syscall_execve_preflight_unavailable_injection
        syscall_execve_failure_non_destructive
        syscall_execve_bad_env_failure_non_destructive
        syscall_execve_bad_env_preflight_unknown_non_destructive
        syscall_execve_bad_argv_failure_non_destructive
        execve_bad_env_failure_non_destructive
        execve_bad_env_preflight_unknown_non_destructive
        execve_bad_argv_failure_non_destructive
        execve_chain_disabled
        execve_propagate_disabled
        execve_secure_skip
        execve_explicit_peak_env_preserved
        execve_null_env_injection
        fexecve_custom_env_injection
        fexecve_preflight_unavailable_injection
        fexecve_failure_non_destructive
        fexecve_bad_env_failure_non_destructive
        fexecve_bad_env_preflight_unknown_non_destructive
        fexecve_bad_argv_failure_non_destructive
        helper_named_exec_still_checkpointed
        configured_helper_named_exec_still_profiled
        execl_success_checkpoint
        execlp_path_search
        execle_custom_env_injection
        fork_execl_success_checkpoint
        fork_execlp_path_search
        fork_execle_custom_env_injection
        exec_bad_stats_path_nonfatal
        exec_bad_trace_path_nonfatal
        exec_trace_fifo_path_nonblocking
        exec_trace_locked_path_nonblocking
        exec_failure_non_destructive
        no_duplicate_ld_preload
        no_duplicate_ld_preload_whitespace
        no_duplicate_ld_preload_env_entries
        no_duplicate_ld_preload_execvpe_path_fallback
        fork_execve_custom_env_injection
        fork_child_work_exec_checkpoint
        fork_exec_failure_trace
        syscall_clone_execve_custom_env_injection
        syscall_clone_syscall_execve_custom_env_injection
        syscall_clone_exec_failure_trace
        syscall_clone3_execve_custom_env_injection
        syscall_clone3_bad_pointer_failure_non_destructive
        vfork_execve_custom_env_injection
        vfork_exec_failure_trace
        vfork_exec_trace_fifo_path_nonblocking
        vfork_exec_trace_locked_path_nonblocking
        vfork_execvp_enoexec_fallback
        vfork_execvp_empty_path_component
        vfork_large_child_env_injection
        vfork_native_optout_parent_exec
        fork_large_peak_env_injection
        fork_large_peak_target_file_env_injection
        vfork_large_peak_env_injection
        vfork_large_peak_target_file_env_injection
        vfork_huge_peak_env_injection
        vfork_overflow_peak_env_injection
        vfork_duplicate_ld_preload
        clone_vfork_execve_custom_env_injection
        clone_vfork_large_peak_env_injection
        clone_vfork_large_peak_target_file_env_injection
        clone_vfork_huge_peak_env_injection
        clone_vfork_overflow_peak_env_injection
        clone_vfork_exec_failure_trace
        clone_vfork_exec_trace_fifo_path_nonblocking
        clone_vfork_exec_trace_locked_path_nonblocking
        clone_vfork_execvp_enoexec_fallback
        clone_vfork_execvp_empty_path_component
        clone_vfork_duplicate_ld_preload
        clone_vfork_parent_exec_after_child
        clone_vm_execve_custom_env_injection
        clone_vm_exec_failure_trace
        clone_private_child_work_exec_checkpoint
        execvp_path_search
        execvp_empty_path_component
        execvpe_child_env_path_used_fallback
        execvp_path_search_eacces_then_success
        execvp_path_search_eacces_then_success_fallback
        execvp_path_search_fallback
        execvp_enoent_failure
        execvp_enotdir_enoent_failure
        execvp_enotdir_enoent_fallback
        execvp_eacces_failure
        execvp_eacces_fallback
        execvp_enoent_fallback
        execvp_enoexec_fallback
        fork_execvp_enoexec_fallback
        posix_spawn_custom_env_injection
        posix_spawn_null_env_injection
        posix_spawn_bad_env_failure_non_destructive
        posix_spawn_bad_env_preflight_unknown_non_destructive
        posix_spawn_bad_argv_failure_non_destructive
        posix_spawn_preflight_unavailable_injection
        posix_spawn_duplicate_ld_preload
        posix_spawn_actions_attrs_injection
        posix_spawn_actions_close_stderr
        posix_spawn_child_peak_env_only_injection
        posix_spawn_explicit_peak_env_preserved
        posix_spawn_chain_disabled
        posix_spawn_failure_non_destructive
        posix_spawnp_path_search
        posix_spawnp_child_env_path_ignored
        posix_spawnp_empty_path_component
        posix_spawnp_bad_env_failure_non_destructive
        posix_spawnp_bad_env_preflight_unknown_non_destructive
        posix_spawnp_bad_argv_failure_non_destructive
        text_output_not_corrupted)

    if(PEAK_TEST_HAVE_EXECVEAT)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            execveat_custom_env_injection
            execveat_failure_non_destructive
            execveat_bad_env_failure_non_destructive
            execveat_bad_env_preflight_unknown_non_destructive
            execveat_bad_argv_failure_non_destructive)
    endif()

    if(PEAK_TEST_HAVE_EXECVEAT AND PEAK_TEST_HAVE_AT_EMPTY_PATH)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            execveat_empty_path_custom_env
            execveat_empty_path_bad_env_failure_non_destructive
            execveat_empty_path_bad_env_preflight_unknown_non_destructive
            execveat_empty_path_bad_argv_failure_non_destructive)
    endif()

    if(PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            posix_spawn_usevfork_custom_env)
    endif()

    if(PEAK_TEST_HAVE_SYS_EXECVEAT)
        list(APPEND _PEAK_EXEC_CHAIN_TESTS
            syscall_execveat_custom_env_injection
            syscall_execveat_failure_non_destructive
            syscall_execveat_bad_env_failure_non_destructive
            syscall_execveat_bad_env_preflight_unknown_non_destructive
            syscall_execveat_bad_argv_failure_non_destructive)
    endif()

    foreach(_peak_exec_test IN LISTS _PEAK_EXEC_CHAIN_TESTS)
        add_test(NAME test_${_peak_exec_test}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_exec_chain_check.py
                    --mode ${_peak_exec_test}
                    --exe $<TARGET_FILE:test_exec_chain>
                    --libpeak $<TARGET_FILE:peak>)
        set_tests_properties(test_${_peak_exec_test}
            PROPERTIES
                PASS_REGULAR_EXPRESSION "exec_chain_check_ok mode=${_peak_exec_test}"
                FAIL_REGULAR_EXPRESSION "Traceback|AssertionError|exec_chain_check_failed"
                TIMEOUT 20)
        if(_peak_exec_test MATCHES ".*(huge|overflow)_peak_env_injection$")
            set_tests_properties(test_${_peak_exec_test}
                PROPERTIES
                    TIMEOUT 60)
        endif()
    endforeach()
endif()
