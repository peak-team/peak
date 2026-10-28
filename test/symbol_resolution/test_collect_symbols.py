#!/usr/bin/env python3
"""Integration tests with isolated native and Python fixtures."""
import argparse
import _imp
import os
import platform
import re
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import sysconfig
import tempfile


def run(argv, ok=True, timeout=30, env=None, combined=False, cwd=None, text=True, capture_stderr=False):
    p = subprocess.Popen([str(x) for x in argv], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=text, start_new_session=True, env=env, cwd=cwd)
    try:
        out, err = p.communicate(timeout=timeout)
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()
    assert (p.returncode == 0) == ok, (argv, p.returncode, out, err)
    return err if capture_stderr else out + err if combined else out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--collector', required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--strip', default='strip')
    parser.add_argument('--inspect')
    parser.add_argument('--lib')
    args = parser.parse_args()
    args.collector = str(Path(args.collector).resolve())
    with tempfile.TemporaryDirectory(prefix='peak-collect-test-') as tmp:
        root = Path(tmp)

        def build(name, source, output, *flags, cxx=False):
            path = root / name
            path.write_text(source)
            run([args.cxx if cxx else args.cc, path, '-o', output, *flags])

        def collect(target, *options, ok=True, env=None, cwd=None):
            out = run([sys.executable, args.collector, target, *options], ok=ok, env=env, cwd=cwd)
            lines = out.splitlines()
            if ok:
                assert lines == sorted(set(lines)), 'output is not sorted and unique'
                assert all('!' in s and Path(s.split('!', 1)[0]).is_absolute() for s in lines)
            return set(lines)

        def assert_native_origin(module, origin, symbols):
            print(f'{module} import origin: {origin}')
            modules = {Path(symbol.split('!', 1)[0]) for symbol in symbols}
            if origin in ('built-in', 'frozen'):
                assert not any(path.name.startswith(module+'.') for path in modules), modules
            else:
                expected = Path(origin)
                assert expected.is_absolute() and expected.is_file(), (module, origin)
                assert expected.resolve() in modules, (module, origin, modules)

        # Loader search must follow the executable's origin, RUNPATH
        # boundaries, and breadth-first reuse of an already selected SONAME.
        origin = root/'origin'
        (origin/'lib').mkdir(parents=True)
        origin_leaf, origin_mid = origin/'lib/libleaf.so', origin/'lib/libmid.so'
        build('origin-leaf.c', 'int origin_leaf(void){return 7;}', origin_leaf,
              '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,libleaf.so')
        build('origin-mid.c', 'extern int origin_leaf(void); int origin_mid(void){return origin_leaf();}',
              origin_mid, '-shared', '-fPIC', '-nostdlib', '-L'+str(origin/'lib'),
              '-lleaf', '-Wl,-soname,libmid.so')
        origin_app = origin/'app'
        build('origin-main.c', 'extern int origin_mid(void); int main(void){return origin_mid()!=7;}',
              origin_app, '-L'+str(origin/'lib'), '-lmid', '-Wl,-rpath-link,'+str(origin/'lib'))
        origin_env = os.environ.copy()
        origin_env['LD_LIBRARY_PATH'] = '$ORIGIN/lib'
        origin_symbols = collect(origin_app, env=origin_env)
        assert f'{origin_mid}!origin_mid' in origin_symbols
        assert f'{origin_leaf}!origin_leaf' in origin_symbols
        run([origin_app], env=origin_env)

        for library_path in (str(origin/'unused')+';'+str(origin/'lib'),
                             str(origin/'unused')+'::'):
            loader_env = os.environ.copy()
            loader_env['LD_LIBRARY_PATH'] = library_path
            run([origin_app], env=loader_env, cwd=origin/'lib')
            loader_symbols = collect(origin_app, env=loader_env, cwd=origin/'lib')
            assert f'{origin_leaf}!origin_leaf' in loader_symbols
            assert f'{origin_mid}!origin_mid' in loader_symbols
        loader_env['LD_LIBRARY_PATH'] = ''
        run([origin_app], env=loader_env, cwd=origin/'lib', ok=False)
        collect(origin_app, env=loader_env, cwd=origin/'lib', ok=False)

        rpath_plugin = root/'rpath-plugin.so'
        build('rpath-plugin.c', 'extern int origin_leaf(void);int plugin_call(void){return origin_leaf();}',
              rpath_plugin, '-shared', '-fPIC', '-nostdlib', '-L'+str(origin/'lib'), '-lleaf')
        rpath_main = root/'rpath-main'
        build('rpath-main.c', '#include <dlfcn.h>\nint main(void){void*h=dlopen("'+str(rpath_plugin)+'",RTLD_NOW);if(!h)return 1;int(*f)(void)=dlsym(h,"plugin_call");return !f||f()!=7;}',
              rpath_main, '-ldl', '-Wl,--disable-new-dtags,-rpath,'+str(origin/'lib'))
        run([rpath_main])
        plugin_symbols = collect(rpath_main, '--module', str(rpath_plugin))
        assert f'{origin_leaf}!origin_leaf' in plugin_symbols

        search = root/'search'
        (search/'a').mkdir(parents=True)
        (search/'b').mkdir()
        for directory, value in (('a', 1), ('b', 2)):
            build('search-leaf-'+directory+'.c', f'int search_leaf(void){{return {value};}}',
                  search/directory/'libleaf.so', '-shared', '-fPIC', '-nostdlib',
                  '-Wl,-soname,libleaf.so')
        search_parent = search/'a/libparent.so'
        build('search-parent.c', 'extern int search_leaf(void); int search_parent(void){return search_leaf();}',
              search_parent, '-shared', '-fPIC', '-nostdlib', '-L'+str(search/'b'),
              '-lleaf', '-Wl,-rpath,'+str(search/'b'))
        search_app = search/'app'
        build('search-main.c', 'extern int search_parent(void); int main(void){return search_parent()!=2;}',
              search_app, '-L'+str(search/'a'), '-lparent', '-Wl,--disable-new-dtags',
              '-Wl,-rpath,'+str(search/'a'))
        search_symbols = collect(search_app)
        assert f'{search/"b/libleaf.so"}!search_leaf' in search_symbols
        assert f'{search/"a/libleaf.so"}!search_leaf' not in search_symbols
        run([search_app])

        breadth = root/'breadth'
        (breadth/'a').mkdir(parents=True)
        (breadth/'b').mkdir()
        for directory, value in (('a', 1), ('b', 2)):
            build('breadth-same-'+directory+'.c', f'int breadth_same(void){{return {value};}}',
                  breadth/directory/'libsame.so', '-shared', '-fPIC', '-nostdlib',
                  '-Wl,-soname,libsame.so')
        breadth_parent = breadth/'libparent.so'
        build('breadth-parent.c', 'extern int breadth_same(void); int breadth_parent(void){return breadth_same();}',
              breadth_parent, '-shared', '-fPIC', '-nostdlib', '-L'+str(breadth/'a'),
              '-lsame', '-Wl,--disable-new-dtags', '-Wl,-rpath,'+str(breadth/'a'))
        breadth_app = breadth/'app'
        build('breadth-main.c', 'extern int breadth_parent(void), breadth_same(void); int main(void){return breadth_parent()+breadth_same()!=4;}',
              breadth_app, '-L'+str(breadth), '-L'+str(breadth/'b'), '-lparent', '-lsame',
              '-Wl,-rpath,'+str(breadth)+':'+str(breadth/'b'))
        breadth_symbols = collect(breadth_app)
        assert f'{breadth/"b/libsame.so"}!breadth_same' in breadth_symbols
        assert f'{breadth/"a/libsame.so"}!breadth_same' not in breadth_symbols
        run([breadth_app])

        if platform.machine().lower() in ('x86_64', 'amd64'):
            # Let the real loader select the host-supported hwcap level; no
            # assumption about v2/v3/v4 instruction support is needed.
            hwcaps = root/'hwcaps'
            hwcaps.mkdir()
            variants = {1: hwcaps/'libcollector-hwleaf.so'}
            for level in (2, 3, 4):
                directory = hwcaps/'glibc-hwcaps'/f'x86-64-v{level}'
                directory.mkdir(parents=True)
                variants[level] = directory/'libcollector-hwleaf.so'
            for value, library in variants.items():
                build(f'hwcaps-{value}.c', f'int collector_hwleaf(void){{return {value};}}',
                      library, '-shared', '-fPIC', '-nostdlib',
                      '-Wl,-soname,libcollector-hwleaf.so')
            hwcaps_app = hwcaps/'app'
            build('hwcaps-main.c', '#include <stdio.h>\nextern int collector_hwleaf(void); int main(void){printf("%d\\n",collector_hwleaf());return 0;}',
                  hwcaps_app, '-L'+str(hwcaps), '-lcollector-hwleaf', '-Wl,-rpath,$ORIGIN')
            selected_hwcaps = int(run([hwcaps_app]).strip())
            assert selected_hwcaps in variants
            hwcaps_symbols = collect(hwcaps_app)
            assert f'{variants[selected_hwcaps]}!collector_hwleaf' in hwcaps_symbols
            for value, library in variants.items():
                assert (f'{library}!collector_hwleaf' in hwcaps_symbols) == (value == selected_hwcaps)
            if selected_hwcaps == 1:
                print('SKIP optimized hwcaps selection: loader selected the baseline library')

        slash_libraries = [root/'libslash_a.so', root/'libslash_b.so']
        for index, library in enumerate(slash_libraries):
            build('slash-'+str(index)+'.c',
                  f'int slash_{index}(void){{return {index+3};}}', library, '-shared', '-fPIC')
        slash_app = root/'slash-app'
        build('slash-main.c', '#include <stdio.h>\nextern int slash_0(void),slash_1(void);int main(void){printf("%d %d\\n",slash_0(),slash_1());}',
              slash_app, *map(str, slash_libraries))
        # Preserve slash-qualified NEEDED entries while giving providers the same SONAME.
        for index, library in enumerate(slash_libraries):
            build('slash-'+str(index)+'.c',
                  f'int slash_{index}(void){{return {index+3};}}', library, '-shared', '-fPIC',
                  '-Wl,-soname,libslash-common.so')
        assert run([slash_app]).strip() == '3 4'
        slash_symbols = collect(slash_app)
        for index, library in enumerate(slash_libraries):
            assert f'{library}!slash_{index}' in slash_symbols

        symlink_case = root/'loader-symlink'
        (symlink_case/'real').mkdir(parents=True)
        (symlink_case/'alias').mkdir()
        for directory, value in (('real', 1), ('alias', 2)):
            build('symlink-leaf-'+directory+'.c',
                  f'int collector_symlink_leaf(void){{return {value};}}',
                  symlink_case/directory/'libleaf.so', '-shared', '-fPIC', '-nostdlib',
                  '-Wl,-soname,libleaf.so')
        real_provider = symlink_case/'real/libprovider.so'
        alias_provider = symlink_case/'alias/libprovider.so'
        build('symlink-provider.c', 'extern int collector_symlink_leaf(void); int collector_symlink_provider(void){return collector_symlink_leaf();}',
              real_provider, '-shared', '-fPIC', '-nostdlib', '-L'+str(symlink_case/'real'),
              '-lleaf', '-Wl,-rpath,$ORIGIN')
        alias_provider.symlink_to('../real/libprovider.so')
        symlink_app = symlink_case/'app'
        build('symlink-main.c', '#include <stdio.h>\nextern int collector_symlink_provider(void); int main(void){printf("%d\\n",collector_symlink_provider());return 0;}',
              symlink_app, '-L'+str(symlink_case/'alias'), '-lprovider',
              '-Wl,-rpath,'+str(symlink_case/'alias'),
              '-Wl,-rpath-link,'+str(symlink_case/'alias'))
        assert run([symlink_app]).strip() == '2', 'loader-visible origin fixture did not select alias leaf'
        for target in (symlink_app, alias_provider):
            symlink_symbols = collect(target)
            assert f'{real_provider}!collector_symlink_provider' in symlink_symbols
            assert f'{symlink_case/"alias/libleaf.so"}!collector_symlink_leaf' in symlink_symbols
            assert f'{symlink_case/"real/libleaf.so"}!collector_symlink_leaf' not in symlink_symbols

        leaf, provider, app = [root / x for x in ('libleaf.so', 'libprovider.so', 'app')]
        build('leaf.c', 'int duplicate(void){__asm__ volatile(".rept 32; nop; .endr");return 3;} int leaf(void){__asm__ volatile(".rept 32; nop; .endr");return 5;}', leaf,
              '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,libleaf.so')
        build('provider.cpp', '''extern "C" int leaf(void);
extern "C" int duplicate(void){return 8;}
static __attribute__((noinline)) int local_function(){return leaf();}
int mangled_function(int n){return local_function()+n;}
extern "C" int provider(void){return mangled_function(2);}
int data_only=7;
''', provider, '-shared', '-fPIC', '-nostdlib', '-L'+str(root), '-lleaf',
              '-Wl,-rpath,$ORIGIN', '-Wl,-soname,libprovider.so', cxx=True)
        build('main.c', 'extern int provider(void); int main(void){return provider();}', app,
              '-L'+str(root), '-lprovider', '-Wl,-rpath,$ORIGIN')
        for invalid_timeout in ('nan', 'inf', '-1', '0'):
            collect(app, '--timeout', invalid_timeout, ok=False)
        original_leaf = leaf.read_bytes()
        collect(leaf, '-o', leaf, ok=False)
        assert leaf.read_bytes() == original_leaf
        locale_env = os.environ.copy()
        locale_env['LC_ALL'] = 'tr_TR.UTF-8'
        locale_output = run([sys.executable, args.collector, app], env=locale_env)
        symbols = collect(app)
        assert set(locale_output.splitlines()) == symbols
        for module, name in ((leaf, 'leaf'), (leaf, 'duplicate'), (provider, 'duplicate'),
                             (provider, '_Z16mangled_functioni'), (app, 'main')):
            assert f'{module}!{name}' in symbols, (module, name)
        assert any(s.startswith(str(provider)+'!_ZL14local_function') for s in symbols)
        assert f'{provider}!data_only' not in symbols
        assert f'{provider}!leaf' not in symbols, 'undefined function emitted'
        output = root / 'targets.txt'
        collect(app, '-o', output)
        assert set(output.read_text().splitlines()) == symbols
        run([args.strip, '--strip-all', leaf])
        stripped_symbols = collect(leaf)
        assert f'{leaf}!leaf' in stripped_symbols, 'stripped dynsym omitted'
        if args.inspect:
            for selector in sorted(stripped_symbols):
                inspected = run([args.inspect, 'inspect-symbols', selector])
                assert 'candidates=1' in inspected, inspected
        if args.lib:
            profile_app = root/'profile-app'
            build('profile.c', 'extern int leaf(void), duplicate(void); int main(void){int n=0;for(int i=0;i<10;i++)n+=leaf()+duplicate();return n!=80;}',
                  profile_app, '-L'+str(root), '-lleaf', '-Wl,-rpath,$ORIGIN')
            profile_targets = root/'profile-targets.txt'
            collect(leaf, '-o', profile_targets)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('PEAK_', 'FLAME_')) and k != 'LD_PRELOAD'}
            env.update(LD_PRELOAD=str(Path(args.lib).resolve()),
                       PEAK_TARGET_FILE=str(profile_targets), PEAK_VERBOSITY='report',
                       PEAK_HEARTBEAT_INTERVAL='0',
                       PEAK_ENABLE_GLOBAL_HEARTBEAT='false',
                       PEAK_ENABLE_PER_TARGET_HEARTBEAT='false')
            report = run([profile_app], env=env, combined=True)
            count = re.search(r'Recorded calls: (\d+)', report)
            assert count and int(count[1]) == 20, report
        if args.lib:
            # Build a real extension when the selected interpreter supplies its
            # headers. This exercises Python's late dlopen, with no preloading
            # of the extension or direct calls into PEAK's test hooks.
            python_include = sysconfig.get_path('include')
            if python_include and (Path(python_include)/'Python.h').is_file():
                native = root/('peaknative'+sysconfig.get_config_var('EXT_SUFFIX'))
                build('peaknative.c', r'''#include <Python.h>
static PyObject *native_call(PyObject *self, PyObject *args) {
    __asm__ volatile(".rept 32; nop; .endr");
    return PyLong_FromLong(7);
}
static PyMethodDef methods[] = {
    {"call", native_call, METH_NOARGS, "collector profile fixture"},
    {NULL, NULL, 0, NULL}
};
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "peaknative", NULL, -1, methods
};
PyMODINIT_FUNC PyInit_peaknative(void) {
    __asm__ volatile(".rept 32; nop; .endr");
    return PyModule_Create(&module);
}
''', native, '-shared', '-fPIC', '-nostdlib', '-I'+python_include)
                python_targets = root/'python-profile-targets.txt'
                collect(native, '-o', python_targets)
                native_symbols = set(python_targets.read_text().splitlines())
                assert native_symbols == {f'{native}!native_call', f'{native}!PyInit_peaknative'}, native_symbols
                python_profile = root/'python-profile.py'
                python_profile.write_text('import peaknative, time\n'
                                          'time.sleep(1)\n'
                                          'for i in range(10): assert peaknative.call() == 7\n')
                python_env = env.copy()
                python_env.update(PEAK_PROFILE_INTERPRETERS='1',
                                  PEAK_TARGET_FILE=str(python_targets))
                report = run([sys.executable, python_profile], env=python_env, combined=True)
                assert 'Instrumented targets: 2' in report, report
                count = re.search(r'Recorded calls: (\d+)', report)
                assert count and int(count[1]) == 10, report
            else:
                print('SKIP real Python extension profile: Python.h unavailable')
        if args.lib:
            threaded = root/'thread-profile'
            build('thread-profile.c', r'''#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((noinline)) int collector_thread_work(int n) {
    __asm__ volatile(".rept 32; nop; .endr");
    return n + 1;
}
static void *thread_main(void *unused) {
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += collector_thread_work(i);
    return (void *)(long)sum;
}
int main(void) {
    pthread_t thread;
    void *result = NULL;
    if (pthread_create(&thread, NULL, thread_main, NULL) != 0) return 2;
    if (pthread_join(thread, &result) != 0 || (long)result != 55) return 3;
    puts("collector_new_thread_originals_ok");
    return 0;
}
''', threaded, '-pthread', '-Wl,--export-dynamic')
            all_thread_symbols = collect(threaded)
            selected_names = {'collector_thread_work', 'malloc', 'calloc', 'realloc',
                              'free', 'pthread_mutex_lock', 'pthread_mutex_unlock'}
            thread_symbols = {symbol for symbol in all_thread_symbols
                              if symbol.split('!', 1)[1] in selected_names}
            assert {symbol.split('!', 1)[1] for symbol in thread_symbols} == selected_names
            thread_targets = root/'thread-targets.txt'
            thread_targets.write_text('\n'.join(sorted(thread_symbols))+'\n')
            thread_env = env.copy()
            thread_env['PEAK_TARGET_FILE'] = str(thread_targets)
            report = run([threaded], env=thread_env, combined=True)
            assert 'collector_new_thread_originals_ok' in report, report
            assert re.search(r'collector_thread_work[^\n]*\|\s*10\s*\|', report), report
        if args.lib:
            late_leaf, late_parent = root/'liblate-leaf.so', root/'liblate-parent.so'
            build('late-leaf.c', 'int collector_late_leaf(void){__asm__ volatile(".rept 32; nop; .endr");return 7;}',
                  late_leaf, '-shared', '-fPIC', '-nostdlib', '-Wl,-soname,liblate-leaf.so')
            build('late-parent.c', 'extern int collector_late_leaf(void); int collector_late_parent(void){__asm__ volatile(".rept 32; nop; .endr");return collector_late_leaf();}',
                  late_parent, '-shared', '-fPIC', '-nostdlib', '-L'+str(root),
                  '-llate-leaf', '-Wl,-rpath,$ORIGIN')
            late_targets = root/'late-targets.txt'
            collect(late_parent, '-o', late_targets)
            assert set(late_targets.read_text().splitlines()) == {
                f'{late_leaf}!collector_late_leaf', f'{late_parent}!collector_late_parent'}
            late_script = root/'late-profile.py'
            late_script.write_text(f'import ctypes, time\np = ctypes.CDLL({str(late_parent)!r})\n'
                                   'time.sleep(2)\n'
                                   'for i in range(10): assert p.collector_late_parent() == 7\n')
            late_env = env.copy()
            late_env.update(PEAK_PROFILE_INTERPRETERS='1', PEAK_TARGET_FILE=str(late_targets))
            late_report = run([sys.executable, late_script], env=late_env, combined=True)
            assert 'Instrumented targets: 2' in late_report, late_report
            assert re.search(r'Recorded calls: 20\b', late_report), late_report
            for symbol in ('collector_late_parent', 'collector_late_leaf'):
                assert re.search(symbol+r'[^\n]*\|\s*10\s*\|', late_report), late_report
        if args.lib:
            # Distinct files without SONAMEs become distinct DT_NEEDED modules.
            # Their identical layouts let the parent call each mapping directly,
            # without any extra dlopen that could conceal a lost queue request.
            saturation_count = 260
            saturation = root/'saturation'
            saturation.mkdir()
            template = saturation/'template.so'
            build('saturation-target.c', r'''int collector_saturation_target(void){__asm__ volatile(".rept 32; nop; .endr");return 7;}
__asm__(".text\n.globl collector_saturation_unsafe\n.type collector_saturation_unsafe,%function\ncollector_saturation_unsafe:\nret\n.size collector_saturation_unsafe,.-collector_saturation_unsafe\n");
''',
                  template, '-shared', '-fPIC', '-nostdlib')
            table = run(['readelf', '--syms', '--wide', template])
            offsets = {int(line.split()[1], 16) for line in table.splitlines()
                       if line.split() and line.split()[-1] == 'collector_saturation_target'}
            assert len(offsets) == 1, table
            target_offset = offsets.pop()
            saturation_modules = []
            for index in range(saturation_count):
                module = saturation/f'libsaturation-{index:03d}.so'
                shutil.copyfile(template, module)
                saturation_modules.append(module)
            assert len({module.stat().st_ino for module in saturation_modules}) == saturation_count
            saturation_parent = saturation/'parent.so'
            build('saturation-parent.c', r'''#define _GNU_SOURCE
#include <link.h>
#include <string.h>
static int visit(struct dl_phdr_info *info, size_t size, void *data) {
    if (strstr(info->dlpi_name, "/libsaturation-") != NULL) {
        int (*target)(void) = (void *)(info->dlpi_addr + TARGET_OFFSET);
        if (target() == 7) (*(int *)data)++;
    }
    return 0;
}
int collector_saturation_invoke(void) {
    int count = 0;
    dl_iterate_phdr(visit, &count);
    return count;
}
''', saturation_parent, '-shared', '-fPIC', '-DTARGET_OFFSET='+str(target_offset),
                  '-Wl,--no-as-needed', *saturation_modules)
            saturation_symbols = collect(saturation_parent)
            saturation_symbols = {symbol for symbol in saturation_symbols
                                  if symbol.endswith(('!collector_saturation_target', '!collector_saturation_unsafe'))}
            assert saturation_symbols == {
                f'{module}!{name}' for module in saturation_modules
                for name in ('collector_saturation_target', 'collector_saturation_unsafe')}
            ordered_saturation = sorted(saturation_symbols)
            safe_indices = [index for index, symbol in enumerate(ordered_saturation)
                            if symbol.endswith('!collector_saturation_target')]
            unsafe_indices = [index for index, symbol in enumerate(ordered_saturation)
                              if symbol.endswith('!collector_saturation_unsafe')]
            saturation_targets = root/'saturation-targets.txt'
            saturation_targets.write_text('\n'.join(sorted(saturation_symbols))+'\n')
            saturation_script = root/'saturation-profile.py'
            saturation_script.write_text(f'''import ctypes, time
class Diagnostics(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in (
        "enqueued", "drained", "requeued", "dropped_full", "dropped_closed",
        "dropped_noload", "dropped_requeue", "partial_success", "retained_handles")] + [
        ("max_depth", ctypes.c_size_t), ("queue_length", ctypes.c_size_t),
        ("capacity", ctypes.c_uint), ("drain_budget", ctypes.c_uint)]
# Resolve the diagnostics API before the one late dependency-closing load.
process = ctypes.CDLL(None)
snapshot = process.dlopen_interceptor_get_dynamic_attach_diagnostics
snapshot.argtypes = [ctypes.POINTER(Diagnostics)]
hook_state = process.peak_general_listener_hook_state
hook_state.argtypes = [ctypes.c_size_t]
hook_state.restype = ctypes.c_int
stats = Diagnostics()
snapshot(ctypes.byref(stats))
assert stats.capacity < {saturation_count}
# Drain imports/CDLL(None) before the measured single load, so an earlier
# source request cannot accidentally rediscover any omitted dependencies.
deadline = time.monotonic() + 10
quiet_since = None
while True:
    snapshot(ctypes.byref(stats))
    if stats.queue_length == 0 and stats.drained >= stats.enqueued:
        quiet_since = quiet_since or time.monotonic()
        if time.monotonic() - quiet_since >= 0.1:
            break
    else:
        quiet_since = None
    assert time.monotonic() < deadline
    time.sleep(0.01)
baseline_drained = stats.drained
parent = ctypes.CDLL({str(saturation_parent)!r})
deadline = time.monotonic() + 20
while True:
    snapshot(ctypes.byref(stats))
    # PEAK_HOOK_ATTACHED is published after each mutation commits. Queue
    # counters alone can reach zero while the final attach is still active.
    if (stats.drained - baseline_drained >= {saturation_count}
            and stats.queue_length == 0
            and all(hook_state(index) == 1 for index in {safe_indices!r})):
        break
    assert time.monotonic() < deadline, (stats.drained, stats.queue_length)
    time.sleep(0.01)
assert all(hook_state(index) == 0 for index in {unsafe_indices!r})
assert parent.collector_saturation_invoke() == {saturation_count}
print("collector_saturation_originals_ok")
''')
            saturation_env = env.copy()
            saturation_env.update(PEAK_PROFILE_INTERPRETERS='1',
                                  PEAK_TARGET_FILE=str(saturation_targets))
            saturation_report = run([sys.executable, saturation_script],
                                    env=saturation_env, combined=True, timeout=40)
            assert 'collector_saturation_originals_ok' in saturation_report, saturation_report
            assert f'Instrumented targets: {saturation_count}' in saturation_report, saturation_report
            assert re.search(r'Recorded calls: '+str(saturation_count)+r'\b', saturation_report), saturation_report
            assert '!collector_saturation_unsafe|' not in saturation_report, saturation_report
            for module in saturation_modules:
                row = re.escape(str(module)+'!collector_saturation_target')
                assert re.search(row+r'[^\n]*\|\s*1\s*\|', saturation_report), module
        token_provider = root/'libtoken-provider.so'
        build('token.c', 'extern int leaf(void); int token_provider(void){return leaf();}',
              token_provider, '-shared', '-fPIC', '-nostdlib', '-L'+str(root), '-lleaf',
              '-Wl,-rpath,$ORIGIN/$LIB')
        token_atomic = root/'token-targets.txt'
        token_atomic.write_text('previous token output\n')
        collect(token_provider, '-o', token_atomic, ok=False)
        assert token_atomic.read_text() == 'previous token output\n'
        token_result = run([sys.executable, args.collector, token_provider,
                            '--library-dir', root], combined=True)
        assert f'{leaf}!leaf' in token_result
        assert '$LIB' in token_result and 'warning' in token_result.lower()
        saved = root / 'saved.so'
        leaf.rename(saved)
        output.write_text('previous output\n')
        collect(provider, '-o', output, ok=False)
        assert output.read_text() == 'previous output\n'
        saved.rename(leaf)
        package = root / 'package'
        package.mkdir()
        (package/'__init__.py').write_text('from . import native\n')
        (package/'metadata.some').write_text('ordinary package data, not an ELF shared library\n')
        extension = package/'native.so'
        build('extension.c', 'int extension_function(void){return 19;}', extension,
              '-shared', '-fPIC', '-nostdlib')
        namespace = root/'namespace'
        namespace.mkdir()
        other = namespace/'other.so'
        build('other.c', 'int namespace_function(void){return 23;}', other,
              '-shared', '-fPIC', '-nostdlib')
        marker = root/'executed'
        script = root/'offline.py'
        script.write_text(f'import package\nimport namespace.other\nopen({str(marker)!r}, "w").write("bad")\n')
        symbols = collect(script, '--python', sys.executable)
        assert not marker.exists(), 'offline mode executed the script'
        assert not any(symbol.startswith(str(Path(sys.executable).resolve())+'!')
                       or 'libpython' in Path(symbol.split('!', 1)[0]).name
                       for symbol in symbols), 'interpreter baseline leaked into Python inventory'
        assert f'{extension}!extension_function' in symbols
        assert f'{other}!namespace_function' in symbols
        assert f'{leaf}!leaf' in collect(script, '--module', leaf, '--python', sys.executable)
        (root/'namespace.py').write_text('value = 1\n')
        shadow_script = root/'shadow.py'
        shadow_script.write_text('import namespace.other\n')
        assert f'{other}!namespace_function' not in collect(shadow_script)
        (root/'namespace.py').unlink()
        original_script = script.read_bytes()
        collect(script, '-o', script, ok=False)
        assert script.read_bytes() == original_script
        runtime = root/'runtime.py'
        runtime.write_text(f'if False:\n    import definitely_missing_peak_module\nimport ctypes\nctypes.CDLL({str(extension)!r})\n')
        runtime_symbols = collect(runtime, '--run-python')
        assert f'{extension}!extension_function' in runtime_symbols
        assert not any(symbol.startswith(str(Path(sys.executable).resolve())+'!')
                       or 'libpython' in Path(symbol.split('!', 1)[0]).name
                       for symbol in runtime_symbols), 'runtime interpreter baseline leaked'
        locale_script = root/'locale-runtime.py'
        locale_script.write_text('import os\nassert os.environ["LC_ALL"] == "C.UTF-8"\n')
        runtime_locale = os.environ.copy()
        runtime_locale['LC_ALL'] = 'C.UTF-8'
        run([sys.executable, args.collector, locale_script, '--run-python'],
            env=runtime_locale)
        bootstrap_cwd = root/'bootstrap-cwd'
        bootstrap_cwd.mkdir()
        bootstrap_marker = root/'bootstrap-imported'
        for name in ('json', 'importlib'):
            (bootstrap_cwd/(name+'.py')).write_text(
                f'open({str(bootstrap_marker)!r}, "w").write("bad")\n'
                'raise RuntimeError("unrelated bootstrap import")\n')
        bootstrap_script = root/'bootstrap-static.py'
        bootstrap_script.write_text('pass\n')
        bootstrap_env = {key: value for key, value in os.environ.items()
                         if key not in ('PYTHONPATH', 'PYTHONHOME')}
        assert collect(bootstrap_script, cwd=bootstrap_cwd, env=bootstrap_env) == set()
        assert not bootstrap_marker.exists(), 'metadata probe executed a CWD module'

        shadow_cwd = root/'shadow-cwd'
        shadow_cwd.mkdir()
        (shadow_cwd/'math.py').write_text('value = 1\n')
        math_script = root/'math-static.py'
        math_script.write_text('import math, importlib.util\nassert hasattr(math, "sin")\n'
                               'print(importlib.util.find_spec("math").origin)\n')
        isolated_python_env = {key: value for key, value in os.environ.items()
                               if key not in ('PYTHONPATH', 'PYTHONHOME')}
        math_origin = run([sys.executable, math_script], cwd=shadow_cwd,
                          env=isolated_python_env).strip()
        math_symbols = collect(math_script, cwd=shadow_cwd, env=isolated_python_env)
        assert_native_origin('math', math_origin, math_symbols)

        # Use a separately built extension so the CWD-shadow regression remains
        # effective when the distribution compiles math into the interpreter.
        shadow_import_root = root/'shadow-native-imports'
        shadow_import_root.mkdir()
        shadow_native = shadow_import_root/('collector_shadow_native'+
                                            (sysconfig.get_config_var('EXT_SUFFIX') or '.so'))
        build('shadow-native.c', 'int collector_shadow_symbol(void){return 7;}',
              shadow_native, '-shared', '-fPIC', '-nostdlib')
        (shadow_cwd/'collector_shadow_native.py').write_text('SHADOW_MARKER = 1\n')
        shadow_native_script = root/'shadow-native-static.py'
        shadow_native_script.write_text('import importlib.util\n'
                                        'spec = importlib.util.find_spec("collector_shadow_native")\n'+
                                        f'assert spec.origin == {str(shadow_native)!r}\n'+
                                        'if False:\n    import collector_shadow_native\n')
        native_import_env = isolated_python_env.copy()
        native_import_env['PYTHONPATH'] = str(shadow_import_root)
        run([sys.executable, shadow_native_script], cwd=shadow_cwd, env=native_import_env)
        native_import_symbols = collect(shadow_native_script, cwd=shadow_cwd, env=native_import_env)
        assert f'{shadow_native}!collector_shadow_symbol' in native_import_symbols, native_import_symbols
        (root/'dynamic_attribute_module.py').write_text(
            'count = 0\n'
            'def __getattr__(name):\n'
            '    global count\n'
            '    if name == "value":\n'
            '        count += 1\n'
            '        return count\n'
            '    raise AttributeError(name)\n')
        attribute_script = root/'dynamic-attribute.py'
        attribute_script.write_text('from dynamic_attribute_module import value\nassert value == 1\n')
        run([sys.executable, attribute_script], env=isolated_python_env)
        collect(attribute_script, '--run-python', env=isolated_python_env)
        # The selected target interpreter owns source grammar even when an
        # older Python launches the collector. Keep this conditional for CI
        # hosts that install only one interpreter.
        if sys.version_info >= (3, 12):
            older_python = None
            candidates = ['/usr/bin/python3.9', '/usr/bin/python3.10', '/usr/bin/python3.11',
                          shutil.which('python3.9'), shutil.which('python3.10'), shutil.which('python3.11')]
            for candidate in candidates:
                if candidate and Path(candidate).is_file():
                    version = run([candidate, '-c', 'import sys; print(sys.version_info.minor)']).strip()
                    if int(version) < 12:
                        older_python = candidate
                        break
            if older_python:
                grammar_script = root/'selected-grammar.py'
                grammar_script.write_text('type Value = int\nimport package.native\n')
                grammar_output = run([older_python, args.collector, grammar_script,
                                      '--python', sys.executable])
                assert f'{extension}!extension_function' in grammar_output.splitlines()
            else:
                print('SKIP selected Python grammar: older launcher unavailable')
        sibling = root/'sibling-runtime'
        sibling.mkdir()
        (sibling/'pkgutil.py').write_text('LOCAL_MARKER = 7\n')
        sibling_script = sibling/'app.py'
        sibling_script.write_text('import pkgutil\nassert pkgutil.LOCAL_MARKER == 7\n')
        run([sys.executable, sibling_script], env=isolated_python_env)
        collect(sibling_script, '--run-python', '--python', sys.executable,
                env=isolated_python_env)

        if hasattr(sys.flags, 'safe_path'):
            safe_scripts = root/'safe-path-scripts'
            safe_scripts.mkdir()
            (safe_scripts/'math.py').write_text('SHADOW_MARKER = 9\n')
            (safe_scripts/'collector_safe_first_root.py').write_text('MARKER = 9\n')
            first_import_root = root/'safe-path-imports'
            first_import_root.mkdir()
            (first_import_root/'collector_safe_first_root.py').write_text('MARKER = 7\n')
            safe_script = safe_scripts/'app.py'
            safe_script.write_text('import math, importlib.util, collector_safe_first_root\n'
                                   'assert hasattr(math, "sin")\n'
                                   'assert collector_safe_first_root.MARKER == 7\n'
                                   'print(importlib.util.find_spec("math").origin)\n')
            safe_env = isolated_python_env.copy()
            safe_env.update(PYTHONSAFEPATH='1', PYTHONPATH=str(first_import_root))
            safe_math_origin = run([sys.executable, safe_script], env=safe_env).strip()
            safe_symbols = collect(safe_script, '--run-python', '--python', sys.executable,
                                   env=safe_env)
            assert_native_origin('math', safe_math_origin, safe_symbols)
        builtin_script = root/'builtin-shadow.py'
        (root/'sys.py').write_text('invalid Python syntax !\n')
        builtin_script.write_text('import sys\nassert sys.version\n')
        run([sys.executable, builtin_script])
        collect(builtin_script)
        (root/'sys.py').unlink()
        (root/'os.py').write_text('invalid Python syntax !\n')
        frozen_script = root/'frozen-shadow.py'
        frozen_script.write_text('import os\n')
        if _imp.is_frozen('os'):
            collect(frozen_script)
        (root/'os.py').unlink()
        audit_libs = root/'audit-libraries'
        audit_libs.mkdir()
        audit_library = audit_libs/'libaudit.so'
        shutil.copy2(extension, audit_library)
        for load_path in ('./libaudit.so', str(audit_library)):
            audit_script = root/'audit-runtime.py'
            audit_script.write_text('import ctypes, _ctypes, os\n'+
                                    f'os.chdir({str(audit_libs)!r})\n'+
                                    f'library = ctypes.CDLL({load_path!r})\n'+
                                    'assert library.extension_function() == 19\n'
                                    '_ctypes.dlclose(library._handle)\n'+
                                    f'os.chdir({str(root)!r})\n')
            audit_symbols = collect(audit_script, '--run-python')
            assert f'{audit_library}!extension_function' in audit_symbols

        at_exit = root/'atexit-runtime.py'
        at_exit.write_text('import atexit\n'
                           'def load():\n    import ctypes\n'+
                           f'    ctypes.CDLL({str(extension)!r})\n'+
                           'atexit.register(load)\n')
        at_exit_symbols = collect(at_exit, '--run-python')
        assert f'{extension}!extension_function' in at_exit_symbols
        script_real, script_alias = root/'script-real', root/'script-alias'
        script_real.mkdir()
        script_alias.mkdir()
        shutil.copy2(extension, script_alias/'libfixture.so')
        (script_real/'app.py').write_text(
            'import ctypes, os, sys\nfrom pathlib import Path\n'
            'assert sys.argv[0] in ("script-alias/app.py", "./script-alias/app.py")\n'
            f'assert sys.path[0] == {str(script_real)!r}\n'
            f'assert __file__ == os.path.join({str(root)!r}, sys.argv[0])\n'
            'ctypes.CDLL(str(Path(__file__).parent/"libfixture.so"))\n')
        (script_alias/'app.py').symlink_to(script_real/'app.py')
        for script_argument in ('script-alias/app.py', './script-alias/app.py'):
            run([sys.executable, script_argument], cwd=root)
            alias_symbols = collect(script_argument, '--run-python', cwd=root)
            assert f'{script_alias/"libfixture.so"}!extension_function' in alias_symbols

        interpreter_dynamic = run(['readelf', '-W', '-d', sys.executable])
        python_include = sysconfig.get_path('include')
        if ('(RPATH)' in interpreter_dynamic and '[$ORIGIN/../lib]' in interpreter_dynamic
                and '(RUNPATH)' not in interpreter_dynamic and python_include
                and (Path(python_include)/'Python.h').is_file()):
            copied_python = root/'inherited-python/bin/python'
            copied_python.parent.mkdir(parents=True)
            inherited_lib = copied_python.parent.parent/'lib'
            inherited_lib.mkdir()
            shutil.copy2(sys.executable, copied_python)
            inherited_leaf = inherited_lib/'libcollector_inherited.so'
            build('inherited-leaf.c', 'int inherited_leaf(void){return 17;}',
                  inherited_leaf, '-shared', '-fPIC', '-nostdlib',
                  '-Wl,-soname,libcollector_inherited.so')
            inherited_native = root/('collector_inherited'+sysconfig.get_config_var('EXT_SUFFIX'))
            build('inherited-native.c', '#include <Python.h>\nextern int inherited_leaf(void); static struct PyModuleDef m={PyModuleDef_HEAD_INIT,"collector_inherited",NULL,-1,NULL}; PyMODINIT_FUNC PyInit_collector_inherited(void){if(inherited_leaf()!=17)return NULL;return PyModule_Create(&m);}',
                  inherited_native, '-shared', '-fPIC', '-nostdlib', '-I'+python_include,
                  '-L'+str(inherited_lib), '-lcollector_inherited')
            inherited_script = root/'inherited-app.py'
            inherited_script.write_text('import collector_inherited\n')
            inherited_env = os.environ.copy()
            inherited_env['PYTHONHOME'] = sys.prefix
            inherited_env.pop('LD_LIBRARY_PATH', None)
            run([copied_python, inherited_script], env=inherited_env)
            for mode in ((), ('--run-python',)):
                symbols = collect(inherited_script, '--python', str(copied_python), *mode,
                                  env=inherited_env)
                assert f'{inherited_leaf}!inherited_leaf' in symbols
                assert not any(symbol.startswith(str(copied_python)+'!') for symbol in symbols)
        else:
            print('SKIP inherited interpreter RPATH: compatible origin RPATH/Python headers unavailable')

        empty_runtime = root/'empty-runtime.py'
        empty_runtime.write_text('pass\n')
        assert collect(empty_runtime, '--run-python') == set(), 'helper baseline polluted empty target'
        for import_code in ('import _json', 'import importlib; importlib.import_module("_json")'):
            json_runtime = root/'json-runtime.py'
            json_runtime.write_text(import_code+'\nimport importlib.util\nprint(importlib.util.find_spec("_json").origin)\n')
            json_origin = run([sys.executable, json_runtime]).strip()
            json_symbols = collect(json_runtime, '--run-python')
            assert_native_origin('_json', json_origin, json_symbols)
        binary_script = root/'binary-output.py'
        binary_script.write_text('import sys\nsys.stdout.buffer.write(bytes([255, 254]))\n')
        binary_stderr = run([sys.executable, args.collector, binary_script, '--run-python'],
                            text=False, capture_stderr=True)
        assert b'\xff\xfe' in binary_stderr, binary_stderr
        background_marker = root/'background-finished'
        background_code = f'import time; time.sleep(2); open({str(background_marker)!r}, "w").write("bad")'
        background_script = root/'background-runtime.py'
        background_script.write_text('import subprocess, sys\n'+
                                     f'subprocess.Popen([sys.executable, "-c", {background_code!r}])\n')
        collect(background_script, '--run-python', '--timeout', '1')
        run([sys.executable, '-c', 'import time; time.sleep(2.2)'], timeout=5)
        assert not background_marker.exists(), 'completed target descendant survived collection'
        child_marker = root/'child-finished'
        child_code = f'import time; time.sleep(2); open({str(child_marker)!r}, "w").write("bad")'
        slow = root/'slow.py'
        slow.write_text('import subprocess, sys, time\n'+
                        f'subprocess.Popen([sys.executable, "-c", {child_code!r}])\n'+
                        'time.sleep(20)\n')
        collect(slow, '--run-python', '--timeout', '0.2', ok=False)
        run([sys.executable, '-c', 'import time; time.sleep(2.2)'], timeout=5)
        assert not child_marker.exists(), 'timed-out descendant survived'
    print('collector integration checks passed')


if __name__ == '__main__':
    main()
