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
def replace_once(text, old, new):
    if text.count(old) != 1:
        p.error('unexpected pinned ARM source anchor: ' + repr(old[:64]))
    return text.replace(old, new, 1)

if a.arch == 'arm64':
    if a.sha256 != '94ce96c7783d944443c37682dbd51c71b81829e9af0cd89b84075abc01a90f54':
        p.error('unsupported pinned ARM relocator digest')
    s = replace_once(s, '  guint reloc_bytes;\n\n  buf = g_alloca',
        '  guint reloc_bytes;\n'
        '  gboolean peak_check_prefix = scenario == GUM_SCENARIO_ONLINE &&\n'
        '      policy == GUM_RELOCATION_CHECKED;\n'
        '  guint peak_readable = peak_check_prefix\n'
        '      ? peak_arm64_readable_prefix (address, min_bytes) : 0;\n\n  buf = g_alloca')
    s = replace_once(s, '    reloc_bytes = gum_arm64_relocator_read_one (&rl, &insn);',
        '    if (peak_check_prefix && n >= peak_readable) break;\n'
        '    reloc_bytes = gum_arm64_relocator_read_one (&rl, &insn);')
    s = replace_once(s, '  if (policy == GUM_RELOCATION_CHECKED && !rl.eoi)',
        '  if (policy == GUM_RELOCATION_CHECKED && n != 0 &&\n'
        '      (!rl.eoi || (peak_check_prefix && peak_arm64_prefix_has_roots (&rl, n))))')
    s = replace_once(s, '    GHashTableIter iter;\n\n    checked_targets',
        '    GHashTableIter iter;\n'
        '    gboolean peak_prefix_phase = FALSE;\n'
        '    gboolean peak_prefix_incomplete = FALSE;\n'
        '    gboolean peak_has_prefix_roots = peak_check_prefix &&\n'
        '        peak_arm64_prefix_has_roots (&rl, n);\n'
        '    PeakArm64CodeRead peak_read = { 0 };\n\n    checked_targets')
    s = replace_once(s, '    current_code = rl.input_cur;',
        '    current_code = rl.eoi ? NULL : rl.input_cur;')
    s = replace_once(s, '      gboolean carry_on = TRUE;\n\n'
        '      g_hash_table_add (checked_targets, (gpointer) current_code);',
        '      gboolean carry_on = TRUE;\n\n'
        '      /* Drain the original suffix worklist before adding prefix roots. */\n'
        '      if (current_code == NULL)\n'
        '      {\n'
        '        peak_prefix_phase = TRUE;\n'
        '        peak_arm64_queue_prefix_roots (&rl, n, checked_targets, targets_to_check);\n'
        '        g_hash_table_iter_init (&iter, targets_to_check);\n'
        '        if (!g_hash_table_iter_next (&iter, &target, NULL)) break;\n'
        '        current_code = target;\n'
        '        current_address = GUM_ADDRESS (target);\n'
        '        g_hash_table_iter_remove (&iter);\n'
        '      }\n\n'
        '      g_hash_table_add (checked_targets, (gpointer) current_code);')
    s = replace_once(s, '      gum_ensure_code_readable (current_code, current_code_size);',
        '      peak_arm64_begin_code_read (&peak_read, current_code, current_code_size);')
    s = replace_once(s, '      while (carry_on && cs_disasm_iter (capstone, &current_code,\n'
        '          &current_code_size, &current_address, insn))',
        '      while (carry_on && peak_arm64_read_instruction (&peak_read, capstone,\n'
        '          &current_code, &current_code_size, &current_address, insn))')
    s = replace_once(s, '      g_hash_table_iter_init (&iter, targets_to_check);\n'
        '      if (g_hash_table_iter_next (&iter, &target, NULL))',
        '      if (carry_on) peak_prefix_incomplete = TRUE;\n\n'
        '      g_hash_table_iter_init (&iter, targets_to_check);\n'
        '      if (g_hash_table_iter_next (&iter, &target, NULL))')
    s = replace_once(s, '    while (current_code != NULL);',
        '    while (current_code != NULL || (peak_check_prefix && !peak_prefix_phase));')
    s = replace_once(s, '    cs_free (insn, 1);\n\n    cs_close (&capstone);',
        '    if (peak_has_prefix_roots && peak_prefix_incomplete) n = MIN (n, 4);\n'
        '    g_free (peak_read.bytes);\n\n'
        '    cs_free (insn, 1);\n\n    cs_close (&capstone);')
else:
    name = 'gum_x86_relocator_can_relocate'
    # Match a definition, not a call/prototype. The pinned digest locks its syntax.
    pattern = r'(?m)^gboolean\n' + name + r'\s*\('
    matches = list(re.finditer(pattern, s))
    if len(matches) != 1:
        p.error('expected exactly one public can_relocate definition')
    m = matches[0]
    s = s[:m.start()] + 'static gboolean\npeak_x86_legacy_can_relocate (' + s[m.end():]
# Private headers are represented by a version-specific ABI compatibility header.
s, count = re.subn(r'^#include "[^"]+"\s*$', '', s, flags=re.M)
if count != {'arm64': 2, 'x86': 4}[a.arch]:
    p.error('unexpected pinned private include count')
s = '#include "gum' + a.arch + 'relocator-compat.h"\n' + s
if a.arch == 'arm64':
    s = s.replace('#include "gumarm64relocator-compat.h"\n',
        '#include "gumarm64relocator-compat.h"\n#include "arm64-relocator-range.inc"\n', 1)
else:
    s += '\n#include "x86-relocator-range.inc"\n'
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(s)
