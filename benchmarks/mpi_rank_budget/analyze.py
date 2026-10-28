#!/usr/bin/env python3
"""Validate six completed runs, then compare directly measured main wall time."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def parse(root, label):
    text = (root / (label + '.out')).read_text()
    # Receipts must be recorded by the launcher after wait/timeout returns.
    if (root / (label + '.rc')).read_text().strip() != '0':
        raise ValueError('launcher failed or timed out')
    configs = re.findall(r'config ranks=.+$', text, re.M)
    if len(configs) != 1:
        raise ValueError('expected exactly one config')
    c = {k: int(v) for k, v in fields(configs[0]).items()}
    ranks, threads, epochs, calls = [c[k] for k in
                                    ('ranks', 'threads', 'epochs', 'calls_per_thread_epoch')]
    work, wc, ww = [c[k] for k in ('tiny_work', 'warmup_calls', 'warmup_work')]
    if min(ranks, threads, epochs, calls, work, wc, ww) <= 0:
        raise ValueError('nonpositive workload')
    expected_config = dict(medium_work=work, rotate_epochs=ranks, sparse_cadence=1,
                           layout=3, targets=16, hotset=1, exported_targets=256,
                           expected_potential_calls=ranks*threads*epochs*calls,
                           expected_profiled_calls=epochs*calls,
                           expected_warmup_calls=ranks*threads*wc)
    if any(c.get(k) != v for k, v in expected_config.items()):
        raise ValueError('inconsistent config/call counts')
    records = list(re.finditer(r'^(warmup|phase) work=.+$', text, re.M))
    if [m[1] for m in records] != ['warmup', 'phase']:
        raise ValueError('expected ordered warmup and phase records')
    sections = {}
    for i, match in enumerate(records):
        name = match[1]
        f = fields(match[0])
        n = ranks*threads*(wc if name == 'warmup' else epochs*calls)
        w = ww if name == 'warmup' else work
        expected = dict(work=w, cadence=1, oracle_bad=0, mpi_bad=0,
                        potential_target_calls=n,
                        profiled_target_calls=0 if name == 'warmup' else epochs*calls,
                        lcg_steps=n*w, collectives_per_rank=1 if name == 'warmup' else epochs)
        if any(int(f.get(k, '-1')) != v for k, v in expected.items()):
            raise ValueError(name + ': failed oracle/MPI check or incorrect counts')
        chunk = text[match.end():records[i+1].start() if i+1 < len(records) else len(text)]
        metrics = {}
        for metric, lo, mean, hi in re.findall(
                r'^\s*(\w+) rank_min=(\S+) rank_mean=(\S+) rank_max=(\S+)', chunk, re.M):
            values = list(map(float, (lo, mean, hi)))
            if not all(math.isfinite(x) and x >= 0 for x in values) or not values[0] <= values[1] <= values[2]:
                raise ValueError(name + ': invalid elapsed metric ' + metric)
            metrics[metric] = dict(zip(('min', 'mean', 'max'), values))
        required = ('wall_seconds', 'thread_mean_compute_seconds', 'thread_mean_compute_cpu_seconds',
                    'process_user_plus_system_cpu_seconds', 'thread_mean_pre_collective_barrier_seconds',
                    'thread_mean_collective_release_barrier_seconds_OVERLAPS_MPI', 'master_mpi_allreduce_seconds')
        if any(k not in metrics for k in required) or metrics['wall_seconds']['min'] <= 0:
            raise ValueError(name + ': missing/nonpositive elapsed time')
        if not re.fullmatch(r'[0-9a-f]{16}', f.get('checksum', '')):
            raise ValueError(name + ': missing checksum')
        sections[name] = dict(checksum=f['checksum'], metrics=metrics)
    finals = re.findall(r'^process_seconds=(\S+) oracle=(\w+)$', text, re.M)
    if len(finals) != 1 or finals[0][1] != 'PASS':
        raise ValueError('missing/failing final oracle')
    elapsed = float(finals[0][0])
    if not math.isfinite(elapsed) or elapsed < sum(s['metrics']['wall_seconds']['mean'] for s in sections.values()):
        raise ValueError('invalid process elapsed time')
    return dict(config=c, sections=sections, process_seconds=elapsed)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('results', type=Path)
    p.add_argument('--labels', nargs=6, default=['N0', 'A0', 'B0', 'A1', 'B1', 'N1'],
                   help='execution order: native baseline candidate baseline candidate native')
    args = p.parse_args()
    runs, errors = {}, {}
    for label in args.labels:
        try:
            runs[label] = parse(args.results, label)
        except (OSError, ValueError, KeyError) as exc:
            errors[label] = str(exc)
    if not errors:
        reference = runs[args.labels[0]]
        for label, run in runs.items():
            if run['config'] != reference['config'] or any(
                    run['sections'][s]['checksum'] != reference['sections'][s]['checksum']
                    for s in ('warmup', 'phase')):
                errors[label] = 'workload or checksum differs across runs'
    if errors:
        print(json.dumps(dict(valid=False, errors=errors), indent=2))
        return 1
    groups = dict(native=[args.labels[i] for i in (0, 5)],
                  baseline=[args.labels[i] for i in (1, 3)],
                  candidate=[args.labels[i] for i in (2, 4)])
    summary = {}
    for name, labels in groups.items():
        samples = [runs[label]['sections']['phase']['metrics']['wall_seconds']['mean'] for label in labels]
        summary[name] = dict(main_wall_rank_mean_samples=samples,
                             main_wall_rank_mean=statistics.mean(samples))
    native = summary['native']['main_wall_rank_mean']
    for group in summary.values():
        group['main_wall_ratio_to_native'] = group['main_wall_rank_mean']/native
    print(json.dumps(dict(valid=True, summary=summary, runs=runs,
                         note='Two samples are descriptive only. Measured main wall time is used directly; '
                              'rank maxima are never combined. No strict slowdown guarantee or statistical significance claim.'), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
