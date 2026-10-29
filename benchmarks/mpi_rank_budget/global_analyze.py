#!/usr/bin/env python3
"""Validate same-binary off/on prototype data without assuming a performance pass."""
import argparse,json,re,sys,statistics,math
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from sync_summary import workload,peak
from recovery_analyze import parse as recovery_workload
CASES=('allreduce-rotating','allreduce-slack','bcast-slack','iallreduce-critical','funded-allreduce-rotating')
LABELS=('N0','L0','G0','G1','L1','N1')
def global_report(text):
    scopes=list(re.finditer(r'Report scope: ([^\n]*)\n',text));actual=[]
    for i,m in enumerate(scopes):
        block=text[m.start():scopes[i+1].start() if i+1<len(scopes) else len(text)]
        if m[1].startswith('local') and re.search(r'^Instrumented targets: 16$',block,re.M):actual.append(m.start())
    if not actual: raise ValueError('missing actual local scope')
    rows=re.findall(r'MPI global budget: ([^\n]+)',text[:actual[-1]])
    settlement_rows=re.findall(r'MPI global settlement: ([^\n]+)',text[actual[-1]:])
    if not rows and not settlement_rows: raise ValueError('missing global diagnostics')
    fields=dict(re.findall(r'(\w+)=([^\s]+)',settlement_rows[-1] if settlement_rows else rows[-1]))
    fields['diagnostic_origin']='final_settlement' if settlement_rows else 'legacy_pre_report_budget'
    if settlement_rows and rows:fields['pre_report_view']=dict(re.findall(r'(\w+)=([^\s]+)',rows[-1]))
    member_rows=re.findall(r'MPI global member: ([^\n]+)',text if settlement_rows else text[:actual[-1]])
    if member_rows:
        member=dict(re.findall(r'(\w+)=([^\s]+)',member_rows[-1]))
        for key in ('wave_participant','local_credit_ns','returned_total_ns','acknowledged_demand_generation','request_deadline_ns','retiring','cohort_finalized','accounting_invalid','recovery_pending','recovery_target_ns','rejected_spend_count','granted_total_ns','control_consumed_ns','settled_control_credit_ns'):
            if key not in member:
                if key in ('retiring','cohort_finalized','accounting_invalid','recovery_pending','recovery_target_ns','rejected_spend_count','granted_total_ns','control_consumed_ns','settled_control_credit_ns'):continue
                raise ValueError('missing member field '+key)
            member[key]=int(member[key])
            if member[key]<0:raise ValueError('invalid member counter')
        for key in ('refill_rtt_seconds','consumption_rate','closing_seconds'):
            member[key]=float(member[key])
            if not math.isfinite(member[key]) or member[key]<0:raise ValueError('invalid member cost')
        fields['member']=member
    admission_rows=re.findall(r'MPI initial admission: ([^\n]+)',text[:actual[-1]])
    if admission_rows:
        admission=dict(re.findall(r'(\w+)=([^\s]+)',admission_rows[-1]))
        for key in ('bootstrap_finished','first_paid_attach_count','first_local_attach_count','initial_unexposed'):
            if key in admission:
                admission[key]=int(admission[key])
                if admission[key]<0:raise ValueError('invalid initial admission counter')
        fields['initial_admission']=admission
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
    for key,value in list(fields.items()):
        if key.endswith('_seconds') and isinstance(value,str):
            n=float(value)
            if not math.isfinite(n) or n<0:raise ValueError('invalid extra global cost '+key)
            fields[key]=n
    activation_rows=re.findall(r'MPI activation: ([^\n]+)',text)
    if activation_rows:fields['activation']=dict(re.findall(r'(\w+)=([^\s]+)',activation_rows[-1]))
    preparation_rows=re.findall(r'MPI preparation: ([^\n]+)',text)
    if preparation_rows:
        preparation=dict(re.findall(r'(\w+)=([^\s]+)',preparation_rows[-1]))
        for key in ('preparation_seconds','activation_seconds','bootstrap_seconds','charged_startup_seconds'):
            preparation[key]=float(preparation[key])
            if not math.isfinite(preparation[key]) or preparation[key]<0:raise ValueError('invalid preparation cost')
        fields['preparation']=preparation
    service_rows=re.findall(r'MPI global service: ([^\n]+)',text)
    if service_rows:
        service=dict(re.findall(r'(\w+)=([^\s]+)',service_rows[-1]))
        for key in ('settlement','parent_process_id','broker_process_id','broker_final_written'):
            service[key]=int(service[key])
            if service[key]<0:raise ValueError('invalid native service field')
        if 'startup_sealed' in service:
            service['startup_sealed']=int(service['startup_sealed'])
            if service['startup_sealed'] not in (0,1):raise ValueError('invalid startup seal')
        service['broker_cpu_seconds']=float(service['broker_cpu_seconds'])
        if not math.isfinite(service['broker_cpu_seconds']) or service['broker_cpu_seconds']<0:raise ValueError('invalid service CPU')
        fields['service']=service
    broker_rows=re.findall(r'native global broker: ([^\n]+)',text)
    if broker_rows:
        broker=dict(re.findall(r'(\w+)=([^\s]+)',broker_rows[-1]))
        for key in ('pid','all_final_replies_written','reap_confirmed'):
            broker[key]=int(broker[key])
            if broker[key]<0:raise ValueError('invalid native broker proof')
        broker['cpu_seconds']=float(broker['cpu_seconds'])
        if not math.isfinite(broker['cpu_seconds']) or broker['cpu_seconds']<0:raise ValueError('invalid broker CPU')
        fields['native_broker']=broker
    fields['settlement_confirmed']=bool(fields['shutdown_confirmed'])
    fields['last_wave_globally_completed']=fields['root_completed_wave']>=fields['wave']
    fields['observed_emergency_fallback']=fields.get('emergency_fallback_count',0)>0
    fields['unfunded_cost_observed']=fields.get('unfunded_seconds',0)>1e-9
    fields['grace_used_exceeds_cap']=fields.get('grace_used_seconds',0)>0.0005+1e-9
    return fields

def timing_and_events(text):
    timing=[];events=[]
    for line in text.splitlines():
        if any(label in line for label in ('MPI timing:', 'MPI report timing:', 'MPI settlement timing:')):
            values=dict(re.findall(r'(\w+)=([^\s]+)',line))
            values['timing_kind']='report' if 'MPI report timing:' in line else ('settlement' if 'MPI settlement timing:' in line else 'mpi')
            for key,value in list(values.items()):
                if key.endswith('_seconds') or key=='seconds':
                    n=float(value)
                    if not math.isfinite(n) or n<0:raise ValueError('invalid MPI stage timing')
                    values[key]=n
            timing.append(values)
        elif re.search(r'global.*(?:event|trace)',line,re.I):
            events.append({'fields':dict(re.findall(r'(\w+)=([^\s]+)',line)),'raw':line})
    client_requests={};root_requests={};go={};positive_rtt=[];root_grant=[];notice_submit=[];root_wave=[];controller_go={};controller_span=[];overflow=[];helper_count=0
    for item in events:
        f=item['fields'];kind=f.get('kind')
        if 'overflow' in f:overflow.append(int(f['overflow']))
        if 'ns' in f and kind in ('r','R','G','g','S','s','D','I'):
            helper_count+=1;ns=int(f['ns']);key=(f.get('rank'),f.get('demand'))
            if kind=='r':
                client_requests.setdefault(key,ns)
                created=float(f.get('amount','0'))
                if math.isfinite(created) and created>0 and ns/1e9>=created:notice_submit.append(ns/1e9-created)
            elif kind=='g' and key in client_requests:positive_rtt.append((ns-client_requests.pop(key))/1e9)
            elif kind=='R':root_requests.setdefault(key,ns)
            elif kind=='G' and key in root_requests:root_grant.append((ns-root_requests.pop(key))/1e9)
            elif kind=='S':go[f.get('wave')]=ns
            elif kind=='D' and f.get('wave') in go:root_wave.append((ns-go.pop(f.get('wave')))/1e9)
        if kind=='1' and f.get('participant')=='1' and 'monotonic_ns' in f:controller_go[f.get('wave')]=int(f['monotonic_ns'])
        elif kind=='2' and f.get('participant')=='1' and f.get('wave') in controller_go:controller_span.append((int(f['monotonic_ns'])-controller_go.pop(f.get('wave')))/1e9)
    return {'mpi_timing':timing,'events':events,'trace_capture':{'controller_overflow_counts':overflow,'helper_rows':helper_count,'helper_may_be_truncated':helper_count>=256},'observed_local_clock_intervals_seconds':{'notice_to_helper_submission':notice_submit,'request_to_positive_grant_receipt':positive_rtt,'root_request_to_first_grant':root_grant,'root_STOP_to_all_DONE':root_wave,'participant_GO_seen_to_DONE':controller_span},'clock_rule':'Compare event timestamps only within one rank/process or root service clock; cross-node monotonic clocks are not assumed synchronized. Missing or overwritten events do not support a latency claim.'}

def restoration_proof(text, parsed):
    # Benchmark hook IDs follow targets.txt's pressure_000..015 order.
    table=text.split('Detailed function statistics (call):')[-1].split('Function timing statistics')[0]
    counts={int(name):int(calls) for name,calls in re.findall(r'^\|\s*pressure_(\d+)\**\|\s*(\d+)\|',table,re.M)}
    done={};proofs=[]
    for event in parsed['events']:
        f=event['fields']
        if f.get('kind')=='2' and f.get('participant')=='1' and 'wave' in f and 'monotonic_ns' in f:done[int(f['wave'])]=int(f['monotonic_ns'])
        if f.get('kind')=='6' and all(k in f for k in ('completed_wave','hook_id','calls','monotonic_ns')):
            wave=int(f['completed_wave']);hook=int(f['hook_id']);baseline=int(f['calls']);at=int(f['monotonic_ns'])
            if wave>0 and wave in done and at>=done[wave] and counts.get(hook,-1)>baseline:
                proofs.append({'completed_wave':wave,'hook_id':hook,'done_ns':done[wave],'physical_restore_ns':at,'baseline_calls':baseline,'final_calls':counts[hook]})
    return proofs

def validate_meter(root,label,work,policy,global_mode):
    if work.get('exec_seconds') is None:raise ValueError('missing meter interval')
    for rank in range(int(work['config']['ranks'])):
        out=(root/label/f'rank-{rank}.out').read_text()
        er=re.findall(r'^exec_interval rank=(\d+) begin_ns=(\d+) end_ns=(\d+) seconds=([^\s]+)$',out,re.M)
        if len(er)!=1 or int(er[0][0])!=rank or int(er[0][1])<=0 or int(er[0][2])<int(er[0][1]) or not math.isfinite(float(er[0][3])) or not math.isclose(float(er[0][3]),(int(er[0][2])-int(er[0][1]))/1e9,abs_tol=1e-9,rel_tol=0):raise ValueError('invalid/missing rank meter interval')
        err=(root/label/f'rank-{rank}.err').read_text()
        expected=f'bench_exec_meter entry_ld_preload=empty entry_ld_audit=empty policy={policy} global={int(global_mode)}'
        if err.count(expected)!=1 or re.search(r'^PEAK done with: .*exec_meter',err,re.M):raise ValueError('missing native meter entry/policy proof')

def validate_final_runtime(glob,rank):
    activation=glob.get('activation',{})
    if activation.get('policy')!='prepare' or activation.get('targets')!='native-until-paid':raise ValueError('missing prepare/native-until-paid product path')
    admission=glob.get('initial_admission',{})
    if admission.get('bootstrap_finished')!=1 or admission.get('first_local_attach_count')!=0 or admission.get('first_paid_attach_count',-1)<0:raise ValueError('unproven paid initial target admission')
    prep=glob.get('preparation',{})
    if not prep or not math.isclose(prep['charged_startup_seconds'],sum(prep[k] for k in ('preparation_seconds','activation_seconds','bootstrap_seconds')),rel_tol=1e-7,abs_tol=1e-8):raise ValueError('startup charge does not match preparation+activation+bootstrap')
    service=glob.get('service',{})
    if service.get('startup_sealed')!=1:raise ValueError('missing broker-authenticated startup seal')
    if service.get('settlement')!=1 or service.get('broker_process_id',0)<=0 or service.get('parent_process_id',0)<=0 or service.get('broker_final_written') not in (0,1):raise ValueError('missing confirmed native broker service view')
    if glob.get('member',{}).get('cohort_finalized')!=1 or glob.get('shutdown_confirmed')!=1:raise ValueError('missing final cohort acknowledgement')
    if rank==0:
        broker=glob.get('native_broker',{})
        if service.get('broker_final_written')!=1 or broker.get('pid',0)!=service['broker_process_id'] or broker.get('all_final_replies_written')!=1 or broker.get('reap_confirmed')!=1 or service['broker_process_id']==service['parent_process_id']:raise ValueError('root broker is not independently confirmed/reaped')

def potential_calls(work):
    if 'potential_profiled_calls_by_rank' in work:return work['potential_profiled_calls_by_rank']
    return {int(row['rank']):int(row['potential_profiled_calls']) for row in work['phases']['phase']['ranks']}

def elapsed_sample(work,metric):
    if metric=='main_to_finalize':return work['process_seconds']
    if metric=='exec_to_finalize':return work['exec_seconds']
    if metric=='burst_total':return sum(work['phases'][name]['root_wall'] for name in ('burst_before','burst_after'))
    if metric=='native_total':return sum(work['phases'][name]['root_wall'] for name in ('native_before','native_between'))
    if metric=='measured_stages_total':return sum(phase['root_wall'] for phase in work['phases'].values())
    return work['phases']['phase']['root_wall']+(work['phases']['warmup']['root_wall'] if metric=='warmup_main' else 0)

def main():
    p=argparse.ArgumentParser();p.add_argument('results',type=Path);p.add_argument('--require-final-members',action='store_true');p.add_argument('--require-native-broker',action='store_true');p.add_argument('--require-diagnostics',action='store_true');p.add_argument('--set',choices=('small','matrix','funded','targeted','matrix22','final32','escrow_targeted','activation_probe','funded_recovery','final_candidate','final_matrix','all'),default='small');a=p.parse_args();cases={};errors={}
    matrix=tuple(sy+'-'+la for sy in ('allreduce','barrier','bcast','ring','iallreduce') for la in ('rotating','slack','critical','aligned'))
    funded=tuple('funded-'+sy+'-rotating' for sy in ('allreduce','barrier','bcast','ring','iallreduce'))
    funded+=tuple('funded-concurrent-'+sy+'-rotating' for sy in ('allreduce','barrier','bcast','ring','iallreduce'))
    matrix22=matrix+('bcast-rotating-repeat1','bcast-rotating-repeat2')
    activation_cases=tuple(policy+'-'+name for policy in ('immediate','post-init') for name in ('bcast-rotating','barrier-critical','barrier-aligned','funded-concurrent-allreduce-rotating','iallreduce-critical'))
    funded_recovery=tuple('funded-concurrent-'+sy+'-rotating' for sy in ('allreduce','barrier','bcast','ring','iallreduce'))+('earned-recovery-long',)
    selected={'final_matrix':matrix22,'funded_recovery':funded_recovery,'final_candidate':matrix22+funded_recovery,'activation_probe':activation_cases,'escrow_targeted':('bcast-rotating','barrier-critical','barrier-aligned','funded-concurrent-allreduce-rotating','iallreduce-critical'),'matrix22':matrix22,'final32':matrix22+funded,'targeted':('bcast-rotating','barrier-critical','barrier-aligned','funded-concurrent-allreduce-rotating'),'small':CASES,'matrix':matrix,'funded':funded,'all':matrix+funded}[a.set]
    for name in selected:
      try:
        root=a.results/name;long_case=name=='earned-recovery-long';loader=recovery_workload if long_case else workload;runs={l:loader(root,l) for l in LABELS};ref=runs['N0'];reports={}
        for l,r in runs.items():
            if int(r['config']['ranks'])!=32 or int(r['config']['threads'])!=6 or not r['affinity_unique'] or len(set(b['host'] for b in r['bindings'].values()))!=2:raise ValueError('invalid topology')
            if r['config']!=ref['config'] or r['scenario']!=ref['scenario'] or r['bindings']!=ref['bindings'] or any(r['phases'][s]['checksum']!=ref['phases'][s]['checksum'] for s in ref['phases']):raise ValueError('mismatched workload')
            if a.set=='activation_probe':validate_meter(root,l,r,'post-init' if name.startswith('post-init-') else 'immediate',l[0]=='G')
            elif a.set in ('funded_recovery','final_candidate','final_matrix'):validate_meter(root,l,r,'default',l[0]=='G')
            if l[0] not in 'LG':continue
            perrank=[]
            expected=potential_calls(r)
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
                    if a.require_final_members:
                        if glob['diagnostic_origin']!='final_settlement':raise ValueError('missing actual final settlement record')
                        if 'member' not in glob or 'cohort_finalized' not in glob['member']:raise ValueError('missing final cohort member diagnostics')
                        if glob['member']['cohort_finalized']!=1 or glob['shutdown_confirmed']!=1:raise ValueError('cohort settlement not confirmed')
                        if glob['member'].get('accounting_invalid',0)!=0:raise ValueError('final cohort accounting invalid')
                    boot=re.findall(r'MPI global bootstrap: ([^\n]+)',text)
                    if not boot:raise ValueError('missing global MPI bootstrap diagnostics')
                    bootstrap=dict(re.findall(r'(\w+)=([^\s]+)',boot[-1]))
                    if bootstrap.get('backend')!='tcp' or int(bootstrap.get('required_thread_level',-1))!=1 or int(bootstrap.get('provided_thread_level',-1))<1:raise ValueError('invalid FUNNELED bootstrap')
                    glob['bootstrap']=bootstrap
                    if a.set in ('funded_recovery','final_candidate','final_matrix'):validate_final_runtime(glob,rank)
                    if a.require_native_broker and rank==0:
                        broker=glob.get('native_broker',{})
                        if broker.get('pid',0)<=0 or broker.get('all_final_replies_written')!=1 or broker.get('reap_confirmed')!=1:raise ValueError('missing confirmed independent native broker')
                        entries=list((root/l).glob('entry-*.txt'))
                        parent=[int(re.search(r'pid=(\d+)',entry.read_text())[1]) for entry in entries if 'PMI_RANK=0 ' in entry.read_text()]
                        if len(parent)!=1 or broker['pid']==parent[0]:raise ValueError('broker is not an independent root process')

                parsed=timing_and_events(text)
                perrank.append({'local':local,'global':glob,'timing_events':parsed,'post_wave_restoration_proofs':restoration_proof(text,parsed)})
            if l[0]=='G' and a.set in ('funded_recovery','final_candidate','final_matrix') and len({r['global']['service']['broker_process_id'] for r in perrank})!=1:raise ValueError('ranks disagree on native broker PID')
            reports[l]=perrank
        groups={}
        for g,labels in {'native':('N0','N1'),'local':('L0','L1'),'global':('G0','G1')}.items():
            groups[g]={}
            for metric in (('burst_total','native_total','measured_stages_total','main_to_finalize','exec_to_finalize') if long_case else ('main','warmup_main','main_to_finalize','exec_to_finalize') if a.set in ('activation_probe','funded_recovery','final_candidate','final_matrix') else ('main','warmup_main','main_to_finalize')):
                samples=[elapsed_sample(runs[l],metric) for l in labels]
                groups[g][metric]={'samples':samples,'mean':statistics.mean(samples)}
        for g in groups.values():
            for metric,v in g.items():v['ratio_to_native']=v['mean']/groups['native'][metric]['mean']
        coord={l:{'rank0_last_view':reports[l][0]['global'],'view_epochs':[r['global'].get('view_epoch') for r in reports[l]],'ever_active_ranks':sum(bool(r['global'].get('ever_active')) for r in reports[l]),'profiled_targets_by_rank':[r['local']['profiled_targets'] for r in reports[l]],'local_allowance_by_rank':[r['global'].get('allowance_seconds',0) for r in reports[l]],'emergency_ranks':sum(bool(r['global'].get('observed_emergency_fallback')) for r in reports[l]),'runtime_degraded_ranks':sum(r['global'].get('runtime_degraded_samples',0)>0 for r in reports[l]),'invalid_accounting_ranks':sum(r['global'].get('invalid_accounting_samples',0)>0 for r in reports[l]),'settlement_confirmed_ranks':sum(bool(r['global'].get('settlement_confirmed')) for r in reports[l]),'positive_grant_ranks':sum(r['global'].get('granted_total_seconds',0)>0 for r in reports[l]),'reattached_targets':sum(r['local']['reattached'] for r in reports[l]),'recorded_calls':sum(r['local']['calls'] for r in reports[l]),'local_windows':sum(r['local']['windows'] for r in reports[l]),'potential_selected_calls':sum(potential_calls(runs[l]).values()),'selected_call_fraction':sum(r['local']['calls'] for r in reports[l])/sum(potential_calls(runs[l]).values()),'observed_demand_ranks':sum(value>0 for value in potential_calls(runs[l]).values()),'ordinary_emergency_free':all(r['global'].get('emergency_fallback_count',0)==0 for r in reports[l]),'restoration_observed':any(r['local']['reattached']>0 and r['local']['revisited']>0 for r in reports[l])} for l in ('G0','G1')}
        acceptance={
            'global_cumulative_work_mean_within_5pct':groups['global']['measured_stages_total' if long_case else 'warmup_main']['ratio_to_native']<=1.05,
            'global_main_through_finalize_mean_within_5pct':groups['global']['main_to_finalize']['ratio_to_native']<=1.05,
            'ordinary_emergency_free':all(coord[l]['ordinary_emergency_free'] for l in ('G0','G1')),
            'positive_credit_for_all_demand_ranks':all(all(reports[l][rank]['global'].get('granted_total_seconds',0)>0 for rank,value in potential_calls(runs[l]).items() if value>0) for l in ('G0','G1')),
            'coverage_not_below_local':sum(r['local']['profiled_targets'] for l in ('G0','G1') for r in reports[l])>=sum(r['local']['profiled_targets'] for l in ('L0','L1') for r in reports[l]),
            'any_restore_revisit_observed':any(coord[l]['restoration_observed'] for l in ('G0','G1')),
            'restoration_after_root_wave_requires_event_order_proof':True,
            'strict_recovery_proof_ranks':{l:sum(bool(r['post_wave_restoration_proofs']) for r in reports[l]) for l in ('G0','G1')},
            'interpretation':'Grant distribution and coverage comparisons diagnose utility, not universal budget correctness. Exhausted pools may deny a hot rank. Per-case restoration is not required when work ends; assess separate recovery diagnostics. Emergency counts require available-funding, healthy-view and deadline context. No combined pass is inferred from these flags.',
        }
        cases[name]={'acceptance_findings':acceptance,'performance':groups,'coordination_diagnostics':coord,'reports':reports,'workloads':runs}
      except (OSError,ValueError,KeyError) as e:errors[name]=str(e)
    diagnostics={}
    for name in (tuple(policy+'-'+name for policy in ('immediate','post-init') for name in ('diagnostic-short','diagnostic-funded')) if a.set=='activation_probe' else ('diagnostic-short','diagnostic-funded')):
        root=a.results/name
        if not root.exists():
            if a.require_diagnostics:errors[name]='missing requested diagnostic arm'
            continue
        try:
            work=workload(root,'D0');rows=[]
            if a.set=='activation_probe':validate_meter(root,'D0',work,'post-init' if name.startswith('post-init-') else 'immediate',True)
            elif a.set in ('final_candidate','final_matrix'):validate_meter(root,'D0',work,'default',True)
            for rank in range(32):
                text=(root/'D0'/f'rank-{rank}.err').read_text();parsed=timing_and_events(text)
                local=peak(text);glob=global_report(text)
                expected=int(work['phases']['phase']['ranks'][rank]['potential_profiled_calls'])
                if local['calls']>expected:raise ValueError('excess diagnostic observed calls')
                if glob['configured']!=1 or not glob['ever_active']:raise ValueError('diagnostic global transport never active')
                if a.require_final_members:
                    if glob['diagnostic_origin']!='final_settlement' or glob.get('member',{}).get('cohort_finalized')!=1 or glob['shutdown_confirmed']!=1:raise ValueError('diagnostic cohort settlement not confirmed')
                    if glob['member'].get('accounting_invalid',0)!=0:raise ValueError('diagnostic final cohort accounting invalid')
                if a.set in ('final_candidate','final_matrix'):validate_final_runtime(glob,rank)
                rows.append({'rank':rank,'local':local,'global':glob,'timing_events':parsed,'post_wave_restoration_proofs':restoration_proof(text,parsed)})
            if a.require_native_broker:
                broker=rows[0]['global'].get('native_broker',{})
                if broker.get('pid',0)<=0 or broker.get('all_final_replies_written')!=1 or broker.get('reap_confirmed')!=1:raise ValueError('diagnostic missing confirmed native broker')
            diagnostics[name]={'workload':work,'ranks':rows,'restoration_proof_ranks':sum(bool(r['post_wave_restoration_proofs']) for r in rows)}
        except (OSError,ValueError,KeyError) as e:errors[name]=str(e)
    pooled={}
    repeat_names=('bcast-rotating','bcast-rotating-repeat1','bcast-rotating-repeat2')
    if all(name in cases for name in repeat_names):
        repeat_ref=cases[repeat_names[0]]['workloads']['N0']
        for name in repeat_names[1:]:
            other=cases[name]['workloads']['N0']
            if any(other[key]!=repeat_ref[key] for key in ('config','scenario','bindings')) or any(other['phases'][phase]['checksum']!=repeat_ref['phases'][phase]['checksum'] for phase in ('warmup','phase')):
                errors['bcast-repeat-pairing']='repeated work/config/binding mismatch'
        # Keep all independently matched groups visible; pool all six samples equally.
        for group in ('native','local','global'):
            pooled[group]={}
            for metric in (('main','warmup_main','main_to_finalize','exec_to_finalize') if a.set in ('activation_probe','final_candidate','final_matrix') else ('main','warmup_main','main_to_finalize')):
                samples=[sample for name in repeat_names for sample in cases[name]['performance'][group][metric]['samples']]
                pooled[group][metric]={'samples':samples,'mean':statistics.mean(samples),'minimum':min(samples),'maximum':max(samples),'source_cases':list(repeat_names)}
        for group in pooled.values():
            for metric,row in group.items():row['ratio_to_native']=row['mean']/pooled['native'][metric]['mean']
    print(json.dumps({'bcast_rotating_six_sample_pooled':pooled,'diagnostics':diagnostics,'valid':not errors and len(cases)==len(selected),'errors':errors,'cases':cases,'note':'Final fresh=0 is expected after service shutdown. Do not sum snapshots from different epochs or ranks; use coordinator diagnostics as recorded. global_spent conservatively includes the sum of actual per-rank control costs. wave_span is a root GO-to-all-DONE diagnostic and does not replace or discount those summed charges. Do not sum snapshots from different epochs or substitute a max-rank window. allowance is rank-local unconsumed reservation, budget is job-wide. This is not a hard slowdown proof.'},indent=2));return bool(errors)
if __name__=='__main__':raise SystemExit(main())
