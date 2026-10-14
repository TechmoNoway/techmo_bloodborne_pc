# SPDX-License-Identifier: GPL-3.0-or-later
"""Protect Windows guest stores without changing their data or register effects.

The site coordinates come from d4rksp4rt4n's Bloodborne 1.09 analysis in
Supermedo/bloodborne_pc bc893e25eb961182ad9ddd455d3234de743dbc04.
See licenses/WINDOWS-RED-ZONE.md for provenance and coverage limitations.
Runtime use needs only the standard library; the pinned table is shipped beside
this module. No original game file is written.
"""
import bisect
import hashlib
import json
import os
from pathlib import Path
import struct

SITE_FILE = Path(__file__).with_name('red_zone_sites.json')
SITE_SHA256 = '10ae6c6f891c3ed8be71fa8e92ce7236843d07a2b5d5813268eab74b52692e28'
LEA_DOWN = bytes.fromhex('488d642480')            # lea rsp, [rsp-128], flags unchanged
LEA_UP = bytes.fromhex('488da42480000000')        # lea rsp, [rsp+128], flags unchanged
IMAGE_LIMIT = 512 * 1024 * 1024


def enabled(env=None):
    value = (os.environ if env is None else env).get('BB_RED_ZONE', '1')
    if value not in ('0', '1'):
        raise ValueError('BB_RED_ZONE must be 0 or 1')
    return value == '1'


def validate_sites(sites):
    """Reject malformed or overlapping plans before any image mutation."""
    result, end = [], 0
    for row in sites:
        if not isinstance(row, (list, tuple)) or len(row) != 3:
            raise ValueError('Invalid red-zone site')
        offset, original, lengths = row
        if type(offset) is not int or offset < end or not isinstance(original, str):
            raise ValueError('Unsorted or overlapping red-zone sites')
        try:
            body = bytes.fromhex(original)
        except ValueError as error:
            raise ValueError('Invalid red-zone instruction bytes') from error
        if not 5 <= len(body) <= 4096 or not isinstance(lengths, (list, tuple)) or not lengths:
            raise ValueError('Invalid red-zone instruction span')
        if any(type(n) is not int or not 1 <= abs(n) <= 15 for n in lengths):
            raise ValueError('Invalid red-zone instruction lengths')
        if sum(abs(n) for n in lengths) != len(body) or not any(n < 0 for n in lengths):
            raise ValueError('Red-zone site does not describe complete store instructions')
        end = offset + len(body)
        if end > IMAGE_LIMIT:
            raise ValueError('Red-zone site outside the image limit')
        result.append((offset, body, tuple(lengths)))
    return result


def load_sites():
    raw = SITE_FILE.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SITE_SHA256:
        raise ValueError('Red-zone site table differs from the reviewed version')
    plan = json.loads(raw)
    if plan.get('eboot') != 'Bloodborne 01.09' or len(plan.get('sites', [])) != 607:
        raise ValueError('Unsupported red-zone site table')
    return validate_sites(plan['sites']), plan


def patch_ranges(path):
    """Read selected startup writes so a later patch cannot overwrite a detour."""
    raw = Path(path).read_bytes()
    if len(raw) < 16:
        raise ValueError('Truncated startup patch file')
    magic = raw[:8]
    if magic == b'BBPATCH2':
        if len(raw) < 24 or struct.unpack_from('<Q', raw, 8)[0] != 0x400000:
            raise ValueError('Unsupported startup patch base')
        count, = struct.unpack_from('<Q', raw, 16)
        position = 24
    elif magic == b'BBPATCH1':
        count, = struct.unpack_from('<Q', raw, 8)
        position = 16
    else:
        raise ValueError('Unsupported startup patch format')
    if count > 100000:
        raise ValueError('Too many startup patches')
    ranges = []
    for _ in range(count):
        if position + 16 > len(raw):
            raise ValueError('Truncated startup patch record')
        offset, size = struct.unpack_from('<QQ', raw, position)
        position += 16
        if not size or size > len(raw) - position or offset + size > IMAGE_LIMIT:
            raise ValueError('Invalid startup patch range')
        ranges.append((offset, size))
        position += size
    if position != len(raw):
        raise ValueError('Unexpected startup patch trailer')
    return ranges


def merge_ranges(ranges):
    merged = []
    for offset, size in sorted(ranges):
        if type(offset) is not int or type(size) is not int or offset < 0 or size <= 0:
            raise ValueError('Invalid protected conflict range')
        end = offset + size
        if merged and offset <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((offset, end))
    return merged


def protect(image, sites, base, executable_ranges, blocked_ranges=()):
    """Build all detours transactionally; caller appends the returned RX segment.

    Negative lengths in the pinned plan identify non-stack stores. Positive
    instructions, including stack loads, run with the original RSP. Reviewed
    spans have no RIP-relative operand, control flow, stack-pointer modification
    or interior branch target. A decoder is deliberately not needed at startup.
    """
    if type(base) is not int or base < len(image) or base > IMAGE_LIMIT:
        raise ValueError('Invalid red-zone trampoline base')
    conflicts = merge_ranges(blocked_ranges)
    ends = [end for _, end in conflicts]
    code, updates = bytearray(), []
    report = {'enabled': True, 'table_sha256': SITE_SHA256, 'total_sites': len(sites),
              'applied_sites': 0, 'protected_stores': 0, 'changed_sites': [],
              'conflicting_sites': [], 'outside_code_sites': [], 'patched_ranges': []}
    for offset, original, lengths in sites:
        end = offset + len(original)
        if end > len(image) or not any(start <= offset and end <= start + size
                                      for start, size in executable_ranges):
            report['outside_code_sites'].append(hex(offset))
            continue
        if image[offset:end] != original:
            report['changed_sites'].append(hex(offset))
            continue
        at = bisect.bisect_right(ends, offset)
        if at < len(conflicts) and conflicts[at][0] < end:
            report['conflicting_sites'].append(hex(offset))
            continue
        trampoline = base + len(code)
        cursor = 0
        stores = 0
        for length in lengths:
            instruction = original[cursor:cursor + abs(length)]
            if length < 0:
                code += LEA_DOWN + instruction + LEA_UP
                stores += 1
            else:
                code += instruction
            cursor += abs(length)
        try:
            code += b'\xe9' + struct.pack('<i', end - (base + len(code) + 5))
            detour = b'\xe9' + struct.pack('<i', trampoline - (offset + 5))
        except struct.error as error:
            raise ValueError('Red-zone detour exceeds rel32 range') from error
        code += b'\xcc' * (-len(code) % 16)
        if base + len(code) > IMAGE_LIMIT:
            raise ValueError('Red-zone trampolines exceed image limit')
        updates.append((offset, detour + b'\xcc' * (len(original) - 5)))
        report['protected_stores'] += stores
        report['patched_ranges'].append([offset, len(original)])
    # An exception above leaves the original image intact.
    for offset, replacement in updates:
        image[offset:offset + len(replacement)] = replacement
    report['applied_sites'] = len(updates)
    report['trampoline_bytes'] = len(code)
    return bytes(code), report
