rm test_mpi mpi.log
mpicxx -o test_mpi test_mpi.cu -L/home1/apps/nvidia/Linux_aarch64/24.7/cuda/lib64 -lcudart -I/home1/apps/nvidia/Linux_aarch64/24.7/cuda/lib64/include -fPIC --std=c++17
LD_PRELOAD=../../build/src/libpeak.so mpirun -N 2 ./test_mpi --random --verbose > mpi.log