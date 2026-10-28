#ifndef PEAK_MACH_H
#define PEAK_MACH_H
#include <stdint.h>
typedef unsigned int thread_act_t;
typedef thread_act_t* thread_act_array_t;
typedef unsigned int mach_msg_type_number_t;
typedef int kern_return_t;
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef unsigned int* thread_state_t;
#define MACH_PORT_NULL 0
#define MACH_PORT_VALID(x) ((x) != 0)
#define KERN_SUCCESS 0
#define KERN_INVALID_ARGUMENT 4
unsigned int mach_task_self(void);
thread_act_t mach_thread_self(void);
kern_return_t task_threads(unsigned int, thread_act_array_t*, mach_msg_type_number_t*);
kern_return_t thread_suspend(thread_act_t);
kern_return_t thread_resume(thread_act_t);
kern_return_t thread_get_state(thread_act_t, int, thread_state_t, mach_msg_type_number_t*);
kern_return_t mach_port_deallocate(unsigned int, unsigned int);
kern_return_t vm_deallocate(unsigned int, vm_address_t, vm_size_t);
#endif /* PEAK_MACH_H */
