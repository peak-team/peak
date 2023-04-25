#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int functionA(int num);

__attribute__((noinline)) int functionB(int num) {
 //  usleep(200000);
 // printf("B: %d \n", num);
  if (num > 1) {
    return functionA(num - 1);
  }
  return 0;
}

__attribute__((noinline)) int functionA(int num) {
 // usleep(200000);
 // printf("A: %d \n", num);
  if (num > 1) {
    return functionB(num - 1);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int input;
//  if(argc<2) {printf("error: no argument\n"); exit(0);}
//  input=atoi(argv[1]);
  input=100000;
  printf("input=%d\n",input);
  int a = functionA(input);
  printf("a=%d\n",a);
  return 0;
}