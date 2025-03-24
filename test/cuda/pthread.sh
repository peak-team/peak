#!/bin/bash

# Define variables
LD_PRELOAD_PATH="../../build/src/libpeak.so"

# Remove old files
rm pthread.log test_pthread

# Compile the CUDA pthread program
nvcc -o test_pthread test_pthread.cu -Xcompiler -pthread

# Run the pthread program with LD_PRELOAD
export PEAK_TARGET=_Z7kernel1mib,_Z7kernel2mib,_Z7kernel3mib,_Z7kernel4mib,_Z7kernel5mib
LD_PRELOAD=$LD_PRELOAD_PATH ./test_pthread --verbose --random > pthread.log