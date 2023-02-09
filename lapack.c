
#include "simpleperf.h"
#include "hash.h"
#include "stack.h"

#define func_group "LAPACK"





// LAPACK Single

#include "lapack_wrapper/single.c"


// LAPACK Double

#include "lapack_wrapper/double.c"

// LAPACK Complex
  
#include "lapack_wrapper/complex.c"

// LAPACK Complex*16

#include "lapack_wrapper/complex16.c"

