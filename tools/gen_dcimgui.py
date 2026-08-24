#!/usr/bin/env python3
# /// script
# requires-python = ">=3.8"
# dependencies = ["ply"]
# ///
"""Regenerates lib/dcimgui/* from lib/imgui via lib/dear_bindings.

Run this after updating the lib/imgui submodule, then review the diff and commit.
`uv run tools/gen_dcimgui.py`
"""
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEAR_BINDINGS = REPO_ROOT / "lib" / "dear_bindings"
IMGUI = REPO_ROOT / "lib" / "imgui"
OUT = REPO_ROOT / "lib" / "dcimgui"

def run(*args):
    subprocess.run(
        [sys.executable, "dear_bindings.py", *args],
        cwd=DEAR_BINDINGS,
        check=True,
    )

run("-o", str(OUT / "dcimgui"), str(IMGUI / "imgui.h"))
run("-o", str(OUT / "dcimgui_internal"), "--include", str(IMGUI / "imgui.h"), str(IMGUI / "imgui_internal.h"))
