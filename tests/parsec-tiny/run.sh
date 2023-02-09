#!/bin/bash

date
ml intel/22.3.0 intel22/impi/22.3.0
module li


jobid=$SLURM_JOB_ID

#touch id-$jobid 
#scontrol show hostname $SLURM_NODELIST >>id-$jobid

#socket=`lscpu|grep Socket|awk '{print $NF}'`
#core_per_socket=`lscpu|grep "Core(s) per socket"|awk '{print $NF}'`
 core_per_node=$((socket*core_per_socket))
node=$SLURM_JOB_NUM_NODES 
total_core=$((node * core_per_node))


export OMP_NUM_THREADS=1
rank_per_node=56
rank=$((rank_per_node * node))

#source ~cazes/texascale_settings.sh

EXE=/scratch1/07893/junjieli/lccf/evaluation/parsec-debug/parsec.x


echo "running with node=" $node, " rank=" $rank, "thread_per_rank=" $OMP_NUM_THREADS , "total_cpu_core=" $total_core

#mpirun -n $node -ppn 1 [[ -d /tmp/log_files_dir ]] || mkdir /tmp/log_files_dir
mpirun -n $node -ppn 1  mkdir /tmp/log_files_dir

export I_MPI_PIN_DOMAIN=omp
#mpirun -n $rank -ppn $rank_per_node $EXE
 mpirun -ppn $rank_per_node -configfile ./mpmd.txt

date
