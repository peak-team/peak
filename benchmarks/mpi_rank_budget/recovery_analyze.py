#!/usr/bin/env python3
"""Independent fixed-work oracle for the four-stage earned-recovery fixture."""
import math
import re
from pathlib import Path
from sync_analyze import fields

MASK=(1<<64)-1
A=6364136223846793005
B=1442695040888963407
STAGES=('native_before','burst_before','native_between','burst_after')

def advance(x,a,b,n):
    # Binary exponentiation of the affine map, using unbounded Python integers.
    while n:
        if n&1:x=(a*x+b)&MASK
        b=(a*b+b)&MASK
        a=(a*a)&MASK
        n>>=1
    return x

def cycle(x,work):
    for target in range(16):
        for _ in range(work):x=(A*x+B+target)&MASK
    return x

def parse(root,label):
    if (root/f'{label}.rc').read_text().strip()!='0':raise ValueError('failed launcher receipt')
    text=(root/label/'rank-0.out').read_text()
    configs=[fields(line) for line in text.splitlines() if line.startswith('recovery_config ')]
    if len(configs)!=1:raise ValueError('missing recovery config')
    c=configs[0];n=int(c['ranks']);nt=int(c['threads']);pt=int(c['profile_threads']);work=int(c['native_work']);rounds=int(c['burst_rounds']);before=int(c['native_before_calls']);between=int(c['native_between_calls']);burst_work=int(c['burst_work'])
    if min(n,nt,pt,work,rounds,before,between,burst_work)<=0 or pt>nt or c['targets']!='16' or c['exported_targets']!='256':raise ValueError('invalid recovery configuration')
    bindings={};cpus={}
    for line in text.splitlines():
        if not line.startswith('affinity '):continue
        f=fields(line);rank=int(f['rank']);masks=re.findall(r't(\d+)=\[([0-9,]*)\]',line)
        if rank in bindings or len(masks)!=nt or sorted(int(t) for t,_ in masks)!=list(range(nt)):raise ValueError('bad recovery affinity')
        bindings[rank]={'host':f['host'],'masks':masks}
        for _,mask in masks:cpus.setdefault(f['host'],[]).extend(int(v) for v in mask.split(',') if v)
    if sorted(bindings)!=list(range(n)):raise ValueError('missing recovery affinity')
    unique=all(len(v)==len(set(v)) for v in cpus.values()) and all(len(mask.split(','))==1 for r in bindings.values() for _,mask in r['masks'])
    state={(r,t):(0x12345678+r*100003+t)&MASK for r in range(n) for t in range(nt)}
    phases={};b=cycle(0,burst_work);a=(cycle(1,burst_work)-b)&MASK
    for stage,name in enumerate(STAGES):
        header=[fields(line) for line in text.splitlines() if line.startswith(f'recovery_stage stage={stage} ')]
        rows=[fields(line) for line in text.splitlines() if line.startswith(f'recovery_thread stage={stage} ')]
        ranks=[fields(line) for line in text.splitlines() if line.startswith(f'recovery_rank stage={stage} ')]
        if len(header)!=1 or header[0]['name']!=name or header[0]['oracle_bad']!='0':raise ValueError('recovery stage oracle failure')
        h=header[0];burst=bool(stage%2);calls=rounds*16 if burst else (before if stage==0 else between);steps=calls*burst_work if burst else calls*work
        if int(h['total_calls'])!=n*nt*calls or int(h['total_steps'])!=n*nt*steps or int(h['potential_profiled_calls'])!=(n*pt*calls if burst else 0):raise ValueError('recovery stage count mismatch')
        if sorted((int(r['rank']),int(r['tid'])) for r in rows)!=sorted(state):raise ValueError('missing/duplicate recovery thread')
        total=0
        for row in rows:
            key=(int(row['rank']),int(row['tid']))
            expected=advance(state[key],a,b,rounds) if burst else advance(state[key],A,B,steps)
            if int(row['checksum'],16)!=expected or int(row['oracle'],16)!=expected:raise ValueError('independent recovery oracle mismatch')
            if int(row['calls'])!=calls or int(row['lcg_steps'])!=steps or int(row['potential_profiled_calls'])!=(calls if burst and key[1]<pt else 0):raise ValueError('recovery thread count mismatch')
            for k in ('compute','cpu'):
                if not math.isfinite(float(row[k])) or float(row[k])<0:raise ValueError('invalid recovery thread timing')
            state[key]=expected;total^=expected
        if int(h['checksum'],16)!=total:raise ValueError('recovery checksum mismatch')
        if sorted(int(r['rank']) for r in ranks)!=list(range(n)):raise ValueError('missing/duplicate recovery rank')
        for row in ranks:
            for k in ('wall','completion_wait','process_cpu'):
                if not math.isfinite(float(row[k])) or float(row[k])<0:raise ValueError('invalid recovery rank timing')
        wall=float(h['root_wall'])
        if not math.isfinite(wall) or wall<=0:raise ValueError('invalid recovery root wall')
        phases[name]={'root_wall':wall,'checksum':h['checksum'],'threads':rows,'ranks':ranks}
    finals=re.findall(r'^process_seconds=(\S+) oracle=PASS$',text,re.M)
    if len(finals)!=1:raise ValueError('missing recovery final PASS')
    seconds=float(finals[0])
    if not math.isfinite(seconds) or seconds<sum(p['root_wall'] for p in phases.values()):raise ValueError('invalid recovery main interval')
    es=[fields(line) for line in text.splitlines() if line.startswith('exec_interval rank=0 ')]
    if len(es)!=1:raise ValueError('missing recovery meter interval')
    e=es[0];duration=float(e['seconds']);start=int(e['begin_ns']);end=int(e['end_ns'])
    if start<=0 or end<start or not math.isfinite(duration) or not math.isclose(duration,(end-start)/1e9,abs_tol=1e-9,rel_tol=0) or duration+1e-6<seconds:raise ValueError('invalid recovery exec interval')
    return {'config':c,'scenario':{'name':'earned-recovery-long','sync':'barrier','layout':'all-ranks'},'phases':phases,'process_seconds':seconds,'exec_seconds':duration,'bindings':bindings,'affinity_unique':unique,'potential_profiled_calls_by_rank':{rank:2*rounds*16*pt for rank in range(n)}}
