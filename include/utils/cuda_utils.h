#ifndef CUDA_UTILS_H
#define CUDA_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <cuda.h>
#include <cuda_runtime_api.h>

/**
 * Demangles a C++ mangled symbol name.
 *
 * @param mangled_name The mangled symbol (e.g., "_Z3fooi").
 * @return A newly allocated string with the demangled name.
 *         Caller must free the returned string using free().
 */
char* demangle(const char* mangled_name);

#ifdef __cplusplus
}
#endif

#endif // CUDA_UTILS_H