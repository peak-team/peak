#define PEAK_C_SYMBOL(prefix, suffix)                                         \
    __attribute__((noinline, used, visibility("default")))                   \
    int peak_symbol_c_rich_##prefix##suffix(int value)                        \
    {                                                                          \
        return value + 1;                                                       \
    }

#define PEAK_C_SYMBOL_GROUP(prefix) \
    PEAK_C_SYMBOL(prefix, 0) PEAK_C_SYMBOL(prefix, 1) \
    PEAK_C_SYMBOL(prefix, 2) PEAK_C_SYMBOL(prefix, 3) \
    PEAK_C_SYMBOL(prefix, 4) PEAK_C_SYMBOL(prefix, 5) \
    PEAK_C_SYMBOL(prefix, 6) PEAK_C_SYMBOL(prefix, 7) \
    PEAK_C_SYMBOL(prefix, 8) PEAK_C_SYMBOL(prefix, 9) \
    PEAK_C_SYMBOL(prefix, a) PEAK_C_SYMBOL(prefix, b) \
    PEAK_C_SYMBOL(prefix, c) PEAK_C_SYMBOL(prefix, d) \
    PEAK_C_SYMBOL(prefix, e) PEAK_C_SYMBOL(prefix, f)

PEAK_C_SYMBOL_GROUP(a)
PEAK_C_SYMBOL_GROUP(b)
PEAK_C_SYMBOL_GROUP(c)
PEAK_C_SYMBOL_GROUP(d)
PEAK_C_SYMBOL_GROUP(e)
PEAK_C_SYMBOL_GROUP(f)
PEAK_C_SYMBOL_GROUP(g)
PEAK_C_SYMBOL_GROUP(h)
