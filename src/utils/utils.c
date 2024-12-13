#include "utils.h"

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
    "ibrun",                // TACC-specific launcher
    "mpirun",               // Generic MPI launcher (Open MPI, MPICH, etc.)
    "mpiexec",              // Alias for mpirun in many MPI distributions
    "mpiexec.hydra",        // MPICH Hydra process manager
    "mpirun_rsh",           // RSH-based MPI launcher
    "prterun",              // Open MPI (PRTE runtime launcher)
    "orterun",              // Older Open MPI runtime launcher (deprecated)
    "srun",                 // Slurm job launcher with MPI support
    "jsrun",                // IBM Spectrum MPI launcher (LSF)
    "aprun",                // Cray MPI launcher
    "hydra_bstrap_proxy",   // MPICH Hydra bootstrap proxy
    "hydra_pmi_proxy",      // MPICH Hydra PMI proxy
    "tau_exec",             // Performance profiling wrapper for MPI
    "mpiexec_mpt",          // SGI MPT MPI launcher
    //"yod",                  // IBM Blue Gene MPI launcher
    //"poe"                   // IBM Parallel Operating Environment (POE) launcher
    "lscpu",
    "hostname",
    "numactl",
    // Common Linux commands
    "sh",
    "bash", 
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
