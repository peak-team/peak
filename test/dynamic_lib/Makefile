# Build matrix for A<->B and main, plus a CPython extension "customCpackage".
# CMake passes Python flags when it owns the build; standalone make falls back
# to python-config.
CC      ?= gcc
AR      ?= ar
LD      ?= ld
CFLAGS  ?= -O2 -fPIC -Wall -Wextra -fvisibility=hidden
LDSO    ?= -shared
RPATH   := -Wl,-rpath,'$$ORIGIN'

# Python config
PYTHON          ?= python3
PY_INCLUDE      ?= $(shell $(PYTHON)-config --includes 2>/dev/null || $(PYTHON)-config --includes)
PY_LDFLAGS      ?= $(shell $(PYTHON)-config --ldflags 2>/dev/null || $(PYTHON)-config --ldflags)
PY_EXT_SUFFIX   ?= $(shell $(PYTHON) -c 'import sysconfig;print(sysconfig.get_config_var("EXT_SUFFIX") or "")')
BUILD_PYTHON_EXTENSIONS ?= 1

# Sources
LIBA_SRC := libA.c
LIBB_SRC := libB.c
MAIN_SRC := main.c
PY_SRC   := customCpackage.c
FLAGS_TEST_SRC := dlopen_flags_test.c
NESTED_OUTER_SRC := nested_dlopen_outer.c
NESTED_IFUNC_OUTER_SRC := nested_ifunc_outer.c
RESOLVER_NESTED_IFUNC_SRC := resolver_nested_ifunc.c
STARTUP_RESOLVER_NESTED_IFUNC_SRC := startup_resolver_nested_ifunc.c
CONCURRENT_DLOPEN_SRC := concurrent_dlopen.c
DLOPEN_MUTATION_GUARD_SRC := dlopen_mutation_guard.c
DLOPEN_MUTATION_FAIRNESS_SRC := dlopen_mutation_fairness.c
DLOPEN_TEARDOWN_HOT_SRC := dlopen_teardown_hot.c
DLOPEN_REPLACE_FAILURE_SRC := dlopen_replace_failure.c
FORK_INFLIGHT_DLOPEN_SRC := fork_inflight_dlopen.c
IFUNC_TARGET_SRC := ifunc_target.c
STATEFUL_IFUNC_TARGET_SRC := stateful_ifunc_target.c
STATEFUL_IFUNC_TEST_SRC := stateful_ifunc_test.c
BLOCKING_IFUNC_TARGET_SRC := blocking_ifunc_target.c
BLOCKING_IFUNC_TEST_SRC := blocking_ifunc_test.c
VERSIONED_IFUNC_TARGET_SRC := versioned_ifunc_target.c
VERSIONED_IFUNC_TEST_SRC := versioned_ifunc_test.c
VERSIONED_IFUNC_MAP := versioned_ifunc.map
EXTERNAL_IFUNC_IMPL_SRC := external_ifunc_impl.c
EXTERNAL_IFUNC_TARGET_SRC := external_ifunc_target.c
EXTERNAL_IFUNC_PROVIDER_TEST_SRC := ifunc_external_provider_retention_test.c
NOTYPE_TARGET_SRC := notype_target.S
NOTYPE_EXECUTABLE_SRC := notype_executable_test.c
STARTUP_UNLOADABLE_TARGET_SRC := startup_unloadable_target.c
STARTUP_LOADER_SRC := startup_loader.c
STARTUP_PROVIDER_TEST_SRC := startup_provider_retention_test.c
STARTUP_LOCAL_CALLABLE_LOADER_SRC := startup_local_callable_loader.c
STARTUP_LOCAL_CALLABLE_TEST_SRC := startup_local_callable_test.c
FORK_CONSTRUCTOR_TARGET_SRC := fork_constructor_target.c
FORK_CONSTRUCTOR_TEST_SRC := fork_constructor_test.c
LAZY_UNRESOLVED_SRC := lazy_unresolved.c
LAZY_UNRESOLVED_O := lazy_unresolved.pic.o
NOTYPE_EXECUTABLE_O := notype_target.exec.o

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
DL_LIBS :=
ELF_LAZY_FIXTURES :=
ELF_CALLABLE_FIXTURES :=
ELF_LIFECYCLE_FIXTURES :=
ifeq ($(UNAME_S),Linux)
DL_LIBS := -ldl
ELF_LAZY_FIXTURES := libLazyUnresolved.so
ELF_LIFECYCLE_FIXTURES := libStartupUnloadableTarget.so libStartupLoader.so startup_provider_retention_test libForkConstructorTarget.so fork_constructor_test
ifneq ($(filter x86_64 amd64 aarch64 arm64,$(UNAME_M)),)
ELF_CALLABLE_FIXTURES := libIfuncTarget.so libStatefulIfuncTarget.so stateful_ifunc_test libBlockingIfuncTarget.so blocking_ifunc_test libVersionedIfuncTarget.so versioned_ifunc_test libExternalIfuncImpl.so libExternalIfuncTarget.so ifunc_external_provider_retention_test libNotypeTarget.so libNestedIfuncOuter.so libResolverNestedIfunc.so notype_executable_test startup_resolver_nested_ifunc_test libStartupLocalCallableLoader.so startup_local_callable_test
endif
endif

PYTHON_EXTENSION_TARGETS :=
ifeq ($(BUILD_PYTHON_EXTENSIONS),1)
PYTHON_EXTENSION_TARGETS := \
     customCpackage_dynA_dynB$(PY_EXT_SUFFIX) \
     customCpackage_dynA_staB$(PY_EXT_SUFFIX) \
     customCpackage_staA_dynB$(PY_EXT_SUFFIX) \
     customCpackage_staA_staB$(PY_EXT_SUFFIX)
endif

LIBA_O   := libA.pic.o
LIBB_O   := libB.pic.o
MAIN_O   := main.o
PY_O     := customCpackage.pic.o

# --------------------------------------------------------------------
# Top-level
# --------------------------------------------------------------------
.PHONY: all clean explain
all: libB.so libBSecond.so libB.a libA_dynB.so libA_staB.so libA.a \
     main_dynA_dynB main_dynA_staB main_staA_dynB main_staA_staB \
     dlopen_flags_test \
     concurrent_dlopen_test \
     dlopen_mutation_guard_test \
     dlopen_mutation_fairness_test \
     dlopen_teardown_hot_test \
     dlopen_replace_failure_test \
     fork_inflight_dlopen_test \
     libNestedOuter.so \
     $(ELF_LAZY_FIXTURES) \
     $(ELF_CALLABLE_FIXTURES) \
     $(ELF_LIFECYCLE_FIXTURES) \
     $(PYTHON_EXTENSION_TARGETS)

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
	$(CC) $(LDSO) -o $@ $^ $(DL_LIBS) -Wl,-soname,libB.so

libBSecond.so: $(LIBB_O)
	$(CC) $(LDSO) -o $@ $^ $(DL_LIBS) -Wl,-soname,libBSecond.so

libB.a: $(LIBB_O)
	$(AR) rcs $@ $^

# --------------------------------------------------------------------
# libA: shared+dynB, shared+staB, and static
# --------------------------------------------------------------------
libA_dynB.so: $(LIBA_O) libB.so
	$(CC) $(LDSO) -o $@ $(LIBA_O) -L. -lB $(DL_LIBS) $(RPATH) -Wl,-soname,libA_dynB.so

libA_staB.so: $(LIBA_O) libB.a
	$(CC) $(LDSO) -o $@ $(LIBA_O) -Wl,-Bstatic -L. -lB -Wl,-Bdynamic $(DL_LIBS) $(RPATH) -Wl,-soname,libA_staB.so

libA.a: $(LIBA_O)
	$(AR) rcs $@ $^

# --------------------------------------------------------------------
# Executables: all four combinations
# --------------------------------------------------------------------
main_dynA_dynB: $(MAIN_O) libA_dynB.so libB.so
	$(CC) -o $@ $(MAIN_O) -L. -lA_dynB $(DL_LIBS) $(RPATH)

main_dynA_staB: $(MAIN_O) libA_staB.so
	$(CC) -o $@ $(MAIN_O) -L. -lA_staB $(DL_LIBS) $(RPATH)

main_staA_dynB: $(MAIN_O) libA.a libB.so
	$(CC) -o $@ $(MAIN_O) -Wl,-Bstatic -L. -lA -Wl,-Bdynamic -L. -lB $(DL_LIBS) $(RPATH)

main_staA_staB: $(MAIN_O) libA.a libB.a
	$(CC) -o $@ $(MAIN_O) -Wl,-Bstatic -L. -lA -lB -Wl,-Bdynamic $(DL_LIBS) $(RPATH)

dlopen_flags_test: $(FLAGS_TEST_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS)

concurrent_dlopen_test: $(CONCURRENT_DLOPEN_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS) -pthread

dlopen_mutation_guard_test: $(DLOPEN_MUTATION_GUARD_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $< $(DL_LIBS) -pthread

dlopen_mutation_fairness_test: $(DLOPEN_MUTATION_FAIRNESS_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $< $(DL_LIBS) -pthread

dlopen_teardown_hot_test: $(DLOPEN_TEARDOWN_HOT_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS) -pthread

dlopen_replace_failure_test: $(DLOPEN_REPLACE_FAILURE_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $< $(DL_LIBS)

fork_inflight_dlopen_test: $(FORK_INFLIGHT_DLOPEN_SRC) libB.so libA_staB.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS) -pthread

libNestedOuter.so: $(NESTED_OUTER_SRC) libB.so
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-soname,libNestedOuter.so

libNestedIfuncOuter.so: $(NESTED_IFUNC_OUTER_SRC) libIfuncTarget.so
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-soname,libNestedIfuncOuter.so

libResolverNestedIfunc.so: $(RESOLVER_NESTED_IFUNC_SRC) libB.so
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-soname,libResolverNestedIfunc.so

libIfuncTarget.so: $(IFUNC_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-s -Wl,-soname,libIfuncTarget.so

libStatefulIfuncTarget.so: $(STATEFUL_IFUNC_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-s -Wl,-soname,libStatefulIfuncTarget.so

stateful_ifunc_test: $(STATEFUL_IFUNC_TEST_SRC) libStatefulIfuncTarget.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS)

libBlockingIfuncTarget.so: $(BLOCKING_IFUNC_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-s -Wl,-soname,libBlockingIfuncTarget.so

blocking_ifunc_test: $(BLOCKING_IFUNC_TEST_SRC) libBlockingIfuncTarget.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS)

libVersionedIfuncTarget.so: $(VERSIONED_IFUNC_TARGET_SRC) $(VERSIONED_IFUNC_MAP)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $(VERSIONED_IFUNC_TARGET_SRC) -Wl,--version-script=$(VERSIONED_IFUNC_MAP) -Wl,-s -Wl,-soname,libVersionedIfuncTarget.so

versioned_ifunc_test: $(VERSIONED_IFUNC_TEST_SRC) libVersionedIfuncTarget.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS)

libExternalIfuncImpl.so: $(EXTERNAL_IFUNC_IMPL_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-soname,libExternalIfuncImpl.so

libExternalIfuncTarget.so: $(EXTERNAL_IFUNC_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-s -Wl,-soname,libExternalIfuncTarget.so

ifunc_external_provider_retention_test: $(EXTERNAL_IFUNC_PROVIDER_TEST_SRC) libExternalIfuncImpl.so libExternalIfuncTarget.so
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $< $(DL_LIBS)

libNotypeTarget.so: $(NOTYPE_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-s -Wl,-soname,libNotypeTarget.so

$(NOTYPE_EXECUTABLE_O): $(NOTYPE_TARGET_SRC)
	$(CC) -O2 -Wall -Wextra -c $< -o $@

notype_executable_test: $(NOTYPE_EXECUTABLE_SRC) $(NOTYPE_EXECUTABLE_O)
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $^

startup_resolver_nested_ifunc_test: $(STARTUP_RESOLVER_NESTED_IFUNC_SRC) libB.so
	$(CC) -O2 -Wall -Wextra -Wl,--export-dynamic -o $@ $< $(DL_LIBS)

libStartupUnloadableTarget.so: $(STARTUP_UNLOADABLE_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-soname,libStartupUnloadableTarget.so

libStartupLoader.so: $(STARTUP_LOADER_SRC) libStartupUnloadableTarget.so
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-soname,libStartupLoader.so

startup_provider_retention_test: $(STARTUP_PROVIDER_TEST_SRC) libStartupLoader.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -L. -lStartupLoader $(DL_LIBS) $(RPATH)

libStartupLocalCallableLoader.so: $(STARTUP_LOCAL_CALLABLE_LOADER_SRC) libIfuncTarget.so libNotypeTarget.so
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< $(DL_LIBS) -Wl,-soname,libStartupLocalCallableLoader.so

startup_local_callable_test: $(STARTUP_LOCAL_CALLABLE_TEST_SRC) libStartupLocalCallableLoader.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -L. -lStartupLocalCallableLoader $(DL_LIBS) $(RPATH)

libForkConstructorTarget.so: $(FORK_CONSTRUCTOR_TARGET_SRC)
	$(CC) $(CFLAGS) $(LDSO) -o $@ $< -Wl,-soname,libForkConstructorTarget.so

fork_constructor_test: $(FORK_CONSTRUCTOR_TEST_SRC) libForkConstructorTarget.so
	$(CC) -O2 -Wall -Wextra -o $@ $< $(DL_LIBS)

$(LAZY_UNRESOLVED_O): $(LAZY_UNRESOLVED_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# This fixture specifically requires lazy binding.  Some MPI compiler wrappers
# (Intel MPI in particular) append -Wl,-z,now after every caller-supplied link
# option, which makes RTLD_LAZY invalid for a deliberately unresolved PLT
# relocation.  Compile with the configured compiler, but link this non-MPI DSO
# directly so the wrapper cannot silently add DF_BIND_NOW.
libLazyUnresolved.so: $(LAZY_UNRESOLVED_O)
	$(LD) -shared -o $@ $< --allow-shlib-undefined -soname libLazyUnresolved.so

# --------------------------------------------------------------------
# Python extension modules: four combinations
# --------------------------------------------------------------------
customCpackage_dynA_dynB$(PY_EXT_SUFFIX): $(PY_O) libA_dynB.so libB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -L. -lA_dynB $(DL_LIBS) $(PY_LDFLAGS) $(RPATH)

customCpackage_dynA_staB$(PY_EXT_SUFFIX): $(PY_O) libA_staB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -L. -lA_staB $(DL_LIBS) $(PY_LDFLAGS) $(RPATH)

customCpackage_staA_dynB$(PY_EXT_SUFFIX): $(PY_O) libA.a libB.so
	$(CC) $(LDSO) -o $@ $(PY_O) -Wl,-Bstatic -L. -lA -Wl,-Bdynamic -L. -lB $(DL_LIBS) $(PY_LDFLAGS) $(RPATH)

customCpackage_staA_staB$(PY_EXT_SUFFIX): $(PY_O) libA.a libB.a
	$(CC) $(LDSO) -o $@ $(PY_O) -Wl,-Bstatic -L. -lA -lB -Wl,-Bdynamic $(DL_LIBS) $(PY_LDFLAGS) $(RPATH)

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
	rm -f *.o *.so *.a main_* dlopen_flags_test concurrent_dlopen_test dlopen_mutation_guard_test dlopen_mutation_fairness_test dlopen_teardown_hot_test dlopen_replace_failure_test fork_inflight_dlopen_test fork_constructor_test stateful_ifunc_test blocking_ifunc_test versioned_ifunc_test ifunc_external_provider_retention_test notype_executable_test startup_resolver_nested_ifunc_test startup_provider_retention_test startup_local_callable_test customCpackage*.so
