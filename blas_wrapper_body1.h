  struct item* item; 
  char str[10];

  strcpy(str,__func__);

  if(!blasperf_init_flag) blas_init(); 
  item = hash_get(str); 
#ifdef _OPENMP
  omp_set_lock(&lock);
#endif
  if (item == NULL ) item = hash_insert(str, 0, 0.0);
#ifdef _OPENMP
  omp_unset_lock(&lock);
#endif

  orig_blas = dlsym(RTLD_NEXT, __func__);

#ifdef _OPENMP
#pragma omp atomic
#endif
  item->value.time -= mysecond(); 

// BLAS is called here
