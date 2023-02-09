// BLAS is called here
//

  local_time=mysecond()-local_time;
//#pragma omp atomic
  if ( layer_count == 1) { 
     item->value.time_di += local_time;
     libtime+=local_time; 
     item->value.count_di++;
  }
  item->value.time_in += local_time;
  item->value.time_ex += local_time - layer_time[layer_count];
  layer_time[layer_count-1] += local_time;
  item->value.count ++;

//  printf("layer count %d\n", layer_count);
//  printf("local time %.3f, layer time %.3f\n",local_time, layer_time[layer_count]);
  layer_count--;

  if (peakprof_debug>1) hash_show();
  if (peakprof_debug>0) printf("PEAKPROF: done with %s\n",__func__);

#ifdef _OPENMP
}
#endif

 


