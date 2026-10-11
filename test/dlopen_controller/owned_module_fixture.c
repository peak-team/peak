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

/* Isolated namespaces cannot resolve the executable's weak event callback.
 * Tests can explicitly register an observer while the application owns it. */
static void (*namespace_unload_observer)(unsigned int);
static unsigned int namespace_observer_id;

__attribute__((visibility("default"), noinline))
void
peak_dlopen_owned_fixture_set_unload_observer(void (*observer)(unsigned int),
                                              unsigned int id)
{
    namespace_unload_observer = observer;
    namespace_observer_id = id;
}

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
    if (namespace_unload_observer != 0) {
        namespace_unload_observer(namespace_observer_id);
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

__attribute__((noinline, used))
static int
peak_dlopen_owned_fixture_static(void)
{
    return 99;
}

__attribute__((visibility("default"), noinline))
void*
peak_dlopen_owned_fixture_static_address(void)
{
    return peak_dlopen_owned_fixture_static;
}
