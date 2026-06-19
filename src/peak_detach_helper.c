#define _GNU_SOURCE

#include "peak_detach_helper_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PEAK_DETACH_HELPER_STOP_TIMEOUT_MS 5000L

typedef struct {
    pid_t tid;
    int attached;
    int detach_signal;
} PeakHeldThread;

static PeakHeldThread held_threads[PEAK_DETACH_HELPER_MAX_THREADS];
static size_t held_thread_count = 0;

static int
read_exact(int fd, void* buffer, size_t size)
{
    char* cursor = (char*)buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t n = read(fd, cursor + done, size - done);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }

    return 1;
}

static int
write_exact(int fd, const void* buffer, size_t size)
{
    const char* cursor = (const char*)buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t n = send(fd, cursor + done, size - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }

    return 0;
}

static long
monotonic_milliseconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static PeakDetachHelperStatus
detach_held_threads(int* errno_out)
{
    PeakDetachHelperStatus status = PEAK_DETACH_HELPER_STATUS_OK;
    size_t retained_count = 0;

    if (errno_out != NULL) {
        *errno_out = 0;
    }

    for (size_t i = 0; i < held_thread_count; i++) {
        if (held_threads[i].attached) {
            if (ptrace(PTRACE_DETACH,
                       held_threads[i].tid,
                       NULL,
                       (void*)(intptr_t)held_threads[i].detach_signal) != 0) {
                if (errno != ESRCH) {
                    if (errno_out != NULL && *errno_out == 0) {
                        *errno_out = errno;
                    }
                    status = PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
                    held_threads[retained_count++] = held_threads[i];
                    continue;
                }
            }
        }
    }

    held_thread_count = retained_count;
    return status;
}

static PeakDetachHelperStatus
cleanup_held_threads_or_release_failed(PeakDetachHelperStatus intended_status,
                                       int* errno_out)
{
    int cleanup_errno = 0;
    PeakDetachHelperStatus cleanup_status;

    if (held_thread_count == 0) {
        return intended_status;
    }

    cleanup_status = detach_held_threads(&cleanup_errno);
    if (cleanup_status != PEAK_DETACH_HELPER_STATUS_OK) {
        if (errno_out != NULL && *errno_out == 0) {
            *errno_out = cleanup_errno;
        }
        return PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED;
    }

    return intended_status;
}

static int
parse_tid_name(const char* name, pid_t* tid_out)
{
    char* end = NULL;
    long value;

    errno = 0;
    value = strtol(name, &end, 10);
    if (errno != 0 || end == name || *end != '\0' || value <= 0) {
        return -1;
    }

    *tid_out = (pid_t)value;
    return 0;
}

static int
wait_for_ptrace_stop(pid_t tid, int* detach_signal_out)
{
    long deadline = monotonic_milliseconds() + PEAK_DETACH_HELPER_STOP_TIMEOUT_MS;

    if (detach_signal_out != NULL) {
        *detach_signal_out = 0;
    }

    for (;;) {
        int status = 0;
        pid_t waited = waitpid(tid, &status, __WALL | WNOHANG);

        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (waited == 0) {
            if (monotonic_milliseconds() >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            usleep(1000);
            continue;
        }

        if (WIFSTOPPED(status)) {
            int detach_signal = 0;

#ifdef PTRACE_EVENT_STOP
            if ((status >> 16) != PTRACE_EVENT_STOP) {
                detach_signal = WSTOPSIG(status);
            }
#else
            detach_signal = WSTOPSIG(status) == SIGTRAP ? 0 : WSTOPSIG(status);
#endif
            if (detach_signal_out != NULL) {
                *detach_signal_out = detach_signal;
            }
            return 0;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            errno = ESRCH;
            return -1;
        }
    }
}

static uint64_t
peak_regs_pc(const struct user_regs_struct* regs)
{
#if defined(__x86_64__)
    return (uint64_t)regs->rip;
#else
    (void)regs;
    return 0;
#endif
}

static int
peak_regs_set_pc(struct user_regs_struct* regs, uint64_t pc)
{
#if defined(__x86_64__)
    regs->rip = pc;
    return 0;
#else
    (void)regs;
    (void)pc;
    errno = ENOTSUP;
    return -1;
#endif
}

static PeakHeldThread*
find_held_thread(pid_t tid)
{
    for (size_t i = 0; i < held_thread_count; i++) {
        if (held_threads[i].tid == tid) {
            return &held_threads[i];
        }
    }
    return NULL;
}

static int
tid_is_held(pid_t tid)
{
    return find_held_thread(tid) != NULL;
}

static PeakDetachHelperStatus
verify_no_unstopped_threads(pid_t pid, pid_t controller_tid, int* errno_out)
{
    char task_path[64];
    DIR* task_dir;

    snprintf(task_path, sizeof(task_path), "/proc/%ld/task", (long)pid);
    task_dir = opendir(task_path);
    if (task_dir == NULL) {
        *errno_out = errno;
        return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
    }

    for (;;) {
        struct dirent* entry;
        pid_t tid;

        errno = 0;
        entry = readdir(task_dir);
        if (entry == NULL) {
            break;
        }

        if (parse_tid_name(entry->d_name, &tid) != 0 ||
            tid == controller_tid) {
            continue;
        }

        if (!tid_is_held(tid)) {
            closedir(task_dir);
            *errno_out = EAGAIN;
            return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
        }
    }

    if (errno != 0) {
        *errno_out = errno;
        closedir(task_dir);
        return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
    }

    closedir(task_dir);
    return PEAK_DETACH_HELPER_STATUS_OK;
}

static int
send_response(int fd,
              PeakDetachHelperStatus status,
              int errno_value,
              uint32_t thread_count)
{
    PeakDetachHelperResponse response = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .status = (uint32_t)status,
        .thread_count = thread_count,
        .errno_value = errno_value
    };

    return write_exact(fd, &response, sizeof(response));
}

static PeakDetachHelperStatus
stop_target_threads(int fd, pid_t pid, pid_t controller_tid, int* errno_out)
{
    char task_path[64];
    DIR* task_dir;
    PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];
    uint32_t snapshot_count = 0;

    *errno_out = 0;
    if (cleanup_held_threads_or_release_failed(
            PEAK_DETACH_HELPER_STATUS_OK,
            errno_out) != PEAK_DETACH_HELPER_STATUS_OK) {
        return PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED;
    }

    snprintf(task_path, sizeof(task_path), "/proc/%ld/task", (long)pid);
    task_dir = opendir(task_path);
    if (task_dir == NULL) {
        *errno_out = errno;
        return errno == EACCES || errno == EPERM ?
            PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED :
            PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
    }

    for (;;) {
        struct dirent* entry;
        pid_t tid;
        PeakDetachHelperThreadSnapshot* snapshot;

        errno = 0;
        entry = readdir(task_dir);
        if (entry == NULL) {
            break;
        }

        if (parse_tid_name(entry->d_name, &tid) != 0 ||
            tid == controller_tid) {
            continue;
        }

        if (snapshot_count >= PEAK_DETACH_HELPER_MAX_THREADS) {
            closedir(task_dir);
            *errno_out = E2BIG;
            return cleanup_held_threads_or_release_failed(
                PEAK_DETACH_HELPER_STATUS_THREAD_LIMIT,
                errno_out);
        }

        snapshot = &snapshots[snapshot_count];
        snapshot->tid = tid;
        snapshot->status = PEAK_DETACH_HELPER_THREAD_OK;
        snapshot->pc = 0;

#if defined(PTRACE_SEIZE) && defined(PTRACE_INTERRUPT)
        if (ptrace(PTRACE_SEIZE, tid, NULL, (void*)0) != 0) {
            PeakDetachHelperStatus intended_status;
            *errno_out = errno;
            closedir(task_dir);
            intended_status = *errno_out == EACCES || *errno_out == EPERM ?
                PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED :
                PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
            return cleanup_held_threads_or_release_failed(intended_status,
                                                          errno_out);
        }

        PeakHeldThread* held_thread = &held_threads[held_thread_count];
        held_thread->tid = tid;
        held_thread->attached = 1;
        held_thread->detach_signal = 0;
        held_thread_count++;

        if (ptrace(PTRACE_INTERRUPT, tid, NULL, NULL) != 0) {
            PeakDetachHelperStatus intended_status;
            *errno_out = errno;
            closedir(task_dir);
            intended_status = *errno_out == EACCES || *errno_out == EPERM ?
                PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED :
                PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
            return cleanup_held_threads_or_release_failed(intended_status,
                                                          errno_out);
        }

        if (wait_for_ptrace_stop(tid, &held_thread->detach_signal) != 0) {
            PeakDetachHelperStatus intended_status;
            *errno_out = errno;
            closedir(task_dir);
            intended_status = *errno_out == ETIMEDOUT ?
                PEAK_DETACH_HELPER_STATUS_TIMEOUT :
                PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
            return cleanup_held_threads_or_release_failed(intended_status,
                                                          errno_out);
        }
#else
        *errno_out = ENOTSUP;
        closedir(task_dir);
        return cleanup_held_threads_or_release_failed(
            PEAK_DETACH_HELPER_STATUS_UNSUPPORTED,
            errno_out);
#endif

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0) {
            *errno_out = errno;
            closedir(task_dir);
            return cleanup_held_threads_or_release_failed(
                PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR,
                errno_out);
        }

        snapshot->pc = peak_regs_pc(&regs);
        snapshot_count++;
    }

    if (errno != 0) {
        *errno_out = errno;
        closedir(task_dir);
        return cleanup_held_threads_or_release_failed(
            PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR,
            errno_out);
    }

    closedir(task_dir);

    PeakDetachHelperStatus verify_status =
        verify_no_unstopped_threads(pid, controller_tid, errno_out);
    if (verify_status != PEAK_DETACH_HELPER_STATUS_OK) {
        return cleanup_held_threads_or_release_failed(verify_status,
                                                      errno_out);
    }

    if (send_response(fd,
                      PEAK_DETACH_HELPER_STATUS_OK,
                      *errno_out,
                      snapshot_count) != 0 ||
        write_exact(fd, snapshots, sizeof(snapshots[0]) * snapshot_count) != 0) {
        *errno_out = errno;
        return cleanup_held_threads_or_release_failed(
            PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
            errno_out);
    }

    return PEAK_DETACH_HELPER_STATUS_OK;
}

static int
write_memory_via_proc_mem(pid_t pid,
                          uint64_t address,
                          const uint8_t* bytes,
                          size_t size)
{
    char mem_path[64];
    int fd;
    ssize_t written;

    snprintf(mem_path, sizeof(mem_path), "/proc/%ld/mem", (long)pid);
    fd = open(mem_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    written = pwrite(fd, bytes, size, (off_t)address);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;

    if (written < 0 || (size_t)written != size) {
        if (written >= 0) {
            errno = EIO;
        }
        return -1;
    }

    return 0;
}

static int
write_memory_via_ptrace(uint64_t address,
                        const uint8_t* bytes,
                        size_t size)
{
    if (held_thread_count == 0) {
        errno = ESRCH;
        return -1;
    }

    pid_t tid = held_threads[0].tid;
    size_t word_size = sizeof(long);
    uint64_t start = address;
    uint64_t end = address + size;

    for (uint64_t cursor = start; cursor < end;) {
        uint64_t aligned = cursor & ~(uint64_t)(word_size - 1);
        size_t offset = (size_t)(cursor - aligned);
        size_t chunk = word_size - offset;
        if (chunk > end - cursor) {
            chunk = (size_t)(end - cursor);
        }

        errno = 0;
        long word = ptrace(PTRACE_PEEKTEXT, tid, (void*)(uintptr_t)aligned, NULL);
        if (word == -1 && errno != 0) {
            return -1;
        }

        uint8_t word_bytes[sizeof(long)];
        memcpy(word_bytes, &word, sizeof(word_bytes));
        memcpy(word_bytes + offset, bytes + (cursor - start), chunk);
        memcpy(&word, word_bytes, sizeof(word));

        if (ptrace(PTRACE_POKETEXT, tid, (void*)(uintptr_t)aligned,
                   (void*)word) != 0) {
            return -1;
        }

        cursor += chunk;
    }

    return 0;
}

static int
write_tracee_memory(pid_t pid,
                    uint64_t address,
                    const uint8_t* bytes,
                    size_t size)
{
    if (size == 0 || size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        errno = EINVAL;
        return -1;
    }

    if (write_memory_via_proc_mem(pid, address, bytes, size) == 0) {
        return 0;
    }

    return write_memory_via_ptrace(address, bytes, size);
}

static int
read_memory_via_proc_mem(pid_t pid,
                         uint64_t address,
                         uint8_t* bytes,
                         size_t size)
{
    char mem_path[64];
    int fd;
    ssize_t nread;

    snprintf(mem_path, sizeof(mem_path), "/proc/%ld/mem", (long)pid);
    fd = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    nread = pread(fd, bytes, size, (off_t)address);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;

    if (nread < 0 || (size_t)nread != size) {
        if (nread >= 0) {
            errno = EIO;
        }
        return -1;
    }

    return 0;
}

static int
read_memory_via_ptrace(uint64_t address, uint8_t* bytes, size_t size)
{
    if (held_thread_count == 0) {
        errno = ESRCH;
        return -1;
    }

    pid_t tid = held_threads[0].tid;
    size_t word_size = sizeof(long);
    uint64_t start = address;
    uint64_t end = address + size;

    for (uint64_t cursor = start; cursor < end;) {
        uint64_t aligned = cursor & ~(uint64_t)(word_size - 1);
        size_t offset = (size_t)(cursor - aligned);
        size_t chunk = word_size - offset;
        if (chunk > end - cursor) {
            chunk = (size_t)(end - cursor);
        }

        errno = 0;
        long word = ptrace(PTRACE_PEEKTEXT, tid, (void*)(uintptr_t)aligned, NULL);
        if (word == -1 && errno != 0) {
            return -1;
        }

        uint8_t word_bytes[sizeof(long)];
        memcpy(word_bytes, &word, sizeof(word_bytes));
        memcpy(bytes + (cursor - start), word_bytes + offset, chunk);

        cursor += chunk;
    }

    return 0;
}

static int
read_tracee_memory(pid_t pid, uint64_t address, uint8_t* bytes, size_t size)
{
    if (size == 0 || size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        errno = EINVAL;
        return -1;
    }

    if (read_memory_via_proc_mem(pid, address, bytes, size) == 0) {
        return 0;
    }

    return read_memory_via_ptrace(address, bytes, size);
}

static int
verify_tracee_memory(pid_t pid,
                     uint64_t address,
                     const uint8_t* expected,
                     size_t size)
{
    uint8_t actual[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];

    if (read_tracee_memory(pid, address, actual, size) != 0) {
        return -1;
    }

    if (memcmp(actual, expected, size) != 0) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static PeakDetachHelperStatus
validate_instruction(const PeakDetachHelperInstruction* instruction,
                     int* errno_out)
{
    if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC) {
        PeakHeldThread* held_thread = find_held_thread((pid_t)instruction->tid);
        struct user_regs_struct regs;

        if (held_thread == NULL || !held_thread->attached) {
            *errno_out = ESRCH;
            return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
        }
        if (ptrace(PTRACE_GETREGS, held_thread->tid, NULL, &regs) != 0) {
            *errno_out = errno;
            return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
        }
        if (peak_regs_set_pc(&regs, instruction->pc) != 0) {
            *errno_out = errno;
            return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
        }
        return PEAK_DETACH_HELPER_STATUS_OK;
    }

    if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY) {
        if (instruction->address == 0 ||
            instruction->size == 0 ||
            instruction->size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
            *errno_out = EINVAL;
            return PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR;
        }
        return PEAK_DETACH_HELPER_STATUS_OK;
    }

    *errno_out = EINVAL;
    return PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR;
}

static PeakDetachHelperStatus
apply_write_instruction(pid_t pid,
                        const PeakDetachHelperInstruction* instruction,
                        int* errno_out)
{
    if (write_tracee_memory(pid,
                            instruction->address,
                            instruction->bytes,
                            instruction->size) != 0 ||
        verify_tracee_memory(pid,
                             instruction->address,
                             instruction->bytes,
                             instruction->size) != 0) {
        *errno_out = errno;
        return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
    }

    return PEAK_DETACH_HELPER_STATUS_OK;
}

static PeakDetachHelperStatus
apply_set_pc_instruction(const PeakDetachHelperInstruction* instruction,
                         int* errno_out)
{
    PeakHeldThread* held_thread = find_held_thread((pid_t)instruction->tid);
    struct user_regs_struct regs;

    if (held_thread == NULL || !held_thread->attached) {
        *errno_out = ESRCH;
        return PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR;
    }

    if (ptrace(PTRACE_GETREGS, held_thread->tid, NULL, &regs) != 0) {
        *errno_out = errno;
        return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
    }
    if (peak_regs_set_pc(&regs, instruction->pc) != 0 ||
        ptrace(PTRACE_SETREGS, held_thread->tid, NULL, &regs) != 0) {
        *errno_out = errno;
        return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
    }
    if (ptrace(PTRACE_GETREGS, held_thread->tid, NULL, &regs) != 0) {
        *errno_out = errno;
        return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
    }
    if (peak_regs_pc(&regs) != instruction->pc) {
        *errno_out = EIO;
        return PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR;
    }

    return PEAK_DETACH_HELPER_STATUS_OK;
}

static PeakDetachHelperStatus
apply_instructions(pid_t pid,
                   pid_t controller_tid,
                   const PeakDetachHelperInstruction* instructions,
                   uint32_t instruction_count,
                   int* errno_out,
                   int* mutation_started_out)
{
    *errno_out = 0;
    if (mutation_started_out != NULL) {
        *mutation_started_out = 0;
    }

    for (uint32_t i = 0; i < instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction = &instructions[i];
        PeakDetachHelperStatus status =
            validate_instruction(instruction, errno_out);
        if (status != PEAK_DETACH_HELPER_STATUS_OK) {
            return status;
        }
    }

    PeakDetachHelperStatus verify_status =
        verify_no_unstopped_threads(pid, controller_tid, errno_out);
    if (verify_status != PEAK_DETACH_HELPER_STATUS_OK) {
        return verify_status;
    }

    for (uint32_t i = 0; i < instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY) {
            PeakDetachHelperStatus status =
                apply_write_instruction(pid, instruction, errno_out);
            if (mutation_started_out != NULL) {
                *mutation_started_out = 1;
            }
            if (status != PEAK_DETACH_HELPER_STATUS_OK) {
                return status;
            }
        }
    }

    for (uint32_t i = 0; i < instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC) {
            PeakDetachHelperStatus status =
                apply_set_pc_instruction(instruction, errno_out);
            if (mutation_started_out != NULL) {
                *mutation_started_out = 1;
            }
            if (status != PEAK_DETACH_HELPER_STATUS_OK) {
                return status;
            }
        }
    }

    return PEAK_DETACH_HELPER_STATUS_OK;
}

static int
serve_protocol(int fd)
{
    pid_t bound_pid = 0;

    for (;;) {
        PeakDetachHelperRequest request;
        int read_status = read_exact(fd, &request, sizeof(request));
        int errno_value = 0;

        if (read_status == 0) {
            (void)detach_held_threads(NULL);
            return 0;
        }
        if (read_status < 0) {
            (void)detach_held_threads(NULL);
            return 1;
        }

        if (request.magic != PEAK_DETACH_HELPER_MAGIC ||
            request.version != PEAK_DETACH_HELPER_VERSION) {
            (void)send_response(fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EPROTO,
                                0);
            (void)detach_held_threads(NULL);
            return 1;
        }

        if (bound_pid == 0) {
            bound_pid = (pid_t)request.pid;
        } else if ((pid_t)request.pid != bound_pid) {
            (void)send_response(fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EPERM,
                                0);
            (void)detach_held_threads(NULL);
            return 1;
        }

        if (request.command == PEAK_DETACH_HELPER_CMD_STOP) {
            if (request.instruction_count != 0) {
                (void)send_response(fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                (void)detach_held_threads(NULL);
                return 1;
            }
            PeakDetachHelperStatus status =
                stop_target_threads(fd,
                                    (pid_t)request.pid,
                                    (pid_t)request.controller_tid,
                                    &errno_value);
            if (status != PEAK_DETACH_HELPER_STATUS_OK) {
                (void)send_response(fd, status, errno_value, 0);
            }
        } else if (request.command == PEAK_DETACH_HELPER_CMD_EVACUATE) {
            PeakDetachHelperInstruction instructions[PEAK_DETACH_HELPER_MAX_INSTRUCTIONS];
            PeakDetachHelperStatus status;
            int mutation_started = 0;

            if (request.instruction_count > PEAK_DETACH_HELPER_MAX_INSTRUCTIONS ||
                read_exact(fd,
                           instructions,
                           sizeof(instructions[0]) * request.instruction_count) != 1) {
                (void)detach_held_threads(NULL);
                (void)send_response(fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                return 1;
            }

            status = apply_instructions((pid_t)request.pid,
                                        (pid_t)request.controller_tid,
                                        instructions,
                                        request.instruction_count,
                                        &errno_value,
                                        &mutation_started);
            if (status != PEAK_DETACH_HELPER_STATUS_OK) {
                if (mutation_started) {
                    status = PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED;
                } else {
                    status = cleanup_held_threads_or_release_failed(status,
                                                                    &errno_value);
                }
            }
            (void)send_response(fd, status, errno_value, 0);
            if (status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED) {
                return 1;
            }
        } else if (request.command == PEAK_DETACH_HELPER_CMD_RESUME) {
            if (request.instruction_count != 0) {
                (void)detach_held_threads(NULL);
                (void)send_response(fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                return 1;
            }
            PeakDetachHelperStatus detach_status =
                detach_held_threads(&errno_value);
            (void)send_response(fd, detach_status, errno_value, 0);
        } else if (request.command == PEAK_DETACH_HELPER_CMD_SHUTDOWN) {
            if (request.instruction_count != 0) {
                (void)send_response(fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                (void)detach_held_threads(NULL);
                return 1;
            }
            PeakDetachHelperStatus detach_status =
                detach_held_threads(&errno_value);
            (void)send_response(fd, detach_status, errno_value, 0);
            return detach_status == PEAK_DETACH_HELPER_STATUS_OK ? 0 : 1;
        } else {
            (void)detach_held_threads(NULL);
            (void)send_response(fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EINVAL,
                                0);
            return 1;
        }
    }
}

int
main(int argc, char** argv)
{
    sigset_t empty_mask;

    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&empty_mask);
    sigprocmask(SIG_SETMASK, &empty_mask, NULL);

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        puts("peak_detach_helper_self_test_ok");
        return 0;
    }

#if !defined(__linux__) || !defined(__x86_64__)
    (void)argc;
    (void)argv;
    fprintf(stderr, "peak_detach_helper: unsupported platform\n");
    return 2;
#else
    if (argc != 2) {
        fprintf(stderr, "usage: peak_detach_helper <protocol-fd>\n");
        return 2;
    }

    char* end = NULL;
    long fd = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || fd < 0) {
        fprintf(stderr, "peak_detach_helper: invalid protocol fd\n");
        return 2;
    }

    if (send_response((int)fd, PEAK_DETACH_HELPER_STATUS_OK, 0, 0) != 0) {
        return 1;
    }

    return serve_protocol((int)fd);
#endif
}
