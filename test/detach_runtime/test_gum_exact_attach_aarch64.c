#include "frida-gum.h"

#include <stdio.h>
#include <stdlib.h>

extern int peak_test_exact_entry_one(int value);
extern int peak_test_exact_entry_two(int value);
extern int peak_test_exact_dispatch(int value);

static void
count_entry(GumInvocationContext* context, gpointer user_data)
{
    guint* count = (guint*)user_data;

    (void)context;
    (*count)++;
}

static int
expect_call(const char* label,
            int (*function)(int),
            int value,
            int expected_result,
            const guint* entry_one_count,
            const guint* entry_two_count,
            guint expected_entry_one_count,
            guint expected_entry_two_count)
{
    int result = function(value);

    if (result != expected_result ||
        *entry_one_count != expected_entry_one_count ||
        *entry_two_count != expected_entry_two_count) {
        fprintf(stderr,
                "%s: result=%d expected_result=%d entry_one=%u/%u entry_two=%u/%u\n",
                label,
                result,
                expected_result,
                *entry_one_count,
                expected_entry_one_count,
                *entry_two_count,
                expected_entry_two_count);
        return 1;
    }

    return 0;
}

int
main(void)
{
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_one;
    GumAttachReturn attach_two;
    guint entry_one_count = 0;
    guint entry_two_count = 0;
    gboolean attached_one = FALSE;
    gboolean attached_two = FALSE;
    int failed = 0;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(count_entry,
                                          NULL,
                                          &entry_one_count,
                                          NULL);
    listener_two = gum_make_call_listener(count_entry,
                                          NULL,
                                          &entry_two_count,
                                          NULL);
    if (listener_one == NULL || listener_two == NULL) {
        fputs("failed to create exact-attach listeners\n", stderr);
        failed = 1;
        goto cleanup;
    }

    gum_interceptor_begin_transaction(interceptor);
    attach_one = gum_interceptor_peak_attach_exact(
        interceptor,
        (gpointer)peak_test_exact_entry_one,
        listener_one,
        NULL);
    attach_two = gum_interceptor_peak_attach_exact(
        interceptor,
        (gpointer)peak_test_exact_entry_two,
        listener_two,
        NULL);
    gum_interceptor_end_transaction(interceptor);
    attached_one = attach_one == GUM_ATTACH_OK;
    attached_two = attach_two == GUM_ATTACH_OK;
    if (attach_one != GUM_ATTACH_OK || attach_two != GUM_ATTACH_OK) {
        fprintf(stderr,
                "exact attach failed: entry_one=%d entry_two=%d\n",
                attach_one,
                attach_two);
        failed = 1;
        goto cleanup;
    }
    gum_interceptor_flush(interceptor);

    failed |= expect_call("shared dispatcher",
                          peak_test_exact_dispatch,
                          1,
                          12,
                          &entry_one_count,
                          &entry_two_count,
                          0,
                          0);
    failed |= expect_call("entry one",
                          peak_test_exact_entry_one,
                          2,
                          13,
                          &entry_one_count,
                          &entry_two_count,
                          1,
                          0);
    failed |= expect_call("entry two",
                          peak_test_exact_entry_two,
                          3,
                          14,
                          &entry_one_count,
                          &entry_two_count,
                          1,
                          1);
    if (failed) {
        goto cleanup;
    }

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_flush(interceptor);
    attached_one = FALSE;
    failed |= expect_call("detached entry one",
                          peak_test_exact_entry_one,
                          4,
                          15,
                          &entry_one_count,
                          &entry_two_count,
                          1,
                          1);
    failed |= expect_call("entry two remains attached",
                          peak_test_exact_entry_two,
                          5,
                          16,
                          &entry_one_count,
                          &entry_two_count,
                          1,
                          2);

cleanup:
    if (attached_one) {
        gum_interceptor_detach(interceptor, listener_one);
    }
    if (attached_two) {
        gum_interceptor_detach(interceptor, listener_two);
    }
    gum_interceptor_flush(interceptor);
    g_clear_object(&listener_one);
    g_clear_object(&listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    if (failed) {
        return EXIT_FAILURE;
    }

    puts("gum_exact_attach_aarch64_ok");
    return EXIT_SUCCESS;
}
