#ifdef LOCAL_SYMBOL
static
#elif !defined(EXPORTED_SYMBOL)
__attribute__((visibility("hidden")))
#endif
__attribute__((noinline,used,patchable_function_entry(32,0)))
float cblas_sdot(int n,const float* x,int a,const float* y,int b) {
    return n+x[0]+a+y[0]+b;
}
void* fixture_target(void) {return cblas_sdot;}
