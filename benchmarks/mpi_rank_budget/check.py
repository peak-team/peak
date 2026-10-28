#!/usr/bin/env python3
"""Check the exact exported symbol set and first 16 x86 instructions."""
import subprocess,sys,re
from pathlib import Path
binary=sys.argv[1]
expected={'pressure_'+name for name in re.findall(r'TARGET\((\d+),',Path(__file__).with_name('targets.inc').read_text())}
symbols=subprocess.check_output(['nm','-D','--defined-only',binary],text=True)
actual={line.split()[-1] for line in symbols.splitlines() if line.split()[-1].startswith('pressure_')}
assert actual==expected,(expected-actual,actual-expected)
addresses={line.split()[0] for line in symbols.splitlines() if line.split()[-1] in expected}
assert len(addresses)==256,"folded target function addresses"
dis=subprocess.check_output(['objdump','-d','--no-show-raw-insn',binary],text=True)
for name in sorted(expected):
    match=re.search(r'<'+name+r'>:\n(.*?)(?=\n\n|\Z)',dis,re.S)
    assert match,name
    instructions=[s.split(':',1)[1].strip() for s in match[1].splitlines() if ':' in s]
    assert len(instructions)>=16 and all(s=='nop' for s in instructions[:16]),(name,instructions[:16])
print('PASS: 256 unique exported targets, each starts with sixteen single-byte x86 NOPs')
