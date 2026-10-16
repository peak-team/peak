"""Dedicated local observation collector: never signal the target or descendants.

Run only in a fresh, single-child collector process. A missed deadline or child
that fails to finish is recorded as failure and handed to the owner, never killed
or converted to success. This is not the general SSH/transport timeout runner.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time


def qualifies(row):
    """Pure receipt gate. Absent evidence never becomes a successful zero."""
    required = {'collector_pid', 'leader_pid', 'leader_returncode', 'deadline_exceeded', 'children_reaped',
                'adopted_wait_events', 'signal_attempts', 'collector_errors'}
    if not required.issubset(row):
        return False
    return (all(type(row[key]) is int and row[key] > 0 for key in ['collector_pid','leader_pid'])
            and row['collector_pid'] != row['leader_pid']
            and type(row['leader_returncode']) is int and row['leader_returncode'] == 0
            and row['deadline_exceeded'] is False and row['children_reaped'] is True
            and row['signal_attempts'] == [] and row['collector_errors'] == []
            and isinstance(row['adopted_wait_events'], list)
            and all(isinstance(event, dict) and type(event.get('pid')) is int
                    and event['pid'] > 0 and event['pid'] not in [row['collector_pid'],row['leader_pid']]
                    and type(event.get('status')) is int
                    and os.WIFEXITED(event['status'])
                    and os.WEXITSTATUS(event['status']) == 0
                    for event in row['adopted_wait_events'])
            and len({event['pid'] for event in row['adopted_wait_events']}) == len(row['adopted_wait_events']))


def validate_deadlines(timeout, reap_grace):
    if (not math.isfinite(timeout) or not math.isfinite(reap_grace)
            or timeout <= 0 or reap_grace < 0):
        raise ValueError('invalid deadline')


def collect(cmd, directory, timeout=20, reap_grace=3, env=None):
    validate_deadlines(timeout, reap_grace)
    # A dedicated process avoids reaping unrelated children with waitpid(-1).
    children_path = Path('/proc/self/task/{}/children'.format(os.getpid()))
    if children_path.read_text().strip():
        raise RuntimeError('collector must start with no existing children')
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_SET_CHILD_SUBREAPER failed')
    directory = Path(directory)
    directory.mkdir(exist_ok=False)
    row = {'collector_pid': os.getpid(), 'argv': list(cmd),
           'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'timeout_s': timeout, 'reap_grace_s': reap_grace,
           'leader_pid': None, 'leader_returncode': None, 'deadline_exceeded': False,
           'children_reaped': False, 'adopted_wait_events': [],
           'signal_attempts': [], 'collector_errors': [], 'output_final': False}
    # Direct files avoid pipe EOF from a surviving descendant hiding leader exit.
    with (directory/'stdout').open('xb') as stdout, (directory/'stderr').open('xb') as stderr:
        try:
            process = subprocess.Popen(cmd, stdout=stdout, stderr=stderr,
                                       stdin=subprocess.DEVNULL,
                                       start_new_session=True, env=env)
            row['leader_pid'] = process.pid
            (directory/'START.json').write_text(json.dumps(row, indent=2)+'\n')
            try:
                row['leader_returncode'] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                row['deadline_exceeded'] = True  # latched, even if exit races poll
                row['leader_returncode'] = process.poll()
            if row['leader_returncode'] is not None:
                end = time.monotonic() + reap_grace
                while True:
                    try:
                        pid, status = os.waitpid(-1, os.WNOHANG)
                    except ChildProcessError:
                        row['children_reaped'] = True
                        break
                    if pid > 0:
                        row['adopted_wait_events'].append({'pid': pid, 'status': status})
                    elif time.monotonic() >= end:
                        break
                    else:
                        time.sleep(.02)
        except Exception as error:
            row['collector_errors'].append(repr(error))
    row['output_final'] = row['children_reaped'] and row['leader_returncode'] is not None
    row['completion_qualified'] = qualifies(row)
    row['state'] = ('NATURAL_COMPLETION' if row['completion_qualified']
                    else 'FAIL_UNQUALIFIED_NO_SIGNAL_SENT')
    row['next_condition'] = ('Analyze immutable output; no automatic rerun.'
        if row['output_final'] else
        'Owner must follow unfinished exact PID/descendants; no completion claim, retry or automatic termination.')
    if row['output_final']:
        row['output_sha256'] = {name: hashlib.sha256((directory/name).read_bytes()).hexdigest()
                                for name in ['stdout', 'stderr']}
    row['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (directory/'RESULT.json').write_text(json.dumps(row, indent=2)+'\n')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--timeout', type=float, default=20)
    parser.add_argument('--reap-grace', type=float, default=3)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    cmd = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not cmd:
        parser.error('an exact command is required')
    row = collect(cmd, args.output_dir, args.timeout, args.reap_grace)
    print(json.dumps(row, indent=2))
    return 0 if row['completion_qualified'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
