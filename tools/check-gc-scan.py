#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the texture-collector scan-liveness fix pinned and wired.

Eden's RunGarbageCollector budgets visits, not evictions: skipped images burn
`num_iterations` and stop the LRU scan, so images the port keeps (dirty below
the critical mark) shield everything behind them. The port patch in
tools/prepare-vulkan-port.py counts evictions instead and bounds the scan
separately, with dev-settings gc_scan=upstream restoring Eden's rule.

This check inspects the committed tree directly, so it runs on any host with
plain python3:

    python3 -B tools/check-gc-scan.py

When an Eden checkout is present (.deps/mirror-*/src/video_core/...), it also
verifies the three anchors against the real Eden source. Without one, that
part is skipped.
"""
import glob
import json
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
port = (root / "tools/prepare-vulkan-port.py").read_text()
perf = (root / "headless/performance.h").read_text()
main = (root / "headless/main.cpp").read_text()

# 1. The flag exists, defaults to the fix, and has an Eden-behavior escape hatch.
assert "gc_count_visits{false}" in perf, "flag default"
assert "gc_scan=upstream" in perf, "flag documented"
assert 'entry == "gc_scan=upstream"' in main, "settings parse"
assert "gc_count_visits = true" in main, "settings wire"

# 2. The patch declares the scan budget beside the eviction budget.
assert "size_t num_scanned = 0" in port, "scan counter"
assert "max_scanned = 8192" in port, "scan bound"

# 3. The scan stops on evictions exhausted OR scan bound; visits only burn
# under the upstream flag, evictions only without it.
assert "num_iterations == 0 || num_scanned >= max_scanned" in port, "stop rule"
assert "++num_scanned;" in port, "visit count"
assert "num_scanned = 0;" in port, "fresh allowance for the aggressive pass"
assert port.count("gc_count_visits.load(std::memory_order_relaxed)") == 2, \
    "flag read at both burn sites"

# 4. Against real Eden source when a checkout exists: each anchor the patch
# relies on must match exactly once (configure fails on drift too).
mirrors = sorted(glob.glob(str(root / ".deps/mirror-*/src/video_core/texture_cache/texture_cache.h")))
if not mirrors:
    print("check-gc-scan: SKIPPED pin-vs-source (no Eden texture_cache.h; structural checks passed)")
    sys.exit(0)
eden_commit = ""
try:
    manifest = json.loads((root / "UPSTREAM.json").read_text())
    eden_commit = str(manifest.get("eden_commit", ""))
except (OSError, ValueError):
    eden_commit = ""
pinned = [m for m in mirrors if eden_commit and eden_commit in m]
eden_path = Path(pinned[0] if pinned else mirrors[0])
eden = eden_path.read_text()
anchors = [
    "    size_t num_iterations = 0;",
    "        if (num_iterations == 0) {\n            return true;\n        }\n        --num_iterations;",
    "        if ((!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) || (!high_priority_mode && must_download)) {\n            return false;\n        }",
]
for anchor in anchors:
    assert eden.count(anchor) == 1, f"anchor must match {eden_path} exactly once: {anchor[:60]!r}"
print(f"GC scan liveness: evictions counted, scan bounded, flag wired; anchors pinned to {eden_path.name} PASS")
