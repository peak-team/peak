#include "cxx_utils.h"
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

char* strip_arguments(const char* demangled) {
    char* copy = strdup(demangled);
    char* paren = strchr(copy, '(');
    if (paren) {
        *paren = '\0';  // Truncate at '('
    }
    return copy;
}

extern "C" char* demangle(const char* mangled) {
    int status = 0;
    char* result = abi::__cxa_demangle(mangled, 0, 0, &status);
    return (status == 0) ? strip_arguments(result) : strdup(mangled);
}
