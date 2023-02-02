  struct item* item; 
  char str[10];



  if(!simpleperf_init_flag) blas_init(); 

  if (simpleperf_debug>0) printf("SIMPLEPERF: calling %s\n",__func__);

  strcpy(str,__func__);
  item = hash_get(str); 
#ifdef _OPENMP
  omp_set_lock(&lock);
#endif
  if (item == NULL ) {
    item = hash_insert(str);
    strcpy(item->value.fgroup,func_group);
  }
#ifdef _OPENMP
  omp_unset_lock(&lock);
#endif

  orig_f = dlsym(RTLD_NEXT, __func__);

#ifdef _OPENMP
#pragma omp atomic
#endif
  item->value.time -= mysecond(); 

// BLAS is called here
