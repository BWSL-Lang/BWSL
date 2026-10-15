#!/usr/bin/env python3
"""Check that repeatability analysis does not count duplicate or mixed runs."""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("analyze_web", ROOT / "scripts/analyze_wgsl_web.py")
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


class AnalysisTests(unittest.TestCase):
    def test_render_comparisons_keep_resolution_separate_and_skip_interface_regressions(self):
        report = {
            "status": "passed", "kind": "render", "compiler": {"sha256": "render-compiler"},
            "harness_sha256": {"render.mjs": "render-harness"}, "environment": {},
            "settings": {"resolution": 512},
            "cases": {
                "interfaces": {"validation_only": True},
                "mesh": {
                    "source_hashes": {"bwsl": {"vertex": "vertex", "fragment": "fragment"}},
                    "paired_gpu_ratio": {"median": 1, "bootstrap_95_interval": [0.9, 1.1]},
                    "variants": {"bwsl": {"gpu_per_frame": {"median_us": 100}}},
                    "compilation": {}, "frames_per_batch": 128,
                },
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "512.json"
            second = Path(temporary) / "1024.json"
            first.write_text(json.dumps(report))
            report["settings"]["resolution"] = 1024
            second.write_text(json.dumps(report))
            groups = ANALYSIS.summarize([first, second])["comparisons"]
            self.assertEqual(len(groups), 2)
            self.assertEqual({group["identity"]["resolution"] for group in groups}, {512, 1024})
            self.assertTrue(all(group["runs"][0]["gpu_medians_us"]["bwsl"] == 100 for group in groups))
            third = Path(temporary) / "inprocess.json"
            report["settings"]["resolution"] = 512
            report["compilation_mode"] = "fresh-source-in-process"
            report["inprocess_compiler"] = {"sha256": "worker"}
            third.write_text(json.dumps(report))
            groups = ANALYSIS.summarize([first, second, third])["comparisons"]
            self.assertEqual(len(groups), 3)

    def test_only_successful_independent_compatible_runs_count_as_repeats(self):
        first = {
            "status": "passed", "compiler": {"sha256": "compiler-a"},
            "harness_sha256": {"cases.mjs": "harness-a"},
            "environment": {"adapter": {"vendor": "test"}}, "settings": {"elements": 65536},
            "cases": {"kernel": {
                "source_hashes": {"handwritten": "shader-a", "bwsl": "shader-b"},
                "paired_gpu_ratio": {"median": 0.8, "bootstrap_95_interval": [0.7, 0.9]},
                "dispatches_per_batch": 32,
                "variants": {}, "compilation": {},
            }},
        }
        second = deepcopy(first)
        second["cases"]["kernel"]["paired_gpu_ratio"]["median"] = 0.81
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary) / f"run-{index}.json" for index in range(2)]
            def write_reports():
                for path, report in zip(paths, [first, second]):
                    path.write_text(json.dumps(report))
            write_reports()
            result = ANALYSIS.summarize(paths)["comparisons"]
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]["gpu_observation"], "BWSL faster in every recorded interval")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                ANALYSIS.summarize([paths[0], paths[0]])
            second["compiler"]["sha256"] = "compiler-b"
            write_reports()
            result = ANALYSIS.summarize(paths)["comparisons"]
            self.assertEqual(len(result), 2)
            self.assertTrue(all(group["gpu_observation"] == "Needs repeated runs" for group in result))
            second["compiler"]["sha256"] = "compiler-a"
            second["cases"]["kernel"]["paired_gpu_ratio"]["bootstrap_95_interval"] = [0.9, 1.1]
            write_reports()
            result = ANALYSIS.summarize(paths)["comparisons"]
            self.assertEqual(result[0]["gpu_observation"], "No consistent direction outside the recorded intervals")
            second["status"] = "failed"
            write_reports()
            with self.assertRaisesRegex(ValueError, "successful"):
                ANALYSIS.summarize(paths)


if __name__ == "__main__":
    unittest.main()
