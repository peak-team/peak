#!/usr/bin/env python3
"""Check funded MPI hook recovery and early rank-zero finalization."""
import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--libpeak', type=Path, required=True)
p.add_argument('--helper', type=Path, required=True)
p.add_argument('--mpiexec', required=True)
p.add_argument('--np-flag', default='-n')
p.add_argument('--preflags', default='')
p.add_argument('--mode', choices=('recovery', 'early-finalize'), required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='peak-budget-mpi-') as temporary:
    root = Path(temporary)
    targets = root / 'targets.txt'
    targets.write_text(''.join(f'budget_target_{i}\n' for i in range(80)))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PEAK_') and k != 'LD_PRELOAD'}
    env.update(PEAK_MPI_GLOBAL_BUDGET='1', PEAK_GLOBAL_OVERHEAD_RATIO='.05',
               PEAK_TARGET_FILE=str(targets), PEAK_ENABLE_GLOBAL_HEARTBEAT='1',
               PEAK_ENABLE_PER_TARGET_HEARTBEAT='1', PEAK_ENABLE_REATTACH='1',
               PEAK_HEARTBEAT_INTERVAL='.01', PEAK_HB_MAX_US='10000',
               PEAK_REATTACH_COOLDOWN_MS='100', PEAK_DETACH_BACKEND='signal',
               PEAK_SAFE_DETACH_MODE='strict', PEAK_REQUIRE_SAFE_DETACH='1',
               PEAK_OUTPUT_AGGREGATION='local', PEAK_TEXT_OUTPUT='1',
               PEAK_MPI_GLOBAL_TRACE='1', PEAK_MPI_REAL_FINALIZE='1',
               PEAK_DETACH_HELPER=str(a.helper.resolve()),
               TEST_LIB=str(a.libpeak.resolve()), TEST_EXE=str(a.exe.resolve()),
               TEST_MODE=a.mode, TEST_DEST=str(root))
    command = ('rank=${OMPI_COMM_WORLD_RANK:-${PMI_RANK:-${PMIX_RANK:?}}}; '
               'export LD_PRELOAD="$TEST_LIB"; '
               'exec "$TEST_EXE" "$TEST_MODE" '
               '> "$TEST_DEST/rank-$rank.out" 2> "$TEST_DEST/rank-$rank.err"')
    process = subprocess.Popen([a.mpiexec, a.np_flag, '2', *shlex.split(a.preflags),
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
            if (root / f'rank-{rank}.err').exists() else '' for rank in range(2)]
    rank_out = [(root / f'rank-{rank}.out').read_text() if (root / f'rank-{rank}.out').exists() else '' for rank in range(2)]
    assert process.returncode == 0, (process.returncode, out, err, rank_out, logs)
    for rank, text in enumerate(logs):
        records = lambda label: [dict(re.findall(r'(\w+)=([^\s]+)', line))
                                 for line in text.splitlines() if label in line]
        settlement = records('MPI global settlement:')[-1]
        assert settlement['shutdown_confirmed'] == '1', text
        assert settlement['invalid_accounting_samples'] == '0', text
        service = records('MPI global service:')[-1]
        assert service['startup_sealed'] == '1', text
        if rank == 0:
            assert service['broker_process_id'] != service['parent_process_id'], text
            assert service['broker_final_written'] == '1', text
            broker = records('native global broker:')[-1]
            assert broker['reap_confirmed'] == '1', text
        if a.mode == 'recovery':
            events = records('global controller event:')
            initial = [event for event in events if event['kind'] == '7']
            assert len(initial) == 80, text
            batches, count = [], 0
            for event in events:
                if event['kind'] in ('4', '5') and count:
                    batches.append(count)
                    count = 0
                elif event['kind'] == '7':
                    count += 1
            if count:
                batches.append(count)
            assert len(batches) >= 2 and max(batches) <= 64, batches
            calls = re.findall(r'\|\s+budget_target_0\**\s*\|\s+(\d+)\|', text)
            restored = [event for event in events if event['kind'] == '6'
                        and event['hook_id'] == '0' and int(event['completed_wave']) > 0]
            assert calls and any(int(calls[-1]) > int(event['calls'])
                                 for event in restored), text
    if a.mode == 'early-finalize':
        assert 'root_enter_finalize=1' in (root / 'rank-0.out').read_text()
        assert 'result=PASS' in (root / 'rank-1.out').read_text()
print('global_budget_mpi_' + a.mode + '_ok')
