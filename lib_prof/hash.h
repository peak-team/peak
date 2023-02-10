#include <string.h>

typedef struct STAT{
   int count_di; 
   int count;
   double time_di; //direct call time without counting children calls
   double time_in; //inclusive time
   double time_ex; //exclusive time
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
void hash_show_final();
int hash_size;
