rm pthread.log test_pthread
nvcc -o test_pthread test_pthread.cu -Xcompiler -pthread
LD_PRELOAD=../../build/src/libpeak.so ./test_pthread --verbose --random > pthread.log