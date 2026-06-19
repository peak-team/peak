#define _GNU_SOURCE

#include "peak_detach_helper_protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int
environment_is_sanitized(void)
{
    extern char** environ;

    for (size_t i = 0; environ != NULL && environ[i] != NULL; i++) {
        if (strncmp(environ[i], "PEAK_", 5) == 0 ||
            strncmp(environ[i], "LD_PRELOAD=", 11) == 0 ||
            strncmp(environ[i], "LD_AUDIT=", 9) == 0) {
            return 0;
        }
    }

    return 1;
}

static void
log_command(const char* command)
{
    const char* path = getenv("FAKE_DETACH_HELPER_LOG");
    FILE* fp;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        return;
    }

    fprintf(fp, "%s\n", command);
    fclose(fp);
}

static long
env_long_default(const char* name, long fallback)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static int
drain_instructions(int fd, uint32_t instruction_count)
{
    PeakDetachHelperInstruction instruction;

    for (uint32_t i = 0; i < instruction_count; i++) {
        if (read_exact(fd, &instruction, sizeof(instruction)) <= 0) {
            return -1;
        }
    }

    return 0;
}

int
main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        return 2;
    }

    char* end = NULL;
    long fd = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || fd < 0) {
        return 2;
    }

    const char* scenario = getenv("FAKE_DETACH_HELPER_SCENARIO");
    if (scenario == NULL) {
        scenario = "stop-permission";
    }
    long resume_count = 0;
    long fail_resume_index =
        env_long_default("FAKE_DETACH_HELPER_FAIL_RESUME_INDEX", 0);

    if (!environment_is_sanitized()) {
        (void)send_response((int)fd,
                            PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                            EPERM,
                            0);
        return 1;
    }

    if (send_response((int)fd, PEAK_DETACH_HELPER_STATUS_OK, 0, 0) != 0) {
        return 1;
    }
    log_command("START");

    for (;;) {
        PeakDetachHelperRequest request;
        int read_status = read_exact((int)fd, &request, sizeof(request));
        if (read_status <= 0) {
            return read_status == 0 ? 0 : 1;
        }

        if (request.magic != PEAK_DETACH_HELPER_MAGIC ||
            request.version != PEAK_DETACH_HELPER_VERSION) {
            (void)send_response((int)fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EPROTO,
                                0);
            return 1;
        }

        if (drain_instructions((int)fd, request.instruction_count) != 0) {
            return 1;
        }

        if (request.command == PEAK_DETACH_HELPER_CMD_STOP) {
            log_command("STOP");
            if (strcmp(scenario, "success-zero") == 0 ||
                strcmp(scenario, "evacuate-error") == 0 ||
                strcmp(scenario, "evacuate-release-failed") == 0 ||
                strcmp(scenario, "resume-release-failed") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_OK,
                                    0,
                                    0);
                continue;
            }
            if (strcmp(scenario, "stop-permission") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED,
                                    EPERM,
                                    0);
                continue;
            }
            if (strcmp(scenario, "stop-release-failed") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED,
                                    EIO,
                                    0);
                continue;
            }
            if (strcmp(scenario, "stop-timeout") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_TIMEOUT,
                                    ETIMEDOUT,
                                    0);
                continue;
            }
            if (strcmp(scenario, "bad-snapshot") == 0) {
                PeakDetachHelperThreadSnapshot snapshot = {
                    .tid = (int32_t)getpid(),
                    .status = PEAK_DETACH_HELPER_THREAD_GETREGS_FAILED,
                    .pc = 0
                };
                if (send_response((int)fd,
                                  PEAK_DETACH_HELPER_STATUS_OK,
                                  0,
                                  1) != 0 ||
                    write_exact((int)fd, &snapshot, sizeof(snapshot)) != 0) {
                    return 1;
                }
                continue;
            }
            if (strcmp(scenario, "blocked-pc") == 0) {
                PeakDetachHelperThreadSnapshot snapshot = {
                    .tid = (int32_t)getpid(),
                    .status = PEAK_DETACH_HELPER_THREAD_OK,
                    .pc = 0x1234
                };
                if (send_response((int)fd,
                                  PEAK_DETACH_HELPER_STATUS_OK,
                                  0,
                                  1) != 0 ||
                    write_exact((int)fd, &snapshot, sizeof(snapshot)) != 0) {
                    return 1;
                }
                continue;
            }

            (void)send_response((int)fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EINVAL,
                                0);
        } else if (request.command == PEAK_DETACH_HELPER_CMD_EVACUATE) {
            log_command("EVACUATE");
            if (strcmp(scenario, "evacuate-error") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                continue;
            }
            if (strcmp(scenario, "evacuate-release-failed") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED,
                                    EIO,
                                    0);
                continue;
            }
            (void)send_response((int)fd,
                                PEAK_DETACH_HELPER_STATUS_OK,
                                0,
                                0);
        } else if (request.command == PEAK_DETACH_HELPER_CMD_RESUME ||
                   request.command == PEAK_DETACH_HELPER_CMD_SHUTDOWN) {
            log_command(request.command == PEAK_DETACH_HELPER_CMD_RESUME
                            ? "RESUME"
                            : "SHUTDOWN");
            if (request.command == PEAK_DETACH_HELPER_CMD_RESUME) {
                resume_count++;
                if (strcmp(scenario, "resume-release-failed") == 0 &&
                    (fail_resume_index <= 0 ||
                     resume_count == fail_resume_index)) {
                    (void)send_response((int)fd,
                                        PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED,
                                        EIO,
                                        0);
                    continue;
                }
            }
            (void)send_response((int)fd,
                                PEAK_DETACH_HELPER_STATUS_OK,
                                0,
                                0);
            if (request.command == PEAK_DETACH_HELPER_CMD_SHUTDOWN) {
                return 0;
            }
        } else {
            (void)send_response((int)fd,
                                PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                EINVAL,
                                0);
            return 1;
        }
    }
}
