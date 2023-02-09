#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <time.h>
#include <signal.h>
#include <mpi.h>

#define LOCK_FILE_PREFIX "/tmp/lock_flops_count_"
#define MAX_TIME_DIFF_ENV "FLOPS_MAX_DELAY"
#define MAX_TIME_DIFF 60
#define NUM_COUNTERS 8
#define NUM_COUNTERS_UC 2
#define SIGNAL_COUNTS 10

/*! \brief Event select */
// IA32_PERFEVTSELx MSR layout
// //   [0, 7] Event Select
// //   [8, 15] Unit Mask (UMASK)
// //   16 USR
// //   17 OS
// //   18 E Edge_detect
// //   19 PC Pin control
// //   20 INT APIC interrupt enable
// //   21 ANY Any thread (version 3)
// //   22 EN Enable counters
// //   23 INV Invert counter mask
// //   [24, 31] Counter Mask (CMASK)
// //   [32, 63] Reserved
#define PERF_EVENT(event, umask) \
  ( (event)			 \
    | (umask << 8)		 \
    | (1ULL << 16)		 \
    | (1ULL << 17)		 \
    | (0ULL << 21)		 \
    | (1ULL << 22)		 \
    )

#define PERF_EVENT_IMC(event, umask)                \
  ( (event)                                     \
    | (umask << 8)                              \
    | (0UL << 17) /* Clear counter */           \
    | (0UL << 18) /* Edge Detection. */         \
    | (0UL << 20) /* Overflow disable */        \
    | (1UL << 22) /* Enable. */                 \
    | (0UL << 23) /* Invert */                  \
    | (0x0UL << 24) /* Threshold */             \
    )

/* SKX CLX */
#define FP_ARITH_INST_RETIRED_SCALAR_DOUBLE      PERF_EVENT(0xC7, 0x01)
#define FP_ARITH_INST_RETIRED_SCALAR_SINGLE      PERF_EVENT(0xC7, 0x02)
#define FP_ARITH_INST_RETIRED_128B_PACKED_DOUBLE PERF_EVENT(0xC7, 0x04)
#define FP_ARITH_INST_RETIRED_128B_PACKED_SINGLE PERF_EVENT(0xC7, 0x08)
#define FP_ARITH_INST_RETIRED_256B_PACKED_DOUBLE PERF_EVENT(0xC7, 0x10)
#define FP_ARITH_INST_RETIRED_256B_PACKED_SINGLE PERF_EVENT(0xC7, 0x20)
#define FP_ARITH_INST_RETIRED_512B_PACKED_DOUBLE PERF_EVENT(0xC7, 0x40)
#define FP_ARITH_INST_RETIRED_512B_PACKED_SINGLE PERF_EVENT(0xC7, 0x80)
/* SKX CLX IMC*/
#define CAS_COUNT_RD                             PERF_EVENT_IMC(0x4, 0x3)
#define CAS_COUNT_WR                             PERF_EVENT_IMC(0x4, 0xC)

long long configs[NUM_COUNTERS] = {
    FP_ARITH_INST_RETIRED_SCALAR_DOUBLE,
    FP_ARITH_INST_RETIRED_SCALAR_SINGLE,
    FP_ARITH_INST_RETIRED_128B_PACKED_DOUBLE,
    FP_ARITH_INST_RETIRED_128B_PACKED_SINGLE,
    FP_ARITH_INST_RETIRED_256B_PACKED_DOUBLE,
    FP_ARITH_INST_RETIRED_256B_PACKED_SINGLE,
    FP_ARITH_INST_RETIRED_512B_PACKED_DOUBLE,
    FP_ARITH_INST_RETIRED_512B_PACKED_SINGLE
};
char *counter_names[NUM_COUNTERS] = {
    "FP_ARITH_INST_RETIRED.SCALAR_DOUBLE",
    "FP_ARITH_INST_RETIRED.SCALAR_SINGLE",
    "FP_ARITH_INST_RETIRED.128B_PACKED_DOUBLE",
    "FP_ARITH_INST_RETIRED.128B_PACKED_SINGLE",
    "FP_ARITH_INST_RETIRED.256B_PACKED_DOUBLE",
    "FP_ARITH_INST_RETIRED.256B_PACKED_SINGLE",
    "FP_ARITH_INST_RETIRED.512B_PACKED_DOUBLE",
    "FP_ARITH_INST_RETIRED.512B_PACKED_SINGLE"
};
double scaling_factors[NUM_COUNTERS] = {1, 1, 2, 4, 4, 8, 8, 16};

long long configs_uc[NUM_COUNTERS_UC] = {
    CAS_COUNT_RD,
    CAS_COUNT_WR
};

char *counter_names_uc[NUM_COUNTERS_UC] = {
    "CAS_COUNT_RD",
    "CAS_COUNT_WR"
};
#define SOCKETS 2
#define CHANNELS 6



void setup_counters_main() __attribute__((constructor));
void collect_counters_main() __attribute__((destructor));


int signal_received = 0;

int SIGNAL_LIST[SIGNAL_COUNTS] = {
    SIGINT,
    SIGILL,
    SIGSEGV,
    SIGABRT,
    SIGKILL,
    SIGFPE,
    SIGBUS,
    SIGTERM,
    SIGSYS,
    SIGQUIT  
};

/* Redirect signals to here to collect counters and clean up */
void counter_signal_handler(int sig) {
    signal_received = 1;
    for (int i = 0; i < SIGNAL_COUNTS; i++) {
        signal(SIGNAL_LIST[i], SIG_DFL);
    }
    //fprintf(stderr, "Signal Captured: %d\n", sig);
    collect_counters_main();
    // Re-raise the signal to terminate the process.
    raise(sig);
}

/* Register the signals to be redirected */
void redirect_signals() {
    for (int i = 0; i < SIGNAL_COUNTS; i++) {
        signal(SIGNAL_LIST[i], counter_signal_handler);
    }
}

#define check_list_size 14
/* Check if string ends with commands to be ignored*/
int check_string(const char *str) {
    const char *check_list[check_list_size] = {
        "/bin/sh",
        "/bin/bash", 
        "lscpu",
        "hostname",
        "bin/ssh",
        //"awk", "sed", "grep", "lscpu", "mktemp", "rm", "mv",
        "ibrun",
        "mpirun",
        "mpirun_rsh",
        "mpiexec",
        "mpiexec.hydra",
        "numactl",
        "srun",
        "hydra_bstrap_proxy",
        "hydra_pmi_proxy"
    };

    for (int i = 0; i < check_list_size; i++) {
        size_t len = strlen(check_list[i]);
        if (strlen(str) >= len && strcmp(str + strlen(str) - len, check_list[i]) == 0) {
            //printf("The string %s ends with %s\n", str, check_list[i]);
            return 1;
        }
    }
    return 0;
}

int check_lock(char* file_name, char** lock_file) {
    char* tmp_file_name = (char *)malloc(sizeof(char) * (strlen(file_name) + 1));
    *lock_file = (char *)malloc(sizeof(char) * (strlen(LOCK_FILE_PREFIX) + strlen(file_name) + 1));
    snprintf(tmp_file_name, strlen(file_name) + 1, "%s", file_name);
    for(int i = 0; i < strlen(file_name); i++)
        if(tmp_file_name[i] == '/') tmp_file_name[i] = '_';
    sprintf(*lock_file, "%s%s", LOCK_FILE_PREFIX, tmp_file_name);
    int fd = open(*lock_file, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        // Lock file already exists, check timestamp
        fd = open(*lock_file, O_RDWR);
        if (fd < 0) {
            //perror("Failed to open lock file");
            free(tmp_file_name);
            free(*lock_file);
            return -1;
        }
        time_t timestamp;
        int res = read(fd, &timestamp, sizeof(timestamp));
        if (res < sizeof(timestamp)) {
            //perror("Failed to read timestamp from lock file");
            free(tmp_file_name);
            free(*lock_file);
            return -1;
        }
        char* max_time_diff_str = getenv(MAX_TIME_DIFF_ENV);
        int max_time_diff = (max_time_diff_str == NULL)?MAX_TIME_DIFF:atof(max_time_diff_str);
        //printf("%d\n", max_time_diff);
        if (time(NULL) - timestamp > max_time_diff) {
            // Timestamp is more than MAX_TIME_DIFF seconds old, remove lock file
            //printf("Removing stale lock file\n");
            close(fd);
            unlink(*lock_file);
            fd = open(*lock_file, O_CREAT | O_EXCL | O_RDWR, 0644);
            if (fd < 0) {
                //perror("Failed to create lock file");
                free(tmp_file_name);
                free(*lock_file);
                return -1;
            }
        } else {
            //printf("Another instance of the program is already running\n");
            close(fd);
            free(tmp_file_name);
            free(*lock_file);
            return -1;
        }
    }
    // Write current timestamp to lock file
    time_t timestamp = time(NULL);
    int res = write(fd, &timestamp, sizeof(timestamp));
    if (res < sizeof(timestamp)) {
        perror("Failed to write timestamp to lock file");
        close(fd);
        unlink(*lock_file);
        free(tmp_file_name);
        free(*lock_file);
        return -1;
    }
    free(tmp_file_name);
    return fd;
}

void get_argv0(char **argv0) {
    char* buffer = (char *)malloc(sizeof(char) * (1024));
    strcpy(buffer, "null\0");
    FILE *fp = fopen("/proc/self/cmdline", "r");
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
    buffer[n-1] = '\0';
    *argv0 = buffer;
}

int check_MPI() {
    char* pmi_rank = getenv("PMI_RANK");
    char* mvapich_rank = getenv("MV2_COMM_WORLD_RANK");
    char* ompi_rank = getenv("OMPI_COMM_WORLD_RANK");
    if (pmi_rank != NULL || ompi_rank != NULL || mvapich_rank != NULL)
        return 1;
    else
        return 0;
}

int get_MPI_local_rank() {
    char* pmi_rank = getenv("MPI_LOCALRANKID");
    char* mvapich_rank = getenv("MV2_COMM_WORLD_LOCAL_RANK");
    char* ompi_rank = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
    if (pmi_rank != NULL)
        return atoi(pmi_rank);
    else if (mvapich_rank != NULL)
        return atoi(mvapich_rank);
    else if (ompi_rank != NULL)
        return atoi(ompi_rank);
    else
        return -1;
}

int MPI_Finalize(void) {
    return 0;
}

void print_result(char *argv, double* values, double* values_uc, double elapsed_time, int node_count) {
    double flops = 0;
    for (int i = 0; i < NUM_COUNTERS; i++) {
        flops += values[i] * scaling_factors[i]; 
    }
    if (flops > 1e3) {
        double mem_v[NUM_COUNTERS_UC] = {0.0};
        fprintf(stderr, "argv[%d]: %s\n", 0, argv);
        for (int i = 0; i < NUM_COUNTERS_UC; i++) {
            for (int j = 0; j < SOCKETS; j++) {
                fprintf(stderr, "Counter %s_SOCKET%d: %.0f\n", counter_names_uc[i], j, values_uc[i*SOCKETS+j]);
                mem_v[i] += values_uc[i*SOCKETS+j];
            }
            mem_v[i] *= 64.0;
        }
        for (int i = 0; i < NUM_COUNTERS; i++) {
            fprintf(stderr, "Counter %s: %.0f\n", counter_names[i], values[i]);
        }
        fprintf(stderr, "Time[s]:                 %f\n", elapsed_time);
        fprintf(stderr, "Memory Read  [GB]:       %f\n", mem_v[0]/1e9);
        fprintf(stderr, "Memory Write [GB]:       %f\n", mem_v[1]/1e9);
        fprintf(stderr, "Memory BW [GB/s]:        %f\n", (mem_v[0]+mem_v[1])/1e9/elapsed_time);
        fprintf(stderr, "Memory BW/node [GB/s]:   %f\n", (mem_v[0]+mem_v[1])/1e9/elapsed_time/node_count);
        fprintf(stderr, "TFLOP:                   %f\n", flops/1e12);
        fprintf(stderr, "TFLOPS:                  %f\n", flops/elapsed_time/1e12);
        fprintf(stderr, "TFLOPS/node:             %f\n", flops/elapsed_time/1e12/node_count);
    }
}

void reduce_result(char *argv, double* values, double* values_uc, double elapsed_time) {
    int rank, size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if(!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    double sum_values[NUM_COUNTERS] = {0.0};
    double sum_values_uc[NUM_COUNTERS_UC*SOCKETS] = {0.0};
    MPI_Reduce(values, &sum_values, NUM_COUNTERS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(values_uc, &sum_values_uc, NUM_COUNTERS_UC*SOCKETS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    
    MPI_Comm node_comm;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &node_comm);
    int node_rank;
    MPI_Comm_rank(node_comm, &node_rank);
    int node_count = 0;
    int total_node_count;
    if (node_rank == 0) {
        node_count = 1;
    }
    MPI_Reduce(&node_count, &total_node_count, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        print_result(argv, sum_values, sum_values_uc, elapsed_time, total_node_count);
    }
    PMPI_Finalize();
}

int skip_flag;
struct timespec start, end;
char* lock_file_name;
int fd;
long num_cores;
int *fds;
int *fds_uc;
char *argv_o;
int clean_up_done = 0;
void setup_counters_main() {
    get_argv0(&argv_o);
    skip_flag = check_string(argv_o);
    if(skip_flag) {
        return;
    }
    fd = check_lock(argv_o, &lock_file_name); 
    //fprintf(stderr, "open %d %s\n", fd, argv[0]);
    if(fd < 0) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        return;
    } 
    struct perf_event_attr pe[NUM_COUNTERS];
    struct perf_event_attr pe_uc[NUM_COUNTERS_UC];

    num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    fds = (int *)malloc(NUM_COUNTERS*num_cores*sizeof(int));

    for (int i = 0; i < NUM_COUNTERS; i++) {
        memset(&pe[i], 0, sizeof(struct perf_event_attr));
        pe[i].type = PERF_TYPE_RAW;
        pe[i].size = sizeof(struct perf_event_attr);
        pe[i].config = configs[i];
        pe[i].disabled = 1;
        pe[i].read_format = 0;
        pe[i].inherit = 1;
        pe[i].inherit_stat = 1;
        pe[i].exclude_kernel = 1;
        pe[i].exclude_hv = 1;
        for (int j = 0; j < num_cores; j++) {
            fds[i*num_cores+j] = syscall(__NR_perf_event_open, &pe[i], -1, j, -1, PERF_FLAG_FD_CLOEXEC);
            if (fds[i*num_cores+j] == -1) {
                fprintf(stderr, "Error opening counter %s\n", counter_names[i]);
            }
        }
    }
    
    fds_uc = (int *)malloc(NUM_COUNTERS_UC*SOCKETS*CHANNELS*sizeof(int));
    for (int i = 0; i < NUM_COUNTERS_UC; i++) {
        memset(&pe_uc[i], 0, sizeof(struct perf_event_attr));
        pe_uc[i].type = PERF_TYPE_RAW;
        pe_uc[i].size = sizeof(struct perf_event_attr);
        pe_uc[i].config = configs_uc[i];
        pe_uc[i].disabled = 1;
        pe_uc[i].read_format = 0;
        pe_uc[i].inherit = 1;
        pe_uc[i].inherit_stat = 0;
        pe_uc[i].exclude_kernel = 0;
        pe_uc[i].exclude_hv = 0;
        for (int j = 0; j < SOCKETS; j++) {
            for (int k = 0; k < CHANNELS; k++) {
                pe_uc[i].type = 13 + k;
                //pe_uc[i].config |= (13ll+k) << 32ll;
                fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k] = syscall(__NR_perf_event_open, &pe_uc[i], -1, j, -1, PERF_FLAG_FD_CLOEXEC);
                //printf("%d,%d,%d,  %#x, FD: %d\n",i,j,k, pe_uc[i].config, fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k]);
                if (fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k] == -1) {
                    fprintf(stderr, "Error opening counter %s\n", counter_names_uc[i]);
                }
            }
        }
    }


    // Start counting
    for (int i = 0; i < NUM_COUNTERS; i++) {
        for (int j = 0; j < num_cores; j++) {
            ioctl(fds[i*num_cores+j], PERF_EVENT_IOC_RESET, 0);
            ioctl(fds[i*num_cores+j], PERF_EVENT_IOC_ENABLE, 0);
            // long long count;
            // read(fds[i*num_cores+j], &count, sizeof(long long));
            // printf("COUNTER %d CORE %d RAW %lld\n",i, j, count);
        }
    }
    for (int i = 0; i < NUM_COUNTERS_UC; i++) {
        for (int j = 0; j < SOCKETS; j++) {
            for (int k = 0; k < CHANNELS; k++) {
                ioctl(fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k], PERF_EVENT_IOC_RESET, 0);
                ioctl(fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k], PERF_EVENT_IOC_ENABLE, 0);
                //printf("%d,%d,%d,  %#x, FD: %d\n",i,j,k, pe_uc[i].config, fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k]);
            }
        }
    }
    
    //only redirect signals here to ensure setups are done
    redirect_signals();
    //printf("--- Before main ---\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
}
void collect_counters_main() {
    if(clean_up_done){
        return;
    }
    clean_up_done++;
    if(skip_flag) {
        return;
    }
    if(fd < 0) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        if (check_MPI() && !signal_received) {
            double values[NUM_COUNTERS] = {0.0};
            double values_uc[NUM_COUNTERS_UC] = {0.0};
            double elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
            reduce_result(argv_o, values, values_uc, elapsed_time);
        }
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    //printf("--- After main ----\n");

    // Disable counters and read results
    double values[NUM_COUNTERS];
    for (int i = 0; i < NUM_COUNTERS; i++) {
        values[i] = 0;
        for (int j = 0; j < num_cores; j++) {
            ioctl(fds[i*num_cores+j], PERF_EVENT_IOC_DISABLE, 0);
            long long count;
            read(fds[i*num_cores+j], &count, sizeof(long long));
            values[i] += count;
            //printf("COUNTER %d CORE %d RAW %lld\n",i, j, count);
            close(fds[i*num_cores+j]);
        }
    }
    free(fds);

    double values_uc[NUM_COUNTERS_UC * SOCKETS];
    for (int i = 0; i < NUM_COUNTERS_UC; i++) {
        for (int j = 0; j < SOCKETS; j++) {
            values_uc[i*SOCKETS+j] = 0;
            for (int k = 0; k < CHANNELS; k++) {
                ioctl(fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k], PERF_EVENT_IOC_DISABLE, 0);
                long long count;
                read(fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k], &count, sizeof(long long));
                values_uc[i*SOCKETS+j] += count;
                //printf("%d,%d,%d,  %lld to %f, FD: %d\n",i,j,k, count, values_uc[i], fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k]);
                //printf("SOCKETS %d CHANNELS %d  RAW %lld\n",j, k, count);
                close(fds_uc[i*SOCKETS*CHANNELS+j*CHANNELS+k]);
            }
        }
    }
    free(fds_uc);

    double elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    // remove lock file
    close(fd);
    //printf("%s\n", lock_file_name);
    unlink(lock_file_name);
    free(lock_file_name);

    if (check_MPI() && !signal_received) {
        reduce_result(argv_o, values, values_uc, elapsed_time);
    } else {
        print_result(argv_o, values, values_uc, elapsed_time, 1);
    }
    free(argv_o);
}

