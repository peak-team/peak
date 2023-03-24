#include <string.h>

typedef struct STAT2{
   int unsigned long count;
   double time_in; //inclusive time
   int    distribution_count[20];
   double distribution_time[20];
   double distribution_sizesum[20];
   double distribution_sizesumsq[20];
} STAT2;

struct item2 {
    char key[400];
    char key_base[40];
    STAT2 value;
    struct item2* next;
};


unsigned long hash2(char* str);
struct item2* hash2_insert(char* key);
struct item2* hash2_get(char* key);
int hash2_get_size();
void hash2_show();
void hash2_show_sorted();
int hash2_size;

//struct item2* hash2_to_array();
//int compare2_time_in(const void *a, const void *b);
