    if(ifrecord){
      if(isize==0) isize=1;
      int log10_isize=(int)log10(isize);
      char callpath[400]="";
      struct item2* item2;
      strcat(callpath,layer_caller[0]);
      for(int j=1;j<=layer_count+1;j++) { 
         strcat(callpath,"->");
         strcat(callpath,layer_caller[j]);
      }
      item2 = hash2_get(callpath); 
      if (item2 == NULL )  item2 = hash2_insert(callpath);
      item2->value.time_in += local_time;
      item2->value.distribution_time[log10_isize] += local_time;
      item2->value.count ++;
      item2->value.distribution_count[log10_isize] ++;
      item2->value.distribution_sizesum[log10_isize] += (float)isize;
      item2->value.distribution_sizesumsq[log10_isize] += (float)isize*(float)isize;
    }

