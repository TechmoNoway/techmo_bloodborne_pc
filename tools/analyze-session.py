#!/usr/bin/env python3
"""Recompute flip statistics for a repeatable section of one recorded session."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from session_diagnostics import frame_summary, write_json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('session', type=Path)
parser.add_argument('--from-seconds', type=float, default=0)
parser.add_argument('--to-seconds', type=float)
args = parser.parse_args()
if args.from_seconds < 0 or (args.to_seconds is not None and args.to_seconds <= args.from_seconds):
    parser.error('Use a nonnegative start and an end greater than the start.')
result = frame_summary(args.session / 'frames.csv', args.from_seconds, args.to_seconds)
write_json(args.session / 'benchmark.json', result)
print(json.dumps(result, indent=2))
