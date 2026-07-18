#ifndef PEAK_PARENT_REGISTRY_INTERNAL_H
#define PEAK_PARENT_REGISTRY_INTERNAL_H

#ifdef PEAK_UTILS_TESTING
typedef enum {
    PEAK_PARENT_FAULT_NONE = 0,
    PEAK_PARENT_FAULT_PROC_READ,
    PEAK_PARENT_FAULT_SNAPSHOT_READ,
    PEAK_PARENT_FAULT_TEMP_WRITE,
    PEAK_PARENT_FAULT_TEMP_FSYNC,
    PEAK_PARENT_FAULT_RENAME,
    PEAK_PARENT_FAULT_DIR_FSYNC,
} PeakParentRegistryFault;

typedef struct {
    const char* proc_root;
    const char* xdg_runtime_dir;
    const char* tmp_root;
    PeakParentRegistryFault fault;
    unsigned int fault_after;
} PeakParentRegistryTestConfig;

int peak_check_parent_process_for_test(const PeakParentRegistryTestConfig* config);
#endif

#endif
