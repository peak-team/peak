#ifndef PEAK_DETACH_HELPER_PROTOCOL_H
#define PEAK_DETACH_HELPER_PROTOCOL_H

/**
 * @file detach_helper_protocol.h
 * @brief Define the fixed-width protocol shared with the detach helper.
 */

#include <stdint.h>

#define PEAK_DETACH_HELPER_MAGIC 0x50444a48u
#define PEAK_DETACH_HELPER_VERSION 1u
#define PEAK_DETACH_HELPER_MAX_THREADS 4096u
#define PEAK_DETACH_HELPER_MAX_BATCH_WRITES 64u
#define PEAK_DETACH_HELPER_MAX_INSTRUCTIONS \
    (PEAK_DETACH_HELPER_MAX_THREADS + PEAK_DETACH_HELPER_MAX_BATCH_WRITES)
#define PEAK_DETACH_HELPER_MAX_PATCH_BYTES 32u

typedef enum {
    PEAK_DETACH_HELPER_CMD_STOP = 1,
    PEAK_DETACH_HELPER_CMD_EVACUATE = 2,
    PEAK_DETACH_HELPER_CMD_RESUME = 3,
    PEAK_DETACH_HELPER_CMD_SHUTDOWN = 4
} PeakDetachHelperCommand;

typedef enum {
    PEAK_DETACH_HELPER_STATUS_OK = 0,
    PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR = 1,
    PEAK_DETACH_HELPER_STATUS_UNSUPPORTED = 2,
    PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED = 3,
    PEAK_DETACH_HELPER_STATUS_THREAD_LIMIT = 4,
    PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR = 5,
    PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR = 6,
    PEAK_DETACH_HELPER_STATUS_TIMEOUT = 7,
    PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED = 8
} PeakDetachHelperStatus;

typedef enum {
    PEAK_DETACH_HELPER_THREAD_OK = 0,
    PEAK_DETACH_HELPER_THREAD_ATTACH_FAILED = 1,
    PEAK_DETACH_HELPER_THREAD_WAIT_FAILED = 2,
    PEAK_DETACH_HELPER_THREAD_GETREGS_FAILED = 3
} PeakDetachHelperThreadStatus;

typedef enum {
    PEAK_DETACH_HELPER_INSTRUCTION_SET_PC = 1,
    PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY = 2,
    PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE = 3
} PeakDetachHelperInstructionAction;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command;
    int32_t pid;
    int32_t controller_tid;
    uint32_t instruction_count;
} PeakDetachHelperRequest;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t thread_count;
    int32_t errno_value;
} PeakDetachHelperResponse;

typedef struct {
    int32_t tid;
    uint32_t status;
    uint64_t pc;
} PeakDetachHelperThreadSnapshot;

typedef struct {
    int32_t tid;
    uint32_t action;
    uint64_t pc;
    uint64_t address;
    uint32_t size;
    uint8_t bytes[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
} PeakDetachHelperInstruction;

#endif /* PEAK_DETACH_HELPER_PROTOCOL_H */
