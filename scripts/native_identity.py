"""Identify the executable selected for this launch, independently of folder names."""
import hashlib
import json
from pathlib import Path


def identify_runtime(executable):
    executable = Path(executable).resolve()
    result = {'path': str(executable), 'available': False, 'manifest_match': None}
    try:
        with executable.open('rb') as stream:
            before = executable.stat()
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            after = executable.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            result['error'] = 'executable_changed_during_hash'
            return result, {}
        result.update(available=True, sha256=digest, size_bytes=after.st_size)
    except OSError as error:
        result['error'] = type(error).__name__
        return result, {}
    try:
        manifest = json.loads((executable.parent / 'native-build.json').read_text(encoding='utf-8-sig'))
        if not isinstance(manifest, dict):
            raise ValueError('manifest is not an object')
    except (OSError, ValueError):
        result['manifest_status'] = 'missing_or_invalid'
        return result, {}
    binaries = manifest.get('binaries', {})
    record = binaries.get(executable.name, {}) if isinstance(binaries, dict) else {}
    expected = record.get('sha256') if isinstance(record, dict) else None
    if isinstance(expected, str) and len(expected) == 64:
        result['expected_sha256'] = expected
        result['manifest_match'] = expected.lower() == digest
        result['manifest_status'] = 'matched' if result['manifest_match'] else 'mismatch'
        if result['manifest_match'] and isinstance(manifest.get('source_identity'), dict):
            result['build_identity'] = manifest['source_identity']
    else:
        result['manifest_status'] = 'executable_not_recorded'
    # Version claims remain in native_build; only a matched hash links them to this EXE.
    return result, manifest
