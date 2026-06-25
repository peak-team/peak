#include "utils.h"

#include <errno.h>
#include <strings.h>

#define PEAK_JIT_ENABLE_ENV "PEAK_JIT_ENABLE"
#define PEAK_PROFILE_INTERPRETERS_ENV "PEAK_PROFILE_INTERPRETERS"
#define PEAK_PROFILE_DECISION_UNKNOWN (-1)

static int peak_process_profile_enabled_cache = PEAK_PROFILE_DECISION_UNKNOWN;

double peak_second()
{
    struct timespec measure;

    // Get the current time as the start time
    clock_gettime(CLOCK_MONOTONIC, &measure);

    // Return the elapsed time in seconds
    return (double)measure.tv_sec + (double)measure.tv_nsec * 1e-9;
}

int check_parent_process(char* lock_file, int* need_to_clean)
{
    *need_to_clean = 0;
    int fd = open(lock_file, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        // PPID file already exists
        fd = open(lock_file, O_RDWR);
        if (fd < 0) {
            //perror("Failed to open PPID file");
            return -1;
        }
    } else {
        *need_to_clean = 1;
    }
    // Write current PPID to lock file
    pid_t mypid = getpid();
    pid_t parentpid = getppid();

    FILE* fp = fdopen(fd, "r+"); // open the file in read mode using fdopen()
    if (fp == NULL) {
        perror("fdopen");
        close(fd);
        return -1;
    }

    flock(fd, LOCK_EX); // Obtain an exclusive lock on the file

    int found_parent = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) { // read each line of the file
        int num;
        if (sscanf(line, "%d", &num) == 1) { // extract the integer from the line
            if (num == parentpid) { // compare the integer with the desired PPID
                found_parent = 1;
                // fprintf(stderr, "Found PPID %d in file\n", parentpid);
                break; // stop searching if a match is found
            }
        }
    }
    fprintf(fp, "%d\n", mypid);
    //fprintf(stderr, "wrote %d with flag %d\n", mypid, flg);
    fflush(fp); // Flush the output buffer

    flock(fd, LOCK_UN); // Release the lock

    fclose(fp);
    close(fd);
    return found_parent;
}

void remove_ppid_file(char* lock_file)
{
    unlink(lock_file);
}

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

/**
 * @brief Compare function used by qsort to compare two doubles.
 *
 * @param a Pointer to the first double to compare.
 * @param b Pointer to the second double to compare.
 *
 * @return -1 if a < b, 0 if a == b, or 1 if a > b.
 */
static int cmpfunc_double(const void* a, const void* b)
{
    if (*(double*)a < *(double*)b) {
        return -1;
    } else if (*(double*)a > *(double*)b) {
        return 1;
    } else {
        return 0;
    }
}

double median_double(double* arr, size_t n)
{
    qsort(arr, n, sizeof(double), cmpfunc_double);
    if (n % 2 == 0) {
        return (double)(arr[n / 2 - 1] + arr[n / 2]) / 2.0;
    } else {
        return (double)arr[n / 2];
    }
}

// Hardcoded list of substrings, null-terminated
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
    //"yod",                  // IBM Blue Gene MPI launcher
    //"poe"                   // IBM Parallel Operating Environment (POE) launcher
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
    "scp",
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
