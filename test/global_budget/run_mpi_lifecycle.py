#!/usr/bin/env python3
"""Check funded MPI hook recovery and early rank-zero finalization."""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
from contextlib import contextmanager

@contextmanager
def artifact_directory():
    root = Path(tempfile.mkdtemp(prefix='peak-budget-mpi-'))
    try:
        yield root
    except BaseException:
        # Keep the original complete run, rather than losing raw evidence
        # when an oracle or launcher fails inside a temporary-directory scope.
        print(f'preserved_mpi_failure={root}', file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--libpeak', type=Path, required=True)
p.add_argument('--broker', type=Path, required=True)
p.add_argument('--mpiexec', required=True)
p.add_argument('--np-flag', default='-n')
p.add_argument('--preflags', default='')
p.add_argument('--ranks', type=int, default=2)
p.add_argument('--mode', choices=('recovery', 'early-finalize', 'startup-delay'), required=True)
a = p.parse_args()
with artifact_directory() as root:
    targets = root / 'targets.txt'
    targets.write_text(''.join(f'budget_target_{i}\n' for i in range(80)))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PEAK_') and k not in ('LD_PRELOAD', 'DYLD_INSERT_LIBRARIES')}
    env.update(PEAK_MPI_GLOBAL_BUDGET='1', PEAK_GLOBAL_OVERHEAD_RATIO='.05',
               PEAK_TARGET_FILE=str(targets), PEAK_ENABLE_GLOBAL_HEARTBEAT='1',
               PEAK_ENABLE_PER_TARGET_HEARTBEAT='1', PEAK_ENABLE_REATTACH='1',
               PEAK_HEARTBEAT_INTERVAL='.01', PEAK_HB_MAX_US='10000',
               PEAK_REATTACH_COOLDOWN_MS='100', PEAK_DETACH_BACKEND='signal',
               PEAK_SAFE_DETACH_MODE='strict', PEAK_REQUIRE_SAFE_DETACH='1',
               PEAK_OUTPUT_AGGREGATION='local', PEAK_TEXT_OUTPUT='1',
               PEAK_MPI_GLOBAL_TRACE='1', PEAK_MPI_REAL_FINALIZE='1',
               PEAK_GLOBAL_BUDGET_BROKER=str(a.broker.resolve()),
               TEST_LIB=str(a.libpeak.resolve()),
               TEST_PRELOAD_ENV=('DYLD_INSERT_LIBRARIES' if sys.platform == 'darwin' else 'LD_PRELOAD'), TEST_EXE=str(a.exe.resolve()),
               TEST_MODE=a.mode, TEST_DEST=str(root))
    if sys.platform == 'darwin':
        env.pop('PEAK_DETACH_BACKEND', None)
    command = ('rank=${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-${PMIX_RANK:?}}}; '
               'export "$TEST_PRELOAD_ENV=$TEST_LIB"; '
               'exec "$TEST_EXE" "$TEST_MODE" '
               '> "$TEST_DEST/rank-$rank.out" 2> "$TEST_DEST/rank-$rank.err"')
    process = subprocess.Popen([a.mpiexec, a.np_flag, str(a.ranks), *shlex.split(a.preflags),
                                '/bin/sh', '-c', command], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        out, err = process.communicate(timeout=35)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        out, err = process.communicate()
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    logs = [(root / f'rank-{rank}.err').read_text()
            if (root / f'rank-{rank}.err').exists() else '' for rank in range(a.ranks)]
    rank_out = [(root / f'rank-{rank}.out').read_text() if (root / f'rank-{rank}.out').exists() else '' for rank in range(a.ranks)]
    assert process.returncode == 0, (process.returncode, out, err, rank_out, logs)
    for rank, text in enumerate(logs):
        records = lambda label: [dict(re.findall(r'(\w+)=([^\s]+)', line))
                                 for line in text.splitlines() if label in line]
        settlement = records('MPI global settlement:')[-1]
        assert settlement['shutdown_confirmed'] == '1', text
        assert settlement['invalid_accounting_samples'] == '0', text
        service = records('MPI global service:')[-1]
        assert service['startup_sealed'] == '1', text
        if a.mode == 'startup-delay':
            bill = records('MPI global local bill:')[-1]
            admission = records('MPI initial admission:')[-1]
            assert bill['written'] == '1' and bill['final'] == '1', bill
            assert bill['acknowledged_sample_sequence'] == bill['sample_sequence'], bill
            # The old 10 ms pre-init poll produced about 120 full rounds during
            # this 1.2 s delay. Allow startup/teardown events, but no such polling.
            assert 0 < int(bill['sample_sequence']) <= 16, bill
            assert int(bill['profile_debit_ns']) == 0, bill
            assert admission['initial_installed_targets'] == '80', admission
            assert admission['initial_observation_used_calls'] == '0', admission
            assert 'startup_delay_ready=1 calls=0 physical=80' in rank_out[rank], rank_out[rank]
        if rank == 0:
            assert service['broker_process_id'] != service['parent_process_id'], text
            assert service['broker_final_written'] == '1', text
            broker = records('native global broker:')[-1]
            assert broker['reap_confirmed'] == '1', text
        if a.mode == 'recovery':
            member = records('MPI global member:')[-1]
            bill = records('MPI global local bill:')[-1]
            # Metadata prepayment is a separate typed debit, never evidence
            # that a target actually produced paid profiling work.
            assert int(bill['profile_debit_ns']) > 0, bill
            assert int(member['granted_total_ns']) == (
                int(member['returned_total_ns']) + int(member['control_consumed_ns'])
                + int(bill['frontend_debit_ns']) + int(bill['profile_debit_ns'])
                + int(member['local_credit_ns'])), (member, bill)
            events = records('global controller event:')
            # Initial hooks use the standard constructor installation. Paid
            # continuation/recovery remains mandatory, but no initial runtime
            # reattachment transaction is expected or invented.
            admission = records('MPI initial admission:')[-1]
            assert admission['initial_installed_targets'] == '80', admission
            assert admission['initial_observation_limit_calls'] == str(80 * 64), admission
            assert admission['initial_observation_used_calls'] == '64', admission
            assert float(admission['initial_observation_profile_seconds']) > 0, admission
            assert not [event for event in events if event['kind'] == '7'], text
            assert 'initial_observation calls=64 checksum=' in rank_out[rank], rank_out[rank]
            assert 'selective_exhaustion hot_attached=0 cold_attached=79' in rank_out[rank], rank_out[rank]
            calls = re.findall(r'\|\s+budget_target_0\**\s*\|\s+(\d+)\|', text)
            restored = [event for event in events if event['kind'] == '6'
                        and event['hook_id'] == '0' and int(event['completed_wave']) > 0]
            assert calls and any(int(calls[-1]) > int(event['calls'])
                                 for event in restored), text
    if a.mode == 'early-finalize':
        assert 'root_enter_finalize=1' in (root / 'rank-0.out').read_text()
        assert 'result=PASS' in (root / 'rank-1.out').read_text()
print('global_budget_mpi_' + a.mode + '_ok')
