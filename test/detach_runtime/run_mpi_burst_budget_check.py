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
p.add_argument('--policy', choices=('enabled', 'heartbeat-off', 'global-off', 'single', 'conflict', 'large'), default='enabled')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='peak-mpi-burst-') as tmp:
    trace = Path(tmp) / 'trace.csv'
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('PEAK_', 'PMI', 'OMPI_', 'MV2_', 'I_MPI_', 'SLURM_')):
            env.pop(key)
    target_count = 65 if a.policy == 'large' else 16
    targets = ','.join(['peak_mpi_burst_target'] + [f'peak_mpi_burst_cold_{i}' for i in range(target_count - 1)])
    env.update(LD_PRELOAD=a.libpeak, PEAK_TARGET=targets,
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
    proc = subprocess.Popen([a.exe, a.mode, str(target_count), '1' if a.policy in ('enabled', 'large') else '0'], env=env, cwd=tmp,
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
    enabled = a.policy in ('enabled', 'large')
    assert ('burst_guard=enabled' if enabled else 'burst_guard=disabled') in policy, output
    if a.policy == 'conflict': assert 'source=unverified' in policy, policy
    if a.policy in ('enabled', 'large', 'heartbeat-off', 'global-off'):
        assert 'effective_rank_target=0.003125' in policy and 'world_size=32' in policy, policy
    rows = list(csv.reader(trace.open())) if trace.exists() else []
    burst = [row for row in rows if 'mpi-burst' in row]
    if enabled:
        assert f'runtime_detached={target_count}' in output, output
        assert burst, (output, rows)
        success = [row for row in burst if row[3:6] == ['detach', 'success', '1']]
        assert len(success) == target_count, (output, burst)
        assert all(len(row) >= 22 and float(row[21]) >= 2 for row in burst), burst
        assert 'burst_action=rank-wide' in policy and 'burst_waves=1' in policy, policy
        assert f'burst_unsampled_targets={target_count - 1}' in policy, policy
        cold = [row for row in success if row[2].startswith('peak_mpi_burst_cold_')]
        assert len(cold) == target_count - 1 and all(int(row[18]) == 0 for row in cold), cold
        batches = {row[11] for row in success}
        if target_count <= 64:
            assert len(batches) == 1 and all(int(row[9]) == target_count for row in success), success
        else:
            assert len(batches) >= 2 and all(1 <= int(row[9]) <= 64 for row in success), success
        assert ',reattach,success,' not in trace.read_text(), rows
    else:
        assert not burst, (output, rows)
    print(f'mpi_burst_budget_check_ok mode={a.mode} policy={a.policy}')
