#define _GNU_SOURCE

#include "peak_detach_helper_protocol.h"

#include <errno.h>
#include <limits.h>
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
        ssize_t n = write(fd, cursor + done, size - done);
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
env_u64(const char* name, uint64_t* value_out)
{
    const char* value = getenv(name);
    char* end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *value_out = (uint64_t)parsed;
    return 1;
}

static int
env_u64_file(const char* name, uint64_t* value_out)
{
    const char* path = getenv(name);
    FILE* fp;
    char buffer[64];
    char* end = NULL;
    unsigned long long parsed;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    errno = 0;
    parsed = strtoull(buffer, &end, 0);
    if (errno != 0 || end == buffer ||
        (*end != '\0' && *end != '\n' && *end != '\r')) {
        fprintf(stderr, "invalid %s file value: %s\n", name, buffer);
        return -1;
    }

    *value_out = (uint64_t)parsed;
    return 1;
}

static int
env_i32_selector(const char* name, int32_t target_pid, int32_t* value_out)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    if (strcmp(value, "self") == 0 || strcmp(value, "getpid") == 0) {
        *value_out = (int32_t)getpid();
        return 1;
    }
    if (strcmp(value, "target-pid") == 0) {
        *value_out = target_pid;
        return 1;
    }

    errno = 0;
    parsed = strtol(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -1;
    }

    *value_out = (int32_t)parsed;
    return 1;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
env_hex_bytes(const char* name, uint8_t* bytes_out, uint32_t* size_out)
{
    const char* value = getenv(name);
    size_t len;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    len = strlen(value);
    if ((len % 2) != 0 || len / 2 > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        fprintf(stderr, "invalid %s hex byte length: %s\n", name, value);
        return -1;
    }

    for (size_t i = 0; i < len / 2; i++) {
        int high = hex_nibble(value[i * 2]);
        int low = hex_nibble(value[i * 2 + 1]);
        if (high < 0 || low < 0) {
            fprintf(stderr, "invalid %s hex byte: %s\n", name, value);
            return -1;
        }
        bytes_out[i] = (uint8_t)((high << 4) | low);
    }

    *size_out = (uint32_t)(len / 2);
    return 1;
}

static const char*
instruction_action_name(uint32_t action)
{
    switch (action) {
        case PEAK_DETACH_HELPER_INSTRUCTION_SET_PC:
            return "SET_PC";
        case PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY:
            return "WRITE_MEMORY";
        case PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE:
            return "SINGLE_STEP_OUT_OF_RANGE";
        default:
            return "UNKNOWN";
    }
}

static int
read_instructions(int fd,
                  PeakDetachHelperInstruction* instructions,
                  uint32_t instruction_count)
{
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (read_exact(fd, &instructions[i], sizeof(instructions[i])) <= 0) {
            return -1;
        }
    }

    return 0;
}

static int
validate_expected_actions(const PeakDetachHelperInstruction* instructions,
                          uint32_t instruction_count)
{
    const char* expected = getenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS");
    char* copy;
    char* cursor;
    uint32_t index = 0;

    if (expected == NULL || expected[0] == '\0') {
        return 0;
    }

    copy = strdup(expected);
    if (copy == NULL) {
        return -1;
    }

    cursor = copy;
    while (cursor != NULL) {
        char* comma = strchr(cursor, ',');
        uint32_t expected_action;

        if (comma != NULL) {
            *comma = '\0';
        }

        if (strcmp(cursor, "SET_PC") == 0) {
            expected_action = PEAK_DETACH_HELPER_INSTRUCTION_SET_PC;
        } else if (strcmp(cursor, "WRITE_MEMORY") == 0) {
            expected_action = PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY;
        } else if (strcmp(cursor, "SINGLE_STEP_OUT_OF_RANGE") == 0) {
            expected_action =
                PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE;
        } else {
            fprintf(stderr, "unknown expected action: %s\n", cursor);
            free(copy);
            return -1;
        }

        if (index >= instruction_count ||
            instructions[index].action != expected_action) {
            fprintf(stderr,
                    "unexpected EVACUATE action %u: expected %s, got %s\n",
                    index,
                    cursor,
                    index < instruction_count
                        ? instruction_action_name(instructions[index].action)
                        : "MISSING");
            free(copy);
            return -1;
        }

        index++;
        cursor = comma != NULL ? comma + 1 : NULL;
    }

    if (index != instruction_count) {
        fprintf(stderr,
                "unexpected EVACUATE instruction count: expected %u, got %u\n",
                index,
                instruction_count);
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

static int
validate_evacuate_instructions(const PeakDetachHelperInstruction* instructions,
                               uint32_t instruction_count,
                               int32_t target_pid)
{
    uint64_t expected_set_pc = 0;
    int32_t expected_set_pc_tid = 0;
    uint64_t expected_write_address = 0;
    uint64_t expected_write_size_u64 = 0;
    uint8_t expected_write_bytes[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint32_t expected_write_byte_count = 0;
    int expect_set_pc =
        env_u64("FAKE_DETACH_HELPER_EXPECT_SET_PC", &expected_set_pc);
    int expect_set_pc_tid =
        env_i32_selector("FAKE_DETACH_HELPER_EXPECT_SET_PC_TID",
                         target_pid,
                         &expected_set_pc_tid);
    int expect_write_address =
        env_u64("FAKE_DETACH_HELPER_EXPECT_WRITE_ADDRESS",
                &expected_write_address);
    int expect_write_size =
        env_u64("FAKE_DETACH_HELPER_EXPECT_WRITE_SIZE",
                &expected_write_size_u64);
    int expect_write_bytes =
        env_hex_bytes("FAKE_DETACH_HELPER_EXPECT_WRITE_BYTES_HEX",
                      expected_write_bytes,
                      &expected_write_byte_count);
    int saw_set_pc = 0;
    int saw_write = 0;

    if (expect_set_pc < 0 || expect_set_pc_tid < 0 ||
        expect_write_address < 0 || expect_write_size < 0 ||
        expect_write_bytes < 0) {
        return -1;
    }
    if (expect_write_size > 0 &&
        expected_write_size_u64 > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        fprintf(stderr,
                "unexpected WRITE_MEMORY size expectation: 0x%llx\n",
                (unsigned long long)expected_write_size_u64);
        return -1;
    }
    if (expect_write_size > 0 && expect_write_bytes > 0 &&
        expected_write_size_u64 != expected_write_byte_count) {
        fprintf(stderr,
                "WRITE_MEMORY size/bytes expectation mismatch: size %llu, bytes %u\n",
                (unsigned long long)expected_write_size_u64,
                expected_write_byte_count);
        return -1;
    }
    if (validate_expected_actions(instructions, instruction_count) != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction = &instructions[i];

        if ((expect_set_pc > 0 || expect_set_pc_tid > 0) &&
            instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC) {
            if (expect_set_pc > 0 && instruction->pc != expected_set_pc) {
                fprintf(stderr,
                        "unexpected SET_PC target: expected 0x%llx, got 0x%llx\n",
                        (unsigned long long)expected_set_pc,
                        (unsigned long long)instruction->pc);
                return -1;
            }
            if (expect_set_pc_tid > 0 &&
                instruction->tid != expected_set_pc_tid) {
                fprintf(stderr,
                        "unexpected SET_PC tid: expected %d, got %d\n",
                        expected_set_pc_tid,
                        instruction->tid);
                return -1;
            }
            saw_set_pc = 1;
        }

        if ((expect_write_address > 0 || expect_write_size > 0 ||
             expect_write_bytes > 0) &&
            instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY) {
            if (expect_write_address > 0 &&
                instruction->address != expected_write_address) {
                fprintf(stderr,
                        "unexpected WRITE_MEMORY address: expected 0x%llx, got 0x%llx\n",
                        (unsigned long long)expected_write_address,
                        (unsigned long long)instruction->address);
                return -1;
            }
            if (expect_write_size > 0 &&
                instruction->size != (uint32_t)expected_write_size_u64) {
                fprintf(stderr,
                        "unexpected WRITE_MEMORY size: expected %llu, got %u\n",
                        (unsigned long long)expected_write_size_u64,
                        instruction->size);
                return -1;
            }
            if (expect_write_bytes > 0 &&
                (instruction->size != expected_write_byte_count ||
                 memcmp(instruction->bytes,
                        expected_write_bytes,
                        expected_write_byte_count) != 0)) {
                fprintf(stderr, "unexpected WRITE_MEMORY bytes\n");
                return -1;
            }
            saw_write = 1;
        }
    }

    if ((expect_set_pc > 0 || expect_set_pc_tid > 0) && !saw_set_pc) {
        fprintf(stderr, "missing expected SET_PC instruction\n");
        return -1;
    }
    if ((expect_write_address > 0 || expect_write_size > 0 ||
         expect_write_bytes > 0) &&
        !saw_write) {
        fprintf(stderr, "missing expected WRITE_MEMORY instruction\n");
        return -1;
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
    long synthetic_stop_count = 0;
    long fail_resume_index =
        env_long_default("FAKE_DETACH_HELPER_FAIL_RESUME_INDEX", 0);

    if (!environment_is_sanitized()) {
        log_command("UNSANITIZED_ENV");
        (void)send_response((int)fd,
                            PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                            EPERM,
                            0);
        return 1;
    }

    if (send_response((int)fd, PEAK_DETACH_HELPER_STATUS_OK, 0, 0) != 0) {
        log_command("HANDSHAKE_SEND_FAILED");
        fprintf(stderr,
                "fake helper handshake send failed: fd=%ld errno=%d\n",
                fd,
                errno);
        return 1;
    }
    log_command("START");

    for (;;) {
        PeakDetachHelperRequest request;
        PeakDetachHelperInstruction
            instructions[PEAK_DETACH_HELPER_MAX_INSTRUCTIONS];
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

        if (request.instruction_count > PEAK_DETACH_HELPER_MAX_INSTRUCTIONS ||
            read_instructions((int)fd,
                              instructions,
                              request.instruction_count) != 0) {
            return 1;
        }

        if (request.command == PEAK_DETACH_HELPER_CMD_STOP) {
            log_command("STOP");
            if (strcmp(scenario, "synthetic-stop") == 0 ||
                (strcmp(scenario, "synthetic-stop-once") == 0 &&
                 synthetic_stop_count == 0) ||
                (strcmp(scenario, "synthetic-stop-file-once") == 0 &&
                 synthetic_stop_count == 0)) {
                uint64_t pc = 0;
                int got_pc = env_u64("FAKE_DETACH_HELPER_STOP_PC", &pc);
                int32_t stop_tid = (int32_t)getpid();
                int got_stop_tid =
                    env_i32_selector("FAKE_DETACH_HELPER_STOP_TID",
                                     request.pid,
                                     &stop_tid);
                PeakDetachHelperThreadSnapshot snapshot = {
                    .tid = 0,
                    .status = PEAK_DETACH_HELPER_THREAD_OK,
                    .pc = 0
                };

                if (got_stop_tid < 0) {
                    (void)send_response((int)fd,
                                        PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                        EINVAL,
                                        0);
                    continue;
                }
                if (got_pc == 0) {
                    got_pc =
                        env_u64_file("FAKE_DETACH_HELPER_STOP_PC_FILE", &pc);
                }
                if (got_pc == 0 &&
                    strcmp(scenario, "synthetic-stop-file-once") == 0) {
                    (void)send_response((int)fd,
                                        PEAK_DETACH_HELPER_STATUS_OK,
                                        0,
                                        0);
                    continue;
                }
                if (got_pc <= 0) {
                    (void)send_response((int)fd,
                                        PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                        EINVAL,
                                        0);
                    continue;
                }
                snapshot.tid = stop_tid;
                snapshot.pc = pc;
                if (send_response((int)fd,
                                  PEAK_DETACH_HELPER_STATUS_OK,
                                  0,
                                  1) != 0 ||
                    write_exact((int)fd, &snapshot, sizeof(snapshot)) != 0) {
                    return 1;
                }
                synthetic_stop_count++;
                continue;
            }
            if (strcmp(scenario, "success-zero") == 0 ||
                strcmp(scenario, "synthetic-stop-once") == 0 ||
                strcmp(scenario, "synthetic-stop-file-once") == 0 ||
                strcmp(scenario, "evacuate-error") == 0 ||
                strcmp(scenario, "evacuate-release-failed") == 0 ||
                strcmp(scenario, "resume-release-failed") == 0 ||
                strcmp(scenario, "shutdown-missing-response") == 0) {
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
            if (strcmp(scenario, "stop-missing-response") == 0) {
                return 0;
            }
            if (strcmp(scenario, "stop-release-failed") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED,
                                    EIO,
                                    0);
                continue;
            }
            if (strcmp(scenario, "stop-timeout") == 0 ||
                strcmp(scenario, "stop-timeout-delayed") == 0) {
                if (strcmp(scenario, "stop-timeout-delayed") == 0) {
                    usleep(5500 * 1000);
                }
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_TIMEOUT,
                                    ETIMEDOUT,
                                    0);
                continue;
            }
            if (strcmp(scenario, "stop-unsupported") == 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_UNSUPPORTED,
                                    ENOSYS,
                                    0);
                continue;
            }
            if (strcmp(scenario, "duplicate-snapshot") == 0) {
                uint64_t pc = 0;
                if (env_u64("FAKE_DETACH_HELPER_STOP_PC", &pc) <= 0) {
                    (void)send_response((int)fd,
                                        PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                        EINVAL,
                                        0);
                    continue;
                }
                PeakDetachHelperThreadSnapshot snapshots[2] = {
                    {
                        .tid = (int32_t)getpid(),
                        .status = PEAK_DETACH_HELPER_THREAD_OK,
                        .pc = pc
                    },
                    {
                        .tid = (int32_t)getpid(),
                        .status = PEAK_DETACH_HELPER_THREAD_OK,
                        .pc = pc
                    }
                };
                if (send_response((int)fd,
                                  PEAK_DETACH_HELPER_STATUS_OK,
                                  0,
                                  2) != 0 ||
                    write_exact((int)fd, snapshots, sizeof(snapshots)) != 0) {
                    return 1;
                }
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
            if (validate_evacuate_instructions(instructions,
                                               request.instruction_count,
                                               request.pid) != 0) {
                (void)send_response((int)fd,
                                    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR,
                                    EPROTO,
                                    0);
                continue;
            }
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
            if (request.command == PEAK_DETACH_HELPER_CMD_SHUTDOWN &&
                strcmp(scenario, "shutdown-missing-response") == 0) {
                return 0;
            }
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
