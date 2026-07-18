#include "parent_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef PEAK_UTILS_TESTING
#include "internal/utils/parent_registry_internal.h"
#else
enum {
    PEAK_PARENT_FAULT_NONE = 0,
    PEAK_PARENT_FAULT_PROC_READ,
    PEAK_PARENT_FAULT_SNAPSHOT_READ,
    PEAK_PARENT_FAULT_TEMP_WRITE,
    PEAK_PARENT_FAULT_TEMP_FSYNC,
    PEAK_PARENT_FAULT_RENAME,
    PEAK_PARENT_FAULT_DIR_FSYNC,
};
#endif

#define PEAK_PARENT_LOCK_NAME "parent-registry.lock"
#define PEAK_PARENT_DATA_NAME "parent-registry.data"
#define PEAK_PARENT_BACKUP_NAME ".parent-registry.backup"
#define PEAK_PARENT_MAX_SNAPSHOT (1024U * 1024U)

typedef struct {
    pid_t pid;
    unsigned long long starttime;
} PeakParentEntry;

typedef struct {
    const char* proc_root;
    const char* xdg_runtime_dir;
    const char* tmp_root;
#ifdef PEAK_UTILS_TESTING
    PeakParentRegistryFault fault;
    unsigned int fault_after;
    unsigned int fault_count;
#endif
} PeakParentContext;

static int parent_fault(PeakParentContext* context, int fault)
{
#ifdef PEAK_UTILS_TESTING
    if ((int)context->fault == fault && context->fault_count++ == context->fault_after) {
        errno = EIO;
        return -1;
    }
#else
    (void)context;
    (void)fault;
#endif
    return 0;
}

static int validate_directory_fd(int fd, uid_t uid, mode_t mode)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != uid || (st.st_mode & 0777) != mode) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int open_private_subdirectory(int parent_fd, const char* name, uid_t uid)
{
    bool created = false;
    if (mkdirat(parent_fd, name, 0700) == 0) {
        created = true;
    } else if (errno != EEXIST) {
        return -1;
    }

    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    if ((created && fchmod(fd, 0700) != 0) || validate_directory_fd(fd, uid, 0700) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int open_parent_runtime_directory(PeakParentContext* context)
{
    uid_t uid = geteuid();
    const char* xdg_runtime_dir = context->xdg_runtime_dir;
    if (xdg_runtime_dir == NULL) {
        xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    }
    if (xdg_runtime_dir != NULL && xdg_runtime_dir[0] == '/') {
        int xdg_fd = open(xdg_runtime_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (xdg_fd >= 0) {
            if (validate_directory_fd(xdg_fd, uid, 0700) == 0) {
                int runtime_fd = open_private_subdirectory(xdg_fd, "peak", uid);
                close(xdg_fd);
                if (runtime_fd >= 0) {
                    return runtime_fd;
                }
            } else {
                close(xdg_fd);
            }
        }
    }

    const char* tmp_root = context->tmp_root == NULL ? "/tmp" : context->tmp_root;
    int tmp_fd = open(tmp_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (tmp_fd < 0) {
        return -1;
    }
    char name[64];
    int length = snprintf(name, sizeof(name), "peak-%lu", (unsigned long)uid);
    if (length < 0 || (size_t)length >= sizeof(name)) {
        close(tmp_fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    int runtime_fd = open_private_subdirectory(tmp_fd, name, uid);
    int saved_errno = errno;
    close(tmp_fd);
    errno = saved_errno;
    return runtime_fd;
}

static int secure_open_regular_at(int directory_fd, const char* name, bool create,
                                  bool* created)
{
    int fd = -1;
    *created = false;
    if (create) {
        fd = openat(directory_fd, name,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            *created = true;
            if (fchmod(fd, 0600) != 0) {
                int saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
            }
        } else if (errno != EEXIST) {
            return -1;
        }
    }
    if (fd < 0) {
        fd = openat(directory_fd, name,
                    (create ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            return -1;
        }
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

static int read_process_starttime(PeakParentContext* context, pid_t pid,
                                  unsigned long long* starttime)
{
#ifdef __linux__
    if (parent_fault(context, PEAK_PARENT_FAULT_PROC_READ) != 0) {
        return -1;
    }
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%ld/stat", context->proc_root, (long)pid);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    char stat_line[4096];
    ssize_t count = read(fd, stat_line, sizeof(stat_line) - 1);
    int saved_errno = errno;
    if (close(fd) != 0 && count >= 0) {
        return -1;
    }
    errno = saved_errno;
    if (count <= 0 || (size_t)count == sizeof(stat_line) - 1) {
        if (count >= 0) {
            errno = EINVAL;
        }
        return -1;
    }
    stat_line[count] = '\0';

    char* fields = strrchr(stat_line, ')');
    if (fields == NULL) {
        errno = EINVAL;
        return -1;
    }
    fields++;
    unsigned int field_number = 2;
    char* save = NULL;
    for (char* token = strtok_r(fields, " ", &save); token != NULL;
         token = strtok_r(NULL, " ", &save)) {
        field_number++;
        if (field_number == 22) {
            char* end = NULL;
            errno = 0;
            unsigned long long value = strtoull(token, &end, 10);
            if (errno != 0 || end == token || (*end != '\0' && *end != '\n')) {
                errno = EINVAL;
                return -1;
            }
            *starttime = value;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
#else
    (void)context;
    (void)pid;
    (void)starttime;
    errno = ENOSYS;
    return -1;
#endif
}

static int parse_registry_entry(const char* line, PeakParentEntry* entry)
{
    long pid;
    char extra;
    if (sscanf(line, "%ld %llu %c", &pid, &entry->starttime, &extra) != 2 ||
        pid <= 0 || pid > INT_MAX) {
        return -1;
    }
    entry->pid = (pid_t)pid;
    return 0;
}

static int read_snapshot(PeakParentContext* context, int directory_fd, char** contents,
                         size_t* length, bool* existed)
{
    *contents = NULL;
    *length = 0;
    *existed = false;
    bool ignored_created;
    int fd = secure_open_regular_at(directory_fd, PEAK_PARENT_DATA_NAME, false,
                                    &ignored_created);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    *existed = true;
    char* data = malloc(PEAK_PARENT_MAX_SNAPSHOT + 1);
    if (data == NULL) {
        close(fd);
        return -1;
    }
    size_t used = 0;
    while (used < PEAK_PARENT_MAX_SNAPSHOT) {
        if (parent_fault(context, PEAK_PARENT_FAULT_SNAPSHOT_READ) != 0) {
            free(data);
            close(fd);
            return -1;
        }
        ssize_t count = read(fd, data + used, PEAK_PARENT_MAX_SNAPSHOT - used);
        if (count < 0) {
            int saved_errno = errno;
            free(data);
            close(fd);
            errno = saved_errno;
            return -1;
        }
        if (count == 0) {
            break;
        }
        used += (size_t)count;
    }
    char extra;
    ssize_t extra_count = read(fd, &extra, 1);
    int saved_errno = errno;
    if (close(fd) != 0 && extra_count >= 0) {
        extra_count = -1;
        saved_errno = errno;
    }
    if (extra_count != 0) {
        free(data);
        errno = extra_count > 0 ? EFBIG : saved_errno;
        return -1;
    }
    data[used] = '\0';
    *contents = data;
    *length = used;
    return 0;
}

static int remove_unexpected_backup(int directory_fd, const struct stat* backup_st)
{
    int flags = S_ISDIR(backup_st->st_mode) ? AT_REMOVEDIR : 0;
    if (unlinkat(directory_fd, PEAK_PARENT_BACKUP_NAME, flags) != 0) {
        return -1;
    }
    return fsync(directory_fd);
}

static int secure_snapshot_stat(const struct stat* st)
{
    return S_ISREG(st->st_mode) && st->st_uid == geteuid() &&
           (st->st_mode & 0777) == 0600;
}

static int recover_snapshot_backup(int directory_fd)
{
    struct stat backup_st;
    if (fstatat(directory_fd, PEAK_PARENT_BACKUP_NAME, &backup_st,
                AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    struct stat data_st;
    int data_result = fstatat(directory_fd, PEAK_PARENT_DATA_NAME, &data_st,
                             AT_SYMLINK_NOFOLLOW);
    if (data_result != 0 && errno != ENOENT) {
        return -1;
    }

    bool legitimate_pre_rename =
        data_result == 0 && secure_snapshot_stat(&backup_st) &&
        secure_snapshot_stat(&data_st) && backup_st.st_dev == data_st.st_dev &&
        backup_st.st_ino == data_st.st_ino && backup_st.st_nlink == 2 &&
        data_st.st_nlink == 2;
    bool legitimate_post_rename =
        data_result == 0 && secure_snapshot_stat(&backup_st) &&
        secure_snapshot_stat(&data_st) &&
        (backup_st.st_dev != data_st.st_dev || backup_st.st_ino != data_st.st_ino) &&
        backup_st.st_nlink == 1 && data_st.st_nlink == 1;

    if (legitimate_pre_rename || legitimate_post_rename) {
        if (unlinkat(directory_fd, PEAK_PARENT_BACKUP_NAME, 0) != 0) {
            return -1;
        }
        return fsync(directory_fd);
    }
    return remove_unexpected_backup(directory_fd, &backup_st);
}

static int append_entry(PeakParentEntry** entries, size_t* count, size_t* capacity,
                        PeakParentEntry entry)
{
    for (size_t index = 0; index < *count; index++) {
        if ((*entries)[index].pid == entry.pid &&
            (*entries)[index].starttime == entry.starttime) {
            return 0;
        }
    }
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(**entries)) {
            errno = EOVERFLOW;
            return -1;
        }
        PeakParentEntry* resized = realloc(*entries, new_capacity * sizeof(**entries));
        if (resized == NULL) {
            return -1;
        }
        *entries = resized;
        *capacity = new_capacity;
    }
    (*entries)[(*count)++] = entry;
    return 0;
}

static int collect_live_entries(PeakParentContext* context, char* snapshot,
                                pid_t my_pid, unsigned long long my_starttime,
                                pid_t parent_pid, unsigned long long parent_starttime,
                                PeakParentEntry** entries, size_t* entry_count,
                                int* found_parent)
{
    size_t capacity = 0;
    *entry_count = 0;
    *entries = NULL;
    *found_parent = 0;
    char* save = NULL;
    for (char* line = strtok_r(snapshot, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        PeakParentEntry entry;
        if (parse_registry_entry(line, &entry) != 0) {
            continue;
        }
        unsigned long long current_starttime;
        if (read_process_starttime(context, entry.pid, &current_starttime) != 0) {
            if (errno == ENOENT || errno == ESRCH) {
                continue;
            }
            free(*entries);
            *entries = NULL;
            return -1;
        }
        if (current_starttime != entry.starttime) {
            continue;
        }
        if (entry.pid == parent_pid && entry.starttime == parent_starttime) {
            *found_parent = 1;
        }
        if (entry.pid == my_pid && entry.starttime == my_starttime) {
            continue;
        }
        if (append_entry(entries, entry_count, &capacity, entry) != 0) {
            free(*entries);
            *entries = NULL;
            return -1;
        }
    }
    PeakParentEntry self = {my_pid, my_starttime};
    if (append_entry(entries, entry_count, &capacity, self) != 0) {
        free(*entries);
        *entries = NULL;
        return -1;
    }
    return 0;
}

static int write_all(PeakParentContext* context, int fd, const char* data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        if (parent_fault(context, PEAK_PARENT_FAULT_TEMP_WRITE) != 0) {
            return -1;
        }
        ssize_t count = write(fd, data + written, length - written);
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            return -1;
        }
        written += (size_t)count;
    }
    return 0;
}

static int rollback_snapshot(int directory_fd, bool had_snapshot)
{
    if (had_snapshot) {
        if (renameat(directory_fd, PEAK_PARENT_BACKUP_NAME,
                     directory_fd, PEAK_PARENT_DATA_NAME) != 0) {
            return -1;
        }
    } else if (unlinkat(directory_fd, PEAK_PARENT_DATA_NAME, 0) != 0 && errno != ENOENT) {
        return -1;
    }
    return fsync(directory_fd);
}

static long parent_registry_thread_id(void)
{
#ifdef __linux__
    return (long)syscall(SYS_gettid);
#else
    return (long)getpid();
#endif
}

static int install_snapshot(PeakParentContext* context, int directory_fd,
                            const PeakParentEntry* entries, size_t entry_count,
                            bool had_snapshot)
{
    char temp_name[128];
    int temp_fd = -1;
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        int length = snprintf(temp_name, sizeof(temp_name), ".parent-registry.tmp.%ld.%ld.%u",
                              (long)getpid(), parent_registry_thread_id(), attempt);
        if (length < 0 || (size_t)length >= sizeof(temp_name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        temp_fd = openat(directory_fd, temp_name,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (temp_fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (temp_fd < 0) {
        return -1;
    }
    if (fchmod(temp_fd, 0600) != 0) {
        goto fail_temp;
    }
    for (size_t index = 0; index < entry_count; index++) {
        char record[96];
        int length = snprintf(record, sizeof(record), "%ld %llu\n",
                              (long)entries[index].pid, entries[index].starttime);
        if (length < 0 || (size_t)length >= sizeof(record) ||
            write_all(context, temp_fd, record, (size_t)length) != 0) {
            goto fail_temp;
        }
    }
    if (parent_fault(context, PEAK_PARENT_FAULT_TEMP_FSYNC) != 0 || fsync(temp_fd) != 0) {
        goto fail_temp;
    }
    if (close(temp_fd) != 0) {
        temp_fd = -1;
        goto fail_temp;
    }
    temp_fd = -1;

    unlinkat(directory_fd, PEAK_PARENT_BACKUP_NAME, 0);
    if (had_snapshot && linkat(directory_fd, PEAK_PARENT_DATA_NAME,
                               directory_fd, PEAK_PARENT_BACKUP_NAME, 0) != 0) {
        goto fail_temp;
    }
    if (parent_fault(context, PEAK_PARENT_FAULT_RENAME) != 0 ||
        renameat(directory_fd, temp_name, directory_fd, PEAK_PARENT_DATA_NAME) != 0) {
        if (had_snapshot) {
            unlinkat(directory_fd, PEAK_PARENT_BACKUP_NAME, 0);
        }
        goto fail_temp;
    }
    temp_name[0] = '\0';
    if (parent_fault(context, PEAK_PARENT_FAULT_DIR_FSYNC) != 0 || fsync(directory_fd) != 0) {
        int saved_errno = errno;
        if (rollback_snapshot(directory_fd, had_snapshot) != 0) {
            return -1;
        }
        errno = saved_errno;
        return -1;
    }
    if (had_snapshot) {
        unlinkat(directory_fd, PEAK_PARENT_BACKUP_NAME, 0);
    }
    return 0;

fail_temp:
    {
        int saved_errno = errno;
        if (temp_fd >= 0) {
            close(temp_fd);
        }
        if (temp_name[0] != '\0') {
            unlinkat(directory_fd, temp_name, 0);
        }
        errno = saved_errno;
    }
    return -1;
}

static int check_parent_process_with_context(PeakParentContext* context)
{
    int directory_fd = open_parent_runtime_directory(context);
    if (directory_fd < 0) {
        return -1;
    }
    bool lock_created;
    int lock_fd = secure_open_regular_at(directory_fd, PEAK_PARENT_LOCK_NAME, true,
                                         &lock_created);
    (void)lock_created;
    if (lock_fd < 0) {
        close(directory_fd);
        return -1;
    }
    if (flock(lock_fd, LOCK_EX) != 0) {
        int saved_errno = errno;
        close(lock_fd);
        close(directory_fd);
        errno = saved_errno;
        return -1;
    }

    int result = -1;
    char* snapshot = NULL;
    size_t snapshot_length;
    bool had_snapshot;
    if (recover_snapshot_backup(directory_fd) != 0) {
        goto out;
    }
    if (read_snapshot(context, directory_fd, &snapshot, &snapshot_length, &had_snapshot) != 0) {
        goto out;
    }
    (void)snapshot_length;
    if (snapshot == NULL) {
        snapshot = strdup("");
        if (snapshot == NULL) {
            goto out;
        }
    }

    pid_t my_pid = getpid();
    pid_t parent_pid = getppid();
    unsigned long long my_starttime;
    unsigned long long parent_starttime;
    if (read_process_starttime(context, my_pid, &my_starttime) != 0 ||
        read_process_starttime(context, parent_pid, &parent_starttime) != 0) {
        goto out;
    }
    PeakParentEntry* entries;
    size_t entry_count;
    int found_parent;
    if (collect_live_entries(context, snapshot, my_pid, my_starttime,
                             parent_pid, parent_starttime, &entries, &entry_count,
                             &found_parent) != 0) {
        goto out;
    }
    if (install_snapshot(context, directory_fd, entries, entry_count, had_snapshot) == 0) {
        result = found_parent;
    }
    free(entries);

out:
    {
        int saved_errno = errno;
        free(snapshot);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        close(directory_fd);
        errno = saved_errno;
    }
    return result;
}

int check_parent_process(void)
{
    PeakParentContext context = {
        .proc_root = "/proc",
        .xdg_runtime_dir = NULL,
        .tmp_root = "/tmp",
    };
    return check_parent_process_with_context(&context);
}

#ifdef PEAK_UTILS_TESTING
int peak_check_parent_process_for_test(const PeakParentRegistryTestConfig* config)
{
    PeakParentContext context = {
        .proc_root = config->proc_root == NULL ? "/proc" : config->proc_root,
        .xdg_runtime_dir = config->xdg_runtime_dir,
        .tmp_root = config->tmp_root == NULL ? "/tmp" : config->tmp_root,
        .fault = config->fault,
        .fault_after = config->fault_after,
        .fault_count = 0,
    };
    return check_parent_process_with_context(&context);
}
#endif
