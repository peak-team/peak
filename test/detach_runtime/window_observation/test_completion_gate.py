"""Pure receipt logic only. Never launch or terminate a process."""
import copy
import unittest
from unittest.mock import patch
import bounded_observation as runner

GOOD = dict(collector_pid=100, leader_pid=101,leader_returncode=0, deadline_exceeded=False, children_reaped=True,
            adopted_wait_events=[{'pid': 123, 'status': 0}], signal_attempts=[], collector_errors=[])

class CompletionGate(unittest.TestCase):
    def test_complete_receipt(self):
        self.assertTrue(runner.qualifies(GOOD))
    def test_no_child_is_allowed_only_after_echild(self):
        row=copy.deepcopy(GOOD);row['adopted_wait_events']=[]
        self.assertTrue(runner.qualifies(row))
        row['children_reaped']=False;self.assertFalse(runner.qualifies(row))
    def test_every_missing_required_field(self):
        for key in GOOD:
            row=copy.deepcopy(GOOD);row.pop(key)
            with self.subTest(key=key):self.assertFalse(runner.qualifies(row))
    def test_failures_remain_failures(self):
        for key,value in [('leader_returncode',-9),('leader_returncode',None),
                          ('leader_returncode',1),('deadline_exceeded',True),
                          ('children_reaped',False),('collector_errors',['error']),
                          ('signal_attempts',[{'signal':9}]),
                          ('adopted_wait_events',[{'pid':123,'status':9}]),
                          ('adopted_wait_events',[{'pid':123,'status':256}]),
                          ('adopted_wait_events',[{'pid':123}]),
                          ('adopted_wait_events',[{'status':0}]),
                          ('adopted_wait_events',[{'pid':101,'status':0}]),
                          ('adopted_wait_events',[{'pid':123,'status':0}]*2),
                          ('collector_pid',None),('leader_pid',0)]:
            row=copy.deepcopy(GOOD);row[key]=value
            with self.subTest(key=key,value=value):self.assertFalse(runner.qualifies(row))
    def test_gate_has_no_process_side_effect(self):
        with patch.object(runner.subprocess,'Popen',side_effect=AssertionError('no launch')):
            for _ in range(3):self.assertTrue(runner.qualifies(GOOD))
    def test_finite_deadlines_without_process_actions(self):
        runner.validate_deadlines(20,3)
        runner.validate_deadlines(1,0)
        for timeout,grace in [(0,3),(-1,3),(20,-1),(float('nan'),3),
                              (float('inf'),3),(20,float('nan')),(20,float('inf'))]:
            with self.subTest(timeout=timeout,grace=grace):
                with self.assertRaises(ValueError):runner.validate_deadlines(timeout,grace)

if __name__=='__main__':unittest.main()
