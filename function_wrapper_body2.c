// BLAS is called here
//

#pragma omp atomic
  item->value.time += mysecond();

#pragma omp atomic
  item->value.count ++;

//  printf("\n------------ in %s wrapper -----------\n",__func__);
//  printf ("%s time = %.6f seconds, count = %d\n",__func__,item->value.time, item->value.count);
//  printf("------------------------------------------\n\n");

 if (libperf_debug>0) hash_show();

 


