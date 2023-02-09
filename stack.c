#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>



// N will be the capacity of the Static Stack
#define N 1000
  
// // Initializing the top of the stack to be -1
int top = -1;
  
// // Initializing the stack using an array
int stack[N];
//


void push(double x){
    // Checking overflow state
    if(top == N-1)
        printf("Overflow State: can't add more elements into the stack\n");
    else{
        printf("Enter element to be pushed into the stack:%.3f\n ",x);
        top+=1;
        stack[top] = x;
    }
}

double pop(){
    // Checking underflow state
    if(top == -1)
        printf("Underflow State: Stack already empty, can't remove any element\n");
    else{
        double x = stack[top];
        printf("Popping %.3f out of the stack\n", x);
        top-=1;
        return x;
    }
    return 0.0;
}

double peek(){
    double x = stack[top];
    printf("%.3f is the top most element of the stack\n", x);
    return x;
}

bool isEmpty(){
    if(top == -1){
        printf("Stack is empty: Underflow State\n");
        return true;
    }
    printf("Stack is not empty\n");
    return false;
}

bool isFull(){
    if(top == N-1){
        printf("Stack is full: Overflow State\n");
        return true;
    }
    printf("Stack is not full\n");
    return false;
}

