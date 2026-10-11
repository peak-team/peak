#define _GNU_SOURCE 1
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv) {
    assert(argc==3);void* h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(h);
    void*(*get)(void)=dlsym(h,"fixture_target");assert(get);
    if(strcmp(argv[2],"dynamic")) assert(!dlsym(h,"cblas_sdot"));
    float(*target)(int,const float*,int,const float*,int)=get();
    void*(*other_get)(void)=dlsym(h,"duplicate_target");
    float(*other)(int,const float*,int,const float*,int)=other_get?other_get():NULL;
    sleep(1);float x=1,y=2;
    for(int i=0;i<100;i++) {
        assert(target(1,&x,1,&y,1)==6);
        if(other) assert(other(1,&x,1,&y,1)==96);
    }
    printf("ORDINARY_LATE_ORIGINAL_PASS calls=100 duplicate=%d\n",other!=NULL);
    assert(!dlclose(h));return 0;
}
