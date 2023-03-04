#include <string.h>

typedef struct STAT{
   int count_di; 
   int count;
   double time_di; //direct call time without counting children calls
   double time_in; //inclusive time
   double time_ex; //exclusive time
   char fgroup[20]; 
   char comment[20];
} STAT;

struct item {
    char key[40];
    STAT value;
    struct item* next;
};


unsigned long hash(char* str);
struct item* hash_insert(char* key);
struct item* hash_get(char* key);
int hash_get_size();
void hash_show();
void hash_show_final();
int hash_size;

struct item* hash_to_array();
int compare_time_di(const void *a, const void *b);
int compare_time_ex(const void *a, const void *b);
int compare_time_in(const void *a, const void *b);
