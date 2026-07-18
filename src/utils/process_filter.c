#include "process_filter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PEAK_JIT_ENABLE_ENV "PEAK_JIT_ENABLE"
#define PEAK_PROFILE_INTERPRETERS_ENV "PEAK_PROFILE_INTERPRETERS"
#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_FILE_ENV "PEAK_TARGET_FILE"
#define PEAK_TARGET_GROUP_ENV "PEAK_TARGET_GROUP"
#define PEAK_GPU_TARGET_ENV "PEAK_GPU_TARGET"
#define PEAK_GPU_TARGET_FILE_ENV "PEAK_GPU_TARGET_FILE"
#define PEAK_GPU_MONITOR_ALL_ENV "PEAK_GPU_MONITOR_ALL"
#define PEAK_MEMORY_PROFILE_ENV "PEAK_MEMORY_PROFILE"
#define PEAK_PROFILE_DECISION_UNKNOWN (-1)

static int peak_process_profile_enabled_cache = PEAK_PROFILE_DECISION_UNKNOWN;
static int peak_process_requests_work_cache = PEAK_PROFILE_DECISION_UNKNOWN;

void get_argv0(char** argv0)
{
    char* buffer = (char*)malloc(sizeof(char) * (1024));
    strcpy(buffer, "null\0");
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (!fp) {
        perror("fopen");
        *argv0 = buffer;
        return;
    }

    int n = fread(buffer, 1, 1024, fp);
    if (n == 0) {
        perror("fread");
        *argv0 = buffer;
        return;
    }
    buffer[n - 1] = '\0';
    *argv0 = buffer;
}

// Hardcoded list of command basenames, null-terminated.
static const char *check_list[] = {
    "peak_detach_helper",   // Peak's out-of-process safe-detach helper
    "ibrun",                // TACC-specific launcher
    "mpirun",               // Generic MPI launcher (Open MPI, MPICH, etc.)
    "mpiexec",              // Alias for mpirun in many MPI distributions
    "mpiexec.hydra",        // MPICH Hydra process manager
    "mpirun_rsh",           // RSH-based MPI launcher
    "prterun",              // Open MPI (PRTE runtime launcher)
    "prted",                // PRTE daemon used by Open MPI
    "orterun",              // Older Open MPI runtime launcher (deprecated)
    "orted",                // Open MPI daemon for job launching
    "prun",                 // Another launcher found in some Open MPI
    "srun",                 // Slurm job launcher with MPI support
    "jsrun",                // IBM Spectrum MPI launcher (LSF)
    "aprun",                // Cray MPI launcher
    "hydra_bstrap_proxy",   // MPICH Hydra bootstrap proxy
    "hydra_pmi_proxy",      // MPICH Hydra PMI proxy
    "pmi_proxy",           // Generic PMI proxy
    "pmi2_proxy",          // PMI2 proxy (if present in your environment)
    "pmix_server",         // PMIx server daemon (if applicable)
    "pmix_proxy",          // PMIx proxy (if applicable)
    "tau_exec",             // Performance profiling wrapper for MPI
    "mpiexec_mpt",          // SGI MPT MPI launcher
    // Additional MPI/OpenSHMEM related launchers/daemons
    "oshrun",              // OpenSHMEM program launcher
    "shmemrun",            // Another OpenSHMEM launcher
    "mpd",                 // Old MPICH daemon
    "mpdboot",             // Old MPICH daemon bootstrap
    "mpdallexit",          // Old MPICH daemon shutdown
    "mpdtrace",            // Old MPICH daemon trace tool
    // HPC job schedulers and related commands (Slurm, LSF, PBS/Torque)
    "salloc",
    "sbatch",
    "squeue",
    "scancel",
    "sinfo",
    "scontrol",
    "sreport",
    "sacct",
    "bsub",                // LSF job submission
    "bjobs",               // LSF job listing
    "bkill",               // LSF job termination
    "qsub",                // PBS/Torque job submission
    "qstat",               // PBS/Torque job status
    "qdel",                // PBS/Torque job deletion
    "qhold",               // PBS/Torque hold job
    "qalter",              // PBS/Torque alter job attributes
    // Common Linux commands
    "lscpu",
    "hostname",
    "numactl",
    "sh",
    "bash",
    "lmod",
    "ml",
    "modulecmd",
    "env",
    "timeout",
    "time",
    "awk",
    "sed",
    "grep",
    "ls",
    "cat",
    "rm",
    "cp",
    "mv",
    "chmod",
    "chown",
    "find",
    "pwd",
    "echo",
    "whoami",
    "date",
    "mkdir",
    "mktemp",
    "rmdir",
    "df",
    "du",
    "top",
    "ps",
    "kill",
    "uname",
    "ifconfig",
    "ping",
    "curl",
    "wget",
    "scp",
    "rsync",
    "zip",
    "unzip",
    "tar",
    "gzip",
    "gunzip",
    "sort",
    "uniq",
    "head",
    "tail",
    "tee",
    "cut",
    "tr",
    "wc",
    "diff",
    "patch",
    "make",
    "node",
    "npm",
    "git",
    "ssh",
    "sftp",
    "bc",
    "which",
    "seq",
    "stty",
    "tty",
    "tput",
    "clear",
    "basename",
    "dirname",
    "readlink",
    "realpath",
    "xargs",
    "expr",
    "test",
    "[",
    "true",
    "false",
    NULL // Null terminator to mark the end of the list
};

/**
 * @brief Extracts the base name of a command from a path.
 *
 * For example, given "/bin/awk", it will return "awk".
 *
 * @param path The full path of the command.
 * @return Pointer to the base name within the input string.
 */
static const char *get_base_name(const char *path) {
    const char *base = strrchr(path, '/');
    return (base != NULL) ? base + 1 : path;
}

static int
peak_env_truthy(const char* value)
{
    return value != NULL &&
           (strcasecmp(value, "1") == 0 ||
            strcasecmp(value, "true") == 0 ||
            strcasecmp(value, "yes") == 0 ||
            strcasecmp(value, "on") == 0);
}

static int
peak_env_nonempty(const char* name)
{
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0';
}

void
peak_set_process_requests_work(int enabled)
{
    __atomic_store_n(&peak_process_requests_work_cache,
                     enabled ? 1 : 0,
                     __ATOMIC_RELEASE);
}

int
peak_process_requests_work(void)
{
    int cached = __atomic_load_n(&peak_process_requests_work_cache,
                                 __ATOMIC_ACQUIRE);
    if (cached != PEAK_PROFILE_DECISION_UNKNOWN) {
        return cached;
    }

    int requested =
        peak_env_nonempty(PEAK_TARGET_ENV) ||
        peak_env_nonempty(PEAK_TARGET_FILE_ENV) ||
        peak_env_nonempty(PEAK_TARGET_GROUP_ENV) ||
        peak_env_nonempty(PEAK_GPU_TARGET_ENV) ||
        peak_env_nonempty(PEAK_GPU_TARGET_FILE_ENV) ||
        peak_env_truthy(getenv(PEAK_GPU_MONITOR_ALL_ENV)) ||
        peak_env_truthy(getenv(PEAK_MEMORY_PROFILE_ENV));

    int expected = PEAK_PROFILE_DECISION_UNKNOWN;
    if (!__atomic_compare_exchange_n(&peak_process_requests_work_cache,
                                     &expected,
                                     requested,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        requested = expected;
    }

    return requested;
}

static int
peak_command_is_jit_runtime(const char* command)
{
    const char* base_name;

    if (command == NULL) {
        return 0;
    }

    base_name = get_base_name(command);
    return strcmp(base_name, "node") == 0 ||
           strcmp(base_name, "nodejs") == 0;
}

static int
starts_with(const char* str, const char* prefix)
{
    size_t prefix_len;

    if (str == NULL || prefix == NULL) {
        return 0;
    }

    prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}

static int
is_versioned_command(const char* command, const char* prefix)
{
    const char* suffix;

    if (!starts_with(command, prefix)) {
        return 0;
    }

    suffix = command + strlen(prefix);
    return *suffix == '\0' || (*suffix >= '0' && *suffix <= '9');
}

int
check_interpreter_command(const char* command)
{
    const char* base_name;

    if (command == NULL) {
        return 0;
    }

    base_name = get_base_name(command);
    return is_versioned_command(base_name, "lua") ||
           strcmp(base_name, "luajit") == 0 ||
           is_versioned_command(base_name, "python") ||
           is_versioned_command(base_name, "perl") ||
           is_versioned_command(base_name, "tclsh");
}

static int
arg_looks_like_module_helper(const char* arg)
{
    return arg != NULL &&
           (strstr(arg, "lmod") != NULL ||
            strstr(arg, "Lmod") != NULL ||
            strstr(arg, "modulecmd") != NULL ||
            strstr(arg, "/Modules/") != NULL);
}

int check_command(const char *str) {
    if (!str) {
        return 0; // Invalid input
    }

    const char *base_name = get_base_name(str);

    // Iterate through the check_list for a match
    for (const char **entry = check_list; *entry != NULL; ++entry) {
        if (strcmp(base_name, *entry) == 0) {
            return 1; // Match found
        }
    }

    return 0; // No match found
}

int check_module_helper_command(int argc, char *const argv[]) {
    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return 0;
    }

    if (!check_interpreter_command(argv[0])) {
        return 0;
    }

    for (int i = 0; i < argc && argv[i] != NULL; i++) {
        if (arg_looks_like_module_helper(argv[i])) {
            return 1;
        }
    }

    return 0;
}

int
peak_should_profile_command(int argc, char *const argv[])
{
    const char* command;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return 0;
    }

    command = argv[0];
    if (check_interpreter_command(command)) {
        return peak_env_truthy(getenv(PEAK_PROFILE_INTERPRETERS_ENV));
    }

    if (!check_command(command)) {
        return 1;
    }

    return peak_env_truthy(getenv(PEAK_JIT_ENABLE_ENV)) &&
           peak_command_is_jit_runtime(command);
}

void
peak_set_process_profile_enabled(int enabled)
{
    __atomic_store_n(&peak_process_profile_enabled_cache,
                     enabled ? 1 : 0,
                     __ATOMIC_RELEASE);
}

static int
peak_process_profile_from_proc_cmdline(void)
{
    char buffer[4096];
    char* argv[128];
    int argc = 0;
    int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    int fd = open("/proc/self/cmdline", open_flags);
    if (fd < 0) {
        return PEAK_PROFILE_DECISION_UNKNOWN;
    }

    ssize_t nread = read(fd, buffer, sizeof(buffer) - 1);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (nread <= 0) {
        return PEAK_PROFILE_DECISION_UNKNOWN;
    }
    buffer[nread] = '\0';

    for (ssize_t i = 0; i < nread && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1;) {
        while (i < nread && buffer[i] == '\0') {
            i++;
        }
        if (i >= nread) {
            break;
        }

        argv[argc++] = &buffer[i];
        while (i < nread && buffer[i] != '\0') {
            i++;
        }
        if (i < nread) {
            buffer[i++] = '\0';
        }
    }
    argv[argc] = NULL;

    if (argc <= 0) {
        return PEAK_PROFILE_DECISION_UNKNOWN;
    }

    return peak_should_profile_command(argc, argv);
}

static int
peak_process_profile_from_proc_exe(void)
{
    char exe[4096];
    ssize_t nread = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (nread <= 0) {
        return PEAK_PROFILE_DECISION_UNKNOWN;
    }

    exe[nread] = '\0';
    char* argv[] = {
        exe,
        NULL
    };
    return peak_should_profile_command(1, argv);
}

int
peak_process_profile_enabled(void)
{
    int cached = __atomic_load_n(&peak_process_profile_enabled_cache,
                                 __ATOMIC_ACQUIRE);
    if (cached != PEAK_PROFILE_DECISION_UNKNOWN) {
        return cached;
    }

    int enabled = peak_process_profile_from_proc_cmdline();
    if (enabled == PEAK_PROFILE_DECISION_UNKNOWN) {
        enabled = peak_process_profile_from_proc_exe();
    }
    if (enabled == PEAK_PROFILE_DECISION_UNKNOWN) {
        enabled = 1;
    }

    int expected = PEAK_PROFILE_DECISION_UNKNOWN;
    if (!__atomic_compare_exchange_n(&peak_process_profile_enabled_cache,
                                     &expected,
                                     enabled,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        enabled = expected;
    }

    return enabled;
}
