#!/usr/bin/env python3
"""Application rank queries must exclude PEAK startup queries."""
import argparse
import csv
import os
from pathlib import Path
import re
import signal
import socket
import uuid
import subprocess
import sys
import tempfile


def validate_csv_reports(files, producers):
    ranks = set()
    sample_fields = ('count', 'per_thread', 'per_rank', 'call_max_s', 'call_min_s',
                     'total_s', 'exclusive_s', 'thread_max_s', 'thread_min_s',
                     'overhead_s', 'dropped_calls', 'dropped_threads')
    for path in files:
        match = re.search(r'-r([01])-p([0-9]+)-', path.name)
        if path.suffix != '.csv' or not match:
            raise RuntimeError(f'unrecognized rank CSV {path.name}')
        rank, pid = map(int, match.groups())
        with path.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        if pid not in producers.values():
            if not rows or any(not (r['function'].startswith('PEAK_CAPABILITY_') or
                    r['function'] in ('PEAK_CUDA_API_COVERAGE', 'PEAK_DEGRADED_CAPABILITIES')) or
                    any(float(r[field]) != 0 for field in sample_fields) for r in rows):
                raise RuntimeError(f'auxiliary CSV contains profiling data: {path.name}')
            continue
        if producers[rank] != pid or rank in ranks:
            raise RuntimeError(f'duplicate/misranked application CSV {path.name}')
        ranks.add(rank)
        rows = [r for r in rows if r['function'] == 'MPI_Comm_rank']
        if len(rows) != 1 or int(rows[0]['count']) != 1:
            raise RuntimeError(f'expected one profiled application call: {path.name}: {rows}')
    if ranks != {0, 1}:
        raise RuntimeError(f'missing rank CSV: {ranks}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--library', required=True)
    parser.add_argument('--rank', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if args.rank:
        env = dict(os.environ)
        env['DYLD_INSERT_LIBRARIES' if sys.platform == 'darwin' else 'LD_PRELOAD'] = args.library
        os.execvpe(command[0], command, env)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PEAK_') and k not in ('LD_PRELOAD', 'LD_AUDIT', 'DYLD_INSERT_LIBRARIES')}
    env.update(PEAK_TARGET='MPI_Comm_rank', PEAK_ENABLE_GLOBAL_HEARTBEAT='1',
               PEAK_ENABLE_PER_TARGET_HEARTBEAT='0', PEAK_GLOBAL_OVERHEAD_RATIO='.05',
               PEAK_OVERHEAD_RATIO='.05', PEAK_OUTPUT_AGGREGATION='local',
               PEAK_TEXT_OUTPUT='1', PEAK_MPI_OVERHEAD_SYNC_TRACE='1',
               PEAK_HEARTBEAT_INTERVAL='.1', PEAK_HB_MIN_US='10000',
               PEAK_HB_MAX_US='500000')
    # This chooses a route, not a reservation: the module binds after launch.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as route:
        route.bind(('127.0.0.1', 0))
        port = route.getsockname()[1]
    env['PEAK_MPI_OVERHEAD_SYNC_ENDPOINT'] = f'127.0.0.1:{port}'
    env['PEAK_MPI_OVERHEAD_SYNC_TOKEN'] = uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix='peak-mpi-self-profile-') as directory:
        process = subprocess.Popen(command, cwd=directory, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        timed_out = False
        try:
            try:
                output, errors = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(process.pid, signal.SIGKILL)
                output, errors = process.communicate()
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        sys.stdout.buffer.write(output)
        sys.stderr.buffer.write(errors)
        if timed_out:
            raise RuntimeError('MPI application timed out after 30s')
        if process.returncode != 0:
            raise RuntimeError(f'MPI application exited {process.returncode}')
        for rank in (0, 1):
            expected = f'self_profile_science rank={rank} valid=1 sum=3'
            if output.decode().count(expected) != 1:
                raise RuntimeError(f'missing scientific result for rank {rank}')
        producer_rows = re.findall(
            r'self_profile_science rank=([01]) valid=1 sum=3 pid=([1-9][0-9]*)',
            output.decode())
        producers = {int(rank): int(pid) for rank, pid in producer_rows}
        if len(producer_rows) != 2 or set(producers) != {0, 1} or len(set(producers.values())) != 2:
            raise RuntimeError(f'invalid application rank/PID producers: {producer_rows}')
        receipts = re.findall(r"\[peak\] MPI overhead sync: ([^\n]+)", errors.decode())
        startup_ranks = set()
        for receipt in receipts:
            fields = dict(item.split('=', 1) for item in receipt.split() if '=' in item)
            rank = int(fields['rank'])
            if rank in startup_ranks or rank not in (0, 1):
                raise RuntimeError(f'duplicate/unexpected module rank: {fields}')
            if int(fields['helper_created']) != 1 or int(fields['startup_queries']) != 1:
                raise RuntimeError(f'module local startup path was not exercised: {fields}')
            if int(fields['creator_excluded']) != 1:
                raise RuntimeError(f'module creator was not excluded: {fields}')
            if int(fields['ever_ready']) != 1 or any(int(fields[key]) <= 0 for key in
                    ('policy_or_reply_updates', 'records_received', 'records_sent')):
                raise RuntimeError(f'module transport did not become live: {fields}')
            if int(fields['ever_global_accounting']) != 1:
                raise RuntimeError(f'no actual HB global accounting return: {fields}')
            startup_ranks.add(rank)
        if startup_ranks != {0, 1}:
            raise RuntimeError(f'missing module startup receipts: {startup_ranks}')
        files = list(Path(directory).glob('peak_statslog*'))
        validate_csv_reports(files, producers)
    print('mpi_bootstrap_self_profile_ok')


if __name__ == '__main__':
    main()
