function(peak_exec_raw_syscall_supported output_var system_name system_processor)
    set(_peak_exec_supported OFF)
    if("${system_name}" MATCHES "^Linux$" AND
       "${system_processor}" MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
        set(_peak_exec_supported ON)
    endif()
    set(${output_var} ${_peak_exec_supported} PARENT_SCOPE)
endfunction()

function(peak_detach_helper_supported output_var system_name system_processor)
    # The helper depends on Linux ptrace/register ABI support, not on the
    # syscall trampoline. Keep this predicate independent as architectures
    # gain either capability on their own schedule.
    set(_peak_detach_helper_supported OFF)
    if("${system_name}" MATCHES "^Linux$" AND
       "${system_processor}" MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
        set(_peak_detach_helper_supported ON)
    endif()
    set(${output_var} ${_peak_detach_helper_supported} PARENT_SCOPE)
endfunction()

macro(peak_exec_configure_platform_support)
    peak_exec_raw_syscall_supported(
        PEAK_EXEC_RAW_SYSCALL_SUPPORTED
        "${CMAKE_SYSTEM_NAME}"
        "${CMAKE_SYSTEM_PROCESSOR}")
    peak_detach_helper_supported(
        PEAK_DETACH_HELPER_SUPPORTED
        "${CMAKE_SYSTEM_NAME}"
        "${CMAKE_SYSTEM_PROCESSOR}")
endmacro()

macro(peak_exec_enable_raw_syscall_language)
    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        enable_language(ASM)
    endif()
endmacro()

function(peak_exec_append_raw_syscall_trampoline sources_var trampoline_source)
    set(_peak_exec_sources ${${sources_var}})
    if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
        list(APPEND _peak_exec_sources "${trampoline_source}")
    endif()
    set(${sources_var} ${_peak_exec_sources} PARENT_SCOPE)
endfunction()

# Standalone budget tools have no listener runtime. Mach-O requires explicit
# lookup permission even for weak imports that may resolve to null at runtime.
function(peak_budget_configure_optional_runtime target)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        foreach(_peak_budget_optional_symbol IN ITEMS
                pthread_listener_exclude_current_thread
                pthread_listener_mark_next_created_thread_helper
                peak_global_budget_listener_idle_try_guard
                peak_global_budget_listener_idle_unguard
                peak_global_budget_listener_admission_waiting
                peak_global_budget_listener_credit_ready)
            target_link_options(${target} PRIVATE "LINKER:-U,_${_peak_budget_optional_symbol}")
        endforeach()
    endif()
endfunction()
