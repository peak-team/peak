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
LD_PRELOAD=$LD_PRELOAD_PATH mpirun -N 2 ./test_mpi --random --verbose > mpi.log