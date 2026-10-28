"""Run actual generic/direct callbacks with a heartbeat too late to help."""
import argparse
import csv
import os
from pathlib import Path
import signal
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--exe', required=True)
p.add_argument('--libpeak', required=True)
p.add_argument('--mode', choices=('generic', 'fast'), required=True)
p.add_argument('--policy', choices=('enabled', 'heartbeat-off', 'global-off', 'single', 'conflict'), default='enabled')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='peak-mpi-burst-') as tmp:
    trace = Path(tmp) / 'trace.csv'
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('PEAK_', 'PMI', 'OMPI_', 'MV2_', 'I_MPI_', 'SLURM_')):
            env.pop(key)
    env.update(LD_PRELOAD=a.libpeak, PEAK_TARGET='peak_mpi_burst_target',
               PEAK_MAX_NUM_THREADS='16', PEAK_HEARTBEAT_INTERVAL='10',
               PEAK_HB_MAX_US='10000000', PEAK_ENABLE_PER_TARGET_HEARTBEAT='0',
               PEAK_ENABLE_GLOBAL_HEARTBEAT='1', PEAK_GLOBAL_OVERHEAD_RATIO='.1',
               PEAK_ENABLE_REATTACH='0', PEAK_COST='0', PEAK_DETACH_COUNT='1000000000',
               PEAK_DETACH_BACKEND='signal', PEAK_SAFE_DETACH_MODE='strict',
               PEAK_REQUIRE_SAFE_DETACH='1', PEAK_TEXT_OUTPUT='1',
               PEAK_DETACH_TRACE_PATH=str(trace), PMI_RANK='0', PMI_SIZE='32')
    if a.policy == 'heartbeat-off': env['PEAK_HEARTBEAT_INTERVAL'] = '0'
    if a.policy == 'global-off': env['PEAK_ENABLE_GLOBAL_HEARTBEAT'] = '0'
    if a.policy == 'single': env['PMI_SIZE'] = '1'
    if a.policy == 'conflict':
        env.update(OMPI_COMM_WORLD_RANK='0', OMPI_COMM_WORLD_SIZE='8')
    proc = subprocess.Popen([a.exe, a.mode], env=env, cwd=tmp,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=15)
    finally:
        # Also bound/reap descendants on failure or interruption.
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        if proc.poll() is None: proc.wait()
    if proc.returncode == 77: raise SystemExit(77)
    assert proc.returncode == 0, output
    assert 'mpi_burst_workload_ok' in output, output
    policy = next((line for line in output.splitlines() if 'MPI budget risk policy:' in line), '')
    assert policy, output
    enabled = a.policy == 'enabled'
    assert ('burst_guard=enabled' if enabled else 'burst_guard=disabled') in policy, output
    if a.policy == 'conflict': assert 'source=unverified' in policy, policy
    if a.policy in ('enabled', 'heartbeat-off', 'global-off'):
        assert 'effective_rank_target=0.003125' in policy and 'world_size=32' in policy, policy
    rows = list(csv.reader(trace.open())) if trace.exists() else []
    burst = [row for row in rows if 'mpi-burst' in row]
    if enabled:
        assert burst, (output, rows)
        assert any('detach' in row and 'success' in row for row in burst), (output, burst)
        # The useful-compute phase lasts ~192ms. A premature request is a bug.
        assert all(len(row) >= 22 and float(row[21]) >= 2 for row in burst), burst
    else:
        assert not burst, (output, rows)
    print(f'mpi_burst_budget_check_ok mode={a.mode} policy={a.policy}')
