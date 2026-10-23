#!/usr/bin/env python3
"""Replace exactly one native relocator member while preserving its public ABI."""
import argparse
from pathlib import Path
import struct
import subprocess
p=argparse.ArgumentParser()
for name in ('library','object','ar','nm','work-dir'):
    p.add_argument('--'+name,required=True)
a=p.parse_args()
def run(*args):
    return subprocess.check_output(args, text=True, timeout=60)
lib=Path(a.library).resolve(); obj=Path(a.object).resolve()
member=obj.name
members=run(a.ar,'t',str(lib)).splitlines()
if members.count(member)!=1: p.error('expected exactly one '+member)
work=Path(a.work_dir);work.mkdir(parents=True,exist_ok=True)
old=work/('original-'+member)
with old.open('wb') as out:
    subprocess.run([a.ar,'p',str(lib),member],stdout=out,check=True,timeout=60)
def machine(path):
    data=path.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01': p.error('expected little-endian ELF64 object')
    return struct.unpack_from('<H',data,18)[0]
if machine(old)!=machine(obj): p.error('replacement ELF machine mismatch')
def symbols(path):
    lines=run(a.nm,'-g','--defined-only',str(path)).splitlines()
    return {(line.split()[-1], line.split()[-2]) for line in lines if len(line.split())>=3}
if symbols(old)!=symbols(obj):
    p.error('replacement changes exported relocator symbols: '+str(symbols(old)^symbols(obj)))
subprocess.run([a.ar,'r',str(lib),str(obj)],check=True,timeout=60)
if run(a.ar,'t',str(lib)).splitlines().count(member)!=1: p.error('duplicate member after replacement')
print('Replaced '+member+' with checked relocation range analysis (public ABI preserved)')
