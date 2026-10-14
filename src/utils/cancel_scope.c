#include "internal/cancel_scope.h"

__thread PeakCancelScope* peak_cancel_scope_current
    __attribute__((visibility("hidden")));
