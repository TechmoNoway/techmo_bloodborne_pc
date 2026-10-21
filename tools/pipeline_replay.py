"""Capture and repeatedly reconstruct local pipeline records; never launches the game."""
from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess

KEY = re.compile(r"[gc]_0x[0-9a-f]{16}")
MAX_FILE = 64 * 1024 * 1024
MAX_BUNDLE = 2 * 1024**3
ROOT = Path(__file__).resolve().parents[1]
PHASES = ("record_decode", "metadata_read", "metadata_decode", "spirv_read", "spirv_validate",
          "shader_module", "descriptor_layout", "pipeline_layout", "pipeline_state", "native_pipeline", "other")


def percentile95(values: list[float]):
    values = sorted(values)
    return values[math.ceil(.95 * len(values)) - 1] if values else None


def stage_identity(program: int, permutation: int) -> str:
    # Mirror common/hash.h's u64 HashCombine; cross-checked by the native fixture.
    return f"{program ^ ((permutation + 0x9e3779b9 + (program << 12) + (program >> 4)) & (2**64 - 1)):016x}"


def classify_spirv(manifest: dict, audits: dict, variants: dict, cache: Path | None) -> dict:
    files = manifest["files"]
    translator = files.get("translator.compiler", {}).get("sha256")
    referenced = {s for a in audits.values() for s in a["stages"] if s != "0000000000000000"}
    groups = defaultdict(list)
    for name, record in files.items():
        if name.endswith(".spv"):
            groups[record["sha256"]].append(name)
    details, candidates, candidate_count = [], [], 0
    states = defaultdict(int)
    pair_totals = defaultdict(int)
    lookups = {name: set(v["lookup_equal_to"]) for name, v in variants.items()}
    compared = {name: set(v.get("lookup_compared_to", [])) for name, v in variants.items()}
    for sha, names in sorted(groups.items()):
        if len(names) < 2:
            continue
        members = []
        for name in sorted(names):
            member = {"binary": name, "compiler_state": "unidentified", "metadata_state": "not_inspected"}
            match = re.fullmatch(r"0x([0-9a-f]{16})_(\d+)\.spv", name)
            if match and int(match[2]) < 4096:
                program, permutation = int(match[1], 16), int(match[2])
                stage = stage_identity(program, permutation)
                prefix = "0x" + stage
                stamp = files.get(prefix + ".compiler", {}).get("sha256")
                member.update(program=f"{program:016x}", permutation=permutation, stage_id=stage,
                              compiler_state="missing_stamp" if stamp is None else
                              "current_stamp" if translator is not None and stamp == translator else "stale_stamp",
                              referenced_by_selected_key=stage in referenced,
                              observed_in_replay=name in variants)
                if prefix + ".meta" not in files:
                    member["metadata_state"] = "missing"
                elif cache is not None:
                    with (cache / (prefix + ".meta")).open("rb") as stream:
                        header = stream.read(24)
                    if len(header) != 24:
                        member["metadata_state"] = "truncated_header"
                    else:
                        version, binary_version, recorded_stage, recorded_permutation = struct.unpack("<IIQQ", header)
                        member.update(metadata_version=version, binary_version=binary_version,
                                      metadata_state="header_consistent" if recorded_stage == int(stage, 16) and
                                      recorded_permutation == permutation else "header_identity_mismatch")
                if name in variants:
                    member["replay_metadata"] = {k: v for k, v in variants[name].items()
                                                 if k not in ("lookup_equal_to", "lookup_compared_to")}
                    member["replay_metadata"]["lookup_match_count"] = len(lookups[name])
            states[member["compiler_state"]] += 1
            members.append(member)
        program_ids = {m["program"] for m in members if "program" in m}
        scope = "unidentified" if not program_ids else "same_program" if len(program_ids) == 1 else "cross_program"
        current_by_program = defaultdict(list)
        for member in members:
            if (member["compiler_state"] == "current_stamp" and
                    member.get("metadata_state") == "header_consistent"):
                current_by_program[member["program"]].append(member)
        pair_details = []
        pair_counts = defaultdict(int)
        for family in current_by_program.values():
            for i, a in enumerate(family):
                for b in family[i + 1:]:
                    left, right = variants.get(a["binary"]), variants.get(b["binary"])
                    reason = "not_both_observed_in_replay"
                    if left and right:
                        equal = a["binary"] in lookups[b["binary"]] or b["binary"] in lookups[a["binary"]]
                        checked = (a["binary"] in compared[b["binary"]] or b["binary"] in compared[a["binary"]] or
                                   ("lookup_compared_to" not in left and "lookup_compared_to" not in right and
                                    not left.get("comparisons_truncated", False) and not right.get("comparisons_truncated", False)))
                        if not equal and not checked:
                            reason = "not_compared_by_native_audit"
                        elif not equal:
                            reason = "lookup_specialization_differs"
                        elif left["binding_start"] != right["binding_start"]:
                            reason = "lookup_equal_but_binding_start_differs"
                        elif (left.get("fingerprint_source") != "persisted_bytes" or
                              right.get("fingerprint_source") != "persisted_bytes"):
                            reason = "fingerprint_provenance_unknown"
                        elif "static_info_fingerprint" in left and "static_info_fingerprint" in right:
                            if left["static_info_fingerprint"] != right["static_info_fingerprint"]:
                                reason = "lookup_equal_but_static_info_fingerprint_differs"
                            elif left["descriptor_signature"] != right["descriptor_signature"]:
                                reason = "lookup_equal_but_descriptor_layout_differs"
                            else:
                                reason = "mutual_lookup_equal_static_info_and_layout"
                        elif left["info_fingerprint"] != right["info_fingerprint"]:
                            reason = "lookup_equal_but_info_fingerprint_differs"
                        else:
                            reason = "mutual_lookup_equal_binding_and_info_fingerprint"
                    pair = {"binaries": [a["binary"], b["binary"]], "classification": reason,
                            "captured_info_differs": left["info_fingerprint"] != right["info_fingerprint"] if left and right else None}
                    pair_counts[reason] += 1
                    pair_totals[reason] += 1
                    if pair_counts[reason] <= 10:
                        pair_details.append(pair)
                    if reason in ("mutual_lookup_equal_binding_and_info_fingerprint", "mutual_lookup_equal_static_info_and_layout"):
                        candidate_count += 1
                        if pair_counts[reason] <= 10:
                            candidates.append(pair)
        details.append({"sha256": sha, "scope": scope, "members": members,
                        "pair_counts": dict(pair_counts), "total_current_same_program_pairs": sum(pair_counts.values()),
                        "current_same_program_pairs": pair_details})
    return {"groups": len(details), "member_compiler_states": dict(states),
            "same_program_groups": sum(d["scope"] == "same_program" for d in details),
            "cross_program_groups": sum(d["scope"] == "cross_program" for d in details),
            "groups_with_current_same_program_pairs": sum(bool(d["current_same_program_pairs"]) for d in details),
            "pair_classifications": dict(pair_totals), "review_candidate_count": candidate_count,
            "review_candidates": candidates, "details": details,
            "example_limit_per_class_per_group": 10,
            "note": "Mutual lookup equality and matching fingerprints are review evidence, not proof of safe merging. "
                    "Header checks alone do not validate metadata ABI or SPIR-V. No records are merged."}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def normalize_key(value: str) -> str:
    value = value.removesuffix(".key").lower()
    if not KEY.fullmatch(value):
        raise ValueError("Key must be g_0x0123456789abcdef or c_0x0123456789abcdef")
    return value


def log_keys(text: str) -> list[str]:
    """Only explicit pipeline diagnostics, never arbitrary GPU/shader addresses."""
    found = set()
    for line in text.splitlines():
        if "Pipeline" not in line and "pipeline" not in line:
            continue
        for kind, number in re.findall(r"\b(gfx|graphics|compute)\s+(0x[0-9a-fA-F]+)\b", line):
            number = int(number, 16)
            if number < 2**64:
                found.add(f"{'c' if kind == 'compute' else 'g'}_0x{number:016x}")
    return sorted(found)


def capture(source: Path, bundle: Path, keys: list[str]) -> dict:
    source, bundle = source.resolve(strict=True), bundle.resolve()
    if not source.is_dir() or bundle == source or source in bundle.parents:
        raise ValueError("Bundle must be a new directory outside the live cache")
    selected = set(map(normalize_key, keys))
    existing = {p.stem for p in source.glob("*.key") if KEY.fullmatch(p.stem)}
    if selected - existing:
        raise ValueError(f"Requested records are not captured: {sorted(selected - existing)}")
    if not existing:
        raise ValueError("No pipeline keys in source cache")
    bundle.mkdir(parents=True, exist_ok=False)
    destination = bundle / "cache"
    destination.mkdir()
    records, total = {}, 0
    for path in sorted(source.iterdir()):
        if path.suffix not in {".meta", ".spv", ".compiler", ".key"} and path.name != "profile.bin":
            continue
        if path.suffix == ".key" and (not KEY.fullmatch(path.stem) or selected and path.stem not in selected):
            continue
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_FILE:
            raise ValueError(f"Invalid or oversized cache file: {path.name}")
        data = path.read_bytes()
        total += len(data)
        if len(data) > MAX_FILE or total > MAX_BUNDLE:
            raise ValueError("Bundle exceeds size limits")
        # Detect writes during capture instead of benchmarking a torn generation.
        if digest(path.read_bytes()) != digest(data):
            raise ValueError(f"Cache changed during capture: {path.name}; capture after closing the game")
        (destination / path.name).write_bytes(data)
        records[path.name] = {"sha256": digest(data), "bytes": len(data)}
    if "profile.bin" not in records or "translator.compiler" not in records:
        raise ValueError("Profile/translator stamp missing; source is not a current replayable cache")
    # Check the complete snapshot again: metadata and binaries may be replaced independently.
    for name, record in records.items():
        if digest((source / name).read_bytes()) != record["sha256"]:
            raise ValueError(f"Cache changed during capture: {name}")
    manifest = {
        "schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
        "source": str(source), "files": records,
        "case_id": digest(json.dumps(records, sort_keys=True).encode()),
        "selected_keys": sorted(selected or existing),
        "contains_game_derived_data": True,
    }
    write_json(bundle / "manifest.json", manifest)
    return manifest


def verify_bundle(bundle: Path) -> dict:
    manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        raise ValueError("Unsupported bundle schema")
    cache = bundle / "cache"
    files = manifest["files"]
    if set(files) != {p.name for p in cache.iterdir()}:
        raise ValueError("Bundle files added/removed since capture")
    total = 0
    for name, record in files.items():
        if Path(name).name != name or name in {".", ".."}:
            raise ValueError("Invalid bundle filename")
        path = cache / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_FILE:
            raise ValueError(f"Invalid bundle file: {name}")
        data = path.read_bytes()
        total += len(data)
        if total > MAX_BUNDLE or len(data) != record["bytes"] or digest(data) != record["sha256"]:
            raise ValueError(f"Bundle changed/corrupt: {name}")
    if digest(json.dumps(files, sort_keys=True).encode()) != manifest["case_id"]:
        raise ValueError("Bundle manifest changed")
    return manifest


def summarize(events: list[dict], manifest: dict, cache: Path | None = None) -> dict:
    samples, audits = defaultdict(list), {}
    pending, errors = set(), []
    device = None
    variants = {}
    startup, startup_keys = None, {}
    for event in events:
        if event["event"] == "device":
            device = event
        elif event["event"] == "startup":
            startup = event
        elif event["event"] == "startup_key":
            startup_keys[event["key"]] = event
        elif event["event"] == "audit":
            audits[event["key"]] = event
        elif event["event"] == "shader_variant":
            variants[event["binary"]] = event
        elif event["event"] == "begin":
            pending.add((event["key"], event["round"]))
        elif event["event"] == "sample":
            pending.discard((event["key"], event["round"]))
            samples[event["key"]].append(event)
        elif event["event"] == "fatal":
            errors.append(event["error"])
    timings = {}
    for key, runs in samples.items():
        values = [r["reconstruct_ms"] for r in runs if r["ok"]]
        timings[key] = {
            "ok": len(values), "failed": len(runs) - len(values),
            "median_ms": statistics.median(values) if values else None,
            "max_ms": max(values) if values else None,
            "samples": runs,
            "phase_median_ms": {p: statistics.median([r["phases_ms"][p] for r in runs
                                 if r["ok"] and p in r.get("phases_ms", {})])
                                for p in PHASES if any(r["ok"] and p in r.get("phases_ms", {}) for r in runs)},
        }
    groups = {}
    for field in ("canonical", "dynamic_projection", "stages"):
        indexed = defaultdict(list)
        for key, audit in audits.items():
            # Include auxiliary generated shaders / all saved pipeline state.
            identity = (audit["kind"], json.dumps(audit[field]), audit["support"])
            indexed[identity].append(key)
        groups[field] = [sorted(v) for v in indexed.values() if len(v) > 1]
    # Equal canonical keys stored under different names are definite aliases.
    # Different keys with equal projections are candidates, NOT safe merge proofs.
    groups["dynamic_projection"] = [
        g for g in groups["dynamic_projection"]
        if len({audits[k]["canonical"] for k in g}) > 1
    ]
    spirv = defaultdict(list)
    for name, record in manifest["files"].items():
        if name.endswith(".spv"):
            spirv[record["sha256"]].append(name)
    rejection_hints = {}
    files = manifest["files"]
    translator = files.get("translator.compiler", {}).get("sha256")
    for key, timing in timings.items():
        if not timing["failed"] or key not in audits:
            continue
        hints = []
        for stage in audits[key]["stages"]:
            if not re.fullmatch("[0-9a-f]{16}", stage) or int(stage, 16) == 0:
                continue
            prefix = "0x" + stage
            if prefix + ".meta" not in files:
                hints.append(prefix + ": metadata missing")
            stamp = files.get(prefix + ".compiler", {}).get("sha256")
            if stamp is None:
                hints.append(prefix + ": compiler stamp missing")
            elif stamp != translator:
                hints.append(prefix + ": compiler stamp differs from translator")
        rejection_hints[key] = hints or ["Inspect loader/driver diagnostics; no stamp issue identified"]
    all_ms = sorted(r["reconstruct_ms"] for v in samples.values() for r in v if r["ok"])
    timed = [(k, r) for k, values in samples.items() for r in values if r["ok"] and "phases_ms" in r]
    phase_stats = {}
    for key, sample in timed:
        phases = sample["phases_ms"]
        if set(phases) != set(PHASES) or any(not math.isfinite(t) or t < 0 for t in phases.values()):
            errors.append(f"Invalid phase measurements: {key} round {sample['round']}")
        elif abs(sum(phases.values()) - sample["reconstruct_ms"]) > .1:
            errors.append(f"Phase total differs from reconstruction: {key} round {sample['round']}")
        elif phases["native_pipeline"] <= 0:
            errors.append(f"Native Vulkan timing absent: {key} round {sample['round']}")
    for phase in PHASES:
        values = [r["phases_ms"][phase] for _, r in timed if phase in r["phases_ms"]]
        if values:
            phase_stats[phase] = {"total_ms": sum(values), "median_ms": statistics.median(values),
                                  "p95_ms": percentile95(values), "max_ms": max(values),
                                  "slow_sample_total_ms": sum(r["phases_ms"].get(phase, 0) for _, r in timed
                                                              if r["reconstruct_ms"] > 33)}
    return {
        "schema": 1, "case_id": manifest["case_id"], "device": device,
        "errors": errors, "incomplete": sorted(pending), "pipelines": timings,
        "rejection_hints": rejection_hints,
        "startup": startup, "startup_keys": startup_keys,
        "phase_summary": phase_stats,
        "top_slow_samples": sorted([dict(r, key=k) for k, r in timed],
                                   key=lambda r: r["reconstruct_ms"], reverse=True)[:20],
        "rounds": {str(n): {"samples": len(v := [r["reconstruct_ms"] for runs in samples.values()
                                                for r in runs if r["ok"] and r["round"] == n]),
                            "p95_ms": percentile95(v), "over_33_ms": sum(t > 33 for t in v)}
                   for n in sorted({r["round"] for runs in samples.values() for r in runs})},
        "spirv_classification": classify_spirv(manifest, audits, variants, cache),
        "counts": {"successful": len(all_ms),
                   "failed": sum(t["failed"] for t in timings.values()),
                   "over_33_ms": sum(t > 33 for t in all_ms),
                   "over_50_ms": sum(t > 50 for t in all_ms),
                   "over_100_ms": sum(t > 100 for t in all_ms)},
        "p95_reconstruct_ms": all_ms[math.ceil(.95 * len(all_ms)) - 1] if all_ms else None,
        "audit": {"canonical_aliases": groups["canonical"],
                  "name_hash_mismatches": [{"key": k, "expected": a["expected_name"]}
                                          for k, a in audits.items()
                                          if a.get("expected_name", k) != k],
                  "dynamic_state_candidates": groups["dynamic_projection"],
                  "same_stages_groups": groups["stages"],
                  "identical_spirv_candidates": [v for v in spirv.values() if len(v) > 1]},
        "limitations": [
            "Measures production cached-pipeline reconstruction, not gameplay FPS/frame time.",
            "No guest shader translation, draw submission, scheduling contention or image validation.",
            "Empty means empty application VkPipelineCache; driver/OS caches may remain warm.",
            "Dynamic-state/SPIR-V candidates need review of metadata, descriptors and derived static state.",
            "Identical-SPIR-V audit includes all dependency files, including unselected shaders.",
        ],
    }


def render_report(summary: dict) -> str:
    lines = ["# Pipeline replay", "", f"Case: `{summary['case_id']}`", "",
             f"Successful: {summary['counts']['successful']}; failed: {summary['counts']['failed']}.",
             "", "| Pipeline | Median ms | Max ms | Passed / failed |",
             "|---|---:|---:|---:|"]
    for key, value in sorted(summary["pipelines"].items(),
                             key=lambda kv: kv[1]["max_ms"] or 0, reverse=True):
        lines.append(f"| `{key}` | {value['median_ms']} | {value['max_ms']} | {value['ok']} / {value['failed']} |")
    if summary.get("startup"):
        startup = summary["startup"]
        lines += ["", "## Production startup warm-up", "",
                  f"Loaded {startup['loaded']} of {startup['records']}; rejected {startup['rejected']}; "
                  f"total {startup['total_ms']:.3f} ms.",
                  "Per-key map availability is recorded in startup_keys. No per-key replay timings were collected.",
                  "The input database is read-only; shader rebuilding/migration is skipped."]
    lines += ["", "## Exclusive reconstruction phases", "",
              "| Phase | Total ms | Median ms | p95 ms | Max ms | ms in samples >33 ms |",
              "|---|---:|---:|---:|---:|---:|"]
    for name, value in sorted(summary.get("phase_summary", {}).items(), key=lambda kv: kv[1]["total_ms"], reverse=True):
        lines.append(f"| {name} | {value['total_ms']:.3f} | {value['median_ms']:.3f} | {value['p95_ms']:.3f} | "
                     f"{value['max_ms']:.3f} | {value['slow_sample_total_ms']:.3f} |")
    classification = summary.get("spirv_classification", {})
    lines += ["", "## SPIR-V classification", "",
              f"Same-program groups: {classification.get('same_program_groups', 0)}; "
              f"cross-program groups: {classification.get('cross_program_groups', 0)}.",
              f"Compiler states of members: {classification.get('member_compiler_states', {})}.",
              f"Review candidate pairs: {classification.get('review_candidate_count', 0)}. No merging is performed.",
              "", "## Key audit", ""]
    for name, groups in summary["audit"].items():
        lines.append(f"- {name}: {len(groups)} groups (details in report.json).")
    lines += ["", "## Limits", ""] + ["- " + s for s in summary["limitations"]]
    lines += ["", "## Errors / interrupted records", "", repr(summary["errors"]), repr(summary["incomplete"])]
    return "\n".join(lines) + "\n"


def run(bundle: Path, output: Path, exe: Path, repeats: int, mode: str, timeout: int) -> dict:
    if mode == "startup" and repeats != 1:
        raise ValueError("Startup mode requires one repetition in a fresh helper process")
    bundle, output, exe = bundle.resolve(), output.resolve(), exe.resolve(strict=True)
    manifest = verify_bundle(bundle)
    if output == bundle or bundle in output.parents:
        raise ValueError("Run output must be outside the immutable bundle")
    output.mkdir(parents=True, exist_ok=False)
    user = output / "user"
    user.mkdir()
    env = os.environ.copy()
    env["BB_GPU_USER_DIR"] = str(user)
    toolchain = ROOT / "builds/_internal/toolchain/clang64/bin"
    env["PATH"] = os.pathsep.join((str(exe.parent), str(toolchain), env.get("PATH", "")))
    command = [str(exe), str(bundle / "cache"), str(output / "events.jsonl"), str(repeats), mode]
    timed_out = False
    with (output / "process.log").open("wb") as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env,
                                    cwd=output, timeout=timeout, check=False)
            returncode = result.returncode
        except subprocess.TimeoutExpired:
            returncode, timed_out = -1, True
    events, parse_errors = [], []
    event_file = output / "events.jsonl"
    if event_file.exists():
        for line in event_file.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                parse_errors.append("Truncated event at process exit")
    summary = summarize(events, manifest, bundle / "cache")
    summary.update(returncode=returncode, timed_out=timed_out, repeats=repeats,
                   executable_sha256=digest(exe.read_bytes()),
                   command=command, environment={k: v for k, v in env.items() if k.startswith("BB_")})
    summary["errors"].extend(parse_errors)
    expected = 0 if mode == "startup" else len(manifest["selected_keys"]) * repeats
    summary["expected_samples"] = expected
    if summary["counts"]["successful"] + summary["counts"]["failed"] != expected:
        summary["errors"].append("Incomplete sample count")
    if mode == "startup":
        startup = summary.get("startup")
        if (not startup or startup["loaded"] != len(manifest["selected_keys"]) or startup["rejected"] or
                startup["records"] != len(manifest["selected_keys"]) or
                sorted(summary["startup_keys"]) != sorted(manifest["selected_keys"]) or
                not all(v["ok"] for v in summary["startup_keys"].values())):
            summary["errors"].append("Startup did not restore every selected native pipeline")
    try:
        verify_bundle(bundle)
    except ValueError as error:
        summary["errors"].append(str(error))
    summary["passed"] = not (returncode or summary["errors"] or summary["incomplete"] or summary["counts"]["failed"])
    write_json(output / "report.json", summary)
    (output / "report.md").write_text(render_report(summary), encoding="utf-8")
    return summary


def compare(before: dict, after: dict, threshold: float) -> dict:
    if any(report.get("device", {}).get("mode") == "startup" for report in (before, after)):
        raise ValueError("Startup mode has no per-key timings to compare")
    if before["case_id"] != after["case_id"] or not before["passed"] or not after["passed"]:
        raise ValueError("Comparison needs the same immutable case and two successful runs")
    if before.get("repeats") != after.get("repeats"):
        raise ValueError("Comparison needs identical repetition counts")
    if not math.isfinite(threshold) or threshold < 0:
        raise ValueError("Regression threshold must be finite and nonnegative")
    for field in ("gpu", "vendor_id", "device_id", "driver", "mode"):
        if before["device"][field] != after["device"][field]:
            raise ValueError(f"Incomparable runs: {field} differs")
    rows = []
    for key, old in before["pipelines"].items():
        new = after["pipelines"][key]
        delta = new["median_ms"] - old["median_ms"]
        rows.append({"key": key, "before_ms": old["median_ms"], "after_ms": new["median_ms"],
                     "delta_ms": delta, "regression": delta > threshold})
    return {"threshold_ms": threshold, "regressions": sum(r["regression"] for r in rows),
            "pipelines": sorted(rows, key=lambda r: r["delta_ms"], reverse=True)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("cache", type=Path)
    cap.add_argument("bundle", type=Path)
    cap.add_argument("--key", action="append", default=[], type=normalize_key)
    cap.add_argument("--from-log", type=Path)
    replay = sub.add_parser("run")
    replay.add_argument("bundle", type=Path)
    replay.add_argument("output", type=Path)
    replay.add_argument("--exe", type=Path, default=ROOT / "out/gpu/bb-pipeline-lab.exe")
    replay.add_argument("--repeats", type=int, choices=range(1, 101), metavar="1..100")
    replay.add_argument("--mode", choices=("empty", "warm", "startup"), default="empty")
    replay.add_argument("--timeout", type=int, default=600)
    diff = sub.add_parser("compare")
    diff.add_argument("before", type=Path)
    diff.add_argument("after", type=Path)
    diff.add_argument("--threshold-ms", type=float, default=5)
    args = parser.parse_args()
    try:
        if args.operation == "capture":
            keys = args.key
            if args.from_log:
                matched = log_keys(args.from_log.read_text(encoding="utf-8", errors="replace"))
                if not matched:
                    raise ValueError("No explicit pipeline keys in log; nothing selected")
                keys += matched
            result = capture(args.cache, args.bundle, keys)
            print(f"Captured {len(result['selected_keys'])} pipelines: {args.bundle}")
        elif args.operation == "run":
            result = run(args.bundle, args.output, args.exe,
                         args.repeats or (1 if args.mode == "startup" else 3), args.mode, args.timeout)
            print(f"{'PASS' if result['passed'] else 'FAIL'}: {args.output / 'report.md'}")
            return 0 if result["passed"] else 1
        else:
            result = compare(json.loads(args.before.read_text(encoding="utf-8")),
                             json.loads(args.after.read_text(encoding="utf-8")), args.threshold_ms)
            print(json.dumps(result, indent=2))
            return 1 if result["regressions"] else 0
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
