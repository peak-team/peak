__attribute__((visibility("default"), noinline))
int
peak_mpi_init_deferred_target(int value)
{
    volatile int result = value;

    /*
     * Keep a normal patchable body even under release optimization. The test
     * exercises post-MPI module discovery, not the small-prologue rejection
     * policy.
     */
    result += 1;
    result += 2;
    result += 3;
    result += 4;
    result -= 4;
    result -= 3;
    result -= 2;
    return result;
}
