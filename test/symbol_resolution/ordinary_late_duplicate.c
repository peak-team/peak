static __attribute__((noinline,used,patchable_function_entry(32,0)))
float cblas_sdot(int n,const float* x,int a,const float* y,int b) {
    return 90+n+x[0]+a+y[0]+b;
}
void* duplicate_target(void) {return cblas_sdot;}
