#include "parent_registry.h"
#include "internal/utils/parent_registry_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    unsigned long long starttime;
} TestRecord;

static int failures;

static void expect(bool condition, const char* label)
{
    if (!condition) {
        fprintf(stderr, "failed: %s (errno=%d)\n", label, errno);
        failures++;
    }
}

static void make_temp_dir(char path[PATH_MAX], const char* pattern, mode_t mode)
{
    snprintf(path, PATH_MAX, "%s", pattern);
    expect(mkdtemp(path) != NULL, "mkdtemp");
    expect(chmod(path, mode) == 0, "chmod temp directory");
}

static void join_path(char output[PATH_MAX], const char* directory, const char* name)
{
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    expect(length >= 0 && length < PATH_MAX, "join path");
}

static void runtime_dir_path(char output[PATH_MAX], const char* xdg_root)
{
    join_path(output, xdg_root, "peak");
}

static void fallback_dir_path(char output[PATH_MAX], const char* tmp_root)
{
    char name[64];
    snprintf(name, sizeof(name), "peak-%lu", (unsigned long)geteuid());
    join_path(output, tmp_root, name);
}

static void write_text(const char* path, const char* contents, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    expect(fd >= 0, "open text fixture");
    if (fd < 0) {
        return;
    }
    size_t length = strlen(contents);
    expect(write(fd, contents, length) == (ssize_t)length, "write text fixture");
    expect(fchmod(fd, mode) == 0, "chmod text fixture");
    expect(close(fd) == 0, "close text fixture");
}

static char* read_text(const char* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 1024 * 1024) {
        close(fd);
        return NULL;
    }
    char* contents = malloc((size_t)st.st_size + 1);
    if (contents == NULL) {
        close(fd);
        return NULL;
    }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t count = read(fd, contents + used, (size_t)st.st_size - used);
        if (count <= 0) {
            free(contents);
            close(fd);
            return NULL;
        }
        used += (size_t)count;
    }
    contents[used] = '\0';
    close(fd);
    return contents;
}

static size_t read_records(const char* path, TestRecord* records, size_t capacity)
{
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    char line[256];
    size_t count = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        long pid;
        unsigned long long starttime;
        char extra;
        expect(sscanf(line, "%ld %llu %c", &pid, &starttime, &extra) == 2,
               "registry record parses exactly");
        if (count < capacity) {
            records[count] = (TestRecord){(pid_t)pid, starttime};
        }
        count++;
    }
    fclose(fp);
    return count;
}

static bool has_record(const TestRecord* records, size_t count, pid_t pid,
                       unsigned long long starttime)
{
    for (size_t index = 0; index < count; index++) {
        if (records[index].pid == pid && records[index].starttime == starttime) {
            return true;
        }
    }
    return false;
}

static void write_fake_stat(const char* root, pid_t pid, unsigned long long starttime)
{
    char pid_name[64];
    char directory[PATH_MAX];
    char stat_path[PATH_MAX];
    snprintf(pid_name, sizeof(pid_name), "%ld", (long)pid);
    join_path(directory, root, pid_name);
    expect(mkdir(directory, 0700) == 0 || errno == EEXIST, "mkdir fake proc pid");
    join_path(stat_path, directory, "stat");

    int fd = open(stat_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    expect(fd >= 0, "open fake stat");
    if (fd < 0) {
        return;
    }
    char line[2048];
    int used = snprintf(line, sizeof(line), "%ld (test process) ", (long)pid);
    for (int field = 3; field <= 52 && used > 0 && used < (int)sizeof(line); field++) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%llu%s",
                         field == 22 ? starttime : 0ULL, field == 52 ? "\n" : " ");
    }
    expect(used > 0 && used < (int)sizeof(line), "format fake stat");
    expect(write(fd, line, (size_t)used) == used, "write fake stat");
    close(fd);
}

static void setup_fake_proc(char root[PATH_MAX], unsigned long long self_start,
                            unsigned long long parent_start)
{
    make_temp_dir(root, "/tmp/peak-proc-XXXXXX", 0700);
    write_fake_stat(root, getpid(), self_start);
    write_fake_stat(root, getppid(), parent_start);
}

static PeakParentRegistryTestConfig make_config(const char* proc_root,
                                                const char* xdg_root,
                                                const char* tmp_root)
{
    PeakParentRegistryTestConfig config = {
        .proc_root = proc_root,
        .xdg_runtime_dir = xdg_root,
        .tmp_root = tmp_root,
        .fault = PEAK_PARENT_FAULT_NONE,
        .fault_after = 0,
    };
    return config;
}

static void cleanup_runtime(const char* runtime_dir)
{
    static const char* names[] = {
        "parent-registry.data",
        "parent-registry.lock",
        ".parent-registry.backup",
        NULL,
    };
    char path[PATH_MAX];
    for (const char** name = names; *name != NULL; name++) {
        join_path(path, runtime_dir, *name);
        unlink(path);
    }
    rmdir(runtime_dir);
}

static void cleanup_fake_proc(const char* root, const pid_t* extra_pids, size_t extra_count)
{
    pid_t pids[64];
    size_t count = 0;
    pids[count++] = getpid();
    pids[count++] = getppid();
    for (size_t index = 0; index < extra_count && count < 64; index++) {
        pids[count++] = extra_pids[index];
    }
    for (size_t index = 0; index < count; index++) {
        char pid_name[64];
        char directory[PATH_MAX];
        char stat_path[PATH_MAX];
        snprintf(pid_name, sizeof(pid_name), "%ld", (long)pids[index]);
        join_path(directory, root, pid_name);
        join_path(stat_path, directory, "stat");
        unlink(stat_path);
        rmdir(directory);
    }
    rmdir(root);
}

static void test_xdg_and_fallback_security(void)
{
    char proc_root[PATH_MAX];
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    setup_fake_proc(proc_root, 101, 202);
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config(proc_root, xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) == 0, "validated XDG runtime used");
    runtime_dir_path(runtime_dir, xdg_root);
    struct stat st;
    expect(stat(runtime_dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
               (st.st_mode & 0777) == 0700,
           "XDG PEAK directory is private");
    cleanup_runtime(runtime_dir);

    expect(chmod(xdg_root, 0755) == 0, "make XDG runtime non-private");
    fallback_dir_path(runtime_dir, tmp_root);
    static const mode_t masks[] = {0022, 0077};
    for (size_t index = 0; index < sizeof(masks) / sizeof(masks[0]); index++) {
        mode_t old_mask = umask(masks[index]);
        expect(peak_check_parent_process_for_test(&config) == 0,
               "invalid XDG falls back under configured umask");
        expect(stat(runtime_dir, &st) == 0 && st.st_uid == geteuid() &&
                   (st.st_mode & 0777) == 0700,
               "UID fallback directory is private under configured umask");
        cleanup_runtime(runtime_dir);

        expect(mkdir(runtime_dir, 0777) == 0, "create unsafe fallback directory");
        expect(chmod(runtime_dir, 0755) == 0,
               "set exact unsafe fallback mode independent of umask");
        expect(stat(runtime_dir, &st) == 0 && (st.st_mode & 0777) == 0755,
               "unsafe fallback fixture has exact intended mode");
        expect(peak_check_parent_process_for_test(&config) == -1,
               "unsafe existing fallback directory rejected under configured umask");
        expect(rmdir(runtime_dir) == 0, "remove unsafe fallback directory");
        umask(old_mask);
    }
    rmdir(xdg_root);
    rmdir(tmp_root);
    cleanup_fake_proc(proc_root, NULL, 0);
}

static void test_identity_pruning_and_live_parent(void)
{
    char proc_root[PATH_MAX];
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char data_path[PATH_MAX];
    setup_fake_proc(proc_root, 303, 404);
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config(proc_root, xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) == 0, "initialize identity registry");
    runtime_dir_path(runtime_dir, xdg_root);
    join_path(data_path, runtime_dir, "parent-registry.data");

    char fixture[256];
    snprintf(fixture, sizeof(fixture),
             "424242 1\n%ld 399\n%ld 404\n%ld\nmalformed\n%ld 404\n",
             (long)getppid(), (long)getppid(), (long)getpid(), (long)getppid());
    write_text(data_path, fixture, 0600);
    expect(peak_check_parent_process_for_test(&config) == 1,
           "live parent matches while stale identities do not");
    TestRecord records[8];
    size_t count = read_records(data_path, records, 8);
    expect(count == 2, "dead mismatched legacy malformed and duplicate records pruned");
    expect(has_record(records, count, getppid(), 404), "live parent record retained");
    expect(has_record(records, count, getpid(), 303), "self record stored");
    expect(!has_record(records, count, getppid(), 399), "reused PID identity removed");

    cleanup_runtime(runtime_dir);
    rmdir(xdg_root);
    rmdir(tmp_root);
    cleanup_fake_proc(proc_root, NULL, 0);
}

static void test_failure_atomic_faults(void)
{
    static const PeakParentRegistryFault faults[] = {
        PEAK_PARENT_FAULT_PROC_READ,
        PEAK_PARENT_FAULT_SNAPSHOT_READ,
        PEAK_PARENT_FAULT_TEMP_WRITE,
        PEAK_PARENT_FAULT_TEMP_FSYNC,
        PEAK_PARENT_FAULT_RENAME,
        PEAK_PARENT_FAULT_DIR_FSYNC,
    };
    char proc_root[PATH_MAX];
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char data_path[PATH_MAX];
    setup_fake_proc(proc_root, 505, 606);
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config(proc_root, xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) == 0, "initialize fault registry");
    runtime_dir_path(runtime_dir, xdg_root);
    join_path(data_path, runtime_dir, "parent-registry.data");
    const char* prior = "424242 77\nlegacy-entry\n";

    for (size_t index = 0; index < sizeof(faults) / sizeof(faults[0]); index++) {
        write_text(data_path, prior, 0600);
        config.fault = faults[index];
        config.fault_after = 0;
        expect(peak_check_parent_process_for_test(&config) == -1,
               "injected transaction failure fails safely");
        char* after = read_text(data_path);
        expect(after != NULL && strcmp(after, prior) == 0,
               "injected failure preserves exact prior snapshot");
        free(after);
    }

    char valid_prior[128];
    snprintf(valid_prior, sizeof(valid_prior), "%ld 606\n424242 77\n",
             (long)getppid());
    write_text(data_path, valid_prior, 0600);
    config.fault = PEAK_PARENT_FAULT_PROC_READ;
    config.fault_after = 2;
    expect(peak_check_parent_process_for_test(&config) == -1,
           "proc failure while validating snapshot fails safely");
    char* after = read_text(data_path);
    expect(after != NULL && strcmp(after, valid_prior) == 0,
           "mid-snapshot proc failure preserves prior snapshot");
    free(after);

    write_text(data_path, valid_prior, 0600);
    config.fault = PEAK_PARENT_FAULT_TEMP_WRITE;
    config.fault_after = 1;
    expect(peak_check_parent_process_for_test(&config) == -1,
           "partial temporary write fails safely");
    after = read_text(data_path);
    expect(after != NULL && strcmp(after, valid_prior) == 0,
           "partial temporary write preserves prior snapshot");
    free(after);

    unlink(data_path);
    static const PeakParentRegistryFault first_write_faults[] = {
        PEAK_PARENT_FAULT_TEMP_WRITE,
        PEAK_PARENT_FAULT_TEMP_FSYNC,
        PEAK_PARENT_FAULT_RENAME,
        PEAK_PARENT_FAULT_DIR_FSYNC,
    };
    for (size_t index = 0;
         index < sizeof(first_write_faults) / sizeof(first_write_faults[0]); index++) {
        config.fault = first_write_faults[index];
        config.fault_after = 0;
        expect(peak_check_parent_process_for_test(&config) == -1,
               "failed initial transaction fails safely");
        expect(access(data_path, F_OK) != 0 && errno == ENOENT,
               "failed initial transaction preserves absent registry");
    }

    cleanup_runtime(runtime_dir);
    rmdir(xdg_root);
    rmdir(tmp_root);
    cleanup_fake_proc(proc_root, NULL, 0);
}

static void test_unsafe_files_rejected(void)
{
    char proc_root[PATH_MAX];
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char data_path[PATH_MAX];
    char target_path[PATH_MAX];
    char lock_path[PATH_MAX];
    setup_fake_proc(proc_root, 707, 808);
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config(proc_root, xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) == 0, "initialize security registry");
    runtime_dir_path(runtime_dir, xdg_root);
    join_path(data_path, runtime_dir, "parent-registry.data");
    join_path(lock_path, runtime_dir, "parent-registry.lock");
    join_path(target_path, xdg_root, "symlink-target");
    write_text(target_path, "do-not-follow\n", 0600);
    unlink(data_path);
    expect(symlink(target_path, data_path) == 0, "create snapshot symlink");
    expect(peak_check_parent_process_for_test(&config) == -1, "snapshot symlink rejected");
    char* target = read_text(target_path);
    expect(target != NULL && strcmp(target, "do-not-follow\n") == 0,
           "snapshot symlink target unchanged");
    free(target);
    unlink(data_path);
    expect(peak_check_parent_process_for_test(&config) == 0, "recreate secure snapshot");
    expect(chmod(data_path, 0666) == 0, "make snapshot permissions unsafe");
    expect(peak_check_parent_process_for_test(&config) == -1,
           "unsafe snapshot permissions rejected");

    expect(chmod(data_path, 0600) == 0, "restore snapshot permissions");
    expect(chmod(lock_path, 0666) == 0, "make lock permissions unsafe");
    expect(peak_check_parent_process_for_test(&config) == -1,
           "unsafe stable lock permissions rejected");
    expect(chmod(lock_path, 0600) == 0, "restore lock permissions");

    expect(unlink(lock_path) == 0, "remove stable lock fixture");
    expect(symlink(target_path, lock_path) == 0, "create stable lock symlink");
    expect(peak_check_parent_process_for_test(&config) == -1,
           "stable lock symlink rejected");
    target = read_text(target_path);
    expect(target != NULL && strcmp(target, "do-not-follow\n") == 0,
           "stable lock symlink target unchanged");
    free(target);
    expect(unlink(lock_path) == 0, "remove stable lock symlink");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "recreate secure stable lock");

    int oversized_fd = open(data_path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    expect(oversized_fd >= 0, "open oversized snapshot fixture");
    off_t oversized_length = 1024 * 1024 + 1;
    expect(oversized_fd >= 0 && ftruncate(oversized_fd, oversized_length) == 0,
           "create oversized snapshot fixture");
    if (oversized_fd >= 0) {
        close(oversized_fd);
    }
    expect(peak_check_parent_process_for_test(&config) == -1,
           "oversized snapshot rejected");
    struct stat oversized_st;
    expect(stat(data_path, &oversized_st) == 0 && oversized_st.st_size == oversized_length,
           "oversized snapshot preserved exactly");

    unlink(target_path);
    cleanup_runtime(runtime_dir);
    rmdir(xdg_root);
    rmdir(tmp_root);
    cleanup_fake_proc(proc_root, NULL, 0);
}

static void test_interrupted_transaction_recovery(void)
{
    char proc_root[PATH_MAX];
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char data_path[PATH_MAX];
    char backup_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char hostile_path[PATH_MAX];
    setup_fake_proc(proc_root, 909, 1001);
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config(proc_root, xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) == 0,
           "initialize interrupted transaction registry");
    runtime_dir_path(runtime_dir, xdg_root);
    join_path(data_path, runtime_dir, "parent-registry.data");
    join_path(backup_path, runtime_dir, ".parent-registry.backup");
    join_path(temp_path, runtime_dir, ".crash-stage-temp");
    join_path(hostile_path, xdg_root, "hostile-hardlink-source");

    expect(link(data_path, backup_path) == 0, "simulate crash before snapshot rename");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "recover crash before snapshot rename");
    expect(access(backup_path, F_OK) != 0 && errno == ENOENT,
           "pre-rename crash backup cleaned");

    expect(link(data_path, backup_path) == 0, "preserve old snapshot before rename");
    write_text(temp_path, "424242 1\n", 0600);
    expect(rename(temp_path, data_path) == 0, "simulate crash after snapshot rename");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "recover crash after snapshot rename");
    TestRecord records[4];
    expect(read_records(data_path, records, 4) == 1,
           "post-rename recovered snapshot remains parseable");
    expect(access(backup_path, F_OK) != 0 && errno == ENOENT,
           "post-rename crash backup cleaned");

    write_text(hostile_path, "hostile-backup\n", 0600);
    expect(link(hostile_path, backup_path) == 0,
           "install unexpected external hardlink as backup");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "unexpected backup hardlink does not deny registry update");
    expect(access(backup_path, F_OK) != 0 && errno == ENOENT,
           "unexpected backup hardlink name removed");
    char* hostile_contents = read_text(hostile_path);
    expect(hostile_contents != NULL && strcmp(hostile_contents, "hostile-backup\n") == 0,
           "external hardlink source remains unchanged");
    free(hostile_contents);
    struct stat hostile_st;
    expect(stat(hostile_path, &hostile_st) == 0 && hostile_st.st_nlink == 1,
           "external hardlink source link count restored");

    expect(link(hostile_path, backup_path) == 0,
           "install unexpected hardlink without data snapshot");
    expect(unlink(data_path) == 0, "remove data before hostile backup recovery");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "backup-only hostile hardlink is removed without denial");
    expect(access(backup_path, F_OK) != 0 && errno == ENOENT,
           "backup-only hostile hardlink name removed");
    expect(read_records(data_path, records, 4) == 1,
           "fresh snapshot created after hostile backup removal");

    expect(symlink(hostile_path, backup_path) == 0,
           "install hostile backup symlink");
    expect(peak_check_parent_process_for_test(&config) == 0,
           "hostile backup symlink removed without persistent denial");
    expect(access(backup_path, F_OK) != 0 && errno == ENOENT,
           "hostile backup symlink name removed");

    unlink(hostile_path);
    cleanup_runtime(runtime_dir);
    rmdir(xdg_root);
    rmdir(tmp_root);
    cleanup_fake_proc(proc_root, NULL, 0);
}

static void test_crash_and_concurrent_registration(void)
{
    enum { workers = 8, iterations = 32 };
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char data_path[PATH_MAX];
    char lock_path[PATH_MAX];
    int release_pipe[2];
    int result_pipe[2];
    pid_t children[workers];
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    PeakParentRegistryTestConfig config = make_config("/proc", xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) >= 0, "register stress parent");
    runtime_dir_path(runtime_dir, xdg_root);
    join_path(lock_path, runtime_dir, "parent-registry.lock");
    struct stat initial_lock_st;
    expect(stat(lock_path, &initial_lock_st) == 0, "stat initial stable lock");
    expect(pipe(release_pipe) == 0 && pipe(result_pipe) == 0, "create stress pipes");
    for (int index = 0; index < workers; index++) {
        children[index] = fork();
        expect(children[index] >= 0, "fork concurrent registrant");
        if (children[index] == 0) {
            close(release_pipe[1]);
            close(result_pipe[0]);
            int result = 1;
            for (int iteration = 0; iteration < iterations; iteration++) {
                if (peak_check_parent_process_for_test(&config) != 1) {
                    result = 0;
                    break;
                }
            }
            (void)write(result_pipe[1], &result, sizeof(result));
            char token;
            (void)read(release_pipe[0], &token, 1);
            _exit(result ? 0 : 1);
        }
    }
    close(release_pipe[0]);
    close(result_pipe[1]);
    for (int index = 0; index < workers; index++) {
        int result = 0;
        expect(read(result_pipe[0], &result, sizeof(result)) == sizeof(result) && result == 1,
               "concurrent registration repetitions pass");
    }
    join_path(data_path, runtime_dir, "parent-registry.data");
    TestRecord records[workers + 2];
    size_t count = read_records(data_path, records, workers + 2);
    expect(count == workers + 1, "stable lock retains every live concurrent registrant");
    struct stat final_lock_st;
    expect(stat(lock_path, &final_lock_st) == 0 &&
               final_lock_st.st_dev == initial_lock_st.st_dev &&
               final_lock_st.st_ino == initial_lock_st.st_ino,
           "lock inode remains stable across snapshot renames");
    for (int index = 0; index < workers; index++) {
        expect(write(release_pipe[1], "x", 1) == 1, "release registrant");
    }
    close(release_pipe[1]);
    close(result_pipe[0]);
    for (int index = 0; index < workers; index++) {
        int status;
        expect(waitpid(children[index], &status, 0) == children[index] &&
                   WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "concurrent registrant exits cleanly");
    }
    expect(peak_check_parent_process_for_test(&config) >= 0,
           "dead concurrent registrants pruned after crash-style exits");
    count = read_records(data_path, records, workers + 2);
    expect(count == 1, "only current live registrant remains");

    cleanup_runtime(runtime_dir);
    rmdir(xdg_root);
    rmdir(tmp_root);
}

static void test_lifecycle_repetitions_ignore_legacy_global(void)
{
    enum { repetitions = 220 };
    char xdg_root[PATH_MAX];
    char tmp_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char legacy_path[PATH_MAX];
    make_temp_dir(xdg_root, "/tmp/peak-xdg-XXXXXX", 0700);
    make_temp_dir(tmp_root, "/tmp/peak-tmp-XXXXXX", 0700);
    join_path(legacy_path, tmp_root, "lock_peak_ppid_list");
    const char* legacy = "1\n999999\n";
    write_text(legacy_path, legacy, 0644);
    PeakParentRegistryTestConfig config = make_config("/proc", xdg_root, tmp_root);
    expect(peak_check_parent_process_for_test(&config) >= 0, "register lifecycle parent");

    for (int iteration = 0; iteration < repetitions; iteration++) {
        pid_t child = fork();
        expect(child >= 0, "fork lifecycle child");
        if (child == 0) {
            _exit(peak_check_parent_process_for_test(&config) == 1 ? 0 : 1);
        }
        int status;
        expect(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "lifecycle child recognizes registered parent");
    }
    char* unchanged = read_text(legacy_path);
    expect(unchanged != NULL && strcmp(unchanged, legacy) == 0,
           "legacy global-style registry is ignored and unchanged");
    free(unchanged);
    expect(peak_check_parent_process_for_test(&config) >= 0,
           "final lifecycle cleanup succeeds with stale legacy file present");

    runtime_dir_path(runtime_dir, xdg_root);
    cleanup_runtime(runtime_dir);
    unlink(legacy_path);
    rmdir(xdg_root);
    rmdir(tmp_root);
}

int main(void)
{
    test_xdg_and_fallback_security();
    test_identity_pruning_and_live_parent();
    test_failure_atomic_faults();
    test_unsafe_files_rejected();
    test_interrupted_transaction_recovery();
    test_crash_and_concurrent_registration();
    test_lifecycle_repetitions_ignore_legacy_global();
    if (failures != 0) {
        return 1;
    }
    puts("parent_registry_ok");
    return 0;
}
