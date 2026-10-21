"""Recompile captured shader inputs in fresh processes; never writes the source cache."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STAGE = re.compile(r"0x[0-9a-f]{16}")
MAX_INPUT = 4 * 1024 * 1024


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def binary_name(data: bytes, stage: str) -> str:
    # Only decode the bounded identity prefix. The native helper validates the
    # complete capture, checksum, ABI and metadata generation before translation.
    if not 40 <= len(data) <= MAX_INPUT or struct.unpack_from('<II', data) != (0x49435354, 3):
        raise ValueError('unsupported shader capture')
    offset = 8
    for _ in range(2):
        if offset + 8 > len(data):
            raise ValueError('truncated capture identity')
        size, = struct.unpack_from('<Q', data, offset)
        offset += 8
        if size > 256 or offset + size > len(data):
            raise ValueError('invalid capture identity string')
        offset += size
    if offset + 24 > len(data):
        raise ValueError('truncated shader identity')
    hw, sw, program, permutation = struct.unpack_from('<IIQQ', data, offset)
    identity = program ^ ((permutation + 0x9e3779b9 + (program << 12) + (program >> 4)) & (2**64 - 1))
    if hw > 6 or sw >= 6 or permutation >= 4096 or f'0x{identity:016x}' != stage:
        raise ValueError('shader capture identity mismatch')
    return f'0x{program:016x}_{permutation}.spv'


def describe_spirv(data: bytes) -> dict:
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V size')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    histogram = Counter()
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 65535
        if size == 0 or offset + size > len(words):
            raise ValueError('invalid SPIR-V instruction')
        histogram[opcode] += 1
        offset += size
    return dict(sha256=digest(data), bytes=len(data), bound=words[3],
                opcodes=dict(sorted(histogram.items())))


def read_regular(path: Path, limit: int) -> bytes:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
        raise ValueError(f'missing, linked or oversized file: {path}')
    data = path.read_bytes()
    if not data or len(data) > limit:
        raise ValueError(f'empty or oversized file: {path}')
    return data


def summarize(runs: list[dict], stages: list[str]) -> dict:
    summary = {}
    for stage in stages:
        records = [r['stages'][stage] for r in runs]
        summary[stage] = dict(
            binary=records[0]['binary'],
            unique_binaries=len({r['spirv']['sha256'] for r in records}),
            unique_metadata=len({r['metadata_sha256'] for r in records}),
            unique_captures=len({r['capture_sha256'] for r in records}),
            unique_opcode_histograms=len({json.dumps(r['spirv']['opcodes'], sort_keys=True) for r in records}))
    binary_stable = len({r['compiler'] for r in runs}) == 1 and all(
        s['unique_binaries'] == 1 for s in summary.values())
    return dict(binary_stable=binary_stable, stages=summary)


def run(args) -> dict:
    source, output = args.cache.resolve(), args.output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError('output and source cache must be separate directories')
    helper, validator, checker = args.helper.resolve(), args.validator.resolve(), args.lookup_checker.resolve()
    helper_hash = digest(read_regular(helper, 512 * 1024 * 1024))
    validator_hash = digest(read_regular(validator, 64 * 1024 * 1024))
    checker_hash = digest(read_regular(checker, 512 * 1024 * 1024))
    stages = list(dict.fromkeys(args.stage or sorted(p.stem for p in source.glob('*.input')
                                                   if p.stat().st_size > 0)))
    if not 1 <= len(stages) <= 4096 or any(not STAGE.fullmatch(s) for s in stages):
        raise ValueError('select 1..4096 valid captured stage IDs')
    snapshot = {'profile.bin': read_regular(source / 'profile.bin', MAX_INPUT)}
    names = {}
    for stage in stages:
        data = read_regular(source / (stage + '.input'), MAX_INPUT)
        names[stage] = binary_name(data, stage)
        snapshot[stage + '.input'] = data
        snapshot[stage + '.meta'] = read_regular(source / (stage + '.meta'), MAX_INPUT)
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(helper.parent), str(checker.parent), str(validator.parent), env.get('PATH', '')])
    report = dict(source=str(source), helper_sha256=helper_hash, validator_sha256=validator_hash,
                  lookup_checker_sha256=checker_hash,
                  source_sha256={n: digest(b) for n, b in snapshot.items()}, runs=[])
    (output / 'source-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    for index in range(args.runs):
        case = output / f'run-{index:03d}'
        case.mkdir()
        (case / 'profile.bin').write_bytes(snapshot['profile.bin'])
        # Change unrelated traversal/allocation history without changing shader inputs.
        for stage in stages[::1 if index % 2 == 0 else -1]:
            for ext in ('.input', '.meta'):
                (case / (stage + ext)).write_bytes(snapshot[stage + ext])
        with (case / 'rebuild.log').open('wb') as log:
            result = subprocess.run([str(helper), str(case)], stdout=log, stderr=subprocess.STDOUT,
                                    env=env, cwd=case, timeout=args.timeout)
        expected = f'{len(stages)} rebuilt, 0 current, 0 skipped'
        if result.returncode or expected not in (case / 'rebuild.log').read_text(errors='replace'):
            raise RuntimeError(f'incomplete rebuild: {case / "rebuild.log"}')
        compiler = read_regular(case / 'translator.compiler', 128)
        # The serialized InfoPersistent contains unused static_vector storage and
        # SRT host pointers. Check typed fields and lookup compatibility natively;
        # retain raw hashes as evidence, never claim their bytes are canonical.
        with (case / 'lookup.log').open('wb') as log:
            result = subprocess.run([str(checker), '--compare-lookups', str(case),
                                     str(output / 'run-000')], stdout=log, stderr=subprocess.STDOUT,
                                    env=env, cwd=case, timeout=args.timeout)
        if result.returncode or f'PASS: {len(stages)} typed shader metadata records match reference' not in (
                case / 'lookup.log').read_text(errors='replace'):
            raise RuntimeError(f'metadata/lookup comparison failed: {case / "lookup.log"}')
        records = {}
        for stage in stages:
            if read_regular(case / (stage + '.compiler'), 128) != compiler:
                raise RuntimeError(f'uncommitted shader: {stage}')
            binary = case / names[stage]
            spirv = describe_spirv(read_regular(binary, 64 * 1024 * 1024))
            result = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(binary)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=30)
            if result.returncode:
                (case / (stage + '.validation.log')).write_bytes(result.stdout)
                raise RuntimeError(f'SPIR-V validation failed: {binary}')
            records[stage] = dict(binary=names[stage], spirv=spirv,
                metadata_sha256=digest(read_regular(case / (stage + '.meta'), MAX_INPUT)),
                capture_sha256=digest(read_regular(case / (stage + '.input'), MAX_INPUT)))
        report['runs'].append(dict(index=index, compiler=compiler.decode('ascii'), stages=records))
        print(f'Completed fresh-process run {index + 1}/{args.runs}: {len(stages)} validated shaders', flush=True)
    report['source_unchanged'] = all(read_regular(source / name, MAX_INPUT) == data
                                     for name, data in snapshot.items())
    report['tools_unchanged'] = (digest(helper.read_bytes()) == helper_hash and
                                  digest(validator.read_bytes()) == validator_hash and
                                  digest(checker.read_bytes()) == checker_hash)
    report.update(summarize(report['runs'], stages))
    report['typed_metadata_matches'] = True
    report['stable'] = report['binary_stable'] and report['source_unchanged'] and report['tools_unchanged']
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True, help='read-only captured input directory')
    parser.add_argument('--output', type=Path, required=True, help='new isolated result directory')
    parser.add_argument('--stage', action='append', help='0x + 16 lowercase hex digits; repeat to select stages')
    parser.add_argument('--runs', type=int, choices=range(2, 65), default=8, metavar='2..64')
    parser.add_argument('--timeout', type=int, choices=range(1, 601), default=120, metavar='1..600')
    parser.add_argument('--helper', type=Path, default=ROOT / 'out/bb-shader-lab.exe')
    parser.add_argument('--validator', type=Path,
                        default=ROOT / 'builds/_internal/toolchain/clang64/bin/spirv-val.exe')
    parser.add_argument('--lookup-checker', type=Path, default=ROOT / 'out/gpu/shader-cache-input-test.exe')
    args = parser.parse_args()
    try:
        report = run(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(2, f'shader reproducibility: {error}\n')
    print(json.dumps(dict(stable=report['stable'], stages=report['stages']), indent=2))
    return 0 if report['stable'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
