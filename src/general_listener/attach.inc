static void
peak_general_overhead_bootstrapping()
{
    GumInvocationListener* listener_bootstrapping =
        g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PEAKGENERAL_LISTENER(listener_bootstrapping)->hook_id = 0;

    PeakDetachRequest mutation_request = {
        .hook_id = 0,
        .symbol_name = "peak_general_overhead_dummy_func",
        .function_address = &peak_general_overhead_dummy_func,
        .interceptor = interceptor,
        .listener = listener_bootstrapping,
        .operation = PEAK_DETACH_OPERATION_ATTACH
    };
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        peak_log_debug("[peak] skipping overhead calibration Gum attach: %s\n",
                   peak_detach_controller_status_string(detach_status));
        peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
        g_object_unref(listener_bootstrapping);
        peak_general_overhead = 0.0;
        return;
    }

    gum_interceptor_begin_transaction(interceptor);
    GumAttachReturn attach_status =
        gum_interceptor_attach(interceptor,
                               &peak_general_overhead_dummy_func,
                               listener_bootstrapping,
                               NULL);
    gum_interceptor_end_transaction(interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("overhead attach finish",
                                                        detach_status);
    }

    if (attach_status != GUM_ATTACH_OK) {
        g_printerr("[peak] overhead calibration Gum attach failed, status=%d\n",
                   attach_status);
        peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
        g_object_unref(listener_bootstrapping);
        peak_general_overhead = 0.0;
        return;
    }

    guint n_tests = 2000;
    double* time = g_new(double, n_tests * 2);
    for (guint i = 0; i < n_tests; i++) {
        time[n_tests + i] = peak_second();
        peak_general_overhead_dummy_func();
        time[n_tests + i] = peak_second() - time[n_tests + i];
    }

    // g_printerr("%10lu times  %10.3f s total  %10.3e s max  %10.3e s min \n",
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->num_calls[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->total_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->max_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->min_time[0]);
    mutation_request.listener = listener_bootstrapping;
    mutation_request.operation = PEAK_DETACH_OPERATION_DETACH;
    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        g_printerr("[peak] fatal overhead calibration Gum detach prepare failed after attach: %s\n",
                   peak_detach_controller_status_string(detach_status));
        peak_general_overhead = 0.0;
        g_free(time);
        peak_detach_controller_abort_after_failed_finish("overhead detach prepare",
                                                        detach_status);
    }
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_detach(interceptor, listener_bootstrapping);
    gum_interceptor_end_transaction(interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("overhead detach finish",
                                                        detach_status);
    }
    if (!peak_general_controller_flush_teardown()) {
        g_printerr("[peak] overhead calibration Gum teardown did not flush; leaving bootstrap listener alive\n");
        g_free(time);
        return;
    }
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
    g_object_unref(listener_bootstrapping);

    for (guint i = 0; i < n_tests; i++) {
        time[i] = peak_second();
        peak_general_overhead_dummy_func();
        time[i] = peak_second() - time[i];
    }

    // g_printerr("orig %.6e time %.6e\n", orig_time, time);
    peak_general_overhead = (median_double(&time[n_tests], n_tests) - median_double(&time[0], n_tests));
    if (peak_general_overhead <= 0.0) {
        peak_general_overhead = peak_general_overhead_floor;
    }
    g_free(time);
}

static void
peak_symbol_map_add_target(GHashTable* table, const char* symbol)
{
    if (symbol == NULL || g_hash_table_contains(table, symbol)) {
        return;
    }

    g_hash_table_insert(table, g_strdup(symbol), g_ptr_array_new());
}

static void peak_build_symbol_map_once(size_t first_target_index) {
    gum_find_functions_matching_initialize = true;
    gum_symbol_demangled_mapping = g_hash_table_new_full(g_str_hash,
                                                         str_equal_function_general,
                                                         g_free,
                                                         (GDestroyNotify) g_ptr_array_unref);
    gum_symbol_short_mapping = g_hash_table_new_full(g_str_hash,
                                                     str_equal_function_general,
                                                     g_free,
                                                     (GDestroyNotify) g_ptr_array_unref);

    for (size_t i = first_target_index; i < peak_hook_address_count; i++) {
        const char* symbol = peak_hook_strings[i];
        if (!peak_symbol_should_use_cpp_map(symbol) ||
            cxa_demangle_status(symbol) == 0) {
            continue;
        }

        peak_symbol_map_add_target(gum_symbol_demangled_mapping, symbol);
        peak_symbol_map_add_target(gum_symbol_short_mapping, symbol);
    }

    if (g_hash_table_size(gum_symbol_demangled_mapping) == 0 &&
        g_hash_table_size(gum_symbol_short_mapping) == 0) {
        return;
    }

    GArray* addresses = gum_find_functions_matching("_Z*");
    for (gsize j = 0; j < addresses->len; j++) {
        gpointer addr = g_array_index(addresses, gpointer, j);
        if (!addr) continue;
        gchar* mangled = gum_symbol_name_from_address(addr);
        if (!mangled) continue;

        char* demangled = cxa_demangle(mangled);
        g_free(mangled);
        if (!demangled) continue;

        GPtrArray* demangled_candidates =
            g_hash_table_lookup(gum_symbol_demangled_mapping, demangled);
        if (demangled_candidates) {
            g_ptr_array_add(demangled_candidates, addr);
        }

        char* function_name = extract_function_name(demangled);
        GPtrArray* short_candidates =
            g_hash_table_lookup(gum_symbol_short_mapping, function_name);
        if (short_candidates) {
            g_ptr_array_add(short_candidates, addr);
        }

        free(function_name);
        free(demangled);
    }

    g_array_free(addresses, TRUE);
}

void peak_general_listener_attach()
{
    peak_general_controller_init_trace_config();
    peak_general_listener_init_attach_policy();
    peak_general_listener_final_heartbeat_overhead_seconds = 0.0;
    peak_general_listener_reset_final_report_snapshot();
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        FALSE,
        memory_order_release);
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
    array_listener_revisited = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detach_flushed = g_new0(gboolean, peak_hook_address_count);
    peak_hook_last_detach_time = g_new0(double, peak_hook_address_count);
    peak_hook_states = g_new0(PeakHookState, peak_hook_address_count);
    peak_hook_next_retry_time = g_new0(double, peak_hook_address_count);
    peak_hook_pending_observed_time = g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_source =
        g_new0(PeakHookRequestSource, peak_hook_address_count);
    peak_hook_pending_request_calls = g_new0(gulong, peak_hook_address_count);
    peak_hook_pending_request_ratio = g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_global_overhead =
        g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_total_time =
        g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_rate = g_new0(double, peak_hook_address_count);
    peak_hook_detach_profile_seconds =
        g_new0(double, peak_hook_address_count);
    peak_hook_reattach_request_calls =
        g_new0(gulong, peak_hook_address_count);
    peak_hook_reattach_request_calls_valid =
        g_new0(gboolean, peak_hook_address_count);
    peak_hook_cached_sample_calls = g_new0(gulong, peak_hook_address_count);
    peak_hook_cached_sample_profile_seconds =
        g_new0(double, peak_hook_address_count);
    peak_hook_cached_sample_valid =
        g_new0(gboolean, peak_hook_address_count);
    peak_hook_retry_count = g_new0(unsigned int, peak_hook_address_count);
    peak_hook_last_retry_status = g_new0(PeakDetachStatus, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    gboolean startup_attach_can_skip_stop =
        peak_general_listener_startup_attach_can_skip_stop();
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0 ||
            strcmp(peak_hook_strings[i], "PMPI_Finalize") == 0) {
            hook_address[i] = NULL;
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "close") == 0) {
            hook_address[i] = peak_general_listener_find_function("peak_close");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "exit") == 0) {
            hook_address[i] = peak_general_listener_find_function("peak_exit");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelEx") == 0) {
            // C++ API template versions, also use cudaLaunchKernelExC internal
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelExC") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernelEx") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_kernel_ex");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaGraphLaunch") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuGraphLaunch") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "dlopen") == 0) {
            peak_log_info("[peak] skipping target dlopen: PEAK owns the dlopen listener used for dynamic attach\n");
            hook_address[i] = NULL;
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else {
            gpointer ptr = peak_general_listener_find_function(peak_hook_strings[i]);
            if (ptr) {
                hook_address[i] = ptr;
                char* demangled = cxa_demangle(peak_hook_strings[i]);
                peak_demangled_strings[i] = g_strdup(demangled);
                free(demangled);
            } else {
                if (peak_symbol_should_use_cpp_map(peak_hook_strings[i])) {
                    if (!gum_find_functions_matching_initialize) {
                        peak_build_symbol_map_once(i);
                    }

                    if (cxa_demangle_status(peak_hook_strings[i]) != 0) {
                        GPtrArray* candidates =
                            g_hash_table_lookup(gum_symbol_demangled_mapping,
                                                peak_hook_strings[i]);
                        if (candidates && candidates->len > 0) {
                            // Candidate is demangled name and it will only match one symbol, so we can directly use it
                            hook_address[i] = g_ptr_array_index(candidates, 0);
                            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                        } else {
                            // Candidates might be in short names mapping, try looking up in short mapping
                            // if multiple candidates exist, we will use the first one
                            candidates = g_hash_table_lookup(gum_symbol_short_mapping,
                                                             peak_hook_strings[i]);
                            if (candidates && candidates->len > 0) {
                                hook_address[i] = g_ptr_array_index(candidates, 0);
                                gchar* mangled = gum_symbol_name_from_address(hook_address[i]);

                                if (mangled != NULL) {
                                    char* demangled = cxa_demangle(mangled);
                                    g_free(mangled);
                                    if (demangled != NULL) {
                                        peak_demangled_strings[i] = g_strdup(demangled);
                                        free(demangled);
                                    } else {
                                        /* Failed to demangle; fall back to original hook string */
                                        peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                                    }
                                } else {
                                    /* Failed to get mangled name; fall back to original hook string */
                                    peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                                }
                            }
                        }
                    }
                }
                if (hook_address[i] == NULL) {
                    peak_dynamic_attach_needed = TRUE;
                }
            }
        }
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            gpointer resolved_hook_address = hook_address[i];
            gboolean duplicate_address = FALSE;
            for (size_t j = 0; j < i; j++) {
                if (hook_address[j] == resolved_hook_address &&
                    array_listener[j] != NULL) {
                    duplicate_address = TRUE;
                    break;
                }
            }
            if (duplicate_address) {
                hook_address[i] = NULL;
                if (peak_demangled_strings[i] == NULL) {
                    peak_demangled_strings[i] =
                        g_strdup(peak_hook_strings[i]);
                }
                peak_log_debug("[peak] skipping duplicate startup target %s at %p\n",
                               peak_hook_strings[i],
                               resolved_hook_address);
                continue;
            }
            if (!peak_general_listener_attach_target_is_supported(
                    peak_hook_strings[i],
                    resolved_hook_address)) {
                hook_address[i] = NULL;
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
                continue;
            }
            GumInvocationListener* new_listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
            PEAKGENERAL_LISTENER(new_listener)->hook_id = i;
            hook_address[i] = NULL;
            PeakGumTargetAttachPlan attach_plan;
            peak_gum_target_attach_plan(resolved_hook_address, &attach_plan);
            PeakDetachRequest mutation_request = {
                .hook_id = i,
                .symbol_name = peak_hook_strings[i],
                .function_address = resolved_hook_address,
                .interceptor = interceptor,
                .listener = new_listener,
                .operation = PEAK_DETACH_OPERATION_ATTACH,
                .blocked_pc_start = attach_plan.mutation_guard_size > 0
                    ? attach_plan.mutation_address
                    : NULL,
                .blocked_pc_size = attach_plan.mutation_guard_size
            };
            PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;
            if (!startup_attach_can_skip_stop &&
                !peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                              &detach_status)) {
                g_printerr("[peak] skipping initial Gum attach for hook %lu (%s): %s\n",
                           (unsigned long)i,
                           peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>",
                           peak_detach_controller_status_string(detach_status));
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
                peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
                g_object_unref(new_listener);
                continue;
            }
            gum_interceptor_begin_transaction(interceptor);
            GumAttachReturn attach_status =
                peak_gum_interceptor_attach_target(interceptor,
                                                   resolved_hook_address,
                                                   new_listener,
                                                   &attach_plan);
            gum_interceptor_end_transaction(interceptor);
            if (!startup_attach_can_skip_stop &&
                !peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                             &detach_status)) {
                peak_detach_controller_abort_after_failed_finish("initial attach finish",
                                                                detach_status);
            }
            if (attach_status == GUM_ATTACH_OK) {
                hook_address[i] = resolved_hook_address;
                array_listener[i] = new_listener;
                array_listener_gum_detached[i] = FALSE;
                array_listener_gum_detach_flushed[i] = TRUE;
                peak_general_controller_set_state_unlocked(i, PEAK_HOOK_ATTACHED);
            } else {
                g_printerr("[peak] Gum initial attach failed for hook %lu (%s), status=%d\n",
                           (unsigned long)i,
                           peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>",
                           attach_status);
                peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
                g_object_unref(new_listener);
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
            }
        }
    }
    if (gum_find_functions_matching_initialize) {
        g_hash_table_destroy(gum_symbol_demangled_mapping);
        g_hash_table_destroy(gum_symbol_short_mapping);
        gum_symbol_demangled_mapping = NULL;
        gum_symbol_short_mapping = NULL;
        gum_find_functions_matching_initialize = false;
    }
    if (peak_hook_address_count) {
        peak_detach_count_overridden =
            peak_general_listener_parse_detach_count_override(&peak_detach_count);
        gboolean need_overhead_calibration =
            peak_detach_cost > 0 ||
            heartbeat_time != 0 ||
            peak_detach_count_overridden;
        if (need_overhead_calibration) {
            peak_general_overhead_bootstrapping();
        } else {
            peak_general_overhead = 0.0;
        }
        if (!peak_detach_count_overridden && peak_detach_cost > 0) {
            if (peak_general_overhead > 0.0) {
                peak_detach_count =
                    (peak_detach_cost > peak_general_overhead) ?
                    peak_detach_cost / peak_general_overhead : 1;
            } else {
                peak_detach_count = G_MAXULONG;
            }
        }
        peak_need_detach[0] = false;
    }
}
