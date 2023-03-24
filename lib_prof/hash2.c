
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h> 

#include "hash2.h"
#include "global.h"
#include "cstr-utils.h"

#define HASH_SIZE 100

#define OUTFILE stdout


struct item2* hashtable2[HASH_SIZE];
static int hash2_count=0;

unsigned long hash2(char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_SIZE;
}

struct item2* hash2_insert(char* key) {
    unsigned long index = hash2(key);
    struct item2* new_item2 = (struct item2*) malloc(sizeof(struct item2));
    strcpy(new_item2->key, key);
    new_item2->value.time_in =  0.0; 
    new_item2->value.count = 0;
    new_item2->next = hashtable2[index];
    memset(new_item2->value.distribution_count,0,20*sizeof(int));
    memset(new_item2->value.distribution_time,0,20*sizeof(double));
    memset(new_item2->value.distribution_sizesum,0,20*sizeof(double));
    memset(new_item2->value.distribution_sizesumsq,0,20*sizeof(double));
    hashtable2[index] = new_item2;
    hash2_count++;
    return new_item2;
}

struct item2* hash2_get(char* key) {
    unsigned long index = hash2(key);
    struct item2* item2 = hashtable2[index];
    while (item2 != NULL) {
        if (strcmp(item2->key, key) == 0)
            return item2;
        item2 = item2->next;
    } 
    return NULL;
}

int hash2_get_size(){ return hash2_count;}



struct item_lite { 
     char key[400]; 
     double time_in;
};

struct item_lite* hash2_to_array(){
    int fn=hash2_get_size();
    struct item_lite* harray=malloc(fn*sizeof(struct item_lite));

    int fi=0;
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item2* item2 = hashtable2[i];
        while (item2 != NULL) {
            strcpy(harray[fi].key,item2->key);
            harray[fi].time_in=item2->value.time_in;
            fi++;
            item2 = item2->next;
        }   
    }   
    return harray;
}


int compare2_time_in(const void *a, const void *b) {
    double va = ((struct item_lite*)a)->time_in;
    double vb = ((struct item_lite*)b)->time_in;
    return (va > vb) ? -1 : (va < vb) ? 1 : 0;
}



void hash2_show() {
    fprintf(OUTFILE,"---------- PEAK Prof: dgemm call path statistics ----------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item2* item2 = hashtable2[i];
        while (item2 != NULL) {
            fprintf(OUTFILE,"call path %s: count: %d, time: %.3f\n", item2->key, item2->value.count, item2->value.time_in);
            for (int j=0;j<20;j++) {
                if(item2->value.distribution_count[j]>0)
                fprintf(OUTFILE, "   size: 10^{%d}~10^{%d} , count: %d, time %.3f\n", \
                                 j,j+1, item2->value.distribution_count[j],item2->value.distribution_time[j]);
            }
            item2 = item2->next;
        }   
    }   
    fprintf(OUTFILE,"----------------------------------------------------\n\n");
    fflush(OUTFILE);
}

void hash2_show_sorted() {
    int fn = hash2_get_size();
    struct item_lite* farray=hash2_to_array();
    qsort(farray, fn, sizeof(struct item_lite), compare2_time_in);
    struct item2* item2; 
    double avg, stdev, sum, sumsq; 
    int count;

    if(record_f) { 
      for(int f=0;*(record_f+f);f++)
      {
        int itmp=0;
        for (int i = 0; i < fn; i++) {
           item2 = hash2_get(farray[i].key);
          // printf("-----debug base: %s  record_f: %s %d\n", strbase(item2->key), record_f[f], f);
          //printf("-----debug %d\n",strcmp(record_f[f],strbase(item2->key)));
           if(strcmp(record_f[f],strbase(item2->key))==0) {
             if(itmp++==0)  {
                   fprintf(OUTFILE,"\n                %10s call path statistics               \n",record_f[f]);
                   fprintf(OUTFILE,"--------------------------------------------------------------\n");
             }
             fprintf(OUTFILE,"%d)  %-50s\n",itmp, item2->key);
             fprintf(OUTFILE,"    -------------------------------------------------------\n");
             fprintf(OUTFILE, "    log10(N) |  avg(N)  | stdev(N) |   count   |    time     \n"); 
             fprintf(OUTFILE,"    -------------------------------------------------------\n");
             for (int j=0;j<20;j++) {
                if(item2->value.distribution_count[j]>0) {
                  count=item2->value.distribution_count[j];
                  sum=item2->value.distribution_sizesum[j];
                  sumsq=item2->value.distribution_sizesumsq[j];
                  avg=sum/(float)count;
                  stdev=sqrt(abs(sumsq/(float)count-avg*avg));
                //  printf("---debug: count=%d  sum=%8.3f  sumsq=%8.3f   avg=%8.3f  stdev=%8.2f\n", count, sum, sumsq, avg,stdev);
                  fprintf(OUTFILE, "       %2d~%-2d | %8.1f | %8.1f | %9d | %10.3f  \n", \
                     j,j+1,avg,stdev,count,item2->value.distribution_time[j]);
                }
             }
             fprintf(OUTFILE,"    -------------------------------------------------------\n");
             fprintf(OUTFILE,"    %31s  %9d   %10.3f\n", "total:", item2->value.count, item2->value.time_in);
             fprintf(OUTFILE,"    -------------------------------------------------------\n");
           }
        }   
      }   
    fprintf(OUTFILE,"--------------------------------------------------------------\n\n");
    }
    fflush(OUTFILE);
}

/*
int main() {
    hash2_insert("key1", 1,0.1);
    hash2_insert("key2", 2,0.3);
    hash2_insert("key3", 5,0.9);
    struct item2* item2 = hash2_get("key1");
    fprintf(OUTFILE,"%d %.3f\n", item2->value.count,item2->value.time_in);
    item = hash2_get("key2");
    fprintf(OUTFILE,"%d %.3f\n", item2->value.count,item2->value.time_in);
    hash2_show();

    return 0;
}
*/

