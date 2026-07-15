extern void peak_test_intentionally_missing_lazy_relocation(void);

__attribute__((visibility("default"), noinline))
void peak_test_call_missing_lazy_relocation(void)
{
    peak_test_intentionally_missing_lazy_relocation();
}
