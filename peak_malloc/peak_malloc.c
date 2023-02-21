#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#define MMAP_FILE_PREFIX "/peak_malloc_"
#define MMAP_FILE_PATH_ENV "PEAK_MALLOC_PATH"

static void * (*original_malloc)(size_t) = NULL;
void (*libc_free)(void*) = NULL;

char* mmap_file_path_str = NULL;
char mmap_file_suffix[1024];

typedef struct {
    unsigned int time_low;
    unsigned short time_mid;
    unsigned short time_hi_and_version;
    unsigned char clock_seq_hi_and_reserved;
    unsigned char clock_seq_low;
    unsigned char node[6];
} uuid_t;

// Declare a static mutex to ensure thread safety
static pthread_mutex_t uuid_mutex = PTHREAD_MUTEX_INITIALIZER;

void generate_uuid(char* uuid_str) {
    uuid_t uuid;

    // Generate a random node ID
    srand(time(NULL));
    for (int i = 0; i < 6; i++) {
        uuid.node[i] = rand() % 256;
    }

    // Set the version number to 4
    uuid.time_hi_and_version = (uuid.time_hi_and_version & 0x0FFF) | 0x4000;

    // Set the clock sequence
    uuid.clock_seq_hi_and_reserved = (uuid.clock_seq_hi_and_reserved & 0x3F) | 0x80;

    // Generate a timestamp
    time_t current_time = time(NULL);
    unsigned long long timestamp = current_time * 10000000ULL + 0x01B21DD213814000ULL;

    // Fill in the UUID fields
    uuid.time_low = (unsigned int)timestamp;
    uuid.time_mid = (unsigned short)(timestamp >> 32);
    uuid.time_hi_and_version = (unsigned short)(timestamp >> 48);
    uuid.clock_seq_low = rand() % 256;

    // Format the UUID as a string
    sprintf(uuid_str, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            uuid.time_low, uuid.time_mid, uuid.time_hi_and_version,
            uuid.clock_seq_hi_and_reserved, uuid.clock_seq_low,
            uuid.node[0], uuid.node[1], uuid.node[2], uuid.node[3], uuid.node[4], uuid.node[5]);
}

void *malloc(size_t size) {
    if (!original_malloc) {
        original_malloc = dlsym(RTLD_NEXT, "malloc");
    }
    if (!mmap_file_path_str) {
        mmap_file_path_str = getenv(MMAP_FILE_PATH_ENV);
        if (!mmap_file_path_str) {
            mmap_file_path_str = mmap_file_suffix;
            strcat(mmap_file_suffix, "/dev/shm");
            strcat(mmap_file_suffix, MMAP_FILE_PREFIX);
        } else {
            strcat(mmap_file_suffix, mmap_file_path_str);
            strcat(mmap_file_suffix, MMAP_FILE_PREFIX);
        }
    }
    
    // use original malloc if memory is sufficient
    void *ptr = original_malloc(size + sizeof(char));
    if (ptr) {
        char* mmap_flag_ptr = (char*) ptr;
        *mmap_flag_ptr = 24;
        return ptr + sizeof(char);
    }
    
    // otherwise, use mmap to allocate
    char uuid_str[37];
    generate_uuid(uuid_str);
    char mmap_file_str[1024];
    strcpy(mmap_file_str, mmap_file_suffix);
    strcat(mmap_file_str, uuid_str);

    int fd = open(mmap_file_str, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    ftruncate(fd, size + sizeof(size_t) + 1024 + sizeof(char));
    ptr = mmap(NULL, size + sizeof(size_t) + 1024 + sizeof(char), PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    size_t* size_ptr = (size_t*) ptr;
    *size_ptr = size;
    char* file_name_ptr = (char*) (ptr + sizeof(size_t));
    strncpy(file_name_ptr, mmap_file_str, 1024);
    char* mmap_flag_ptr = (char*) (ptr + sizeof(size_t) + 1024);
    *mmap_flag_ptr = 42;
    return ptr + sizeof(size_t) + 1024 + sizeof(char);
    //printf("Allocated %zu bytes of memory at address %p\n", size, ptr);
}

void free(void *ptr)
{
    if (!libc_free) {
        libc_free = dlsym(RTLD_NEXT, "free");
    }
    if (ptr) {
        char* mmap_flag_ptr = (char*) (ptr - sizeof(char));
        if (*mmap_flag_ptr == 24) {
            libc_free(mmap_flag_ptr);
        } else if (*mmap_flag_ptr == 42) {
            char* file_name_ptr = (char*) (ptr - sizeof(char) - 1024);
            size_t* size_ptr = (size_t*) (ptr - sizeof(size_t) - 1024 - sizeof(char));
            size_t size = *size_ptr;
            munmap((void*) size_ptr, size + sizeof(size_t) + 1024);
            remove(file_name_ptr);
        } else {
            libc_free(ptr);
        }
    }
    //printf("free\n");
}

