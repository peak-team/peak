#!/usr/bin/env python3
"""Real-work recovery and early-root-finalize checks; requires MPI PEAK test hooks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import time


def run(argv, env, timeout, directory, stem):
    process = subprocess.Popen(argv, env=env, start_new_session=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        try:
            out, err = process.communicate(timeout=timeout)
            code = process.returncode
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            out, err = process.communicate()
            code = 124
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    (directory / (stem + '.out')).write_text(out)
    (directory / (stem + '.err')).write_text(err)
    (directory / (stem + '.rc')).write_text(str(code) + '\n')
    return code


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_lifecycle(directory, receipt):
    mode = receipt['mode']
    bindings = {int(rank): pair for rank, pair in receipt['bindings'].items()}
    assert receipt['rc'] == 0, receipt
    if mode in ('recovery', 'skew'):
        assert receipt.get('postwave_restore_revisit'), 'no actual post-wave restore/revisit proved'
    root_text = (directory / 'rank-0.err').read_text()
    original = re.search(r'MPI timing: stage=init original_seconds=([^ ]+)', root_text)
    ratio = float(receipt['environment']['PEAK_GLOBAL_OVERHEAD_RATIO'])
    debt_views = []
    for line in root_text.splitlines():
        if 'MPI global budget:' in line:
            fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
            if fields.get('wave') == '0' and float(fields['remaining_seconds']) == 0 and float(fields['global_spent_seconds']) > float(fields['budget_seconds']):
                debt_views.append(fields)
    allowed = 0
    for record in receipt['ranks']:
        rank = record['rank']
        match = re.search(r't0=\[(\d+)\] t1=\[(\d+)\]', record['affinity'][0])
        assert match and list(map(int, match.groups())) == bindings[rank], record
        fields = record['settlement']
        assert fields and fields['shutdown_confirmed'] == '1', record
        assert fields['runtime_degraded_samples'] == '0' and fields['invalid_accounting_samples'] == '0', record
        count = int(fields['emergency_fallback_count'])
        if count:
            assert mode == 'skew' and count == 1 and original and debt_views, record
            assert float(debt_views[0]['startup_seconds']) > ratio * float(original.group(1)), record
            text = (directory / f'rank-{rank}.err').read_text()
            emergencies, grants = [], []
            for line in text.splitlines():
                event = dict(re.findall(r'(\w+)=([^ ]+)', line))
                if 'global controller event:' in line and event.get('kind') == '3':
                    emergencies.append(event)
                if 'global event:' in line and event.get('kind') == 'g' and float(event.get('amount', 0)) > 0:
                    grants.append(int(event['ns']))
            assert len(emergencies) == count and grants, record
            # Compare only timestamps from this rank, never clocks across nodes.
            assert all(event['wave'] == '0' and event['completed_wave'] == '0' and int(event['monotonic_ns']) < min(grants) for event in emergencies), record
            record['startup_hardcap'] = {'count': count,
                'reason': 'unfunded startup debt before first positive grant; no later emergency',
                'first_local_positive_grant_ns': min(grants), 'root_initial_debt_view': debt_views[0]}
            allowed += count
        assert all(value == '0' for value in record['inactive']), record
        assert record['oracle'] or (mode == 'skew' and rank == 0 and record['root_enter_finalize']), record
    receipt['classified_startup_hardcaps'] = allowed
    receipt['original_receipt_sha256'] = sha(directory / 'receipt.json')
    destination = directory / ('classification-' + str(time.time_ns()) + '.json')
    destination.write_text(json.dumps(receipt, indent=2))
    print('mpi_lifecycle_functional_ok startup_hardcaps=' + str(allowed), flush=True)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library', type=Path)
parser.add_argument('--replay', type=Path)
parser.add_argument('--mpicc', default='/usr/lib64/mpich/bin/mpicc')
parser.add_argument('--mpiexec', default='/usr/lib64/mpich/bin/mpiexec')
parser.add_argument('--ranks', type=int, default=2)
parser.add_argument('--mode', choices=('skew', 'recovery'))
parser.add_argument('--output', type=Path, default=Path('/tmp'))
parser.add_argument('--timeout', type=float, default=35)
args = parser.parse_args()
if args.replay:
    validate_lifecycle(args.replay, json.loads((args.replay / 'receipt.json').read_text()))
    raise SystemExit
if args.library is None or args.mode is None:
    parser.error('--library and --mode are required without --replay')

repo = Path(__file__).resolve().parents[2]
cpus = sorted(os.sched_getaffinity(0))
assert 2 <= args.ranks <= 64 and len(cpus) >= 2 * args.ranks
args.output.mkdir(parents=True, exist_ok=True)
directory = Path(tempfile.mkdtemp(prefix='peak-mpi-' + args.mode + '-' + sha(args.library)[:12] + '-', dir=args.output))
shutil.copy2(args.library, directory / 'libpeak.so')
sources = ('src/general_listener.c', 'src/mpi_interceptor.c', 'src/global_budget.c',
           'src/peak.c', 'src/exec_interceptor.c', 'include/internal/global_budget.h',
           'src/logging.c', 'include/logging.h', 'src/general_listener/report_formatter.c',
           'include/general_listener.h', 'include/mpi_interceptor.h', 'include/internal/exec_interceptor_internal.h',
           'test/global_budget/mpi_lifecycle.c', 'benchmarks/mpi_rank_budget/targets.inc')
provenance = {'library_sha256': sha(directory / 'libpeak.so'), 'harness_sha256': sha(__file__),
              'source_sha256': {name: sha(repo / name) for name in sources}}
assert run([args.mpicc, '-O2', '-g', '-fopenmp', '-fno-ipa-icf', '-rdynamic',
            '-I' + str(repo / 'include'), str(repo / 'test/global_budget/mpi_lifecycle.c'),
            '-ldl', '-o', str(directory / 'fixture')], os.environ.copy(), 30, directory, 'build') == 0
provenance['fixture_sha256'] = sha(directory / 'fixture')
clock_env = {key: value for key, value in os.environ.items() if not key.startswith(('PEAK_', 'PMI', 'PMIX', 'OMPI_', 'MV2_', 'I_MPI_', 'SLURM_')) and key != 'LD_PRELOAD'}
clock_env.update(LD_PRELOAD=str(directory / 'libpeak.so'), PEAK_MPI_GLOBAL_BUDGET='0',
                 PEAK_TEXT_OUTPUT='0', PEAK_TARGET_FILE=str(repo / 'benchmarks/mpi_rank_budget/targets.txt'))
assert run([str(directory / 'fixture'), 'clock-guard'], clock_env, 10, directory, 'bootstrap-clock-guard') == 0

(directory / 'PROVENANCE.json').write_text(json.dumps(provenance, indent=2))
bindings = {rank: cpus[rank*2:rank*2+2] for rank in range(args.ranks)}
branches = []
for rank, pair in bindings.items():
    mask = ','.join(map(str, pair))
    places = ','.join('{' + str(cpu) + '}' for cpu in pair)
    branches.append(f'{rank}) taskset -pc {mask} $$ >&2 || exit 125; export OMP_PLACES={shlex.quote(places)};;')
prefix = 'case "${PMI_RANK:?}" in ' + ' '.join(branches) + ' *) exit 125;; esac; '
command = prefix + 'export LD_PRELOAD=' + shlex.quote(str(directory / 'libpeak.so')) + '; exec ' + shlex.quote(str(directory / 'fixture')) + ' ' + args.mode + ' > "$TEST_DEST/rank-$PMI_RANK.out" 2> "$TEST_DEST/rank-$PMI_RANK.err"'
env = {key: value for key, value in os.environ.items() if not key.startswith(('PEAK_', 'PMI', 'PMIX', 'OMPI_', 'MV2_', 'I_MPI_', 'SLURM_')) and key != 'LD_PRELOAD'}
env.update(OMP_NUM_THREADS='2', OMP_DYNAMIC='FALSE', OMP_PROC_BIND='close',
           PEAK_MPI_GLOBAL_BUDGET='1', PEAK_GLOBAL_OVERHEAD_RATIO='.05',
           PEAK_OVERHEAD_RATIO='.005', PEAK_TARGET_FILE=str(repo / 'benchmarks/mpi_rank_budget/targets.txt'),
           PEAK_ENABLE_PER_TARGET_HEARTBEAT='1', PEAK_ENABLE_GLOBAL_HEARTBEAT='1',
           PEAK_ENABLE_REATTACH='1', PEAK_HEARTBEAT_INTERVAL='.01', PEAK_HB_MAX_US='10000',
           PEAK_REATTACH_COOLDOWN_MS='100', PEAK_SAFE_DETACH_MODE='strict',
           PEAK_REQUIRE_SAFE_DETACH='1', PEAK_DETACH_BACKEND='signal',
           PEAK_OUTPUT_AGGREGATION='local', PEAK_TEXT_OUTPUT='1', PEAK_MPI_REAL_FINALIZE='1',
           PEAK_MPI_TIMING_TRACE='1', PEAK_MPI_GLOBAL_TRACE='1', PEAK_MAX_NUM_THREADS='32',
           TEST_DEST=str(directory))
argv = [args.mpiexec, '-n', str(args.ranks), '/bin/sh', '-c', command]
print('RESULT_ROOT', directory, flush=True)
code = run(argv, env, args.timeout, directory, 'launcher')
receipt = {'rc': code, 'mode': args.mode, 'argv': argv, 'bindings': bindings,
           'environment': {key: value for key, value in env.items() if key.startswith(('PEAK_', 'OMP_'))}, 'ranks': []}
for rank in range(args.ranks):
    out = (directory / f'rank-{rank}.out').read_text()
    err = (directory / f'rank-{rank}.err').read_text()
    settlements = re.findall(r'MPI global settlement: (.*)', err)
    fields = dict(re.findall(r'(\w+)=([^ ]+)', settlements[-1])) if settlements else {}
    receipt['ranks'].append({'rank': rank, 'settlement': fields, 'oracle': 'oracle=PASS' in out,
                            'root_enter_finalize': 'root_enter_finalize=1' in out,
                            'inactive': re.findall(r'inactive=(\d+)', out),
                            'affinity': re.findall(r'^affinity .*', out, re.M)})
proofs = []
for rank in range(args.ranks):
    err = (directory / f'rank-{rank}.err').read_text()
    counts = re.findall(r'\|\s+pressure_000\*+\|\s+(\d+)\|', err)
    final_calls = int(counts[-1]) if counts else 0
    for line in err.splitlines():
        if 'global controller event:' not in line:
            continue
        event = dict(re.findall(r'(\w+)=([^ ]+)', line))
        if event.get('kind') == '6' and event.get('hook_id') == '0' and int(event['completed_wave']) > 0 and final_calls > int(event['calls']):
            proofs.append({'rank': rank, 'event': event, 'final_calls': final_calls})
receipt['postwave_restore_revisit'] = proofs
(directory / 'receipt.json').write_text(json.dumps(receipt, indent=2))
validate_lifecycle(directory, receipt)
