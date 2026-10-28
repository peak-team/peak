#!/usr/bin/env python3
"""Integration tests with isolated native and Python fixtures."""
import argparse
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import sysconfig
import tempfile


def run(argv, ok=True, timeout=30, env=None, combined=False):
    p = subprocess.Popen([str(x) for x in argv], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, start_new_session=True, env=env)
    try:
        out, err = p.communicate(timeout=timeout)
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()
    assert (p.returncode == 0) == ok, (argv, p.returncode, out, err)
    return out + err if combined else out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--collector', required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--strip', default='strip')
    parser.add_argument('--inspect')
    parser.add_argument('--lib')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='peak-collect-test-') as tmp:
        root = Path(tmp)

        def build(name, source, output, *flags, cxx=False):
            path = root / name
            path.write_text(source)
            run([args.cxx if cxx else args.cc, path, '-o', output, *flags])

        def collect(target, *options, ok=True):
            out = run([sys.executable, args.collector, target, *options], ok=ok)
            lines = out.splitlines()
            if ok:
                assert lines == sorted(set(lines)), 'output is not sorted and unique'
                assert all('!' in s and Path(s.split('!', 1)[0]).is_absolute() for s in lines)
            return set(lines)

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
