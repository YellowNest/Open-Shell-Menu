#!/usr/bin/env python3
"""Validate rendered taskbar replacement/restoration independently of process logs."""
import json
import sys
from pathlib import Path
from PIL import Image

def changed(left: Path, right: Path, threshold: int = 25) -> int:
    with Image.open(left) as lhs, Image.open(right) as rhs:
        if lhs.size != rhs.size:
            raise AssertionError(f"Screenshot sizes differ: {lhs.size} != {rhs.size}")
        width, height = lhs.size
        roi = (width // 5, max(0, height - 58), width * 4 // 5, height)
        a = lhs.convert("RGB").crop(roi).tobytes()
        b = rhs.convert("RGB").crop(roi).tobytes()
        return sum(max(abs(a[i + j] - b[i + j]) for j in range(3)) > threshold
                   for i in range(0, len(a), 3))

def verify(directory: Path):
    report = json.loads((directory / "win11-native-x64-proof.json").read_text(encoding="utf-8-sig"))
    assert report["NativeWindows11X64"] is True, report["OS"]
    assert report["OSArchitecture"] == "X64"
    assert report["ProcessArchitecture"] == "X64"
    cycles = report["Cycles"]
    assert len(cycles) == 4, len(cycles)
    baseline = directory / "tap-baseline.png"
    output = []
    for i, cycle in enumerate(cycles, start=1):
        active = directory / f"tap-cycle-{i}-active.png"
        stopped = directory / f"tap-cycle-{i}-stopped.png"
        replacement = changed(active, stopped)
        restoration = changed(baseline, stopped)
        disabled = cycle["EnableStartButton"] == 0
        assert cycle["Passed"] and cycle["MenuStopped"] and cycle["ExplorerPidContinuity"], cycle
        assert restoration <= 60, f"cycle {i}: native Start glyph not restored: {restoration} pixels"
        if disabled:
            assert replacement <= 60, f"cycle {i}: disabled replacement changed taskbar: {replacement}"
        else:
            assert replacement >= 350, f"cycle {i}: enabled replacement not visible: {replacement}"
        output.append(dict(cycle=i, enabled=not disabled,
                           changed_active_vs_stopped=replacement,
                           changed_stopped_vs_baseline=restoration,
                           explorer_pid_unchanged=cycle["ExplorerPidContinuity"]))
    print(json.dumps(dict(result="PASS", platform=report["OS"], cycles=output), indent=2))
    print("NOTE: pixel comparison cannot prove internal XAML property state.")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: check_win11_taskbar_evidence.py EVIDENCE_DIRECTORY")
    verify(Path(sys.argv[1]))
