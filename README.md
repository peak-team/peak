# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## To Compile:

```
mkdir build
cd build
cmake ..
make
``` 

## To Use: 

``LD_PRELOAD=peak.so  ./target_application_here`` 

## Settings
```
 PEAK_TARGET=dgemm_,dgemv_        # functions that will be profiled
 PEAK_COST=10                     # upperlimit of profiling cost. Detach when exceeding the limit.

```



