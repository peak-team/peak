
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "hash.h"

#define HASH_SIZE 200

#define OUTFILE stdout


struct item* hashtable[HASH_SIZE];
static int hash_count=0;

unsigned long hash(char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_SIZE;
}

struct item* hash_insert(char* key) {
    unsigned long index = hash(key);
    struct item* new_item = (struct item*) malloc(sizeof(struct item));
//    new_item->key = strdup(key);
    strcpy(new_item->key, key);
    new_item->value.time_di =  0.0; 
    new_item->value.time_in =  0.0; 
    new_item->value.time_ex =  0.0; 
    new_item->value.count = 0;
    new_item->value.count_di = 0;
    new_item->next = hashtable[index];
    hashtable[index] = new_item;
    hash_count++;
    return new_item;
}

struct item* hash_get(char* key) {
    unsigned long index = hash(key);
    struct item* item = hashtable[index];
    while (item != NULL) {
        if (strcmp(item->key, key) == 0)
            return item;
        item = item->next;
    }   
    return NULL;
}

int hash_get_size(){ return hash_count;}

void hash_show() {
    fprintf(OUTFILE,"---------- PEAK Prof: function statistics (inclusive) ----------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_in);
            item = item->next;
        }   
    }   
    fprintf(OUTFILE,"----------------------------------------------------\n");
    fflush(OUTFILE);
}

void hash_show_final() {
    
    double time_sum;

    time_sum=0.0;
    fprintf(OUTFILE,"\n----------------------  function statistics (direct) --------------------\n");
    fprintf(OUTFILE,"    direct call time (in seconds) and counts\n");
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            if(item->value.count_di>0) 
            { 
              fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count_di, item->value.time_di);
              time_sum+=item->value.time_di;
            }
            item = item->next;
        }   
    }   
    fprintf(OUTFILE,"%62s %10.3f\n","total library time:", time_sum);
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");



    time_sum=0.0;
    fprintf(OUTFILE,"\n-------------------  function statistics (exclusive) --------------------\n");
    fprintf(OUTFILE,"    exclusive call time (in seconds) and counts\n");
//  fprintf(OUTFILE,"    (call time and counts spent in a function exclusing its children functions)\n");
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_ex);
            time_sum+=item->value.time_ex;
            item = item->next;
        }   
    }   
    fprintf(OUTFILE,"%62s %10.3f\n","total library time:", time_sum);
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");


    time_sum=0.0;
    fprintf(OUTFILE,"\n-------------------  function statistics (inclusive) --------------------\n");
    fprintf(OUTFILE,"    inclusive call time (in seconds) and counts\n");
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_in);
            item = item->next;
        }   
    }   
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n\n");


    fflush(OUTFILE);
}


struct item* hash_to_array(){
    int fn=hash_get_size();
    struct item* harray=malloc(fn*sizeof(struct item));

    int fi=0;
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            strcpy(harray[fi].key,item->key);
            harray[fi].value.count_di=item->value.count_di;
            harray[fi].value.count=item->value.count;
            harray[fi].value.time_di=item->value.time_di;
            harray[fi].value.time_in=item->value.time_in;
            harray[fi].value.time_ex=item->value.time_ex;
            strcpy(harray[fi].value.fgroup,item->value.fgroup);
            strcpy(harray[fi].value.comment,item->value.comment);
            harray[fi].next=NULL;
            fi++;
            item = item->next;
        }   
    }   

    return harray;
}


/*
int main() {
    hash_insert("key1", 1,0.1);
    hash_insert("key2", 2,0.3);
    hash_insert("key3", 5,0.9);
    struct item* item = hash_get("key1");
    fprintf(OUTFILE,"%d %.3f\n", item->value.count,item->value.time);
    item = hash_get("key2");
    fprintf(OUTFILE,"%d %.3f\n", item->value.count,item->value.time);
    hash_show();

    return 0;
}
*/



