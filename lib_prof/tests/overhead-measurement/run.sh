#!/bin/bash 

# simplest dlsym based overload for dgemm
gcc  -g -fPIC -shared   mydgemm.c -o my.so 

EXE=test_dgemm
#dgemm test code
ifort -qopenmp -O2 -g -mkl=sequential -o $EXE test_dgemm.F
echo "raw timing:"
time $EXE >output
echo "peak_libprof.so timing:"
time LD_PRELOAD=../../peak_libprof.so $EXE >output
echo "clean dlsym overload timing:"
time LD_PRELOAD=./my.so $EXE >output
