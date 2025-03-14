rm ompmpi.log test_ompmpi
mpicxx -fopenmp -o test_ompmpi test_ompmpi.cu -L/home1/apps/nvidia/Linux_aarch64/24.7/cuda/lib64 -lcudart -I/home1/apps/nvidia/Linux_aarch64/24.7/cuda/lib64/include -fPIC --std=c++17
LD_PRELOAD=../../build/src/libpeak.so mpirun -N 2 ./test_ompmpi --verbose --random > ompmpi.log