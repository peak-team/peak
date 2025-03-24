#!/bin/bash

# Define variables
LD_PRELOAD_PATH="../../build/src/libpeak.so"

# Remove old files
rm pthread.log test_pthread

# Compile the CUDA pthread program
nvcc -o test_pthread test_pthread.cu -Xcompiler -pthread

# Run the pthread program with LD_PRELOAD
LD_PRELOAD=$LD_PRELOAD_PATH ./test_pthread --verbose --random > pthread.log