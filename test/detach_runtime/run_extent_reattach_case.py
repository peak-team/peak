#!/usr/bin/env python3
"""Require physical helper detach/reattach while a noreturn target unwinds."""
import argparse
import csv
import os
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    parser.add_argument('--libpeak', required=True)
    parser.add_argument('--detach-helper', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='peak-extent-reattach-') as directory:
        trace = os.path.join(directory, 'trace.csv')
        env = os.environ.copy()
        env.update({
            'LD_PRELOAD': os.path.realpath(args.libpeak),
            'PEAK_TARGET': ','.join('peak_fastpath_unwind_' + name for name in
                                    ('outer', 'inner', 'recursive', 'escape_outer',
                                     'generic_escape')),
            'PEAK_MAX_NUM_THREADS': '16',
            'PEAK_ENABLE_PER_TARGET_HEARTBEAT': '0',
            'PEAK_ENABLE_GLOBAL_HEARTBEAT': '0',
            'PEAK_ENABLE_REATTACH': '0',
            'PEAK_COST': '0',
            'PEAK_HEARTBEAT_INTERVAL': '0',
            'PEAK_REQUIRE_SAFE_DETACH': '1',
            'PEAK_SAFE_DETACH_MODE': 'strict',
            'PEAK_DETACH_BACKEND': 'helper',
            'PEAK_DETACH_HELPER': os.path.realpath(args.detach_helper),
            'PEAK_DETACH_TRACE_PATH': trace,
        })
        process = subprocess.Popen([args.exe, '--extent-reattach-check'],
                                   env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   start_new_session=True)
        try:
            output, _ = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            print(output)
            raise RuntimeError('extent reattach subprocess timed out')
        finally:
            # Also reap any helper that outlives a failed test process.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        print(output, end='')
        if process.returncode != 0 or 'extent_reattach_unwind_ok' not in output:
            raise RuntimeError('extent reattach target behavior/count check failed')
        with open(trace, newline='', encoding='utf-8') as handle:
            events = []
            for row in csv.reader(handle):
                if (len(row) < 4 or row[2] != 'peak_fastpath_unwind_recursive'
                        or row[3] not in ('detach', 'reattach')):
                    continue
                if len(row) < 7 or row[4:7] != ['success', '1', 'safe']:
                    raise RuntimeError(f'nonphysical or unsafe lifecycle row: {row}')
                events.append(row[3])
        if events != ['detach', 'reattach', 'detach', 'reattach']:
            raise RuntimeError(f'unexpected physical lifecycle: {events}')
    print('extent_reattach_physical_ok')


if __name__ == '__main__':
    main()
