#!/usr/bin/env python3
"""Validate matched MPI synchronization runs; preserve rank imbalance metrics."""
import argparse,json,re,math
from pathlib import Path

def fields(s): return dict(re.findall(r'(\w+)=([^\s]+)',s))
def parse(root,label):
    if (root/f'{label}.rc').read_text().strip()!='0': raise ValueError('failed launcher receipt')
    text=(root/f'{label}.out').read_text()
    config=[fields(s) for s in text.splitlines() if s.startswith('config ')]
    scenario=[fields(s) for s in text.splitlines() if s.startswith('scenario ')]
    if len(config)!=1 or len(scenario)!=1: raise ValueError('missing config/scenario')
    c=config[0]; pt=int(c.get('profile_threads',1)); n=int(c['ranks']); nt=int(c['threads']); epochs=int(c['epochs']); calls=int(c['calls_per_thread_epoch']);wc=int(c['warmup_calls']);ww=int(c['warmup_work']);work=int(c['tiny_work']);layout=scenario[0]['layout']; phases={}
    if not 1<=pt<=nt:raise ValueError('invalid profile thread count')
    if int(c['expected_potential_calls'])!=n*nt*epochs*calls or int(c['expected_profiled_calls'])!=pt*epochs*calls or int(c['expected_warmup_calls'])!=n*nt*wc: raise ValueError('config operation counts mismatch')
    bindings={}
    for line in text.splitlines():
        if not line.startswith('affinity '): continue
        f=fields(line); rank=int(f['rank']); masks=re.findall(r't(\d+)=\[([0-9,]*)\]',line)
        if rank in bindings or len(masks)!=nt: raise ValueError('bad affinity record')
        bindings[rank]={'host':f['host'],'masks':masks}
    if sorted(bindings)!=list(range(n)): raise ValueError('missing affinity')
    byhost={}
    for r in bindings.values():
        for t,mask in r['masks']:
            byhost.setdefault(r['host'],[]).extend(int(v) for v in mask.split(',') if v)
    affinity_unique=all(len(v)==len(set(v)) for v in byhost.values()) and all(len(mask.split(','))==1 for r in bindings.values() for _,mask in r['masks'])
    for phase,name in enumerate(('warmup','phase')):
        headers=[fields(s) for s in text.splitlines() if s.startswith(name+' work=')]
        if len(headers)!=1 or headers[0].get('oracle_bad')!='0' or headers[0].get('mpi_bad')!='0': raise ValueError('oracle failure')
        rows=[fields(s) for s in text.splitlines() if s.startswith(f'rank_metric phase={phase} ')]
        if sorted(int(r['rank']) for r in rows)!=list(range(n)): raise ValueError('missing/duplicate rank metrics')
        if int(headers[0]['potential_target_calls'])!=n*nt*(epochs*calls if phase else wc) or int(headers[0]['profiled_target_calls'])!=(pt*epochs*calls if phase else 0) or int(headers[0]['collectives_per_rank'])!=(epochs if phase else 1): raise ValueError('phase operation counts mismatch')
        steps=0
        for row in rows:
            rank=int(row['rank']); expected_steps=0; expected_calls=0
            for e in range(epochs if phase else 1):
                hot=0 if layout in ('slack','aligned') else e%n
                slow=n-1 if layout=='slack' else hot
                w=work if phase else ww
                if phase and layout!='rotating' and rank==slow: w=512
                expected_steps+=nt*(calls if phase else wc)*w
                if phase and rank==hot: expected_calls+=pt*calls
            if int(row['lcg_steps'])!=expected_steps or int(row['potential_profiled_calls'])!=expected_calls: raise ValueError('rank work mismatch')
            steps+=expected_steps
            for key in ('wall','compute','cpu','sync','process_cpu','post','overlap','wait','completion_wait'):
                if not math.isfinite(float(row[key])) or float(row[key])<0: raise ValueError('bad timing')
        if int(headers[0]['lcg_steps'])!=steps: raise ValueError('total LCG steps mismatch')
        roots=[fields(s) for s in text.splitlines() if s.startswith(f'phase_root_wall phase={phase} ')]
        if len(roots)!=1 or not math.isfinite(float(roots[0]['seconds'])) or float(roots[0]['seconds'])<=0: raise ValueError('missing root wall')
        phases[name]={'checksum':headers[0]['checksum'],'root_wall':float(roots[0]['seconds']),'ranks':rows}
    final=re.findall(r'^process_seconds=(\S+) oracle=PASS$',text,re.M)
    if len(final)!=1 or not math.isfinite(float(final[0])) or float(final[0])<=0: raise ValueError('missing final PASS')
    if float(final[0])<sum(v['root_wall'] for v in phases.values()): raise ValueError('process shorter than measured phases')
    return {'config':config[0],'scenario':scenario[0],'phases':phases,'process_seconds':float(final[0]),'bindings':bindings,'affinity_unique':affinity_unique}

def main():
    p=argparse.ArgumentParser();p.add_argument('results',type=Path);p.add_argument('--labels',nargs='+',required=True);p.add_argument('--require-unique-affinity',action='store_true');a=p.parse_args()
    try:
        runs={label:parse(a.results,label) for label in a.labels};ref=next(iter(runs.values()))
        for r in runs.values():
            if a.require_unique_affinity and not r['affinity_unique']: raise ValueError('overlapping CPU binding')
            if r['config']!=ref['config'] or r['scenario']!=ref['scenario'] or r['bindings']!=ref['bindings'] or any(r['phases'][s]['checksum']!=ref['phases'][s]['checksum'] for s in ('warmup','phase')): raise ValueError('mismatched work')
        print(json.dumps({'valid':True,'runs':runs},indent=2));return 0
    except (ValueError,KeyError,OSError) as e: print(json.dumps({'valid':False,'error':str(e)}));return 1
if __name__=='__main__': raise SystemExit(main())
