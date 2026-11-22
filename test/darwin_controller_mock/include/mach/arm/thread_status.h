#ifndef PEAK_THREAD_STATUS_H
#define PEAK_THREAD_STATUS_H
#include <stdint.h>
typedef struct { uintptr_t __pc; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 1
#define ARM_THREAD_STATE64_COUNT 1
#endif /* PEAK_THREAD_STATUS_H */
