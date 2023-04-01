#!/bin/bash 
EXE=test_dgemm

echo "raw timing:"
time $EXE >output
echo ""
echo "frida-hook.so timing:"
time LD_PRELOAD=./frida-hook.so $EXE >>output
echo ""
echo "clean dlsym overload timing:"
time LD_PRELOAD=./my.so $EXE >>output