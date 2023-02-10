
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "hash.h"

#define HASH_SIZE 200


struct item* hashtable[HASH_SIZE];
int hash_size=0;

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
    new_item->key = strdup(key);
    new_item->value.time_di =  0.0; 
    new_item->value.time_in =  0.0; 
    new_item->value.time_ex =  0.0; 
    new_item->value.count = 0;
    new_item->value.count_di = 0;
    new_item->next = hashtable[index];
    hashtable[index] = new_item;
    hash_size++;
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

void hash_show() {
    fprintf(stdout,"---------- PEAK Prof: function statistics (inclusive) ----------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(stdout,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_in);
            item = item->next;
        }   
    }   
    fprintf(stdout,"----------------------------------------------------\n");
    fflush(stdout);
}

void hash_show_final() {
    
    double time_sum;


    time_sum=0.0;
    fprintf(stdout,"\n----------------------  function statistics (direct) --------------------\n");
    fprintf(stdout,"    time (in seconds) and counts of direct calls\n");
    fprintf(stdout,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            if(item->value.count_di>0) 
            { 
              fprintf(stdout,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count_di, item->value.time_di);
              time_sum+=item->value.time_di;
            }
            item = item->next;
        }   
    }   
    fprintf(stdout,"%62s %10.3f\n","total library time:", time_sum);
    fprintf(stdout,"-------------------------------------------------------------------------\n");



    time_sum=0.0;
    fprintf(stdout,"\n-------------------  function statistics (exclusive) --------------------\n");
    fprintf(stdout,"    exclusive time (in seconds) and counts\n");
    fprintf(stdout,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(stdout,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_ex);
            time_sum+=item->value.time_ex;
            item = item->next;
        }   
    }   
    fprintf(stdout,"%62s %10.3f\n","total library time:", time_sum);
    fprintf(stdout,"-------------------------------------------------------------------------\n");


    time_sum=0.0;
    fprintf(stdout,"\n-------------------  function statistics (inclusive) --------------------\n");
    fprintf(stdout,"    inclusive time (in seconds) and counts\n");
    fprintf(stdout,"-------------------------------------------------------------------------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            fprintf(stdout,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", item->value.fgroup, item->key, item->value.count, item->value.time_in);
            item = item->next;
        }   
    }   
    fprintf(stdout,"-------------------------------------------------------------------------\n\n");


    fflush(stdout);
}



/*
int main() {
    hash_insert("key1", 1,0.1);
    hash_insert("key2", 2,0.3);
    hash_insert("key3", 5,0.9);
    struct item* item = hash_get("key1");
    fprintf(stdout,"%d %.3f\n", item->value.count,item->value.time);
    item = hash_get("key2");
    fprintf(stdout,"%d %.3f\n", item->value.count,item->value.time);
    hash_show();

    return 0;
}
*/



