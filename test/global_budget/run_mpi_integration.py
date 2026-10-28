#!/usr/bin/env python3
"""Opt-in real MPI functional checks. Requires PEAK test hooks; no timing assertions."""
import argparse, hashlib, json, os, subprocess, signal, time, shutil, tempfile, re, shlex
from pathlib import Path


def validate(receipt):
    assert receipt['rc'] == 0 and receipt['oracle_pass'] and receipt['oracle_checks'] == 2
    name = receipt['name']
    if name == 'off':
        assert all(not rank['global'] for rank in receipt['ranks'])
        return
    fields = []
    for rank in receipt['ranks']:
        assert rank['global'] and rank['bootstrap']
        assert 'required_thread_level=1 provided_thread_level=1' in rank['bootstrap'][-1]
        f = dict(re.findall(r'(\w+)=([^ ]+)', rank['global'][-1]))
        assert f['ever_active'] == '1' and f['startup_ready'] == '1'
        assert float(f['grace_used_seconds']) <= .000500001
        fields.append(f)
    if name in ('funded', 'tight'):
        assert all(f['shutdown_confirmed'] == '1' for f in fields)
        assert all(int(f['runtime_degraded_samples']) == 0 for f in fields)
        assert all(int(f['invalid_accounting_samples']) == 0 for f in fields)
    if name == 'funded':
        assert any(float(f['granted_total_seconds']) > 0 for f in fields), 'no positive credit observed'
    if name == 'tight':
        assert int(fields[0]['wave']) > 0, 'no coordinated STOP observed'
        assert all(int(f['completed_wave']) >= int(fields[0]['wave']) for f in fields)
        assert int(fields[0]['root_completed_wave']) >= int(fields[0]['wave'])
    if name in ('disconnect', 'invalid'):
        assert fields[0]['shutdown_confirmed'] == '0'
        assert any(int(f['runtime_degraded_samples']) > 0 for f in fields)
        assert int(fields[0]['invalid_accounting_samples']) == (1 if name == 'invalid' else 0)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library', type=Path)
parser.add_argument('--pressure', type=Path, help='compiled benchmarks/mpi_rank_budget/pressure.c')
parser.add_argument('--mpiexec', type=Path, default=Path('/usr/lib64/mpich/bin/mpiexec'))
parser.add_argument('--mpicc', default='/usr/lib64/mpich/bin/mpicc')
parser.add_argument('--output', type=Path, default=Path('/tmp'))
parser.add_argument('--targets', type=Path)
parser.add_argument('--timeout', type=float, default=50)
parser.add_argument('--replay', type=Path, help='validate an existing five-case output directory only')
args = parser.parse_args()
if args.replay:
    for name in ('off', 'funded', 'tight', 'disconnect', 'invalid'):
        validate(json.loads((args.replay/name/'receipt.json').read_text()))
    print('global_actual_mpi_receipt_replay_ok')
    raise SystemExit
if args.library is None or args.pressure is None:
    parser.error('--library and --pressure are required unless --replay is used')
repo = Path(__file__).resolve().parents[2]
args.targets = (args.targets or repo/'benchmarks/mpi_rank_budget/targets.txt').resolve()
lib = args.library.resolve()
args.output.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix='peak-global-validated-'+digest(lib)[:12]+'-', dir=args.output))
shutil.copy2(lib, root/'libpeak.so')
shutil.copy2(args.pressure, root/'pressure')
shim_source = '#include <mpi.h>\n#include <dlfcn.h>\n#include <stdlib.h>\nint MPI_Barrier(MPI_Comm comm) {\n    static unsigned calls;\n    int rc=PMPI_Barrier(comm),rank,size;\n    PMPI_Comm_rank(MPI_COMM_WORLD,&rank); PMPI_Comm_size(MPI_COMM_WORLD,&size);\n    if (++calls == (unsigned)size+2 && rank==0) {\n        void (*stop)(void)=dlsym(RTLD_DEFAULT,(getenv("TEST_INVALID_ACCOUNTING") && getenv("TEST_INVALID_ACCOUNTING")[0] == \'1\') ? "peak_general_listener_test_global_invalid_accounting" : "peak_general_listener_test_global_disconnect");\n        if(!stop) abort();\n        stop();\n    }\n    return rc;\n}\n'
(root/'disconnect.c').write_text(shim_source)
compiler = subprocess.Popen([args.mpicc, '-shared', '-fPIC', str(root/'disconnect.c'), '-ldl', '-o', str(root/'disconnect.so')], start_new_session=True)
try:
    assert compiler.wait(timeout=30) == 0, 'fault shim compilation failed'
finally:
    try: os.killpg(compiler.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    compiler.wait()
manifest = {'library_sha256': digest(root/'libpeak.so'), 'pressure_sha256': digest(root/'pressure'),
            'fault_shim_sha256': digest(root/'disconnect.so'), 'harness_sha256': digest(__file__),
            'source_sha256': {p: digest(repo/p) for p in ('src/general_listener.c', 'src/mpi_interceptor.c', 'src/global_budget.c', 'include/internal/global_budget.h')}}
(root/'PROVENANCE.json').write_text(json.dumps(manifest, indent=2))
print('RESULT_ROOT', root, flush=True)
for name,mode,ratio,disconnect in [('off','0','.05',False),('funded','1','.05',False),('tight','1','.01',False),('disconnect','1','.05',True),('invalid','1','.05',True)]:
 d=root/name;d.mkdir(exist_ok=True)
 e={k:v for k,v in os.environ.items() if not k.startswith(('PEAK_','PMI','PMIX','OMPI_','MV2_','I_MPI_','SLURM_')) and k!='LD_PRELOAD'}
 e.update(OMP_NUM_THREADS='2',OMP_DYNAMIC='FALSE',OMP_PROC_BIND='close',OMP_PLACES='cores',PEAK_MPI_GLOBAL_BUDGET=mode,PEAK_TARGET_FILE=str(args.targets),PEAK_OVERHEAD_RATIO='.005',PEAK_GLOBAL_OVERHEAD_RATIO=ratio,PEAK_ENABLE_PER_TARGET_HEARTBEAT='1',PEAK_ENABLE_GLOBAL_HEARTBEAT='1',PEAK_ENABLE_REATTACH='1',PEAK_HEARTBEAT_INTERVAL='.01',PEAK_HB_MAX_US='10000',PEAK_REATTACH_COOLDOWN_MS='100',PEAK_SAFE_DETACH_MODE='strict',PEAK_REQUIRE_SAFE_DETACH='1',PEAK_DETACH_BACKEND='signal',PEAK_OUTPUT_AGGREGATION='local',PEAK_TEXT_OUTPUT='1',PEAK_MPI_REAL_FINALIZE='1',PEAK_MAX_NUM_THREADS='32',TEST_DEST=str(d),TEST_LIB=str(root/'libpeak.so'),TEST_SHIM=str(root/'disconnect.so') if disconnect else '',TEST_INVALID_ACCOUNTING='1' if name=='invalid' else '')
 cmd=[str(args.mpiexec),'-n','8','/bin/sh','-c',f'export LD_PRELOAD="$TEST_LIB{":$TEST_SHIM" if disconnect else ""}"; exec {shlex.quote(str(root/'pressure'))} --epochs 512 --calls 2000 --warmup-calls 10000 --warmup-work 131072 > "$TEST_DEST/rank-$PMI_RANK.out" 2> "$TEST_DEST/rank-$PMI_RANK.err"']
 t=time.monotonic();p=subprocess.Popen(cmd,env=e,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 try:o,er=p.communicate(timeout=args.timeout);rc=p.returncode
 except subprocess.TimeoutExpired:
  os.killpg(p.pid,signal.SIGKILL);o,er=p.communicate();rc=124
 finally:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass
  if p.poll() is None:p.wait()
 (d/'launcher.out').write_text(o);(d/'launcher.err').write_text(er);out=(d/'rank-0.out').read_text() if (d/'rank-0.out').exists() else '';reports=[]
 for rank in range(8):
  text=(d/f'rank-{rank}.err').read_text() if (d/f'rank-{rank}.err').exists() else '';reports.append({'rank':rank,'global':[x for x in text.splitlines() if 'MPI global budget:' in x],'bootstrap':[x for x in text.splitlines() if 'MPI global bootstrap:' in x],'transitions':[x for x in text.splitlines() if 'Transition coverage' in x]})
 receipt={'name':name,'rc':rc,'elapsed':time.monotonic()-t,'oracle_pass':'oracle=PASS' in out,'oracle_checks':out.count('oracle_bad=0 mpi_bad=0'),'argv':cmd,'environment':{k:v for k,v in e.items() if k.startswith(('PEAK_','OMP_'))},'ranks':reports}
 (d/'receipt.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt),flush=True)
 validate(receipt)
print('global_actual_mpi_functional_ok',flush=True)
