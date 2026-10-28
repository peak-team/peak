#!/usr/bin/env python3
"""Strict matrix completeness and descriptive performance/accounting summary.

valid means complete verified data, not that any overhead threshold was met.
"""
import argparse,json,math,re,statistics
from pathlib import Path
from sync_analyze import parse
SYNCS=('allreduce','barrier','bcast','ring','iallreduce')
LAYOUTS=('rotating','slack','critical','aligned')
LABELS=('N0','B0','C0','C1','B1','N1')

def finite(value):
    n=float(value)
    if not math.isfinite(n) or n<0: raise ValueError('nonfinite/negative accounting')
    return n

def peak(text,candidate=False,ranks=32):
    # Only actual 16-target local application reports qualify; helper reports do not.
    starts=list(re.finditer(r'Report scope: ([^\n]*)\n',text))
    chosen=None
    for i,m in enumerate(starts):
        if not m[1].startswith('local'): continue
        block=text[m.start():starts[i+1].start() if i+1<len(starts) else len(text)]
        if re.search(r'^Instrumented targets: 16$',block,re.M): chosen=(m.start(),block)
    if chosen is None: raise ValueError('missing final 16-target local application report')
    start,block=chosen
    def integer(pattern):
        m=re.search(pattern,block,re.M)
        if not m: raise ValueError('missing accounting field '+pattern)
        return int(m[1])
    if not re.search(r'^Accounting snapshot: valid$',block,re.M): raise ValueError('invalid accounting snapshot')
    if integer(r'^Failed control windows: (\d+)')!=0: raise ValueError('failed control window')
    match=re.search(r'local profile\+control overhead: profile_seconds=(\S+) control_seconds=(\S+) profile_ratio=(\S+) control_ratio=(\S+) ratio=(\S+)',block)
    windows=re.search(r'local control stop-window overhead: windows=(\d+) wall_seconds=(\S+) ratio=(\S+)',block)
    trans=re.search(r'Transition coverage[^\n]*: (\d+) detached, (\d+) reattached, (\d+) revisited',block)
    elapsed_rows=list(re.finditer(r'^Time: (\S+)',text[:start],re.M));elapsed=elapsed_rows[-1] if elapsed_rows else None
    if not all((match,windows,trans,elapsed)): raise ValueError('missing local cost/transition/elapsed record')
    result=dict(profile_seconds=finite(match[1]),control_seconds=finite(match[2]),profile_ratio=finite(match[3]),control_ratio=finite(match[4]),raw_ratio=finite(match[5]),elapsed_seconds=finite(elapsed[1]),calls=integer(r'^Recorded calls: (\d+)'),profiled_targets=integer(r'^Profiled targets: (\d+)'),windows=int(windows[1]),detached=int(trans[1]),reattached=int(trans[2]),revisited=int(trans[3]),accounting_valid=True)
    window_seconds=finite(windows[2])
    if not math.isclose(window_seconds,result['control_seconds'],rel_tol=1e-5,abs_tol=2e-9): raise ValueError('window/control seconds mismatch')
    if result['elapsed_seconds']<=0 or not math.isclose(result['raw_ratio'],(result['profile_seconds']+result['control_seconds'])/result['elapsed_seconds'],rel_tol=1e-4,abs_tol=2e-6): raise ValueError('cost/elapsed ratio mismatch')
    if result['profiled_targets']>16 or result['elapsed_seconds']<=0: raise ValueError('invalid coverage/elapsed')
    policies=list(re.finditer(r'MPI budget risk policy: ([^\n]+)',text[:start]))
    if candidate:
        if not policies: raise ValueError('missing candidate policy')
        policy=dict(re.findall(r'(\w+)=([^\s]+)',policies[-1][1]))
        if int(policy.get('world_size',0))!=ranks or policy.get('source')=='unverified' or policy.get('burst_guard')!='enabled' or policy.get('burst_action')!='rank-wide': raise ValueError('invalid candidate rank policy')
        if not math.isclose(float(policy['configured_job_target']),.05,rel_tol=1e-6) or not math.isclose(float(policy['effective_rank_target']),.05/ranks,rel_tol=1e-6): raise ValueError('wrong candidate budget')
        result['waves']=int(policy['burst_waves']);result['unsampled_targets']=int(policy['burst_unsampled_targets']);result['policy']=policy
    return result

def workload(root,label):
    # sync_analyze's ordinary layout expects LABEL.out; adapt without copying raw files.
    class View:
        def __truediv__(self,name):
            return root/name if name.endswith('.rc') else root/label/'rank-0.out'
    return parse(View(),label)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('results',type=Path);p.add_argument('--ranks',type=int,default=32);p.add_argument('--threads',type=int,default=6);p.add_argument('--hosts',type=int,default=2);p.add_argument('--allow-shared-affinity',action='store_true');p.add_argument('--workload-only',action='store_true');a=p.parse_args()
    errors={};cases={}
    for sync in SYNCS:
      for layout in LAYOUTS:
        case=sync+'-'+layout;root=a.results/case
        try:
            runs={label:workload(root,label) for label in LABELS};ref=runs['N0']
            for label,r in runs.items():
                if int(r['config']['ranks'])!=a.ranks or int(r['config']['threads'])!=a.threads or r['scenario']!={'sync':sync,'layout':layout}: raise ValueError('unexpected topology/scenario')
                if r['config']!=ref['config'] or r['bindings']!=ref['bindings'] or any(r['phases'][s]['checksum']!=ref['phases'][s]['checksum'] for s in ('warmup','phase')): raise ValueError('paired workload/binding mismatch')
                if len(set(b['host'] for b in r['bindings'].values()))!=a.hosts: raise ValueError('wrong host count')
                if not a.allow_shared_affinity and not r['affinity_unique']: raise ValueError('nonunique CPU binding')
            reports={}
            if not a.workload_only:
                for label in ('B0','B1','C0','C1'):
                    reports[label]=[peak((root/label/f'rank-{rank}.err').read_text(),label.startswith('C'),a.ranks) for rank in range(a.ranks)]
                    expected={int(row['rank']):int(row['potential_profiled_calls']) for row in runs[label]['phases']['phase']['ranks']}
                    if any(row['calls']>expected[rank] for rank,row in enumerate(reports[label])): raise ValueError('recorded calls exceed rank potential')
            groups={}
            for name,labels in {'native':('N0','N1'),'baseline':('B0','B1'),'candidate':('C0','C1')}.items():
                g={}
                for metric in ('main','warmup_main','process'):
                    samples=[runs[l]['process_seconds'] if metric=='process' else runs[l]['phases']['phase']['root_wall']+(runs[l]['phases']['warmup']['root_wall'] if metric=='warmup_main' else 0) for l in labels]
                    g[metric]={'samples':samples,'mean':statistics.mean(samples)}
                groups[name]=g
            for g in groups.values():
                for metric in g:g[metric]['ratio_to_native']=g[metric]['mean']/groups['native'][metric]['mean']
            native_rows=ref['phases']['phase']['ranks']
            imbalance={}
            for metric in ('lcg_steps','cpu','compute','sync','wait','completion_wait'):
                vals=[float(row[metric]) for row in native_rows];imbalance[metric]={'min':min(vals),'max':max(vals),'max_min_ratio':max(vals)/min(vals) if min(vals)>0 else None}
            coverage={label:{'recorded_calls':sum(row['calls'] for row in rows),'recorded_calls_min':min(row['calls'] for row in rows),'recorded_calls_max':max(row['calls'] for row in rows),'potential_profiled_calls':int(ref['config']['expected_profiled_calls']),'profiled_targets_per_rank':[row['profiled_targets'] for row in rows],'windows_per_rank':[row['windows'] for row in rows],'max_raw_rank_ratio':max(row['raw_ratio'] for row in rows)} for label,rows in reports.items()}
            cases[case]={'native_imbalance':imbalance,'coverage':coverage,'performance':groups,'profiler_by_rank':reports,'native_rank_metrics':ref['phases']['phase']['ranks'],'runs':runs,'candidate_process_above_5pct':groups['candidate']['process']['ratio_to_native']>1.05}
        except (OSError,ValueError,KeyError) as e:errors[case]=str(e)
    worst=sorted(((c,v['performance']['candidate']['process']['ratio_to_native']) for c,v in cases.items()),key=lambda v:v[1],reverse=True)
    print(json.dumps({'valid':not errors and len(cases)==20,'errors':errors,'cases':cases,'worst_process_ratios':worst,'note':'Two samples per arm are descriptive. Primary main wall includes a phase-end barrier. Process means main-entry-through-MPI-Finalize only: loader/pre-main constructors and launcher are excluded. Completeness validity is independent of overhead performance; timing categories overlap.'},indent=2))
    return 1 if errors else 0
if __name__=='__main__':raise SystemExit(main())
