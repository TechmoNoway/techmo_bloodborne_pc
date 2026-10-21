import copy
import importlib.util
import json
import struct
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "pipeline_replay", Path(__file__).resolve().parents[1] / "tools/pipeline_replay.py")
lab = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lab)

G = "g_0x0000000000000001"
H = "g_0x0000000000000002"


class ReplayTests(unittest.TestCase):
    def setUp(self):
        # Keep fixtures within the workspace on Windows.
        root = lab.ROOT / "out/test-fixtures/pipeline-replay"
        root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "live"
        self.source.mkdir()
        for name, data in {"profile.bin": b"profile", "translator.compiler": b"compiler",
                           G + ".key": b"graphics", H + ".key": b"second",
                           "shader.meta": b"metadata", "shader.spv": b"spirv",
                           "omit.input": b"game inputs", "unrelated.txt": b"untouched"}.items():
            (self.source / name).write_bytes(data)

    def capture(self, keys=None):
        path = self.root / "bundle"
        return path, lab.capture(self.source, path, keys or [G])

    def test_capture_is_immutable_selected_and_does_not_modify_source(self):
        before = {p.name: p.read_bytes() for p in self.source.iterdir()}
        bundle, manifest = self.capture()
        self.assertEqual(manifest, lab.verify_bundle(bundle))
        self.assertEqual(manifest["selected_keys"], [G])
        self.assertNotIn(H + ".key", manifest["files"])
        self.assertNotIn("omit.input", manifest["files"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.source.iterdir()})
        with self.assertRaises(FileExistsError):
            lab.capture(self.source, bundle, [G])

    def test_changed_blob_and_extra_key_rejected(self):
        bundle, _ = self.capture()
        (bundle / "cache/shader.spv").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "corrupt"):
            lab.verify_bundle(bundle)
        (bundle / "cache/shader.spv").write_bytes(b"spirv")
        (bundle / "cache" / (H + ".key")).write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "added/removed"):
            lab.verify_bundle(bundle)

    def test_capture_rejects_nested_destination_and_missing_selection(self):
        with self.assertRaises(ValueError):
            lab.capture(self.source, self.source / "bundle", [])
        with self.assertRaisesRegex(ValueError, "not captured"):
            lab.capture(self.source, self.root / "bundle", ["c_0x000000000000000a"])
        with self.assertRaises(ValueError):
            lab.normalize_key("../bad.key")

    def test_log_parser_does_not_select_shader_or_guest_addresses(self):
        self.assertEqual(lab.log_keys(
            "Pipeline timeline: gfx 0x1 origin=first-use\n"
            "Pipeline preparation wait: compute 0xabc 23 ms\n"
            "shader hash 0x123\nimage backing 0x456\nPipeline foo 0x777"),
            ["c_0x0000000000000abc", G])

    def events(self):
        audits = [
            {"event": "audit", "key": G, "kind": "graphics", "canonical": "a",
             "dynamic_projection": "p", "support": "aux", "stages": ["vertex", "fragment"]},
            {"event": "audit", "key": H, "kind": "graphics", "canonical": "b",
             "dynamic_projection": "p", "support": "aux", "stages": ["vertex", "fragment"]}]
        return audits + [
            {"event": "begin", "key": G, "round": 0},
            {"event": "sample", "key": G, "round": 0, "ok": True, "reconstruct_ms": 120},
            {"event": "sample", "key": G, "round": 1, "ok": True, "reconstruct_ms": 10},
            {"event": "sample", "key": H, "round": 0, "ok": False, "reconstruct_ms": 999},
            {"event": "begin", "key": H, "round": 1}]

    def test_candidates_are_not_claimed_as_duplicates_and_failures_not_timed(self):
        _, manifest = self.capture()
        summary = lab.summarize(self.events(), manifest)
        self.assertEqual(summary["audit"]["canonical_aliases"], [])
        self.assertEqual(summary["audit"]["dynamic_state_candidates"], [[G, H]])
        self.assertEqual(summary["counts"]["successful"], 2)
        self.assertEqual(summary["counts"]["failed"], 1)
        self.assertEqual(summary["pipelines"][G]["median_ms"], 65)
        self.assertEqual(summary["p95_reconstruct_ms"], 120)
        self.assertEqual(summary["incomplete"], [(H, 1)])

    def test_generated_auxiliary_shader_difference_prevents_candidate_group(self):
        _, manifest = self.capture()
        events = self.events()
        events[1]["support"] = "different generated fragment"
        summary = lab.summarize(events, manifest)
        self.assertEqual(summary["audit"]["dynamic_state_candidates"], [])
        self.assertEqual(summary["audit"]["same_stages_groups"], [])

    def test_alias_detection_and_different_spirv_names(self):
        _, manifest = self.capture()
        manifest["files"]["other.spv"] = manifest["files"]["shader.spv"]
        events = self.events()
        events[1]["canonical"] = "a"
        result = lab.summarize(events, manifest)
        self.assertEqual(result["audit"]["canonical_aliases"], [[G, H]])
        self.assertEqual(result["audit"]["dynamic_state_candidates"], [])
        self.assertEqual(len(result["audit"]["identical_spirv_candidates"]), 1)

    def test_compare_rejects_different_gpu_mode_or_case_and_reports_regression(self):
        base = {"passed": True, "case_id": "case",
                "device": dict(gpu="GPU", vendor_id=1, device_id=2, driver=3, mode="empty"),
                "pipelines": {G: {"median_ms": 10}}}
        after = copy.deepcopy(base)
        after["pipelines"][G]["median_ms"] = 17
        self.assertEqual(lab.compare(base, after, 5)["regressions"], 1)
        for field, value in (("mode", "warm"), ("driver", 4), ("gpu", "other")):
            mismatch = copy.deepcopy(after)
            mismatch["device"][field] = value
            with self.assertRaises(ValueError):
                lab.compare(base, mismatch, 5)
        after["case_id"] = "new"
        with self.assertRaises(ValueError):
            lab.compare(base, after, 5)

    def test_timeout_preserves_last_attempt_and_writes_failed_report(self):
        bundle, _ = self.capture()
        exe = self.root / "helper.exe"
        exe.write_bytes(b"mock helper")
        output = self.root / "timed-out"

        def timeout(command, **kwargs):
            Path(command[2]).write_text(json.dumps(
                {"event": "begin", "key": G, "round": 0}) + "\n", encoding="utf-8")
            raise lab.subprocess.TimeoutExpired(command, kwargs["timeout"])

        with patch.object(lab.subprocess, "run", side_effect=timeout):
            result = lab.run(bundle, output, exe, 3, "empty", 1)
        self.assertFalse(result["passed"])
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["incomplete"], [(G, 0)])
        self.assertIn("Incomplete sample count", result["errors"])
        self.assertTrue((output / "report.md").is_file())
        self.assertEqual(lab.verify_bundle(bundle)["selected_keys"], [G])

    def test_compare_rejects_different_repeats_and_invalid_threshold(self):
        base = {"passed": True, "case_id": "case", "repeats": 3,
                "device": dict(gpu="GPU", vendor_id=1, device_id=2, driver=3, mode="empty"),
                "pipelines": {G: {"median_ms": 10}}}
        after = copy.deepcopy(base)
        after["repeats"] = 2
        with self.assertRaises(ValueError):
            lab.compare(base, after, 5)
        for threshold in (-1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                lab.compare(base, base, threshold)

    def startup_run(self, events):
        bundle, _ = self.capture()
        exe = self.root / "helper.exe"
        exe.write_bytes(b"mock helper")

        def helper(command, **kwargs):
            Path(command[2]).write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
            return lab.subprocess.CompletedProcess(command, 0)

        with patch.object(lab.subprocess, "run", side_effect=helper):
            return lab.run(bundle, self.root / "startup", exe, 1, "startup", 1)

    def startup_events(self):
        return [dict(event="device", mode="startup"),
                dict(event="startup", loaded=1, rejected=0, records=1, total_ms=12.5),
                dict(event="startup_key", key=G, ok=True, warmup="loaded", rejection="none")]

    def test_startup_requires_native_availability_not_reconstruction_samples(self):
        result = self.startup_run(self.startup_events())
        self.assertTrue(result["passed"], result["errors"])
        self.assertEqual(result["expected_samples"], 0)
        self.assertEqual(result["counts"]["successful"], 0)
        self.assertEqual(result["startup"]["loaded"], 1)
        self.assertTrue(result["startup_keys"][G]["ok"])
        with self.assertRaisesRegex(ValueError, "no per-key timings"):
            lab.compare(result, result, 5)

    def test_startup_fails_if_loader_rejects_record(self):
        events = self.startup_events()
        events[1].update(loaded=0, rejected=1)
        events[2].update(ok=False, warmup="rejected", rejection="missing_compiler_stamp")
        result = self.startup_run(events)
        self.assertFalse(result["passed"])
        self.assertIn("Startup did not restore every selected native pipeline", result["errors"])

    def test_startup_fails_if_native_key_evidence_missing(self):
        result = self.startup_run(self.startup_events()[:-1])
        self.assertFalse(result["passed"])
        self.assertIn("Startup did not restore every selected native pipeline", result["errors"])

    def test_startup_repetition_guard_runs_before_helper(self):
        with patch.object(lab.subprocess, "run") as helper:
            with self.assertRaisesRegex(ValueError, "one repetition"):
                lab.run(self.root, self.root, self.root, 3, "startup", 1)
        helper.assert_not_called()

    def test_phase_aggregation_is_exclusive_and_tracks_slow_samples(self):
        _, manifest = self.capture()
        phases = dict.fromkeys(lab.PHASES, 0.0)
        phases.update(native_pipeline=100, metadata_read=15, other=5)
        event = {"event": "sample", "key": G, "round": 0, "ok": True,
                 "reconstruct_ms": 120, "phases_ms": phases}
        report = lab.summarize([event], manifest)
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["phase_summary"]["native_pipeline"]["total_ms"], 100)
        self.assertEqual(report["phase_summary"]["metadata_read"]["slow_sample_total_ms"], 15)
        self.assertEqual(report["top_slow_samples"][0]["key"], G)
        self.assertEqual(report["rounds"]["0"]["p95_ms"], 120)
        phases["native_pipeline"] = 121
        self.assertTrue(lab.summarize([event], manifest)["errors"])
        phases["native_pipeline"] = 0
        event["reconstruct_ms"] = 20
        self.assertIn("Native Vulkan timing absent", lab.summarize([event], manifest)["errors"][0])

    def variant_fixture(self):
        binaries = []
        for program, permutation in ((0x123456, 0), (0x123456, 2), (0x234567, 0)):
            stage = lab.stage_identity(program, permutation)
            binary = f"0x{program:016x}_{permutation}.spv"
            binaries.append(binary)
            (self.source / binary).write_bytes(b"same SPIR-V bytes")
            (self.source / ("0x" + stage + ".compiler")).write_bytes(b"compiler")
            (self.source / ("0x" + stage + ".meta")).write_bytes(
                struct.pack("<IIQQ", 8, 13, int(stage, 16), permutation))
        bundle, manifest = self.capture()
        variants = {b: {"lookup_equal_to": [], "binding_start": [0, 0, 0],
                        "info_fingerprint": "same-info", "fingerprint_source": "persisted_bytes"} for b in binaries[:2]}
        variants[binaries[1]]["lookup_equal_to"] = [binaries[0]]
        return bundle, manifest, binaries, variants

    def test_hashcombine_matches_native_vectors(self):
        self.assertEqual(lab.stage_identity(0x123456, 2), "00000001c16fc956")
        self.assertEqual(lab.stage_identity(0xffffffffffffffff, 4095), "efffffff61c88648")

    def test_spirv_classification_restricts_candidates_to_current_same_program(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["cross_program_groups"], 1)
        self.assertEqual(result["groups_with_current_same_program_pairs"], 1)
        self.assertEqual(len(result["review_candidates"]), 1)
        self.assertEqual(result["review_candidates"][0]["binaries"], binaries[:2])
        self.assertEqual(result["member_compiler_states"]["current_stamp"], 3)
        self.assertFalse(result["details"][0]["members"][2]["observed_in_replay"])

    def test_old_stamp_header_mismatch_or_missing_replay_blocks_candidate(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        stage = lab.stage_identity(0x123456, 2)
        stamp = "0x" + stage + ".compiler"
        manifest["files"][stamp]["sha256"] = "old"
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["review_candidates"], [])
        self.assertEqual(result["member_compiler_states"]["stale_stamp"], 1)
        manifest["files"][stamp]["sha256"] = manifest["files"]["translator.compiler"]["sha256"]
        variants.pop(binaries[1])
        self.assertEqual(lab.classify_spirv(manifest, {}, variants, bundle / "cache")["review_candidates"], [])
        (bundle / "cache" / ("0x" + stage + ".meta")).write_bytes(struct.pack("<IIQQ", 8, 13, int(stage, 16), 99))
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["groups_with_current_same_program_pairs"], 0)

    def test_mutual_lookup_equality_does_not_override_binding_or_info_difference(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        variants[binaries[1]]["binding_start"] = [1, 0, 0]
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["review_candidates"], [])
        self.assertEqual(result["details"][0]["current_same_program_pairs"][0]["classification"],
                         "lookup_equal_but_binding_start_differs")
        variants[binaries[1]]["binding_start"] = [0, 0, 0]
        variants[binaries[1]]["info_fingerprint"] = "different-info"
        self.assertEqual(lab.classify_spirv(manifest, {}, variants, bundle / "cache")["review_candidates"], [])

    def test_captured_values_can_differ_while_static_info_and_layout_match(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        for variant in variants.values():
            variant.update(static_info_fingerprint="static", descriptor_signature=["storage_buffer:1"])
        variants[binaries[1]]["info_fingerprint"] = "other captured values"
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["review_candidate_count"], 1)
        self.assertTrue(result["review_candidates"][0]["captured_info_differs"])
        variants[binaries[1]]["descriptor_signature"] = ["sampled_image:1"]
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["review_candidate_count"], 0)
        self.assertIn("lookup_equal_but_descriptor_layout_differs", result["pair_classifications"])

    def test_native_comparison_limit_is_unknown_not_unequal(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        for variant in variants.values():
            variant.update(lookup_equal_to=[], lookup_compared_to=[], comparisons_truncated=True)
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["pair_classifications"], {"not_compared_by_native_audit": 1})
        variants[binaries[1]]["lookup_compared_to"] = [binaries[0]]
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["pair_classifications"], {"lookup_specialization_differs": 1})

    def test_unattributed_object_fingerprints_are_not_merge_evidence(self):
        bundle, manifest, binaries, variants = self.variant_fixture()
        variants[binaries[1]].pop("fingerprint_source")
        result = lab.classify_spirv(manifest, {}, variants, bundle / "cache")
        self.assertEqual(result["review_candidate_count"], 0)
        self.assertEqual(result["pair_classifications"], {"fingerprint_provenance_unknown": 1})


if __name__ == "__main__":
    unittest.main()
