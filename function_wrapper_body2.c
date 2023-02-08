// BLAS is called here
//

  time=mysecond()-time;
#pragma omp atomic
  item->value.time += time;

#pragma omp atomic
  item->value.count ++;

//  printf("\n------------ in %s wrapper -----------\n",__func__);
//  printf ("%s time = %.6f seconds, count = %d\n",__func__,item->value.time, item->value.count);
//  printf("------------------------------------------\n\n");

 if (simpleperf_debug>1) hash_show();

 


