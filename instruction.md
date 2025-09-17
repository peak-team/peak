# Instructions for reproducing test results in the paper "PEAK: Cost-Adaptive Profiling in a Heartbeat"

All the tests were run on [Frontera](https://www.tacc.utexas.edu/systems/frontera).

1. Download and build PEAK 
    ```shell
    git clone https://github.com/peak-team/peak.git
    cd peak
    mkdir build
    cd build
    cmake --install-prefix=$HOME ..
    make
    ```

## Experiment 1: Reproducing Table 1

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_HEARTBEAT_INTERVAL=0
export PEAK_COST=<value_corresponding_to_Table1>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table1
```

## Experiment 2: Reproducing Table 2

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_HEARTBEAT_INTERVAL=0
export PEAK_COST=<value_corresponding_to_Table2>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table2
```

## Experiment 3: Reproducing Table 3

**Configuration:**
```shell
export PEAK_TARGET='my_sleep_func'
export PEAK_HEARTBEAT_INTERVAL=0
export PEAK_COST=<value_corresponding_to_Table3>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table3
```

## Experiment 4: Reproducing Table 4

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_COST=100   # set to a very large value to effectively disable detachment from here
export PEAK_HEARTBEAT_INTERVAL='0.00005'
export PEAK_HIBERNATION_CYCLE=1000
export PEAK_OVERHEAD_RATIO=<value_corresponding_to_Table4>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table4
```

## Experiment 5: Reproducing Table 5

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_COST=100   # set to a very large value to effectively disable detachment from here
export PEAK_HEARTBEAT_INTERVAL='0.1'
export PEAK_HIBERNATION_CYCLE=50
export PEAK_OVERHEAD_RATIO=<value_corresponding_to_Table5>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table5
```

## Experiment 6: Reproducing Table 6

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_COST=100   # set to a very large value to effectively disable detachment from here
export PEAK_HEARTBEAT_INTERVAL='0.1'
export PEAK_HIBERNATION_CYCLE=50
export PEAK_OVERHEAD_RATIO=<value_corresponding_to_Table6>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table6
```

## Experiment 7: Reproducing Table 7

**Configuration:**
```shell
export PEAK_TARGET='dgemm_'
export PEAK_COST=100   # set to a very large value to effectively disable detachment from here
export PEAK_HEARTBEAT_INTERVAL='0.2'
export PEAK_HIBERNATION_CYCLE=50
export PEAK_OVERHEAD_RATIO=<value_corresponding_to_Table7>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table7
```

## Experiment 8: Reproducing Table 8

**Configuration:**
```shell
export PEAK_TARGET='dgemm_wrapper'
export PEAK_COST=100   # set to a very large value to effectively disable detachment from here
export PEAK_HEARTBEAT_INTERVAL='0.00005'
export PEAK_HIBERNATION_CYCLE=1000
export PEAK_OVERHEAD_RATIO=<value_corresponding_to_Table8>
````

**Usage:**

```shell
LD_PRELOAD=/path/to/libpeak.so ../test/paper/test_table8_9
```