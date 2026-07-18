#ifndef PEAK_EXEC_CHECKPOINT_WRITER_H
#define PEAK_EXEC_CHECKPOINT_WRITER_H

#include <stdbool.h>
#include <stddef.h>

/** One immutable function row captured before an exec checkpoint is written. */
typedef struct {
    const char* name;
    unsigned long num_calls;
    unsigned long threads_seen;
    double total_time;
    double max_total_time;
    double min_total_time;
    double exclusive_time;
    float max_time;
    float min_time;
} PeakExecCheckpointRow;

/**
 * Writes a new checkpoint CSV from caller-owned immutable rows.
 *
 * The writer borrows @p rows and every row name for the duration of the call.
 * It creates an exclusive file using @p checkpoint_index, trying later indices
 * when a checkpoint already exists. A failed write removes its incomplete
 * file. Listener state and snapshot ownership remain with the caller.
 *
 * @return true after the complete file is closed; otherwise false with errno
 *         describing the failure.
 */
bool peak_exec_checkpoint_write_rows(
    unsigned long long checkpoint_index,
    const PeakExecCheckpointRow* rows,
    size_t row_count,
    double overhead_per_call);

#endif /* PEAK_EXEC_CHECKPOINT_WRITER_H */
