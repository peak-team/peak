#!/usr/bin/env python3
"""Prepare the hash-pinned Gum relocator with PEAK's checked range analysis."""
import argparse
import hashlib
import re
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--source', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--arch', required=True, choices=('arm64', 'x86'))
p.add_argument('--sha256', required=True)
a = p.parse_args()
raw = a.source.read_bytes()
if hashlib.sha256(raw).hexdigest() != a.sha256:
    p.error('pinned Frida Gum relocator source SHA256 mismatch')
s = raw.decode('utf-8')
name = 'gum_' + a.arch + '_relocator_can_relocate'
# Match a definition, not a call/prototype. The pinned digest locks its exact syntax.
pattern = r'(?m)^gboolean\n' + name + r'\s*\('
matches = list(re.finditer(pattern, s))
if len(matches) != 1:
    p.error('expected exactly one public can_relocate definition')
m = matches[0]
s = s[:m.start()] + 'static gboolean\npeak_' + a.arch + '_legacy_can_relocate (' + s[m.end():]
# Private headers are represented by a version-specific ABI compatibility header.
s, count = re.subn(r'^#include "[^"]+"\s*$', '', s, flags=re.M)
if count != {'arm64': 2, 'x86': 4}[a.arch]:
    p.error('unexpected pinned private include count')
s = '#include "gum' + a.arch + 'relocator-compat.h"\n' + s
s += '\n#include "' + a.arch + '-relocator-range.inc"\n'
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(s)
