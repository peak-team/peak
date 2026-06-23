#include "general_listener.h"
#include "peak_detach_controller.h"
#include "pthread_listener.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

gboolean* peak_need_detach;
gboolean* peak_detached;
gdouble* heartbeat_overhead;
gboolean** peak_target_thread_called;
size_t peak_hook_address_count;
char** peak_hook_strings;
gulong peak_max_num_threads;
double peak_main_time;
float peak_detach_cost;
unsigned int heartbeat_time;
unsigned int check_interval;
unsigned long long sig_cont_wait_interval;
unsigned long long sig_stop_ack_wait_interval;
float target_profile_ratio;
float global_target_ratio;
float peak_global_reattach_factor;
float peak_global_detach_factor;
bool enable_per_target_heartbeat;
bool enable_global_heartbeat;
bool enable_reattach;
pthread_t heartbeat_thread;

extern gpointer* hook_address;
extern GumInvocationListener** array_listener;

static int failures;

void
dlopen_interceptor_drain_dynamic_attach_queue(void)
{
}

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_TEST_EXPORT __attribute__((visibility("default")))
#else
#define PEAK_TEST_EXPORT
#endif

PEAK_TEST_EXPORT __attribute__((noinline, noclone))
void
strict_helper_target(void)
{
#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
    asm volatile(
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

static void
check_true(const char* label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void
check_status(const char* label,
             PeakDetachStatus actual,
             PeakDetachStatus expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "FAIL: %s: expected %s, got %s\n",
                label,
                peak_detach_controller_status_string(expected),
                peak_detach_controller_status_string(actual));
        failures++;
    }
}

static void
check_contains(const char* label, const char* haystack, const char* needle)
{
    if (haystack == NULL || strstr(haystack, needle) == NULL) {
        fprintf(stderr,
                "FAIL: %s: expected '%s' in '%s'\n",
                label,
                needle,
                haystack != NULL ? haystack : "<null>");
        failures++;
    }
}

static gulong
listener_call_count(void)
{
    PeakGeneralListener* listener;

    if (array_listener == NULL || array_listener[0] == NULL) {
        return 0;
    }
    listener = PEAKGENERAL_LISTENER(array_listener[0]);
    if (listener->num_calls == NULL) {
        return 0;
    }
    return listener->num_calls[0];
}

static gboolean
detach_with_stderr_capture(char** output_out)
{
    char capture_template[] = "/tmp/peak_public_listener_stderr_XXXXXX";
    int capture_fd = mkstemp(capture_template);
    int saved_stderr = -1;
    gboolean result;
    gchar* contents = NULL;

    if (output_out != NULL) {
        *output_out = NULL;
    }
    if (capture_fd < 0) {
        perror("mkstemp");
        failures++;
        return FALSE;
    }

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || dup2(capture_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        if (saved_stderr >= 0) {
            close(saved_stderr);
        }
        close(capture_fd);
        unlink(capture_template);
        failures++;
        return FALSE;
    }
    close(capture_fd);

    fflush(stderr);
    result = peak_general_listener_dettach();
    fflush(stderr);

    if (dup2(saved_stderr, STDERR_FILENO) < 0) {
        perror("restore stderr");
        failures++;
    }
    close(saved_stderr);

    if (!g_file_get_contents(capture_template, &contents, NULL, NULL)) {
        contents = g_strdup("");
    }
    unlink(capture_template);
    if (output_out != NULL) {
        *output_out = contents;
    } else {
        g_free(contents);
    }
    return result;
}

static void
cleanup_public_listener_globals(void)
{
    if (peak_target_thread_called != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            g_free(peak_target_thread_called[i]);
        }
        g_free(peak_target_thread_called);
    }
    g_free(peak_need_detach);
    g_free(peak_detached);
    if (peak_hook_strings != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            g_free(peak_hook_strings[i]);
        }
        g_free(peak_hook_strings);
    }

    peak_target_thread_called = NULL;
    peak_need_detach = NULL;
    peak_detached = NULL;
    peak_hook_strings = NULL;
    peak_hook_address_count = 0;
}

static int
setup_public_listener_fixture(char* log_template,
                              size_t log_template_size,
                              const char* scenario)
{
    int log_fd;

    snprintf(log_template,
             log_template_size,
             "/tmp/peak_public_listener_shutdown_log_XXXXXX");
    log_fd = mkstemp(log_template);
    if (log_fd < 0) {
        perror("mkstemp");
        return -1;
    }
    close(log_fd);

    if (setenv("FAKE_DETACH_HELPER_SCENARIO", scenario, 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_LOG", log_template, 1) != 0 ||
        setenv("PEAK_TEST_CONTROLLER_SHUTDOWN_DRAIN_MS", "20", 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return -1;
    }

    gum_init_embedded();

    peak_max_num_threads = 8;
    peak_hook_address_count = 1;
    peak_hook_strings = g_new0(char*, 1);
    peak_hook_strings[0] = g_strdup("strict_helper_target");
    peak_need_detach = g_new0(gboolean, 1);
    peak_detached = g_new0(gboolean, 1);
    peak_target_thread_called = g_new0(gboolean*, 1);
    peak_target_thread_called[0] = g_new0(gboolean, peak_max_num_threads);
    peak_main_time = 0.0;
    peak_detach_cost = 0.0f;
    heartbeat_time = 0;
    check_interval = 0;
    sig_cont_wait_interval = 1000000ULL;
    sig_stop_ack_wait_interval = 1000000ULL;
    target_profile_ratio = 0.0f;
    global_target_ratio = 0.0f;
    peak_global_reattach_factor = 0.0f;
    peak_global_detach_factor = 0.0f;
    enable_per_target_heartbeat = false;
    enable_global_heartbeat = false;
    enable_reattach = false;
    heartbeat_overhead = NULL;

    pthread_listener_attach();
    peak_general_listener_attach();
    strict_helper_target();

    check_true("public listener fixture resolved target",
               hook_address != NULL && hook_address[0] != NULL);
    check_true("public listener fixture attached listener",
               array_listener != NULL && array_listener[0] != NULL);

    return failures == 0 ? 0 : -1;
}

static int
finish_public_listener_fixture(const char* log_template)
{
    int ok = failures == 0 ? 0 : -1;

    (void)peak_detach_controller_shutdown_helper(NULL);
    (void)setenv("FAKE_DETACH_HELPER_SCENARIO", "success-zero", 1);

    if (hook_address != NULL || array_listener != NULL) {
        if (!peak_general_listener_dettach()) {
            fprintf(stderr, "FAIL: public listener final cleanup\n");
            ok = -1;
        }
    }
    if (!pthread_listener_dettach()) {
        fprintf(stderr, "FAIL: public listener pthread cleanup\n");
        ok = -1;
    }

    cleanup_public_listener_globals();
    gum_deinit_embedded();

    unsetenv("PEAK_TEST_CONTROLLER_SHUTDOWN_DRAIN_MS");
    unsetenv("FAKE_DETACH_HELPER_LOG");
    if (log_template != NULL && log_template[0] != '\0') {
        unlink(log_template);
    }

    return ok;
}

static int
run_shutdown_prepare_fail_closed(void)
{
    char log_template[] = "/tmp/peak_public_listener_shutdown_log_XXXXXX";
    PeakDetachStatus helper_status = PEAK_DETACH_STATUS_ERROR;
    char* detach_log = NULL;
    gulong before_count;
    gulong after_count;

    if (setup_public_listener_fixture(log_template,
                                      sizeof(log_template),
                                      "success-zero") != 0) {
        finish_public_listener_fixture(log_template);
        return EXIT_FAILURE;
    }

    check_true("close healthy helper before prepare failure",
               peak_detach_controller_shutdown_helper(&helper_status) == TRUE);
    check_status("close healthy helper status",
                 helper_status,
                 PEAK_DETACH_STATUS_SAFE);
    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "stop-timeout", 1) != 0) {
        perror("setenv");
        finish_public_listener_fixture(log_template);
        return EXIT_FAILURE;
    }

    check_true("public shutdown prepare failure returns false",
               detach_with_stderr_capture(&detach_log) == FALSE);
    check_contains("public shutdown prepare failure bucket",
                   detach_log,
                   "bucket=prepare-exhausted status=timeout attempts=");
    check_true("public shutdown prepare failure retains hook addresses",
               hook_address != NULL && hook_address[0] != NULL);
    check_true("public shutdown prepare failure retains listener state",
               array_listener != NULL && array_listener[0] != NULL);
    check_true("public shutdown prepare failure does not hold helper threads",
               peak_detach_controller_threads_are_held() == FALSE);
    before_count = listener_call_count();
    strict_helper_target();
    after_count = listener_call_count();
    check_true("public shutdown prepare failure keeps callback state usable",
               after_count > before_count);
    g_free(detach_log);

    check_true("close failing helper after prepare failure",
               peak_detach_controller_shutdown_helper(&helper_status) == TRUE);
    check_status("close failing helper status",
                 helper_status,
                 PEAK_DETACH_STATUS_SAFE);
    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "success-zero", 1) != 0) {
        perror("setenv");
        finish_public_listener_fixture(log_template);
        return EXIT_FAILURE;
    }

    check_true("public shutdown prepare failure later cleanup succeeds",
               peak_general_listener_dettach() == TRUE);
    check_true("public shutdown prepare cleanup frees hook addresses",
               hook_address == NULL);
    check_true("public shutdown prepare cleanup frees listener array",
               array_listener == NULL);

    if (finish_public_listener_fixture(log_template) != 0) {
        return EXIT_FAILURE;
    }
    fprintf(stderr, "general-listener-shutdown-prepare-fail-closed-ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
run_idle_shutdown_io_fail_closed(void)
{
    char log_template[] = "/tmp/peak_public_listener_shutdown_log_XXXXXX";
    char* detach_log = NULL;

    if (setup_public_listener_fixture(log_template,
                                      sizeof(log_template),
                                      "shutdown-missing-response") != 0) {
        finish_public_listener_fixture(log_template);
        return EXIT_FAILURE;
    }

    check_true("public idle shutdown I/O failure returns false",
               detach_with_stderr_capture(&detach_log) == FALSE);
    check_contains("public idle shutdown I/O failure log",
                   detach_log,
                   "detach helper shutdown failed:");
    check_contains("public idle shutdown I/O failure retains listener log",
                   detach_log,
                   "leaving listener state alive");
    check_true("public idle shutdown I/O failure retains hook addresses",
               hook_address != NULL && hook_address[0] != NULL);
    check_true("public idle shutdown I/O failure retains listener state",
               array_listener != NULL && array_listener[0] != NULL);
    check_true("public idle shutdown I/O failure retains callback arrays",
               listener_call_count() > 0);
    check_true("public idle shutdown I/O failure leaves no held helper threads",
               peak_detach_controller_threads_are_held() == FALSE);
    strict_helper_target();
    g_free(detach_log);

    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "success-zero", 1) != 0) {
        perror("setenv");
        finish_public_listener_fixture(log_template);
        return EXIT_FAILURE;
    }

    check_true("public idle shutdown I/O failure later cleanup succeeds",
               peak_general_listener_dettach() == TRUE);
    check_true("public idle shutdown cleanup frees hook addresses",
               hook_address == NULL);
    check_true("public idle shutdown cleanup frees listener array",
               array_listener == NULL);

    if (finish_public_listener_fixture(log_template) != 0) {
        return EXIT_FAILURE;
    }
    fprintf(stderr, "general-listener-idle-shutdown-io-fail-closed-ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "usage: %s general-listener-shutdown-prepare-fail-closed|general-listener-idle-shutdown-io-fail-closed\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "general-listener-shutdown-prepare-fail-closed") == 0) {
        return run_shutdown_prepare_fail_closed();
    }
    if (strcmp(argv[1], "general-listener-idle-shutdown-io-fail-closed") == 0) {
        return run_idle_shutdown_io_fail_closed();
    }

    fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}
