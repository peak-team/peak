#!/bin/bash
export SIMPLEPERF_DEBUG=0    
LD_PRELOAD=/scratch1/07893/junjieli/mytools/profiler_lite-exp/libsimpleperf.so 
#LD_PRELOAD=/scratch1/07893/junjieli/mytools/profiler_lite/libsimpleperf.so 
EXE=/scratch1/07893/junjieli/lccf/evaluation/parsec-debug/parsec.x >& stat.log
$EXE
