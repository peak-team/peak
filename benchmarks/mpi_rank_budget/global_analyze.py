#!/usr/bin/env python3
"""Validate same-binary off/on prototype data without assuming a performance pass."""
import argparse,json,re,sys,statistics,math
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from sync_summary import workload,peak
CASES=('allreduce-rotating','allreduce-slack','bcast-slack','iallreduce-critical','funded-allreduce-rotating')
LABELS=('N0','L0','G0','G1','L1','N1')
def global_report(text):
    scopes=list(re.finditer(r'Report scope: ([^\n]*)\n',text));actual=[]
    for i,m in enumerate(scopes):
        block=text[m.start():scopes[i+1].start() if i+1<len(scopes) else len(text)]
        if m[1].startswith('local') and re.search(r'^Instrumented targets: 16$',block,re.M):actual.append(m.start())
    if not actual: raise ValueError('missing actual local scope')
    rows=re.findall(r'MPI global budget: ([^\n]+)',text[:actual[-1]])
    if not rows: raise ValueError('missing global diagnostics')
    fields=dict(re.findall(r'(\w+)=([^\s]+)',rows[-1]))
    for key in ('configured','ever_active','fresh','wave','completed_wave','view_epoch','root_completed_wave','shutdown_confirmed','decision_sequence','runtime_degraded_samples','invalid_accounting_samples'):
        if key not in fields: raise ValueError('missing global field '+key)
        fields[key]=int(fields[key])
    for key in ('global_spent_seconds','remaining_seconds','allowance_seconds','wave_span_seconds','budget_seconds','reserved_seconds','control_reserve_seconds','granted_total_seconds','startup_seconds','shutdown_seconds'):
        n=float(fields[key])
        if not math.isfinite(n) or n<0: raise ValueError('invalid global cost')
        fields[key]=n
    for key in ('unfunded_seconds','granted_total_seconds','startup_seconds','shutdown_seconds','grace_used_seconds','budget_seconds','reserved_seconds','control_reserve_seconds'):
        if key in fields:
            n=float(fields[key])
            if not math.isfinite(n) or n<0:raise ValueError('invalid global '+key)
            fields[key]=n
    for key in ('emergency_fallback_count','startup_ready'):
        if key in fields:fields[key]=int(fields[key])
    if 'budget_seconds' in fields: fields['spent_exceeds_global_budget']=fields['global_spent_seconds']>fields['budget_seconds']+1e-8
    fields['settlement_confirmed']=bool(fields['shutdown_confirmed'])
    fields['last_wave_globally_completed']=fields['root_completed_wave']>=fields['wave']
    fields['observed_emergency_fallback']=fields.get('emergency_fallback_count',0)>0
    fields['unfunded_cost_observed']=fields.get('unfunded_seconds',0)>1e-9
    fields['grace_used_exceeds_cap']=fields.get('grace_used_seconds',0)>0.0005+1e-9
    return fields

def main():
    p=argparse.ArgumentParser();p.add_argument('results',type=Path);a=p.parse_args();cases={};errors={}
    for name in CASES:
      try:
        root=a.results/name;runs={l:workload(root,l) for l in LABELS};ref=runs['N0'];reports={}
        for l,r in runs.items():
            if int(r['config']['ranks'])!=32 or int(r['config']['threads'])!=6 or not r['affinity_unique'] or len(set(b['host'] for b in r['bindings'].values()))!=2:raise ValueError('invalid topology')
            if r['config']!=ref['config'] or r['scenario']!=ref['scenario'] or r['bindings']!=ref['bindings'] or any(r['phases'][s]['checksum']!=ref['phases'][s]['checksum'] for s in ('warmup','phase')):raise ValueError('mismatched workload')
            if l[0] not in 'LG':continue
            perrank=[]
            expected={int(row['rank']):int(row['potential_profiled_calls']) for row in r['phases']['phase']['ranks']}
            for rank in range(32):
                text=(root/l/f'rank-{rank}.err').read_text();local=peak(text);glob=global_report(text) if l[0]=='G' or 'MPI global budget:' in text else {'configured':0,'ever_active':0}
                policies=re.findall(r'MPI budget risk policy: ([^\n]+)',text)
                if not policies:raise ValueError('missing rank policy')
                policy=dict(re.findall(r'(\w+)=([^\s]+)',policies[-1]))
                if int(policy.get('world_size',0))!=32 or policy.get('source')=='unverified' or not math.isclose(float(policy['configured_job_target']),.05,rel_tol=1e-6):raise ValueError('wrong world/job target')
                local['policy']=policy
                if local['calls']>expected[rank]:raise ValueError('excess observed calls')
                if glob['configured']!=int(l[0]=='G'):raise ValueError('wrong global mode')
                if l[0]=='G':
                    if not glob['ever_active']:raise ValueError('global transport never active')
                    boot=re.findall(r'MPI global bootstrap: ([^\n]+)',text)
                    if not boot:raise ValueError('missing global MPI bootstrap diagnostics')
                    bootstrap=dict(re.findall(r'(\w+)=([^\s]+)',boot[-1]))
                    if bootstrap.get('backend')!='tcp' or int(bootstrap.get('required_thread_level',-1))!=1 or int(bootstrap.get('provided_thread_level',-1))<1:raise ValueError('invalid FUNNELED bootstrap')
                    glob['bootstrap']=bootstrap
                perrank.append({'local':local,'global':glob})
            reports[l]=perrank
        groups={}
        for g,labels in {'native':('N0','N1'),'local':('L0','L1'),'global':('G0','G1')}.items():
            groups[g]={}
            for metric in ('main','warmup_main','main_to_finalize'):
                samples=[runs[l]['process_seconds'] if metric=='main_to_finalize' else runs[l]['phases']['phase']['root_wall']+(runs[l]['phases']['warmup']['root_wall'] if metric=='warmup_main' else 0) for l in labels]
                groups[g][metric]={'samples':samples,'mean':statistics.mean(samples)}
        for g in groups.values():
            for metric,v in g.items():v['ratio_to_native']=v['mean']/groups['native'][metric]['mean']
        coord={l:{'rank0_last_view':reports[l][0]['global'],'view_epochs':[r['global'].get('view_epoch') for r in reports[l]],'ever_active_ranks':sum(bool(r['global'].get('ever_active')) for r in reports[l]),'profiled_targets_by_rank':[r['local']['profiled_targets'] for r in reports[l]],'local_allowance_by_rank':[r['global'].get('allowance_seconds',0) for r in reports[l]],'emergency_ranks':sum(bool(r['global'].get('observed_emergency_fallback')) for r in reports[l]),'runtime_degraded_ranks':sum(r['global'].get('runtime_degraded_samples',0)>0 for r in reports[l]),'invalid_accounting_ranks':sum(r['global'].get('invalid_accounting_samples',0)>0 for r in reports[l]),'settlement_confirmed_ranks':sum(bool(r['global'].get('settlement_confirmed')) for r in reports[l]),'positive_grant_ranks':sum(r['global'].get('granted_total_seconds',0)>0 for r in reports[l]),'reattached_targets':sum(r['local']['reattached'] for r in reports[l]),'recorded_calls':sum(r['local']['calls'] for r in reports[l]),'local_windows':sum(r['local']['windows'] for r in reports[l])} for l in ('G0','G1')}
        cases[name]={'performance':groups,'coordination_diagnostics':coord,'reports':reports,'workloads':runs}
      except (OSError,ValueError,KeyError) as e:errors[name]=str(e)
    print(json.dumps({'valid':not errors and len(cases)==5,'errors':errors,'cases':cases,'note':'Final fresh=0 is expected after service shutdown. Do not sum snapshots from different epochs or ranks; use coordinator diagnostics as recorded. global_spent conservatively sums rank costs; wave_span is a root GO-to-all-DONE diagnostic and does not replace summed control charges. allowance is rank-local unconsumed reservation, budget is job-wide. This is not a hard slowdown proof.'},indent=2));return bool(errors)
if __name__=='__main__':raise SystemExit(main())
