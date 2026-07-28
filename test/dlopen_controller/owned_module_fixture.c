/*
 * A deliberately small provider used by the dynamic-loader ownership tests.
 *
 * The callback is weak so the same file may also be loaded into an isolated
 * dlmopen namespace, where symbols exported by the main executable are not
 * necessarily visible.
 */
extern void peak_dlopen_owned_fixture_event(int loaded)
    __attribute__((weak));
extern void peak_dlopen_owned_fixture_destructor_loader(void)
    __attribute__((weak));

__attribute__((constructor))
static void
peak_dlopen_owned_fixture_loaded(void)
{
    if (peak_dlopen_owned_fixture_event != 0) {
        peak_dlopen_owned_fixture_event(1);
    }
}

__attribute__((destructor))
static void
peak_dlopen_owned_fixture_unloaded(void)
{
    if (peak_dlopen_owned_fixture_event != 0) {
        peak_dlopen_owned_fixture_event(0);
    }
    if (peak_dlopen_owned_fixture_destructor_loader != 0) {
        peak_dlopen_owned_fixture_destructor_loader();
    }
}

__attribute__((visibility("default"), noinline))
int
peak_dlopen_owned_fixture_value(void)
{
    return 42;
}
