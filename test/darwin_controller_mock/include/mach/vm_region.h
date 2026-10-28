#ifndef PEAK_VM_REGION_H
#define PEAK_VM_REGION_H
#include <mach/mach.h>
typedef unsigned int natural_t;
typedef size_t mach_vm_size_t;
typedef struct { int protection; int is_submap; } vm_region_submap_info_data_64_t;
typedef int* vm_region_recurse_info_t;
#define VM_REGION_SUBMAP_INFO_COUNT_64 2
kern_return_t mach_vm_region_recurse(unsigned int, mach_vm_address_t*, mach_vm_size_t*, natural_t*, vm_region_recurse_info_t, mach_msg_type_number_t*);
#endif /* PEAK_VM_REGION_H */
