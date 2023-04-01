#!/bin/bash 

# simplest dlsym based overload for dgemm
gcc  -g -fPIC -shared   mydgemm.c -o my.so 

EXE=test_dgemm
#dgemm test code
ifort -qopenmp -O2 -g -mkl=sequential -o $EXE test_dgemm.F

gcc -shared -fPIC -ffunction-sections -fdata-sections frida-gum-example.c -o frida-hook.so -L. -lfrida-gum -ldl -lrt -lresolv -lm -pthread -static-libgcc -Wl,-z,noexecstack,--gc-sections
