"""Offline tests for benchmark quality/evidence gates, not GPU performance."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("benchmark", ROOT / "docs/gpu-vector-index-hipvs-cuvs/validation/benchmark_build.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkTests(unittest.TestCase):
    def trials(self):
        return [dict(backend=name, create_seconds=seconds, first_query_seconds=0.5,
                     ready_seconds=seconds + 0.5, recall_at_k=0.99,
                     gpu_serves=int(name == "cuvs"), gpu_snapshot_builds=int(name == "cuvs"))
                for name, seconds in [("vsag", 10), ("cuvs", 2)]]

    def test_recall(self):
        self.assertEqual(benchmark.recall([[1, 2], [4, 5]], [[1, 3], [4, 5]]), 0.75)

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError):
            benchmark.recall([[1, 1]], [[1, 2]])

    def test_missing_result_rejected(self):
        with self.assertRaises(ValueError):
            benchmark.recall([[1]], [[1, 2]])

    def test_summary(self):
        result = benchmark.summarize(self.trials(), 0.95)
        self.assertEqual(result["ddl_speedup"], 5)
        self.assertAlmostEqual(result["ready_speedup"], 4.2)

    def test_fallback_is_not_a_speedup(self):
        trials = self.trials(); trials[1]["gpu_serves"] = 0
        with self.assertRaises(ValueError):
            benchmark.summarize(trials, 0.95)

    def test_lazy_build_is_not_ddl_evidence(self):
        trials = self.trials(); trials[1]["gpu_snapshot_builds"] = 0
        with self.assertRaises(ValueError):
            benchmark.summarize(trials, 0.95)

    def test_low_recall_rejected(self):
        with self.assertRaises(ValueError):
            benchmark.summarize(self.trials(), 1)

    def test_no_speedup_is_reported(self):
        trials = self.trials(); trials[1]["ready_seconds"] = 20
        self.assertEqual(benchmark.summarize(trials, 0.95)["status"], "VALID_NO_SPEEDUP")

    def test_cpu_isolation(self):
        trials = self.trials(); trials[0]["gpu_serves"] = 1
        with self.assertRaises(ValueError):
            benchmark.summarize(trials, 0.95)

    def test_evidence_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace"
            path.write_text("old\n"); offset = benchmark.evidence_offset(path)
            path.write_text("old\nnew\n")
            self.assertEqual(benchmark.evidence_since(path, offset), "new\n")
            path.write_text("")
            with self.assertRaises(RuntimeError):
                benchmark.evidence_since(path, offset)

    def test_dataset_repeatability(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed")
        first = benchmark.make_dataset(256, 4, 3, 5, 123)
        second = benchmark.make_dataset(256, 4, 3, 5, 123)
        np.testing.assert_array_equal(first[0], second[0])
        self.assertEqual(first[2], second[2])
        for row in first[0]:
            np.testing.assert_array_equal(np.asarray([float(x) for x in benchmark.vector(row)[1:-1].split(",")], dtype=np.float32), row)


if __name__ == "__main__":
    unittest.main()
