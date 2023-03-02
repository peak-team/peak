# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## Supported Features
**Supported counters:**
1. single and double precision floating point operations as scalar, 128-bit vector, 256-bit vector, 512-bit vector: 
    FP_ARITH_INST_RETIRED_SCALAR_DOUBLE    
    FP_ARITH_INST_RETIRED_SCALAR_SINGLE        
    FP_ARITH_INST_RETIRED_128B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_128B_PACKED_SINGLE      
    FP_ARITH_INST_RETIRED_256B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_256B_PACKED_SINGLE      
    FP_ARITH_INST_RETIRED_512B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_512B_PACKED_SINGLE 
2. memory read and write bandwidth:    
    CAS_COUNT_RD     
    CAS_COUNT_WR  
    
**Supported libraries:**
1. BLAS/CBLAS
2. LAPACK (Fortran)
3. ScaLAPACK/PBLAS (Fortran) 

LAPACKE (C), FFTW on to-do list.


## Instructions :
[PEAK Prof Instructions](lib_prof/README.md)
## Example Output:
[PEAK Prof output for PARSEC](lib_prof/output.md)

