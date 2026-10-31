import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py"
SPEC = importlib.util.spec_from_file_location("contest_eval", MODULE)
contest_eval = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contest_eval)


class MetricsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output_dir = Path(self.temp.name)

    def test_exact_recall_and_duplicate_rejection(self):
        self.assertEqual(contest_eval.recall_at_k([[1, 2]], [[2, 3]]), 0.5)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            contest_eval.validate_ids([[1, 1]], rows=10, k=2)
        with self.assertRaisesRegex(ValueError, "count"):
            contest_eval.validate_ids([[1]], rows=10, k=2)
        with self.assertRaisesRegex(ValueError, "range"):
            contest_eval.validate_ids([[10, 11]], rows=10, k=2)

    def test_percentile_and_pair_ratios(self):
        self.assertEqual(contest_eval.percentile([1, 2, 3, 4], 95), 3.85)
        comparison = contest_eval.paired_metrics(
            {"ready_seconds": 20, "qps": {10: 100, 100: 80},
             "recall": {10: 0.98, 100: 0.99}},
            {"ready_seconds": 10, "qps": {10: 95, 100: 88},
             "recall": {10: 0.977, 100: 0.986}})
        self.assertEqual(comparison["build_speedup"], 2.0)
        self.assertAlmostEqual(comparison["query_speedup"], (0.95 * 1.1) ** 0.5)
        self.assertAlmostEqual(comparison["recall_delta"][10], 0.003)
        self.assertAlmostEqual(comparison["recall_delta"][100], 0.004)

    def test_official_like_gates_include_recall_qps_and_vram(self):
        comparison = {"recall": {10: 0.98, 100: 0.96},
                      "recall_delta": {10: 0.005, 100: 0.005},
                      "query_qps_ratio": {10: 0.90, 100: 0.95}}
        resources = {"candidate_vram_bytes": 125, "baseline_vram_bytes": 100,
                     "candidate_rss_bytes": 96 * 1024**3}
        gates = contest_eval.evaluate_gates(comparison, resources, "sift")
        self.assertTrue(all(item["passed"] for item in gates))
        comparison["recall_delta"][10] = 0.005001
        gates = contest_eval.evaluate_gates(comparison, resources, "sift")
        self.assertFalse(next(x for x in gates if x["name"] == "recall@10_baseline_delta")["passed"])

    def test_failed_evidence_cannot_pass(self):
        result = {"status": "RUNNING", "rounds": [], "errors": ["GPU evidence missing"]}
        contest_eval.write_reports(result, self.output_dir)
        self.assertEqual(json.loads((self.output_dir / "summary.json").read_text())["status"], "FAILED")


class DatasetTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache_dir = Path(self.temp.name)

    def test_full_matrix_has_required_sizes_and_dimensions(self):
        specs = contest_eval.dataset_specs("full")
        shapes = {(s.rows, s.dim) for s in specs}
        self.assertIn((250000, 128), shapes)
        self.assertIn((250000, 256), shapes)
        self.assertIn((250000, 384), shapes)
        self.assertIn((250000, 768), shapes)
        self.assertIn((1000000, 128), shapes)
        self.assertIn((1000000, 256), shapes)
        self.assertTrue(all(s.nq == 4096 for s in specs))

    def test_exact_topk_orders_by_l2_then_id(self):
        base = np.array([[0, 0], [1, 0], [-1, 0], [3, 0]], dtype=np.float32)
        queries = np.array([[0, 0]], dtype=np.float32)
        np.testing.assert_array_equal(contest_eval.exact_topk(base, queries, 3, 2), [[0, 1, 2]])

    def test_regeneration_is_deterministic_and_detects_bad_cache(self):
        spec = contest_eval.DatasetSpec("tiny", 256, 4, 16, "clustered", 7)
        first = contest_eval.load_or_create_dataset(spec, self.cache_dir)
        second = contest_eval.load_or_create_dataset(spec, self.cache_dir)
        np.testing.assert_array_equal(first.base, second.base)
        np.testing.assert_array_equal(first.truth, second.truth)
        self.assertEqual(first.manifest["base_sha256"], second.manifest["base_sha256"])
        manifest = self.cache_dir / "tiny/manifest.json"
        manifest.write_text(manifest.read_text().replace('"rows": 256', '"rows": 255'))
        with self.assertRaisesRegex(RuntimeError, "manifest"):
            contest_eval.load_or_create_dataset(spec, self.cache_dir)


class MysqlCliTest(unittest.TestCase):
    def test_persistent_client_returns_result_sets(self):
        script = r'''
import sys
for statement in sys.stdin.buffer:
    if b"SELECT 1" in statement:
        sys.stdout.buffer.write(b"1\n")
    if b"SELECT id" in statement:
        sys.stdout.buffer.write(b"1\n2\n")
    if b"__SEEKDB_CONTEST_DONE__" in statement:
        sys.stdout.buffer.write(b"__SEEKDB_CONTEST_DONE__\n")
        sys.stdout.buffer.flush()
'''
        client = contest_eval.MysqlCli([sys.executable, "-u", "-c", script])
        try:
            with client.cursor() as cursor:
                cursor.execute("SELECT 1", expected_rows=1)
                self.assertEqual(cursor.fetchone(), ("1",))
                cursor.execute(contest_eval.query_sql("items", 10), ("[1,2]",), expected_rows=2)
                self.assertEqual(cursor.fetchall(), [("1",), ("2",)])
        finally:
            client.close()


class RunnerContractTest(unittest.TestCase):
    def test_official_index_sql_is_fixed(self):
        sql = contest_eval.create_index_sql("items", "cuvs")
        self.assertIn("distance=l2", sql)
        self.assertIn("type=hnsw", sql)
        self.assertIn("lib=cuvs", sql)
        self.assertIn("m=16", sql)
        self.assertIn("ef_construction=200", sql)
        self.assertIn("ef_search=64", sql)

    def test_observer_uses_private_runtime_and_trace(self):
        config = contest_eval.ServerConfig("candidate", Path("/opt/candidate/seekdb"),
            Path("/opt/candidate/bridge.so"), 2982, Path("/work/run/candidate"))
        command, env = contest_eval.observer_command(config)
        self.assertIn("--nodaemon", command)
        self.assertIn("--base-dir", command)
        self.assertIn(str(config.base_dir), command)
        self.assertEqual(env["OB_VSAG_TRACE"], "1")
        self.assertEqual(env["OB_VSAG_TRACE_FILE"], str(config.base_dir / "hipvs.trace"))

    def test_cleanup_requires_owned_binary_and_base_dir(self):
        self.assertFalse(contest_eval.owned_process_cmdline("/bin/other --base-dir /tmp/other",
            Path("/opt/candidate/seekdb"), Path("/work/run/candidate")))
        self.assertTrue(contest_eval.owned_process_cmdline(
            "/opt/candidate/seekdb --nodaemon --base-dir /work/run/candidate",
            Path("/opt/candidate/seekdb"), Path("/work/run/candidate")))

    def test_startup_failure_invokes_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = contest_eval.ServerConfig("candidate", root / "seekdb", root / "bridge.so",
                                               2982, root / "run")
            process = mock.Mock()
            process.poll.return_value = 1
            process.returncode = 1
            sampler = mock.Mock()
            with mock.patch.object(contest_eval.subprocess, "Popen", return_value=process), \
                 mock.patch.object(contest_eval, "ResourceSampler", return_value=sampler):
                server = contest_eval.ObserverServer(config)
                with mock.patch.object(server, "stop") as stop:
                    with self.assertRaisesRegex(RuntimeError, "observer exited"):
                        server.start()
                    stop.assert_called_once()

    def test_query_and_vector_contract(self):
        self.assertEqual(contest_eval.vector_literal(np.array([1, -0.5], dtype=np.float32)), "[1,-0.5]")
        sql = contest_eval.query_sql("items", 100)
        self.assertIn("APPROXIMATE LIMIT 100", sql)
        self.assertNotIn("batch_knn", sql)
        with self.assertRaisesRegex(ValueError, "table"):
            contest_eval.query_sql("items; DROP TABLE x", 10)

    def test_latency_and_gpu_evidence(self):
        summary = contest_eval.latency_summary([0.1, 0.2, 0.3, 0.4])
        self.assertEqual(summary["qps"], 4)
        self.assertAlmostEqual(summary["p95_ms"], 385)
        evidence = contest_eval.gpu_evidence(
            "cuvs_serve id=1\ncuvs_serve id=2\n",
            "serialized GPU CAGRA snapshot built=true graph_bytes=100\n")
        self.assertTrue(evidence["passed"])
        self.assertEqual(evidence["gpu_serves"], 2)
        self.assertFalse(contest_eval.gpu_evidence("", "built=true")["passed"])
        self.assertFalse(contest_eval.gpu_evidence(
            "cuvs_serve", "serialized GPU CAGRA snapshot built=true\nlazily built VSAG fallback")["passed"])

    def test_only_official_shaped_sets_get_official_thresholds(self):
        self.assertEqual(contest_eval.dataset_kind("gaussian250k-d128"), "sift")
        self.assertEqual(contest_eval.dataset_kind("gaussian250k-d256"), "hidden")
        self.assertEqual(contest_eval.dataset_kind("clustered250k-d256"), "local")

    def test_pair_reverses_order_and_writes_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("baseline-observer", "candidate-observer", "baseline-bridge", "candidate-bridge"):
                (root / name).write_bytes(b"test")
            args = SimpleNamespace(mode="quick", dataset=None, rounds=2,
                output_dir=root / "output", data_cache=root / "cache", port=2982,
                baseline_observer=root / "baseline-observer", candidate_observer=root / "candidate-observer",
                baseline_bridge=root / "baseline-bridge", candidate_bridge=root / "candidate-bridge",
                baseline_revision="v5", candidate_revision="candidate")
            order = []

            def fake_run(spec, data, config, raw_dir, cache_dir):
                order.append(config.role)
                baseline = config.role == "baseline"
                return {"ready_seconds": 20 if baseline else 10,
                        "qps": {10: 100, 100: 100}, "recall": {10: 0.99, 100: 0.99},
                        "resources": {"peak_vram_bytes": 100, "peak_rss_bytes": 100}}

            with mock.patch.object(contest_eval, "run_single", fake_run):
                result = contest_eval.run_evaluation(args)
            self.assertEqual(order, ["baseline", "candidate", "candidate", "baseline"])
            self.assertEqual(result["status"], "QUICK_PASS")
            self.assertEqual(len(result["rounds"]), 2)
            self.assertTrue((args.output_dir / "summary.json").exists())

    def test_full_subset_cannot_claim_strict_matrix_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("baseline-observer", "candidate-observer", "baseline-bridge", "candidate-bridge"):
                (root / name).write_bytes(b"test")
            args = SimpleNamespace(mode="full", dataset=["tiny"], rounds=1,
                output_dir=root / "output", data_cache=root / "cache", port=2982,
                baseline_observer=root / "baseline-observer", candidate_observer=root / "candidate-observer",
                baseline_bridge=root / "baseline-bridge", candidate_bridge=root / "candidate-bridge",
                baseline_revision="v5", candidate_revision="candidate")
            spec = contest_eval.DatasetSpec("tiny", 256, 4, 16, "gaussian", 7)

            def fake_run(spec, data, config, raw_dir, cache_dir):
                return {"ready_seconds": 10, "qps": {10: 100, 100: 100},
                        "recall": {10: 1, 100: 1},
                        "resources": {"peak_vram_bytes": 100, "peak_rss_bytes": 100}}

            with mock.patch.object(contest_eval, "dataset_specs", return_value=[spec]), \
                 mock.patch.object(contest_eval, "run_single", fake_run):
                result = contest_eval.run_evaluation(args)
            self.assertEqual(result["status"], "SUBSET_PASS")


if __name__ == "__main__":
    unittest.main()
