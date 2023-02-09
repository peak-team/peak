  struct item* item; 
  char str[10];

  double local_time=0.0;

#ifdef _OPENMP
//  omp_set_lock(&lock);
#pragma omp master 
{
#endif

  if(!peakprof_init_flag) lib_init(); 
  if (peakprof_debug>0) fprintf(stdout,"PEAKPROF: calling %s\n",__func__);

//  memset(layer_time, 0, (MAX_LAYER*sizeof(layer_time[0]));
  for (int i=layer_count+1;i<MAX_LAYER;i++) layer_time[i]=0.0;
  if (layer_count==0) layer_time[0]=0.0;
  layer_count++;

  strcpy(str,__func__);
  item = hash_get(str); 
  if (item == NULL ) {
    item = hash_insert(str);
//    strcpy(item->value.fgroup,func_group);
    item->value.fgroup=func_group;
  }
#ifdef _OPENMP
//  omp_unset_lock(&lock);
 }
#endif

  orig_f = dlsym(RTLD_NEXT, __func__);

#ifdef _OPENMP
#pragma omp master 
{
#endif
  local_time=mysecond();
//  item->value.time -= mysecond(); 

// BLAS is called here
