# Build matrix for A<->B and main, plus a CPython extension "customCpackage"
# Requires Python dev headers (python3-config available)
CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -fPIC -Wall -Wextra -fvisibility=hidden
LDSO    ?= -shared
RPATH   := -Wl,-rpath,'$$ORIGIN'

# Python config
PYTHON          ?= python3
PY_INCLUDE      := $(shell $(PYTHON)-config --includes 2>/dev/null || $(PYTHON)-config --includes)
PY_LDFLAGS      := $(shell $(PYTHON)-config --ldflags 2>/dev/null || $(PYTHON)-config --ldflags)
PY_EXT_SUFFIX   := $(shell $(PYTHON) -c 'import sysconfig;print(sysconfig.get_config_var("EXT_SUFFIX") or "")')

# Sources
LIBA_SRC := libA.c
LIBB_SRC := libB.c
MAIN_SRC := main.c
PY_SRC   := customCpackage.c

LIBA_O   := libA.pic.o
LIBB_O   := libB.pic.o
MAIN_O   := main.o
PY_O     := customCpackage.pic.o

# --------------------------------------------------------------------
# Top-level
# --------------------------------------------------------------------
.PHONY: all clean explain
all: libB.so libB.a libA_dynB.so libA_staB.so libA.a \
     main_dynA_dynB main_dynA_staB main_staA_dynB main_staA_staB \
     customCpackage_dynA_dynB$(PY_EXT_SUFFIX) \
     customCpackage_dynA_staB$(PY_EXT_SUFFIX) \
     customCpackage_staA_dynB$(PY_EXT_SUFFIX) \
     customCpackage_staA_staB$(PY_EXT_SUFFIX)

explain:
	@echo "Executables:"
	@echo "  main_dynA_dynB : main -> libA_dynB.so ; A -> libB.so"
	@echo "  main_dynA_staB : main -> libA_staB.so ; A -> (libB.a inside)"
	@echo "  main_staA_dynB : main -> (libA.a)     ; A -> libB.so"
	@echo "  main_staA_staB : main -> (libA.a)     ; A -> (libB.a)"
	@echo ""
	@echo "Python extension variants:"
	@echo "  customCpackage_dynA_dynB$(PY_EXT_SUFFIX) : module -> libA_dynB.so ; A -> libB.so"
	@echo "  customCpackage_dynA_staB$(PY_EXT_SUFFIX) : module -> libA_staB.so ; A -> (libB.a inside)"
	@echo "  customCpackage_staA_dynB$(PY_EXT_SUFFIX) : module (A static)     ; A -> libB.so"
	@echo "  customCpackage_staA_staB$(PY_EXT_SUFFIX) : module (A,B static)   ; fully self-contained"

# --------------------------------------------------------------------
# Objects
# --------------------------------------------------------------------
$(LIBA_O): $(LIBA_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBB_O): $(LIBB_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(MAIN_O): $(MAIN_SRC)
	$(CC) -O2 -Wall -Wextra -c $< -o $@

$(PY_O): $(PY_SRC)
	$(CC) $(CFLAGS) $(PY_INCLUDE) -c $< -o $@

# --------------------------------------------------------------------
# libB: shared and static
# --------------------------------------------------------------------
libB.so: $(LIBB_O)
	$(CC) $(LDSO) -o $@ $^ -Wl,-soname,libB.so

libB.a: $(LIBB_O)
	$(AR) rcs $@ $^

# --------------------------------------------------------------------
# libA: shared+dynB, shared+staB, and static
# --------------------------------------------------------------------
libA_dynB.so: $(LIBA_O) libB.so
	$(CC) $(LDSO) -o $@ $(LIBA_O) -L. -lB $(RPATH) -Wl,-soname,libA_dynB.so

libA_staB.so: $(LIBA_O) libB.a
	$(CC) $(LDSO) -o $@ $(LIBA_O) -Wl,-Bstatic -L. -lB -Wl,-Bdynamic $(RPATH) -Wl,-soname,libA_staB.so

libA.a: $(LIBA_O)
	$(AR) rcs $@ $^

# --------------------------------------------------------------------
# Executables: all four combinations
# --------------------------------------------------------------------
main_dynA_dynB: $(MAIN_O) libA_dynB.so libB.so
	$(CC) -o $@ $(MAIN_O) -L. -lA_dynB $(RPATH)

main_dynA_staB: $(MAIN_O) libA_staB.so
	$(CC) -o $@ $(MAIN_O) -L. -lA_staB $(RPATH)

main_staA_dynB: $(MAIN_O) libA.a libB.so
	$(CC) -o $@ $(MAIN_O) -Wl,-Bstatic -L. -lA -Wl,-Bdynamic -L. -lB $(RPATH)

main_staA_staB: $(MAIN_O) libA.a libB.a
	$(CC) -o $@ $(MAIN_O) -Wl,-Bstatic -L. -lA -lB -Wl,-Bdynamic $(RPATH)

# --------------------------------------------------------------------
# Python extension modules: four combinations
# --------------------------------------------------------------------
customCpackage_dynA_dynB$(PY_EXT_SUFFIX): $(PY_O) libA_dynB.so libB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -L. -lA_dynB $(PY_LDFLAGS) $(RPATH)

customCpackage_dynA_staB$(PY_EXT_SUFFIX): $(PY_O) libA_staB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -L. -lA_staB $(PY_LDFLAGS) $(RPATH)

customCpackage_staA_dynB$(PY_EXT_SUFFIX): $(PY_O) libA.a libB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -Wl,-Bstatic -L. -lA -Wl,-Bdynamic -L. -lB $(PY_LDFLAGS) $(RPATH)

customCpackage_staA_staB$(PY_EXT_SUFFIX): $(PY_O) libA.a libB.a
	$(CC) $(LDSO) -o $@ $(PY_O) -Wl,-Bstatic -L. -lA -lB -Wl,-Bdynamic $(PY_LDFLAGS) $(RPATH)

# Convenience: copy chosen variant to canonical import name
.PHONY: use_dynA_dynB use_dynA_staB use_staA_dynB use_staA_staB
use_dynA_dynB: customCpackage_dynA_dynB$(PY_EXT_SUFFIX)
	cp $< customCpackage$(PY_EXT_SUFFIX)
use_dynA_staB: customCpackage_dynA_staB$(PY_EXT_SUFFIX)
	cp $< customCpackage$(PY_EXT_SUFFIX)
use_staA_dynB: customCpackage_staA_dynB$(PY_EXT_SUFFIX)
	cp $< customCpackage$(PY_EXT_SUFFIX)
use_staA_staB: customCpackage_staA_staB$(PY_EXT_SUFFIX)
	cp $< customCpackage$(PY_EXT_SUFFIX)

# --------------------------------------------------------------------
# Cleanup
# --------------------------------------------------------------------
clean:
	rm -f *.o *.so *.a main_* customCpackage*.so