#!/usr/bin/env python3
"""Saved-log validation tests; these fixtures are not runtime/performance evidence."""

import unittest

from decode_window_observation import decode


VALID = """WINDOW schema=1 rank=0 start_ns=100 end_ns=1000 elapsed_ns=900
WINDOW_API schema=1 rank=0 index=0 kind=1 arg=0 wall0=110 cpu0=10 cpu1=20 wall1=130 result=1 errors=0
WINDOW_API schema=1 rank=0 index=1 kind=3 arg=1000 wall0=140 cpu0=30 cpu1=50 wall1=200 result=1 errors=0
WINDOW_API schema=1 rank=0 index=2 kind=2 arg=0 wall0=300 cpu0=60 cpu1=70 wall1=330 result=1 errors=0
WINDOW_READER_CPU schema=1 rank=0 reader=0 cut=0 initialized=1 id_rc=0 lo=90 cpu=10 hi=95 rc=0 lo_rc=0 hi_rc=0
WINDOW_READER_CPU schema=1 rank=0 reader=1 cut=0 initialized=1 id_rc=0 lo=96 cpu=20 hi=99 rc=0 lo_rc=0 hi_rc=0
WINDOW_READER_CPU schema=1 rank=0 reader=0 cut=1 initialized=1 id_rc=0 lo=1001 cpu=510 hi=1003 rc=0 lo_rc=0 hi_rc=0
WINDOW_READER_CPU schema=1 rank=0 reader=1 cut=1 initialized=1 id_rc=0 lo=1004 cpu=620 hi=1006 rc=0 lo_rc=0 hi_rc=0
WINDOW_OBSERVE_END schema=1 rank=0 api_count=3 dropped=0
"""


class DecodeTest(unittest.TestCase):
    def check_invalid(self, text, counts=None):
        result = decode(text, 0, counts or {1: 1, 2: 1, 3: 1})
        self.assertFalse(result["valid"])
        self.assertTrue(result["errors"])
        self.assertIsNone(result["measurements"])

    def test_complete(self):
        result = decode(VALID, 0, {1: 1, 2: 1, 3: 1})
        self.assertTrue(result["valid"], result)
        values = result["measurements"]
        self.assertEqual(values["window_ns"], 900)
        self.assertEqual(values["api_clipped_outer_wall_union_ns"], 110)
        self.assertEqual(values["window_outside_api_union_ns"], 790)
        self.assertEqual(values["api_inner_cpu_sum_ns"], 40)
        self.assertEqual([r["sample_cpu_delta_ns"] for r in values["reader_samples"]],
                         [500, 600])

    def test_each_missing_line(self):
        lines = VALID.splitlines()
        for index in range(len(lines)):
            with self.subTest(line=index):
                self.check_invalid("\n".join(lines[:index] + lines[index + 1:]))

    def test_every_missing_field(self):
        lines = VALID.splitlines()
        for index, line in enumerate(lines):
            tokens = line.split()
            for field in range(1, len(tokens)):
                with self.subTest(line=index, field=tokens[field]):
                    broken = lines[:]
                    broken[index] = " ".join(tokens[:field] + tokens[field + 1:])
                    self.check_invalid("\n".join(broken))

    def test_every_duplicate_line(self):
        for line in VALID.splitlines():
            with self.subTest(line=line):
                self.check_invalid(VALID + line)

    def test_bad_values(self):
        replacements = [
            ("schema=1", "schema=2"), ("schema=1", "schema=x"),
            ("index=1", "index=0"), ("kind=2", "kind=4"),
            ("arg=1000", "arg=-1"), ("cpu0=10", "cpu0=-1"),
            ("cpu1=20", "cpu1=9"), ("wall0=110", "wall0=131"),
            ("cpu0=30", "cpu0=1"),
            ("wall0=140", "wall0=120"), ("result=1", "result=0"),
            ("errors=0", "errors=4"), ("initialized=1", "initialized=0"),
            ("id_rc=0", "id_rc=3"), ("hi_rc=0", "hi_rc=-1"),
            ("lo_rc=0", "lo_rc=-1"), (" rc=0", " rc=-1"),
            ("lo=90", "lo=96"), ("hi=95", "hi=101"),
            ("lo=1001", "lo=999"), ("cpu=510", "cpu=9"),
            ("reader=1", "reader=2"), ("cut=1", "cut=2"),
            ("elapsed_ns=900", "elapsed_ns=899"),
            ("api_count=3", "api_count=2"), ("dropped=0", "dropped=1"),
            ("index=0", "index=0 index=0"), ("index=0", "index=0 extra=0"),
            ("index=0", "index"), ("cpu1=20", "cpu1=20.0"),
            ("cpu1=20", "cpu1=18446744073709551616"),
        ]
        for original, replacement in replacements:
            with self.subTest(replacement=replacement):
                self.check_invalid(VALID.replace(original, replacement, 1))

    def test_other_rank_cannot_fill_missing(self):
        self.check_invalid(VALID.replace("rank=0 index=1", "rank=1 index=1"))

    def test_other_complete_rank_is_ignored(self):
        self.assertTrue(decode(VALID + VALID.replace("rank=0", "rank=1"),
                               0, {1: 1, 2: 1, 3: 1})["valid"])

    def test_wrong_protocol(self):
        self.check_invalid(VALID, {1: 1, 2: 2, 3: 1})
        self.check_invalid(VALID, {1: 1, 2: 1, 3: -1})
        self.check_invalid(VALID, {1: 1, 3: 2})
        self.check_invalid(VALID, {1: 128, 2: 1, 3: 1})

    def test_clipped_cpu_is_unknown(self):
        result = decode(VALID.replace("wall0=110", "wall0=80"),
                        0, {1: 1, 2: 1, 3: 1})
        self.assertTrue(result["valid"], result)
        values = result["measurements"]
        self.assertEqual(values["api_clipped_outer_wall_union_ns"], 120)
        self.assertFalse(values["api_all_fully_contained"])
        self.assertIsNone(values["api_inner_cpu_sum_ns"])
        self.assertIsNone(values["api_by_kind"]["1"]["inner_cpu_ns"])

    def test_existing_fixed_work_window_fields(self):
        fields = ("published_calls_lo=100 published_calls_hi=100 target_calls_lo=90 "
                  "target_calls_hi=110 publication_lag_bound=10 lifetime_calls=120")
        text = VALID.replace("elapsed_ns=900", "elapsed_ns=900 " + fields)
        result = decode(text, 0, {1: 1, 2: 1, 3: 1})
        self.assertTrue(result["valid"], result)
        self.assertEqual(result["measurements"]["window_work_counts"]["lifetime_calls"], 120)
        for field in fields.split():
            with self.subTest(field=field):
                self.check_invalid(text.replace(" " + field, "", 1))
        self.check_invalid(text.replace("target_calls_hi=110", "target_calls_hi=89"))
        self.check_invalid(text.replace("lifetime_calls=120", "lifetime_calls=-1"))


if __name__ == "__main__":
    unittest.main()
