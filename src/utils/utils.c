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
