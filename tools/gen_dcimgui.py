#!/usr/bin/env python3
# /// script
# requires-python = ">=3.8"
# dependencies = ["ply"]
# ///
"""Regenerates lib/dcimgui/* from lib/imgui via lib/dear_bindings.

Run this after updating the lib/imgui submodule, then review the diff and commit.
Requires: uv (https://github.com/astral-sh/uv) - run with `uv run tools/gen_dcimgui.py`
"""
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEAR_BINDINGS = REPO_ROOT / "lib" / "dear_bindings"
IMGUI = REPO_ROOT / "lib" / "imgui"
OUT = REPO_ROOT / "lib" / "dcimgui"
MARKER = OUT / ".imgui_commit"

def imgui_commit():
    return subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=IMGUI, capture_output=True, text=True, check=True
    ).stdout.strip()

def run(*args):
    subprocess.run(
        [sys.executable, "dear_bindings.py", *args],
        cwd=DEAR_BINDINGS,
        check=True,
    )

current = imgui_commit()
# Rebuild systems can lose track of whether generated sources are up to date
# (e.g. deleting the build directory wipes MSBuild's .tlog bookkeeping), so
# this check - not file timestamps - is the real source of truth for whether
# regeneration is actually needed.
if MARKER.exists() and MARKER.read_text().strip() == current:
    print(f"dcimgui bindings already up to date with imgui @ {current[:8]}")
    sys.exit(0)

run("-o", str(OUT / "dcimgui"), str(IMGUI / "imgui.h"))
run("-o", str(OUT / "dcimgui_internal"), "--include", str(IMGUI / "imgui.h"), str(IMGUI / "imgui_internal.h"))
MARKER.write_text(current + "\n")
