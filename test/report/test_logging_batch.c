#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail_open, fail_format;
static FILE* checked_open_memstream(char** bytes, size_t* size)
{
    if (fail_open) { errno = ENOMEM; return NULL; }
    return open_memstream(bytes, size);
}
static int checked_vfprintf(FILE* stream, const char* format, va_list args)
{
    if (fail_format) {
        fail_format = 0;
        assert(fwrite("PARTIAL", 1, 7, stream) == 7);
        errno = ENOMEM;
        return -1;
    }
    return vfprintf(stream, format, args);
}
/* Exercise real logging with deterministic allocation/partial-format faults. */
#define open_memstream checked_open_memstream
#define vfprintf checked_vfprintf
#include "../../src/logging.c"
#undef open_memstream
#undef vfprintf

static FILE* capture(void)
{
    FILE* file = tmpfile();
    assert(file != NULL);
    assert(fflush(stderr) == 0);
    assert(dup2(fileno(file), STDERR_FILENO) >= 0);
    peak_log_initialize_output(0);
    return file;
}
static char* contents(FILE* file)
{
    struct stat st;
    assert(fstat(fileno(file), &st) == 0);
    char* bytes = calloc((size_t)st.st_size + 1, 1);
    assert(bytes != NULL);
    assert(pread(fileno(file), bytes, (size_t)st.st_size, 0) == st.st_size);
    return bytes;
}
static void require_text(FILE* file, const char* expected)
{
    char* text = contents(file);
    assert(strcmp(text, expected) == 0);
    free(text);
}
static void emit(void)
{
    peak_log_report("header %d\n", 17);
    for (int i = 0; i < 25; ++i)
        peak_log_report("|%32s|%12d|%12.3f|\n", "a target", i, i * .25);
    peak_log_warn("warning inside report\n");
}
static void equivalence(void)
{
    FILE* file = capture();
    emit();
    assert(peak_log_flush() == 0);
    char* direct = contents(file);
    assert(ftruncate(fileno(file), 0) == 0);
    assert(lseek(fileno(file), 0, SEEK_SET) == 0);
    assert(peak_log_report_batch_begin() == 1);
    assert(peak_log_report_batch_begin() == 0);
    emit();
    require_text(file, "");
    assert(peak_log_report_batch_end() == 0);
    require_text(file, direct);
    free(direct);
    peak_log_shutdown();
    assert(fclose(file) == 0);
}
static void prefixes(void)
{
    FILE* file = capture();
    peak_log_warn("outside\n");
    require_text(file, "outside\n");
    assert(peak_log_report_batch_begin() == 1);
    peak_log_report("first-");
    assert(peak_log_flush() == 0);
    require_text(file, "outside\nfirst-");
    assert(peak_log_flush() == 0);
    peak_log_report("second\n");
    assert(peak_log_report_batch_end() == 0);
    peak_log_warn("after\n");
    require_text(file, "outside\nfirst-second\nafter\n");
    peak_log_shutdown();
    assert(fclose(file) == 0);
}
static void failures(void)
{
    FILE* file = capture();
    fail_open = 1;
    assert(peak_log_report_batch_begin() == 0);
    peak_log_report("direct-");
    fail_open = 0;
    assert(peak_log_report_batch_begin() == 1);
    peak_log_report("prefix-");
    fail_format = 1;
    peak_log_report("replayed-%d-", 7);
    peak_log_report("tail\n");
    assert(peak_log_report_batch_end() == 0);
    require_text(file, "direct-prefix-replayed-7-tail\n");
    peak_log_shutdown();
    assert(fclose(file) == 0);
}
static void full(void)
{
    int fd = open("/dev/full", O_WRONLY);
    assert(fd >= 0 && dup2(fd, STDERR_FILENO) >= 0);
    assert(close(fd) == 0);
    peak_log_initialize_output(0);
    assert(peak_log_report_batch_begin() == 1);
    peak_log_report("undeliverable\n");
    errno = 0;
    assert(peak_log_report_batch_end() == -1 && errno != 0);
    assert(peak_log_flush() == -1);
    peak_log_shutdown();
}
static void fork_flush(void)
{
    FILE* file = capture();
    assert(peak_log_report_batch_begin() == 1);
    peak_log_report("parent-");
    pid_t child = fork();
    assert(child >= 0);
    if (!child) exit(0); /* libc flushes inherited FILEs, including memstream. */
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    require_text(file, "");
    peak_log_report("tail\n");
    assert(peak_log_report_batch_end() == 0);
    require_text(file, "parent-tail\n");
    peak_log_shutdown();
    assert(fclose(file) == 0);
}
static atomic_int ending;
static void* blocked_writer(void* unused)
{
    (void)unused;
    assert(peak_log_report_batch_begin() == 1);
    peak_log_report("blocked\n");
    atomic_store(&ending, 1);
    (void)peak_log_report_batch_end();
    return NULL;
}
static void cancellation(void)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    assert(fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == 0);
    char bytes[4096] = {0};
    while (write(pipefd[1], bytes, sizeof(bytes)) > 0) {}
    assert(errno == EAGAIN);
    assert(fcntl(pipefd[1], F_SETFL, 0) == 0);
    assert(dup2(pipefd[1], STDERR_FILENO) >= 0);
    peak_log_initialize_output(0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocked_writer, NULL) == 0);
    while (!atomic_load(&ending)) usleep(1000);
    usleep(20000);
    assert(pthread_cancel(thread) == 0);
    void* result;
    assert(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
    assert(fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0);
    while (read(pipefd[0], bytes, sizeof(bytes)) > 0) {}
    peak_log_warn("after cancellation\n");
    assert(read(pipefd[0], bytes, sizeof(bytes)) == 19);
    assert(memcmp(bytes, "after cancellation\n", 19) == 0);
    peak_log_shutdown();
    close(pipefd[0]); close(pipefd[1]);
}
int main(int argc, char** argv)
{
    alarm(15);
    assert(setenv("PEAK_VERBOSITY", "warn", 1) == 0);
    if (argc == 2) {
        peak_log_initialize_output(0);
        int owned = strcmp(argv[1], "batch") == 0 ?
                    peak_log_report_batch_begin() : 0;
        for (int i = 0; i < 10000; ++i)
            peak_log_report("row %d with a fixed profiler report payload\n", i);
        if (owned) assert(peak_log_report_batch_end() == 0);
        assert(peak_log_flush() == 0);
        peak_log_shutdown();
        return 0;
    }
    void (*checks[])(void) = {equivalence, prefixes, failures, full,
                              fork_flush, cancellation};
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { alarm(10); checks[i](); _exit(0); }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("logging_batch_test_ok");
    return 0;
}
