
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "hash.h"

#define HASH_SIZE 100


struct item* hashtable[HASH_SIZE];


unsigned long hash(char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_SIZE;
}

struct item* hash_insert(char* key, int count, double time) {
    unsigned long index = hash(key);
    struct item* new_item = (struct item*) malloc(sizeof(struct item));
    new_item->key = strdup(key);
    new_item->value.time = time;
    new_item->value.count = count;
    new_item->next = hashtable[index];
    hashtable[index] = new_item;
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
    printf("---------- BLAS Perf: function statistics ----------\n");
    for (int i = 0; i < HASH_SIZE; i++) {
        struct item* item = hashtable[i];
        while (item != NULL) {
            printf("function: %8s, count: %7d, time: %10.3f\n", item->key, item->value.count, item->value.time);
            item = item->next;
        }   
    }   
    printf("----------------------------------------------------\n");
}

/*
int main() {
    hash_insert("key1", 1,0.1);
    hash_insert("key2", 2,0.3);
    hash_insert("key3", 5,0.9);
    struct item* item = hash_get("key1");
    printf("%d %.3f\n", item->value.count,item->value.time);
    item = hash_get("key2");
    printf("%d %.3f\n", item->value.count,item->value.time);
    hash_show();

    return 0;
}
*/



