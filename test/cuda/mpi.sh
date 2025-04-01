#!/bin/bash

# Define variables
LD_PRELOAD_PATH="../../build/src/libpeak.so"
CUDA_LIB_PATH="/home1/apps/nvidia/Linux_aarch64/24.7/cuda/lib64"
CUDA_INCLUDE_PATH="$CUDA_LIB_PATH/include"

# Remove old files
rm test_mpi mpi.log

# Compile the MPI CUDA program
mpicxx -o test_mpi test_mpi.cu -L$CUDA_LIB_PATH -lcudart -I$CUDA_INCLUDE_PATH -fPIC --std=c++17

# Run the MPI program with LD_PRELOAD
export PEAK_TARGET=run_kernel
export PEAK_GPU_TARGET=kernel1,kernel2,kernel3,kernel4,kernel5
LD_PRELOAD=$LD_PRELOAD_PATH mpirun -np 2 ./test_mpi --num_calls 100 > mpi.log