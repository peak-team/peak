#include <string.h>

typedef struct STAT{
   int count; 
   double time;
   char* fgroup; 
   char comment[20];
} STAT;

struct item {
    char* key;
    STAT value;
    struct item* next;
};


unsigned long hash(char* str);
struct item* hash_insert(char* key);
struct item* hash_get(char* key);
void hash_show();
