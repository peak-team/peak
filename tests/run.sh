#!/bin/bash 


# export OMP_NUM_THREADS=128
# export OMP_PROC_BIND=spread

CC=icc
FC=ifort 
FLAG0="-O2 -g"
LIB=-qmkl 
FLAG="$FLAG0 $LIB"

COMPILER=""
EXE=""

#export BLASPERF_MKL_FAKE=1 # disabled for now
#export BLASPERF_DEBUG=1    # will print stats after every BLAS call

f_type(){
  file=$1
  ext=`echo $file|awk -F. '{print $NF}'`
  EXE=`echo $file|sed "s/.$ext//g"`
  case $ext in 
      c)
      COMPILER=$CC ;;
      C)
      COMPILER=$CC ;;
      f)
      COMPILER=$FC ;;
      f90)
      COMPILER=$FC ;;
      F90)
      COMPILER=$FC ;;
      F)
      COMPILER=$FC ;;
      *)
      echo "error in file type: $ext"; exit;;
  esac
}

for t in test_dgemm.F  test_symv.f90  test_ysr.f90 test_cblas_dgemm.c
do
  f_type $t
  cmd="$COMPILER $FLAG -o $EXE $t"
  $cmd
  LD_PRELOAD=../libblasperf.so  $EXE 
  rm $EXE
done
