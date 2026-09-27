#define _GNU_SOURCE
#include <frida-gum.h>
#include "internal/exec_raw_syscall.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#define PEAK_MAP_CHUNK_SIZE 4096
#define PEAK_MAP_LINE_SIZE 4096
#define PEAK_MAP_TOTAL_LIMIT 65536
#define PEAK_MAP_READ_LIMIT 64
#define PEAK_MAP_LINE_LIMIT 2048
#define PEAK_EINTR_LIMIT 8
/* Conservative low-address admission floor, not a kernel-policy guarantee.
 * Snapshot truncation/overlong lines return NULL and may reduce tiny-hook
 * coverage; protected failure never returns to the old heap-frontier hint. */
#define PEAK_MIN_MAPPING_ADDRESS ((uintptr_t)65536)

typedef struct {
    uintptr_t target;
    uintptr_t first;
    uintptr_t previous_end;
    gsize page_size;
    dev_t executable_device;
    ino_t executable_inode;
    unsigned int target_matches;
    unsigned int main_matches;
} PeakNearSnapshot;

static long
peak_near_syscall(long number, long a1, long a2, long a3, long a4)
{
    for (unsigned int attempt = 0; attempt < PEAK_EINTR_LIMIT; attempt++) {
        long result = peak_exec_raw_syscall6(number, a1, a2, a3, a4, 0, 0);
        if (result != -1 || errno != EINTR) {
            return result;
        }
    }
    return -1;
}

static void
peak_near_close(int fd)
{
    /* Linux releases the descriptor even when close reports EINTR. */
    (void)peak_exec_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}

static int
peak_near_parse_number(const char** cursor, unsigned int base, uint64_t* value)
{
    const char* p = *cursor;
    uint64_t result = 0;
    unsigned int digits = 0;
    for (;;) {
        unsigned int digit;
        if (*p >= '0' && *p <= '9') {
            digit = (unsigned int)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            digit = (unsigned int)(*p - 'a' + 10);
        } else if (*p >= 'A' && *p <= 'F') {
            digit = (unsigned int)(*p - 'A' + 10);
        } else {
            break;
        }
        if (digit >= base || result > (UINT64_MAX - digit) / base) {
            return 0;
        }
        result = result * base + digit;
        p++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *cursor = p;
    *value = result;
    return 1;
}

static int
peak_near_spaces(const char** cursor)
{
    if (**cursor != ' ') {
        return 0;
    }
    while (**cursor == ' ') {
        (*cursor)++;
    }
    return 1;
}

static int
peak_near_parse_map(const char* line, PeakNearSnapshot* snapshot)
{
    const char* p = line;
    uint64_t low, high, offset, device_major, device_minor, inode;
    if (!peak_near_parse_number(&p, 16, &low) || *p++ != '-' ||
        !peak_near_parse_number(&p, 16, &high) ||
        low >= high || low < snapshot->previous_end ||
        low % snapshot->page_size != 0 || high % snapshot->page_size != 0 ||
        !peak_near_spaces(&p)) {
        return 0;
    }
    if (strlen(p) < 5 || (p[0] != 'r' && p[0] != '-') ||
        (p[1] != 'w' && p[1] != '-') ||
        (p[2] != 'x' && p[2] != '-') ||
        (p[3] != 'p' && p[3] != 's')) {
        return 0;
    }
    int executable = p[2] == 'x';
    p += 4;
    if (!peak_near_spaces(&p) ||
        !peak_near_parse_number(&p, 16, &offset) ||
        offset % snapshot->page_size != 0 || !peak_near_spaces(&p) ||
        !peak_near_parse_number(&p, 16, &device_major) || *p++ != ':' ||
        !peak_near_parse_number(&p, 16, &device_minor) ||
        device_major > UINT_MAX || device_minor > UINT_MAX ||
        !peak_near_spaces(&p) || !peak_near_parse_number(&p, 10, &inode) ||
        (*p != '\0' && *p != ' ')) {
        return 0;
    }
    dev_t device = makedev((unsigned int)device_major,
                          (unsigned int)device_minor);
    if (major(device) != device_major || minor(device) != device_minor) {
        return 0;
    }
    if (snapshot->first == UINTPTR_MAX) {
        snapshot->first = (uintptr_t)low;
    }
    snapshot->previous_end = (uintptr_t)high;
    if (low <= snapshot->target && snapshot->target < high) {
        snapshot->target_matches++;
        if (executable && device == snapshot->executable_device &&
            inode == (uint64_t)snapshot->executable_inode) {
            snapshot->main_matches++;
        }
    }
    return 1;
}

static int
peak_near_read_maps(int fd, PeakNearSnapshot* snapshot)
{
    char chunk[PEAK_MAP_CHUNK_SIZE];
    char line[PEAK_MAP_LINE_SIZE];
    gsize total = 0, line_size = 0;
    unsigned int lines = 0;
    for (unsigned int calls = 0; calls < PEAK_MAP_READ_LIMIT; calls++) {
        gsize capacity = sizeof(chunk);
        if (total == PEAK_MAP_TOTAL_LIMIT) {
            capacity = 1; /* Require EOF instead of accepting a capped prefix. */
        } else if (capacity > PEAK_MAP_TOTAL_LIMIT - total) {
            capacity = PEAK_MAP_TOTAL_LIMIT - total;
        }
        long count = peak_near_syscall(SYS_read, fd, (long)chunk,
                                      (long)capacity, 0);
        if (count < 0) {
            return 0;
        }
        if (count == 0) {
            return line_size == 0 && snapshot->first != UINTPTR_MAX;
        }
        if ((gsize)count > PEAK_MAP_TOTAL_LIMIT - total) {
            return 0;
        }
        total += (gsize)count;
        for (long i = 0; i < count; i++) {
            if (chunk[i] == '\0') {
                return 0;
            }
            if (chunk[i] == '\n') {
                if (++lines > PEAK_MAP_LINE_LIMIT) {
                    return 0;
                }
                line[line_size] = '\0';
                if (!peak_near_parse_map(line, snapshot)) {
                    return 0;
                }
                line_size = 0;
            } else {
                if (line_size == sizeof(line) - 1) {
                    return 0;
                }
                line[line_size++] = chunk[i];
            }
        }
    }
    return 0;
}

/* 1 = confirmed ET_EXEC main text; 0 = confirmed unrelated/PIE; -1 = unknown.
 * Unknown classification never falls back to a possibly heap-adjacent hint. */
static int
peak_near_snapshot(uintptr_t target, gsize page_size, uintptr_t* first)
{
    Elf64_Ehdr header;
    struct stat before, after;
    int fd = (int)peak_near_syscall(SYS_openat, AT_FDCWD,
        (long)"/proc/self/exe", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int valid = peak_near_syscall(SYS_fstat, fd, (long)&before, 0, 0) == 0 &&
                S_ISREG(before.st_mode);
    gsize size = 0;
    for (unsigned int reads = 0; valid && size < sizeof(header) && reads < 16;
         reads++) {
        long count = peak_near_syscall(SYS_read, fd, (long)((char*)&header + size),
                                      (long)(sizeof(header) - size), 0);
        if (count <= 0) {
            valid = 0;
        } else {
            size += (gsize)count;
        }
    }
    valid = valid && size == sizeof(header) &&
        peak_near_syscall(SYS_fstat, fd, (long)&after, 0, 0) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_mtime == after.st_mtime &&
        before.st_ctime == after.st_ctime &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    peak_near_close(fd);
    if (!valid || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT ||
        header.e_machine != EM_X86_64 || header.e_ehsize != sizeof(header) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
        header.e_phnum == PN_XNUM) {
        return -1;
    }
    if (header.e_type == ET_DYN) {
        return 0;
    }
    if (header.e_type != ET_EXEC) {
        return -1;
    }
    PeakNearSnapshot snapshot = {
        .target = target, .first = UINTPTR_MAX, .page_size = page_size,
        .executable_device = before.st_dev, .executable_inode = before.st_ino
    };
    fd = (int)peak_near_syscall(SYS_openat, AT_FDCWD,
        (long)"/proc/self/maps", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    valid = peak_near_read_maps(fd, &snapshot);
    peak_near_close(fd);
    if (!valid || snapshot.target_matches != 1 || snapshot.main_matches > 1) {
        return -1;
    }
    *first = snapshot.first;
    return snapshot.main_matches == 1 ? 1 : 0;
}

static int
peak_near_layout(guint n_pages, gsize page_size, uintptr_t gap_end,
                 const GumAddressSpec* spec, uintptr_t* hint, gsize* size)
{
    if (n_pages == 0 || page_size == 0 ||
        (page_size & (page_size - 1)) != 0) {
        return 0;
    }
    gsize pages = (gsize)n_pages + 1;
    if (pages <= n_pages || pages > SIZE_MAX / page_size) {
        return 0;
    }
    *size = pages * page_size;
    uintptr_t end = gap_end & ~((uintptr_t)page_size - 1);
    if (end < *size || end - *size < PEAK_MIN_MAPPING_ADDRESS) {
        return 0;
    }
    *hint = end - *size;
    uintptr_t code = *hint + page_size;
    uintptr_t last = end - 1;
    uintptr_t target = (uintptr_t)spec->near_address;
    return (code > target ? code - target : target - code) <= spec->max_distance &&
           (last > target ? last - target : target - last) <= spec->max_distance;
}

static int
peak_near_accept(uintptr_t actual, uintptr_t hint, gsize size, gsize page_size,
                 uintptr_t gap_end, const GumAddressSpec* spec)
{
    if (actual != hint || actual < PEAK_MIN_MAPPING_ADDRESS ||
        actual > UINTPTR_MAX - size || actual + size > gap_end) {
        return 0;
    }
    uintptr_t target = (uintptr_t)spec->near_address;
    uintptr_t code = actual + page_size;
    uintptr_t last = actual + size - 1;
    return (code > target ? code - target : target - code) <= spec->max_distance &&
           (last > target ? last - target : target - last) <= spec->max_distance;
}

/* Only gumcodeallocator.c.o's undefined caller is routed here. The backend
 * definition/internal callers remain original. Ordinary hint mmap can briefly
 * return an unwanted heap-frontier mapping before rejection: this preference
 * is not a universal concurrent-brk safety guarantee. */
gpointer
peak_gum_try_alloc_n_pages_near_main_low_gap(guint n_pages, GumPageProtection prot,
                                          const GumAddressSpec* spec)
{
    if (spec == NULL) {
        return gum_try_alloc_n_pages_near(n_pages, prot, spec);
    }
    int saved_errno = errno;
    gsize page_size = gum_query_page_size();
    uintptr_t first, hint;
    gsize size;
    gpointer base = NULL;
    if (page_size == 0 || (page_size & (page_size - 1)) != 0) {
        goto out;
    }
    int classification = peak_near_snapshot((uintptr_t)spec->near_address,
                                             page_size, &first);
    if (classification == 0) {
        errno = saved_errno;
        return gum_try_alloc_n_pages_near(n_pages, prot, spec);
    }
    if (classification < 0 ||
        !peak_near_layout(n_pages, page_size, first, spec, &hint, &size)) {
        goto out;
    }
    base = gum_memory_allocate((gpointer)hint, size, page_size, GUM_PAGE_RW);
    if (base == NULL) {
        goto out;
    }
    if (!peak_near_accept((uintptr_t)base, hint, size, page_size, first, spec)) {
        gum_memory_free(base, size);
        base = NULL;
        goto out;
    }
    *((gsize*)base) = size;
    int native_prot = ((prot & GUM_PAGE_READ) ? PROT_READ : 0) |
                      ((prot & GUM_PAGE_WRITE) ? PROT_WRITE : 0) |
                      ((prot & GUM_PAGE_EXECUTE) ? PROT_EXEC : 0);
    if (peak_near_syscall(SYS_mprotect, (long)((char*)base + page_size),
                          (long)(size - page_size), native_prot, 0) != 0 ||
        peak_near_syscall(SYS_mprotect, (long)base, (long)page_size, PROT_READ, 0) != 0) {
        gum_memory_free(base, size);
        base = NULL;
        goto out;
    }
    base = (char*)base + page_size;
out:
    errno = saved_errno;
    return base;
}
