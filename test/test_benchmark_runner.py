#!/usr/bin/env python3
"""Behavioral checks for benchmark collection, provenance and result gates."""

import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "benchmark"))
from compare_results import compare_csv, match_group
from query_runner import parse_timings, run_query


FAKE = r'''#!/usr/bin/env python3
import csv
import sys
if "--version" in sys.argv:
    print("test-qdb 1")
    raise SystemExit(0)
if "--fail" in sys.argv:
    print("query failed", file=sys.stderr)
    raise SystemExit(7)
writer = csv.writer(sys.stdout)
writer.writerow(["revenue", "text"])
writer.writerow(["2.5" if "--different" in sys.argv else "1.5", "commas, and\na newline"])
if "--missing-timings" not in sys.argv:
    print("Returned 1 rows in 0.125 seconds.", file=sys.stderr)
    print("Cpu: 0.075 seconds.", file=sys.stderr)
    print("Planning: 0.001 seconds.", file=sys.stderr)
    print("KernelBuild: 0.002 seconds.", file=sys.stderr)
    print("JitLLVM: 0.003 seconds.", file=sys.stderr)
'''


class BenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="qdb benchmark ")
        self.root = Path(self.directory.name)
        self.binary = self.root / "fake qdb"
        self.binary.write_text(FAKE)
        self.binary.chmod(0o755)
        self.sql = self.root / "query.sql"
        self.sql.write_text("select 1.5 as revenue;\n")

    def tearDown(self):
        self.directory.cleanup()

    def collect(self, arguments=()):
        return run_query(self.binary, "Q19 sf1", self.root / "results", self.root / "log",
                         self.root / "metrics.jsonl", ["--sql", "-i", str(self.sql), *arguments])

    def compare(self, left, right, sql=""):
        for name, rows in [("left", left), ("right", right)]:
            with (self.root / name).open("w", newline="") as output:
                csv.writer(output).writerows(rows)
        return compare_csv(self.root / "left", self.root / "right", sql)

    def campaign(self, extra=(), env=None):
        data = self.root / "data files" / "pq1"
        data.mkdir(parents=True)
        (data / "nation.parquet").write_bytes(b"fixed dataset content")
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
        command = [sys.executable, str(REPO / "benchmark/tpch/compare_runs.py"),
                   "--data", str(data.parent), "--scale", "1", "--queries", "19",
                   "--a", str(self.binary), "--a-commit", commit, "--out", str(self.root / "campaign"), *extra]
        result = subprocess.run(command, text=True, capture_output=True, timeout=60, env=env)
        manifest = self.root / "campaign/manifest.json"
        return result, json.loads(manifest.read_text()) if manifest.exists() else None

    def test_process_costs_are_separate_from_execution(self):
        record = self.collect()
        self.assertEqual(record["processing_seconds"], 0.125)
        self.assertEqual(record["execution_cpu_seconds"], 0.075)
        self.assertEqual(record["planning_seconds"], 0.001)
        self.assertGreater(record["process_wall_seconds"], 0)
        self.assertGreater(record["max_rss_bytes"], 0)
        self.assertGreaterEqual(record["process_user_seconds"], 0)
        self.assertEqual(record["query"], 19)
        self.assertEqual(len(record["sql_sha256"]), 64)
        with Path(record["stdout_path"]).open(newline="") as output:
            rows = list(csv.reader(output))
        self.assertEqual(rows[1], ["1.5", "commas, and\na newline"])
        self.assertEqual(json.loads((self.root / "metrics.jsonl").read_text()), record)

    def test_missing_timings_remain_unknown(self):
        record = self.collect(["--missing-timings"])
        self.assertEqual(record["returncode"], 0)
        self.assertIsNone(record["processing_seconds"])
        self.assertIsNone(record["execution_cpu_seconds"])

    def test_failed_query_keeps_failure_and_raw_output(self):
        record = self.collect(["--fail"])
        self.assertEqual(record["returncode"], 7)
        self.assertIn("query failed", Path(record["stderr_path"]).read_text())
        self.assertIn("rc=7", (self.root / "results").read_text())

    def test_missing_executable_is_a_recorded_failure(self):
        self.binary.unlink()
        record = self.collect()
        self.assertEqual(record["returncode"], 127)
        self.assertIsNone(record["max_rss_bytes"])
        self.assertIsNone(record["process_user_seconds"])
        self.assertTrue(Path(record["stderr_path"]).read_text())

    def test_nonfinite_numeric_strings_are_compared_exactly(self):
        self.assertEqual(self.compare([["value"], ["1e999"]], [["value"], ["2e999"]])["status"], "different")

    def test_nonfinite_tolerance_is_rejected(self):
        result, _ = self.campaign(["--abs-tol", "nan"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite", result.stderr)

    def test_scientific_timings(self):
        record = parse_timings("Returned 1 rows in 2e-5 seconds. Cpu: 3.5E-5 seconds.")
        self.assertEqual(record["processing_seconds"], 2e-5)
        self.assertEqual(record["execution_cpu_seconds"], 3.5e-5)
        self.assertIsNone(record["jit_llvm_seconds"])

    def test_result_multiset_preserves_duplicates(self):
        header = ["number", "text"]
        left = [header, ["1.5", "a,b\nline"], ["1.5", "a,b\nline"], ["2.0", "x"]]
        self.assertEqual(self.compare(left, [header, *reversed(left[1:])])["status"], "equal")
        self.assertEqual(self.compare(left, [header, *left[1:3]])["status"], "different")

    def test_null_is_an_exact_value_in_numeric_columns(self):
        header = ["value"]
        # One NULL cell must not turn the column into a text comparison.
        self.assertEqual(self.compare([header, ["1.0"], ["NULL"]], [header, ["NULL"], ["1.0000000001"]])["status"], "equal")
        self.assertEqual(self.compare([header, ["1.0"], ["NULL"]], [header, ["1.0"], ["2.0"]])["status"], "different")
        self.assertEqual(self.compare([header, ["0.0"], ["1.0"]], [header, ["NULL"], ["1.0"]])["status"], "different")
        self.assertEqual(self.compare([header, ["NULL"]], [header, ["NULL"]])["status"], "equal")

    def test_row_order_is_checked_only_for_ordered_queries(self):
        header = ["key", "value"]
        left = [header, ["a", "1.0"], ["b", "NULL"], ["c", "3.0"]]
        reordered = [header, ["c", "3.0"], ["a", "1.0"], ["b", "NULL"]]
        self.assertEqual(self.compare(left, reordered, "select key, value from t")["status"], "equal")
        self.assertEqual(self.compare(left, reordered, "select key from (select * from t order by 1) x")["status"], "equal")
        self.assertEqual(self.compare(left, reordered, "select sum(v) over (order by k) from t")["status"], "equal")
        self.assertEqual(self.compare(left, left, "select key, value from t order by key")["status"], "equal")
        ordered = self.compare(left, reordered, "select key, value from t -- x\norder  by key")
        self.assertEqual(ordered["status"], "different")
        self.assertEqual(ordered["reason"], "row order differs")

    def test_large_integers_are_exact(self):
        self.assertEqual(self.compare([["key"], ["9007199254740992"]], [["key"], ["9007199254740993"]])["status"], "different")

    def test_numeric_tolerance_and_ambiguous_matching(self):
        self.assertEqual(self.compare([["value"], ["0.0"]], [["value"], ["0.0000001"]])["status"], "equal")
        self.assertEqual(self.compare([["value"], ["0.0"]], [["value"], ["0.00001"]])["status"], "different")
        self.assertTrue(match_group([(1.00000000001, 100.0), (1.00000000002, 200.0)],
                                    [(1.00000000001, 200.0), (1.00000000002, 100.0)], 1e-9, 1e-6))

    def test_limited_difference_is_never_accepted_as_ties(self):
        result = self.compare([["revenue"], ["1.0"]], [["revenue"], ["2.0"]], "select revenue order by revenue limit 1")
        self.assertEqual(result["status"], "requires_review")
        self.assertIn("not proven", result["reason"])

    def test_campaign_freezes_inputs_records_control_and_alternates(self):
        result, manifest = self.campaign(["--a-args=--tag 'two words'", "--b-args=--tag 'two words'"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(manifest["status"], "complete")
        self.assertEqual(len(manifest["queries"]), 22)
        measured = [sample["arm"] for sample in manifest["samples"] if sample["kind"] == "measured"]
        self.assertEqual(measured, ["a", "b", "a", "b", "a", "b"])
        self.assertEqual(manifest["samples"][0]["kind"], "control")
        records = [json.loads(line) for line in (self.root / "campaign/runs.jsonl").read_text().splitlines()]
        control = [record for record in records if record["kind"] == "control"]
        self.assertEqual(len(control), 22)
        for record in records:
            self.assertEqual(record["dataset_manifest_sha256"], manifest["dataset"]["dataset_manifest_sha256"])
            self.assertEqual(record["sql_sha256"], manifest["queries"][str(record["query"])]["sha256"])
            if record["arm"] != "control":
                self.assertIn("two words", record["argv"])
        summary = json.loads((self.root / "campaign/summary.json").read_text())
        self.assertEqual(summary["arms"]["a"]["totals"]["processing_seconds"]["runs"], [0.125] * 3)
        self.assertEqual(summary["arms"]["b"]["totals"]["processing_seconds"]["median"], 0.125)

    def test_campaign_rejects_changed_results(self):
        result, manifest = self.campaign(["--skip-control", "--b-args=--different"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(manifest["status"], "failed")
        self.assertIn("Result comparison", manifest["error"])
        self.assertTrue((self.root / "campaign/runs.jsonl").is_file())

    def test_campaign_rejects_silent_missing_timings(self):
        result, manifest = self.campaign(["--skip-control", "--b-args=--missing-timings"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(manifest["status"], "failed")
        self.assertIn("Missing timings", manifest["error"])

    def test_diagnostic_environment_cannot_enter_timed_runs(self):
        result, _ = self.campaign(env={**os.environ, "QDB_DEBUG_ROW_GROUP_PREDICATE": "1"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Diagnostic environment", result.stderr)

    def test_diagnostic_flag_cannot_enter_timed_runs(self):
        result, _ = self.campaign(["--a-args=--operator-diagnostics"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("diagnostic", result.stderr)

    def test_instrumentation_flag_cannot_enter_timed_runs(self):
        result, _ = self.campaign(["--b-args=--operator-instrumentation"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--operator-instrumentation", result.stderr)

    def test_short_instrumentation_flag_cannot_enter_timed_runs(self):
        result, _ = self.campaign(["--a-args=--instrumentation"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--instrumentation", result.stderr)


if __name__ == "__main__":
    unittest.main()
